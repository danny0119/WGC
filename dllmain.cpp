// dllmain.cpp — WGC截图DLL v3
// 修复: 回调中消费帧+PeekMessage消息泵+处理后释放帧+诊断计数器
#include "pch.h"
#include <d3d11.h>
#include <dwmapi.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <windows.graphics.capture.interop.h>

#pragma comment(lib, "D3D11.lib")
#pragma comment(lib, "Dwmapi.lib")
#pragma comment(lib, "windowsapp.lib")

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Graphics;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

char g_lastError[512] = "";
int g_captureWidth = 0;
int g_captureHeight = 0;
volatile long g_callbackCount = 0;    // ★ 诊断: 回调触发次数

void SetLastError(const char* msg) {
    strncpy_s(g_lastError, msg, sizeof(g_lastError) - 1);
    g_lastError[sizeof(g_lastError) - 1] = '\0';
}

// ★ 提前声明 UUID
struct __declspec(uuid("A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1"))
    IDirect3DDxgiInterfaceAccess : ::IUnknown {
    virtual HRESULT __stdcall GetInterface(GUID const& id, void** object) = 0;
};

class SimpleWGC {
public:
    SimpleWGC(HWND hwnd) : m_hwnd(hwnd), m_hEvent(NULL), m_hasFrame(false) {
        m_hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    }

    ~SimpleWGC() {
        CloseSession();
        if (m_hEvent) CloseHandle(m_hEvent);
    }

    int InitSession() {
        HRESULT hr = S_OK;

        // Step 1: 创建 D3D11 设备
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr, 0, D3D11_SDK_VERSION,
            m_d3dDevice.put(), nullptr, nullptr);
        if (FAILED(hr)) {
            snprintf(g_lastError, sizeof(g_lastError),
                "D3D11CreateDevice failed: 0x%08X", hr);
            return 1;
        }

        // Step 2: 创建 WinRT IDirect3DDevice
        IDirect3DDevice device = nullptr;
        auto dxgiDevice = m_d3dDevice.as<IDXGIDevice>();
        {
            winrt::com_ptr<::IInspectable> inspectable;
            hr = CreateDirect3D11DeviceFromDXGIDevice(
                dxgiDevice.get(), inspectable.put());
            if (FAILED(hr)) {
                snprintf(g_lastError, sizeof(g_lastError),
                    "CreateDirect3D11DeviceFromDXGIDevice failed: 0x%08X", hr);
                return 2;
            }
            inspectable.as(device);
        }

        m_d3dDevice->GetImmediateContext(m_d3dContext.put());

        // Step 3: 创建 CaptureItem，获取精确尺寸
        auto activationFactory = get_activation_factory<GraphicsCaptureItem>();
        if (!activationFactory) {
            snprintf(g_lastError, sizeof(g_lastError), "get_activation_factory failed");
            return 4;
        }

        auto interopFactory = activationFactory.as<IGraphicsCaptureItemInterop>();
        if (!interopFactory) {
            snprintf(g_lastError, sizeof(g_lastError),
                "as<IGraphicsCaptureItemInterop> failed");
            return 5;
        }

        GraphicsCaptureItem captureItem = nullptr;
        hr = interopFactory->CreateForWindow(m_hwnd,
            guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(),
            reinterpret_cast<void**>(put_abi(captureItem)));
        if (FAILED(hr) || !captureItem) {
            snprintf(g_lastError, sizeof(g_lastError),
                "CreateForWindow failed: 0x%08X (hwnd=%p)", hr, (void*)m_hwnd);
            return 6;
        }

        auto size = captureItem.Size();
        g_captureWidth = size.Width;
        g_captureHeight = size.Height;
        if (g_captureWidth <= 0 || g_captureHeight <= 0) {
            RECT rect{};
            GetWindowRect(m_hwnd, &rect);
            g_captureWidth = rect.right - rect.left;
            g_captureHeight = rect.bottom - rect.top;
        }
        if (g_captureWidth <= 0 || g_captureHeight <= 0) {
            g_captureWidth = 1920;
            g_captureHeight = 1080;
        }

        // Step 4: 创建 FramePool
        m_framePool = Direct3D11CaptureFramePool::Create(
            device,
            DirectXPixelFormat::B8G8R8A8UIntNormalized,
            2,
            SizeInt32{ g_captureWidth, g_captureHeight });
        if (!m_framePool) {
            snprintf(g_lastError, sizeof(g_lastError),
                "FramePool::Create failed (size=%dx%d)",
                g_captureWidth, g_captureHeight);
            return 3;
        }

        // Step 5: 创建 Session
        m_session = m_framePool.CreateCaptureSession(captureItem);
        if (!m_session) {
            snprintf(g_lastError, sizeof(g_lastError), "CreateCaptureSession failed");
            return 7;
        }

        // ★★★ Step 6: 回调中消费帧，排空旧帧保留最新 ★★★
        m_framePool.FrameArrived([&](auto& framePool, auto&) {
            InterlockedIncrement(&g_callbackCount);

            // 排空池，只保留最新帧
            while (true) {
                auto frame = framePool.TryGetNextFrame();
                if (!frame) break;
                if (m_latestFrame) {
                    m_latestFrame.Close();  // 归还旧帧缓冲区给池
                }
                m_latestFrame = frame;
            }

            m_hasFrame = true;
            SetEvent(m_hEvent);
            });

        m_session.IsCursorCaptureEnabled(false);
        m_session.StartCapture();

        return 0;
    }

    unsigned char* CaptureWindow(unsigned char* buffer,
        int left, int top, int width, int height) {
        // ===== 等待帧可用 =====
        clock_t timer = std::clock();
        while (!m_hasFrame) {
            // ★★★ 关键: 必须泵消息，否则WinRT回调无法投递 ★★★
            MSG msg;
            while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE) > 0) {
                DispatchMessage(&msg);
            }

            if (WaitForSingleObject(m_hEvent, 10) == WAIT_OBJECT_0) {
                break;
            }

            // 在 CaptureWindow 中修改超时时间
            if (std::clock() - timer > 2000) {  // ★ 从5s改为2s
                snprintf(g_lastError, sizeof(g_lastError),
                    "CaptureWindow timeout (2s), callbacks=%d",
                    InterlockedCompareExchange(&g_callbackCount, 0, 0));
                m_hasFrame = false;
                ResetEvent(m_hEvent);
                return nullptr;
            }
        }

        // ===== 额外泵消息，确保回调已执行完 =====
        MSG msg;
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE) > 0) {
            DispatchMessage(&msg);
        }

        if (!m_latestFrame) {
            snprintf(g_lastError, sizeof(g_lastError),
                "No frame after wait, callbacks=%d",
                InterlockedCompareExchange(&g_callbackCount, 0, 0));
            m_hasFrame = false;
            ResetEvent(m_hEvent);
            return nullptr;
        }

        // ===== 从帧获取纹理 =====
        auto access = m_latestFrame.Surface().as<IDirect3DDxgiInterfaceAccess>();
        winrt::com_ptr<ID3D11Texture2D> texture;
        access->GetInterface(guid_of<ID3D11Texture2D>(), texture.put_void());

        unsigned char* result = nullptr;
        if (texture) {
            // 边界检查
            D3D11_TEXTURE2D_DESC desc;
            texture->GetDesc(&desc);
            if (left + width > (int)desc.Width)
                width = (int)desc.Width - left;
            if (top + height > (int)desc.Height)
                height = (int)desc.Height - top;
            if (left < 0) left = 0;
            if (top < 0) top = 0;

            if (width > 0 && height > 0) {
                result = grabByRegion(texture, buffer, left, top, width, height);
            }
            else {
                snprintf(g_lastError, sizeof(g_lastError),
                    "Invalid region: left=%d top=%d width=%d height=%d",
                    left, top, width, height);
            }
        }
        else {
            snprintf(g_lastError, sizeof(g_lastError), "Failed to get texture from frame");
        }

        // ★★★ 关键: 处理完毕后关闭帧，归还缓冲区给池 ★★★
        m_latestFrame.Close();
        m_latestFrame = nullptr;

        m_hasFrame = false;
        ResetEvent(m_hEvent);

        return result;
    }

private:
    void CloseSession() {
        if (m_latestFrame) {
            m_latestFrame.Close();
            m_latestFrame = nullptr;
        }
        if (m_session) { m_session.Close(); m_session = nullptr; }
        if (m_framePool) { m_framePool.Close(); m_framePool = nullptr; }
    }

    unsigned char* grabByRegion(winrt::com_ptr<ID3D11Texture2D>& texture,
        unsigned char* buffer,
        int left, int top, int width, int height) {
        D3D11_TEXTURE2D_DESC desc;
        texture->GetDesc(&desc);

        D3D11_TEXTURE2D_DESC stagingDesc = desc;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.BindFlags = 0;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        stagingDesc.MiscFlags = 0;
        stagingDesc.Width = width;
        stagingDesc.Height = height;

        winrt::com_ptr<ID3D11Texture2D> userTexture = nullptr;
        check_hresult(m_d3dDevice->CreateTexture2D(&stagingDesc, NULL, userTexture.put()));

        D3D11_BOX sourceRegion;
        sourceRegion.left = left;
        sourceRegion.top = top;
        sourceRegion.right = left + width;
        sourceRegion.bottom = top + height;
        sourceRegion.front = 0;
        sourceRegion.back = 1;

        m_d3dContext->CopySubresourceRegion(
            userTexture.get(), 0, 0, 0, 0, texture.get(), 0, &sourceRegion);

        D3D11_MAPPED_SUBRESOURCE resource;
        check_hresult(m_d3dContext->Map(
            userTexture.get(), NULL, D3D11_MAP_READ, 0, &resource));

        UINT lBmpRowPitch = width * 4;
        auto sptr = static_cast<BYTE*>(resource.pData);
        auto dptr = reinterpret_cast<BYTE*>(buffer);
        UINT lRowPitch = std::min<UINT>(lBmpRowPitch, resource.RowPitch);

        for (size_t h = 0; h < (size_t)height; ++h) {
            memcpy_s(dptr, lBmpRowPitch, sptr, lRowPitch);
            sptr += resource.RowPitch;
            dptr += lBmpRowPitch;
        }
        m_d3dContext->Unmap(userTexture.get(), NULL);

        return buffer;
    }

private:
    HWND m_hwnd;
    HANDLE m_hEvent;
    volatile bool m_hasFrame;
    Direct3D11CaptureFrame m_latestFrame = nullptr;  // ★ 存储最新帧
    winrt::com_ptr<ID3D11Device> m_d3dDevice;
    winrt::com_ptr<ID3D11DeviceContext> m_d3dContext;
    Direct3D11CaptureFramePool m_framePool = nullptr;
    GraphicsCaptureSession m_session = nullptr;
};

SimpleWGC* g_wgc = nullptr;

extern "C" {
    __declspec(dllexport) int init_dxgi(HWND hwnd) {
        if (g_wgc) { delete g_wgc; g_wgc = nullptr; }
        g_captureWidth = 0;
        g_captureHeight = 0;
        g_callbackCount = 0;

        try {
            g_wgc = new SimpleWGC(hwnd);
            int result = g_wgc->InitSession();
            if (result != 0) {
                delete g_wgc;
                g_wgc = nullptr;
            }
            return result;
        }
        catch (...) {
            snprintf(g_lastError, sizeof(g_lastError), "C++ exception in init_dxgi");
            if (g_wgc) { delete g_wgc; g_wgc = nullptr; }
            return -1;
        }
    }

    __declspec(dllexport) unsigned char* grab(
        unsigned char* buffer, int left, int top, int width, int height) {
        try {
            if (g_wgc) {
                return g_wgc->CaptureWindow(buffer, left, top, width, height);
            }
        }
        catch (...) {
            snprintf(g_lastError, sizeof(g_lastError), "C++ exception in grab");
        }
        return nullptr;
    }

    __declspec(dllexport) void destroy() {
        try {
            if (g_wgc) { delete g_wgc; g_wgc = nullptr; }
        }
        catch (...) {}
    }

    __declspec(dllexport) const char* get_last_error() {
        return g_lastError;
    }

    // ★ 返回 WGC 帧尺寸: (width << 16) | height
    __declspec(dllexport) int get_capture_size() {
        if (g_captureWidth > 0 && g_captureHeight > 0) {
            return (g_captureWidth << 16) | (g_captureHeight & 0xFFFF);
        }
        return 0;
    }

    // ★★★ 新增: 返回回调触发次数 (诊断用) ★★★
    __declspec(dllexport) int get_callback_count() {
        return InterlockedCompareExchange(&g_callbackCount, 0, 0);
    }
}