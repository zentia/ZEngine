# 移除 GLFW，使用原生平台接口 — 实施计划

参考 UE 的 `FGenericWindow` / `FWindowsWindow` / `FWindowsApplication` 模式。

## 架构

```
Platform/
├── Generic/
│   ├── GenericWindow.h       # 抽象窗口接口
│   └── GenericApplication.h  # 抽象应用（消息循环）
├── Windows/
│   ├── WindowsWindow.h       # Win32 HWND 封装
│   ├── WindowsWindow.cpp
│   ├── WindowsApplication.h  # Win32 消息循环
│   └── WindowsApplication.cpp
├── Linux/   (暂留 stub)
└── Mac/    (暂留 stub)
```

`WindowSystem` 变为平台无关，内部持有 `GenericApplication*`，将所有操作委托给它。

## 实施步骤

### Step 1: 通用抽象层
- [ ] `GenericWindow.h` — 虚接口，含回调 `std::function` 向量
- [ ] `GenericApplication.h` — 虚接口：`Initialize`, `PollEvents`, `CreateChildWindow`, `GetMainWindow`

### Step 2: Win32 实现
- [ ] `WindowsWindow.h/cpp` — `CreateWindowExW` + `WNDPROC` → 分发到回调
- [ ] `WindowsApplication.h/cpp` — `PumpMessages` (`PeekMessage` 循环)
- [ ] 处理：`WM_KEYDOWN/UP`, `WM_CHAR`, `WM_MOUSE*`, `WM_SIZE`, `WM_CLOSE`, `WM_SETFOCUS`, `WM_PAINT`

### Step 3: 改造 WindowSystem
- [ ] `WindowSystem.h` — 移除 `#include <GLFW/glfw3.h>`，持有 `GenericApplication*`
- [ ] `WindowSystem.cpp` — 根据 `Z_PLATFORM_*` 创建对应 `GenericApplication`
- [ ] 公共 API 用 `void*` 替代 `GLFWwindow*`（Windows 上即 `HWND`）

### Step 4: 更新所有 GLFW 调用点
按优先级：
- [ ] `InputSystem.cpp` — key codes 映射（需建 `KeyCode<->Win32 VK` 表）
- [ ] `EnhancedInputSystem.cpp` — 同上
- [ ] `UISystem.cpp` — 剪贴板 + key mapping
- [ ] `EditorSlateHost.cpp` — 光标、DPI、`GetWindowPos`
- [ ] `FloatingPanelManager.cpp` — 子窗口创建、位置、DPI
- [ ] `RenderDoc.cpp` — 获取 `HWND`
- [ ] `EditorUtility.cpp` — 文件对话框获取 `HWND`
- [ ] `ContentBrowserAssetActions.cpp` — 同上

### Step 5: 构建系统
- [ ] `CMakeLists.txt` — 移除 `glfw` 依赖，按平台编译对应 `.cpp`
- [ ] 3rdparty — 可移除 `glfw` （或保留 Emscripten 路径）

### Step 6: 其他平台
- [ ] Linux — X11 / Wayland stub
- [ ] macOS — Cocoa stub（`mm` 文件）
- [ ] Emscripten — 保留 canvas 路径（或也用原生 JS）

## 关键设计决策

### 窗口句柄类型
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
