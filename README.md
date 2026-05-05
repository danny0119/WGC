# WGC
Windows.Graphics.Capture方案
# Windows Graphics Capture (WGC) DLL 截图方案文档

## 目录

------

[toc]



## 1. 方案概述

本方案基于 Windows 10 1903+ 引入的 **Windows Graphics Capture (WGC)** API，将其封装为 C++ DLL，供 Python 等高级语言调用。

**核心优势：**

- **硬件加速**：直接从 GPU FramePool 获取 D3D11 纹理，零 CPU 拷贝开销（直到 Map 阶段）。
- **安全稳定**：避免了传统 DXGI Desktop Duplication 因安全桌面/UAC 弹窗导致的黑屏或崩溃。
- **区域裁剪**：内置 `CopySubresourceRegion`，支持仅截取窗口的指定区域，减少显存到内存的带宽占用。

------

## 2. 环境与编译指南

### 2.1 运行环境要求

- **操作系统**：Windows 10 1903 (Build 18362) 及以上。
- **运行库**：需安装 [Visual C++ Redistributable for Visual Studio 2022 (x64)](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist)。
- **架构**：仅支持 **x64**（WGC API 限制）。

### 2.2 VS2022 编译配置

1. 创建“动态链接库(DLL)”项目。
2. **配置属性** -> **常规** -> 目标平台版本 >= 10.0.18362.0。
3. **C/C++** -> **语言** -> C++语言标准：**ISO C++17 标准 (/std:c++17)**。
4. **C/C++** -> **命令行** -> 附加选项：添加 `/await`（WinRT 协程支持关键项）。
5. 确保 `pch.h` 中包含了必要的 WinRT 头文件引用。

------

## 3. API 接口详解

DLL 导出了以下 6 个 C 风格接口，调用约定为 `__stdcall`（建议配合 `.def` 文件防止名称粉碎）：

| 函数签名                                                     | 功能说明                                   | 返回值                                     |
| ------------------------------------------------------------ | ------------------------------------------ | ------------------------------------------ |
| `int init_dxgi(HWND hwnd)`                                   | 初始化 WGC 会话，绑定目标窗口              | 0=成功, >0=失败步骤, -1=C++异常            |
| `unsigned char* grab(unsigned char* buffer, int left, int top, int width, int height)` | 捕获指定区域帧，写入 `buffer`              | 成功返回 `buffer` 指针，失败返回 `nullptr` |
| `void destroy()`                                             | 销毁会话，释放 WGC 和 D3D11 资源           | 无                                         |
| `const char* get_last_error()`                               | 获取最近一次错误的详细字符串               | C 字符串指针                               |
| `int get_capture_size()`                                     | 获取 WGC 实际帧池尺寸                      | `(Width << 16) | Height`                   |
| `int get_callback_count()`                                   | 获取 `FrameArrived` 回调触发次数（诊断用） | 整数                                       |



**⚠️ 内存安全约定：** `grab` 函数的 `buffer` 必须由调用方（如 Python）分配，DLL 只负责填充数据。避免跨语言堆内存分配导致泄漏。所需 buffer 大小为 `width * height * 4` (BGRA)。

------

## 4. Python 调用示例与详解

### 4.1 完整调用代码

需安装依赖：`pip install numpy opencv-python pywin32`

```python
import ctypes
import ctypes.wintypes as wintypes
import numpy as np
import win32gui
import cv2

# ================= 1. 加载 DLL =================
dll = ctypes.CDLL('./WGCapture.dll')

# ================= 2. 声明函数签名 =================
dll.init_dxgi.argtypes = [wintypes.HWND]
dll.init_dxgi.restype = ctypes.c_int

dll.grab.argtypes = [ctypes.POINTER(ctypes.c_ubyte), ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int]
dll.grab.restype = ctypes.POINTER(ctypes.c_ubyte)

dll.destroy.argtypes = []
dll.destroy.restype = None

dll.get_last_error.argtypes = []
dll.get_last_error.restype = ctypes.c_char_p

dll.get_capture_size.argtypes = []
dll.get_capture_size.restype = ctypes.c_int

dll.get_callback_count.argtypes = []
dll.get_callback_count.restype = ctypes.c_int

# ================= 3. 查找目标窗口 =================
hwnd = win32gui.FindWindow("Notepad", None)
if not hwnd:
    print("未找到目标窗口")
    exit()

# ================= 4. 初始化会话 =================
ret = dll.init_dxgi(hwnd)
if ret != 0:
    err = dll.get_last_error()
    print(f"初始化失败，错误码: {ret}, 信息: {err.decode('gbk', errors='ignore')}")
    exit()

# ================= 5. 解析帧尺寸并分配内存 =================
size_int = dll.get_capture_size()
width = (size_int >> 16) & 0xFFFF
height = size_int & 0xFFFF
print(f"捕获尺寸: {width}x{height}")

# 预分配 BGRA Buffer (宽*高*4字节)
buf_size = width * height * 4
c_buffer = (ctypes.c_ubyte * buf_size)()

# ================= 6. 循环捕获 =================
print("开始捕获，按 'q' 退出...")
while True:
    # 传入 buffer，全屏捕获 (0,0,width,height)
    ptr = dll.grab(c_buffer, 0, 0, width, height)
    
    if ptr:
        # 零拷贝将 C 数组映射为 Numpy 数组
        img_np = np.frombuffer(c_buffer, dtype=np.uint8).reshape((height, width, 4))
        # BGRA 转 BGR 供 OpenCV 显示
        img_bgr = cv2.cvtColor(img_np, cv2.COLOR_BGRA2BGR)
        
        cv2.imshow("WGC Capture", img_bgr)
        if cv2.waitKey(1) & 0xFF == ord('q'):
            break
    else:
        # 获取错误信息及回调计数诊断
        err = dll.get_last_error()
        cb = dll.get_callback_count()
        print(f"捕获失败: {err.decode('gbk', errors='ignore')}, 回调数: {cb}")

# ================= 7. 销毁会话 =================
dll.destroy()
cv2.destroyAllWindows()
```

### 4.2 Python 端关键优化点

- **预分配 Buffer**：`(ctypes.c_ubyte * buf_size)()` 在循环外创建，避免每帧申请/释放内存。
- **Numpy 零拷贝映射**：`np.frombuffer` 直接索引 C 级内存，不产生额外拷贝。

------

## 5. 核心机制深度解析

### 5.1 消息泵 (`PeekMessage`) 的必要性

WGC 的 `FrameArrived` 回调依赖于 Windows 的 **STA (Single-Threaded Apartment)** 消息机制。如果调用 DLL 的线程（Python 主线程）没有消息循环，WinRT 的底层事件将无法派发。

- **解决**：在 `CaptureWindow` 的等待循环中加入 `PeekMessage` 强制泵消息，确保回调能被触发。

### 5.2 帧池排空机制

WGC 的 FramePool 缓冲区默认只有 2 帧。如果处理速度慢于屏幕刷新率，旧帧会堆积。

- **解决**：在 `FrameArrived` 回调中，使用 `while(TryGetNextFrame)` 排空池，只保留最新的 `m_latestFrame`，并将旧帧 `Close()` 归还显存，确保 `grab` 取到的永远是最新画面。

### 5.3 GPU-CPU 同步安全

在 `grabByRegion` 中，`m_d3dContext->Map` 会**阻塞当前线程**，直到 GPU 执行完前面的 `CopySubresourceRegion` 指令。这意味着 `Map` 返回时，数据已安全拷贝至 `buffer`，随后立即 `m_latestFrame.Close()` 释放帧池显存是绝对安全的。

------

## 6. 进阶：Vulkan 游戏捕获支持

### 6.1 为什么 WGC 默认截不到 Vulkan？

WGC 本质上截取的是 DWM (Desktop Window Manager) 合成后的桌面流。Vulkan/DirectX12 游戏常使用 **Flip Presentation Model** 和 **DirectFlip / Independent Flip** 优化。在此模式下，游戏绕过 DWM 的后缓冲区直接与显示器交换帧，导致 WGC 捕获到的该窗口区域可能是**黑屏**或**旧画面**。

### 6.2 解决方案：Vulkan Hook (类似 OBS 机制)

OBS 针对 Vulkan 黑屏问题，采用了 **API Hooking** 技术。需要增加以下模块：

1. **动态注入 DLL**：将 Hook DLL 注入到目标游戏进程。
2. **拦截 `vkQueuePresentKHR`**：这是 Vulkan 呈现帧的最终函数。
3. **强制拷贝至共享句柄**：在 Hook 函数中，在调用原始 `vkQueuePresentKHR` 前，将即将呈现的 Vulkan Image 拷贝到一个共享的 DXGI Handle（跨进程共享纹理）。
4. **外部捕获进程读取**：主程序（Python 调用的 DLL）通过 `ID3D11Device1::OpenSharedResource1` 打开这个句柄，获取画面。

**实现难点评估：**

- 需编写 MinHook 或 Detours 库的汇编级 Hook 代码。
- 需处理 Vulkan Memory 与 D3D11 Shared Handle 的跨 API 互操作（极其复杂，通常需借助 VK_KHR_external_memory 扩展）。
- **建议**：对于 Python 方案，不要自己写 Vulkan Hook。建议利用 **OBS Virtual Camera** 作为桥梁，或者借助现成的开源库如 **frida-gum** 进行动态注入。



