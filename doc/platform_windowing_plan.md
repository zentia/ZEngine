# 移除 GLFW，使用原生平台接口 — 实施计划

参考 UE 的 `FGenericWindow` / `FWindowsWindow` / `FWindowsApplication` 模式。

> **状态（2026-10-25 逐项对代码核对）**：Windows 与 macOS 已完全脱离 GLFW——引擎源码里**零** `#include <GLFW>`、
> 零 `glfw*()` 调用，3rdparty 里也不再有 glfw；剩下的 “GLFW” 字样只在注释里（`KeyCodes.h` 沿用 GLFW 的键值/修饰键布局，
> 这是有意保留的兼容约定）。Linux 桌面与 Web 没有引擎自有窗口（见 Step 6）。下表的勾选以代码为准，原计划的空勾选框
> 已过时。

## 架构（实际落地）

```
engine/Source/Runtime/Function/Render/Platform/
├── Generic/
│   ├── GenericWindow.h        # 抽象窗口：GetNativeHandle / 尺寸 / DPI + OnKey..OnWindowRefresh 回调向量
│   └── GenericApplication.h   # 抽象应用：Initialize / PollEvents / 子窗口 / 剪贴板 / 光标 / 显示器 / 键鼠状态
├── Windows/
│   ├── WindowsWindow.h/.cpp       # CreateWindowExW + WNDPROC（GWLP_USERDATA 存 this）
│   └── WindowsApplication.h/.cpp  # PeekMessageW 消息泵、剪贴板、光标、显示器工作区
└── MacOS/
    ├── MacOSWindow.h/.mm          # NSWindow + NSView，MetalRHI 挂 CAMetalLayer
    └── MacOSApplication.h/.mm
```

`WindowSystem`（`Function/Render/WindowSystem.{h,cpp}`）平台无关，持有 `std::unique_ptr<GenericApplication>`，由
`CreatePlatformApplication()` 按平台创建；没有引擎自有窗口的平台返回 `nullptr`，`WindowSystem` 退化为输入/事件外壳，
所有访问器都容忍空 application。公共句柄是 `void* GetNativeWindowHandle()`（Windows 上即 `HWND`），**没有**采用原计划的
`WindowHandle` 别名。

## 实施步骤（勾选 = 代码已兑现）

### Step 1: 通用抽象层
- [x] `GenericWindow.h` — 虚接口 + 12 组 `std::function` 回调向量
- [x] `GenericApplication.h` — `Initialize` / `PollEvents` / `CreateChildWindow` / `GetMainWindow`，另有剪贴板、标准光标、
  显示器工作区、`IsKeyDown` / `IsMouseButtonDown`

### Step 2: Win32 实现
- [x] `WindowsWindow.h/cpp` — `CreateWindowExW` + `WNDPROC` → 回调
- [x] `WindowsApplication.h/cpp` — `PeekMessageW` 循环
- [x] 处理 `WM_KEYDOWN/UP`、`WM_SYSKEY*`、`WM_CHAR`、`WM_[LRM]BUTTON*`、`WM_MOUSEMOVE`、`WM_MOUSEWHEEL`、`WM_SIZE`、
  `WM_CLOSE`、`WM_SETFOCUS/KILLFOCUS`、`WM_PAINT`、`WM_DROPFILES`、`WM_DPICHANGED`、`WM_SETCURSOR`
- [x] **2026-10-25 补齐**（此前是 TODO，且实际影响功能）：
  - 修饰键：键盘/鼠标/字符事件的 `mods` 原来恒为 0，`EditorInputManager` 的 Ctrl+Z / Ctrl+Y 撤销重做、ZSlate 的
    Ctrl/Shift/Alt 状态在 Windows 上从来拿不到修饰键。现由 `GetKeyState` 算出 `KeyCodes::MOD_*` 位（与 macOS 的
    `MapModifiers` 同一编码）。
  - 自动重复：`WM_KEYDOWN` 的 lParam 第 30 位 = 重复，上报 `REPEAT(2)`（原来恒为 `PRESS`，与 GLFW / macOS 不一致）。
    `GenericApplication::UpdateKeyState` 同时改为只有 `RELEASE` 才清除按下状态——原实现把 `REPEAT` 当松开，macOS
    上按住 WASD 过了系统重复延迟就会被判为松开。
  - 拖放：`WM_DROPFILES` 原来只把第一个路径读进局部缓冲就丢掉，`OnDrop` 从不触发，AGENTS.md 里写着“已落地”的
    OS 拖拽导入在 Windows 上实际是死的。现在遍历全部文件、转 UTF-8 后派发。
  - `EditorSlateHost` / `FloatingPanelManager` 手写的 `mods & 1/2/4` 把 Ctrl 与 Shift 弄反了（`KeyCodes` 里 SHIFT=1、
    CONTROL=2）；改用 `KeyCodes::MOD_*_VAL` 常量。
- [ ] 仍是 TODO：`WindowsApplication::GetMonitorWorkArea(index)` 不枚举多显示器，任何 index 都返回主显示器
  （`GetMonitorWorkAreaForWindow` 是对的）。

### Step 3: 改造 WindowSystem
- [x] `WindowSystem.h` 不再 include GLFW，持有 `GenericApplication`
- [x] `WindowSystem.cpp` 按 `Z_PLATFORM_*` 创建 `WindowsApplication` / `MacOSApplication`
- [x] 公共 API 用 `void*` 句柄（见上）
- [x] **2026-10-25 修**：Linux 原生构建原来落到 `#error "CreatePlatformApplication: unsupported platform"`，任何链接
  ZRuntime 的 Linux 目标（含 `wsl_verify_linux.sh` 要跑的 `RHIFactoryRegistrySmokeTest`）都编不过；现返回 `nullptr`。
  空 application 的守卫原来只覆盖 Apple / Android / OHOS，Web（emcc 继承 `Z_PLATFORM_LINUX=1`）会在
  `Initialize()` 里解引用空指针——node 下实测修复前 `memory access out of bounds`、修复后通过。

### Step 4: 更新所有 GLFW 调用点
- [x] `InputSystem.cpp` / `EnhancedInputSystem.cpp` — 只消费 `KeyCodes`；VK→KeyCode 映射在 `WindowsWindow.cpp` 的
  `MapVkToKeyCode`，Cocoa 侧在 `MacOSWindow.mm` 的 `MapKeyCode`
- [x] `UISystem.cpp` — 剪贴板走 `GenericApplication`
- [x] `EditorSlateHost.cpp` — 光标、DPI、窗口位置走 `GenericApplication` / `GenericWindow`
- [x] `FloatingPanelManager.cpp` — 子窗口经 `CreateChildWindow`
- [x] `RenderDoc.cpp` / `EditorUtility.cpp` / `ContentBrowserAssetActions.cpp` — 取 `HWND` 经 `GetNativeWindowHandle`

### Step 5: 构建系统
- [x] 不再链接 / 包含 glfw；`engine/3rdparty/` 下已无 glfw 目录
- [x] **2026-10-25 清理**：`engine/3rdparty/CMakeLists.txt` 里给 GLFW 设的 `USE_MSVC_RUNTIME_LIBRARY_DLL` CRT 开关已无
  消费者，删除；README 的 Linux 依赖去掉 `libglfw3-dev`；AGENTS.md 里几处“WindowSystem 调 glfwSetDropCallback”之类
  的描述改为现状

### Step 6: 其他平台
- [ ] Linux — **没有窗口层**：`CreatePlatformApplication` 返回 `nullptr`（无头可用），`VulkanRHI` 的 Linux surface 创建
  （`CreateVulkanSurface`）仍返回 `VK_ERROR_FEATURE_NOT_PRESENT`。要做 Linux 桌面编辑器需 X11（或 Wayland）
  `GenericApplication` + xlib/xcb surface，属独立工作。
- [x] macOS — 已是完整 Cocoa 实现（不是 stub）：`MacOSWindow.mm` 约 810 行、`MacOSApplication.mm` 约 425 行
- [x] Emscripten — 无引擎窗口，canvas 归页面；`WebGL2RHI::CreateGLContext` 原注释说“GLFW shim 已建好上下文”，
  但引擎早已不链接 GLFW（web 链接参数里也没有 `USE_GLFW`），于是没有任何人创建上下文。现改为：页面已有当前上下文就
  沿用，否则自己在 `#canvas` 上创建 WebGL2 上下文。**浏览器里未实测。**

## 关键设计决策

### 窗口句柄类型（原计划；实际落地为 `void* GetNativeWindowHandle()`）
```cpp
// WindowSystem.h 公共 API
#if Z_PLATFORM_WINDOWS
    using WindowHandle = HWND;
#elif Z_PLATFORM_MACOS
    using WindowHandle = void*; // NSWindow*
#else
    using WindowHandle = void*;
#endif
```

### 回调分发
Win32 `WNDPROC` 中通过 `SetWindowLongPtr(hwnd, GWLP_USERDATA)` 存入 `WindowsWindow*`，消息到达时取出并分发到对应 `std::function` 向量。

### 子窗口
每个子窗口是一个独立的 `HWND` + `WindowsWindow` 实例，使用同一个窗口类（同一个 `WNDPROC`）。

## GLFW API → Win32 对照表

| GLFW API | Win32 等价实现 |
|---|---|
| `glfwInit()` | 无需（Win32 直接调用） |
| `glfwCreateWindow()` | `CreateWindowExW()` |
| `glfwDestroyWindow()` | `DestroyWindow()` |
| `glfwTerminate()` | 无需 |
| `glfwSetKeyCallback()` | `WM_KEYDOWN/UP` → `OnKey` |
| `glfwSetCharCallback()` | `WM_CHAR` → `OnChar` |
| `glfwSetMouseButtonCallback()` | `WM_LBUTTONDOWN/UP` 等 → `OnMouseButton` |
| `glfwSetCursorPosCallback()` | `WM_MOUSEMOVE` → `OnCursorPos` |
| `glfwSetScrollCallback()` | `WM_MOUSEWHEEL` → `OnScroll` |
| `glfwSetDropCallback()` | `WM_DROPFILES` → `OnDrop` |
| `glfwSetWindowSizeCallback()` | `WM_SIZE` → `OnWindowSize` |
| `glfwSetWindowCloseCallback()` | `WM_CLOSE` → `OnWindowClose` |
| `glfwSetWindowFocusCallback()` | `WM_SETFOCUS/WM_KILLFOCUS` → `OnWindowFocus` |
| `glfwSetWindowRefreshCallback()` | `WM_PAINT` → `OnWindowRefresh` |
| `glfwPollEvents()` | `PeekMessage` + `TranslateMessage` + `DispatchMessage` |
| `glfwGetFramebufferSize()` | `GetClientRect` + DPI 缩放 |
| `glfwSetWindowTitle()` | `SetWindowTextW()` |
| `glfwShowWindow()` | `ShowWindow(hwnd, SW_SHOW)` |
| `glfwHideWindow()` | `ShowWindow(hwnd, SW_HIDE)` |
| `glfwMaximizeWindow()` | `ShowWindow(hwnd, SW_MAXIMIZE)` |
| `glfwGetWindowPos()` | `GetWindowRect` |
| `glfwSetWindowPos()` | `SetWindowPos` |
| `glfwGetWindowContentScale()` | `GetDpiForWindow` / `GetScaleFactorForMonitor` |
| `glfwSetInputMode()` | `ClipCursor` / `ShowCursor` (鼠标捕获) |
| `glfwGetMouseButton()` | `GetAsyncKeyState` 或跟踪状态 |

## 参考 UE 源码路径

- `../UnrealEngine/Engine/Source/Runtime/ApplicationCore/Public/Windows/WindowsWindow.h`
- `../UnrealEngine/Engine/Source/Runtime/ApplicationCore/Private/Windows/WindowsWindow.cpp`
- `../UnrealEngine/Engine/Source/Runtime/ApplicationCore/Public/Windows/WindowsApplication.h`
- `../UnrealEngine/Engine/Source/Runtime/ApplicationCore/Private/Windows/WindowsApplication.cpp`
- `../UnrealEngine/Engine/Source/Runtime/ApplicationCore/Public/GenericPlatform/GenericWindow.h`
- `../UnrealEngine/Engine/Source/Runtime/ApplicationCore/Public/GenericPlatform/GenericApplication.h`
