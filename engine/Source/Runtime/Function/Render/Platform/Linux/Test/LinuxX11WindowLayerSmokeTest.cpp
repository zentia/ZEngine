// ============================================================================
// LinuxX11WindowLayerSmokeTest — Linux X11 窗口层（commit 6a0a578b）真机验证
// ----------------------------------------------------------------------------
// 证明三件事（对应 doc/platform_windowing_plan.md 的 Linux 落地记录）：
//
//  (a) 无头降级（headless）：无 DISPLAY 时 LinuxApplication::Initialize() 返回
//      false 且不崩、不泄漏（XOpenDisplay 失败路径），GetNativeDisplay()==nullptr。
//      这是 WSL 里 `unset DISPLAY` 直接可跑的路径 —— 窗口层在 headless 工具/CI
//      里不会成为崩溃源。
//
//  (b) 真实窗口（Xvfb 虚拟帧缓冲）：有 DISPLAY 时 Initialize() 返回 true，
//      主窗口建出（GetMainWindow()!=null，GetNativeHandle()!=0），共享的
//      Display* 非空（GetNativeDisplay()!=null，供 VulkanRHI 建 xlib surface），
//      PollEvents() 抽干队列不崩，析构（Shutdown）干净。
//
//  (c) 多显示器枚举（XRandR，C7 子项）：GetMonitorCount()>=1，
//      GetPrimaryMonitorWorkArea() 给出 width/height>0，按 index 查询
//      GetMonitorWorkArea(i) 不越界崩溃；再建一个子窗口（CreateChildWindow）
//      并销毁（DestroyChildWindow），验证子窗口生命周期。
//
// 平台矩阵：本测试只在原生 Linux 桌面（Z_PLATFORM_LINUX && !__EMSCRIPTEN__）
// 实例化 LinuxApplication。Emscripten 下该 TU 的 .cpp 编译为空，且窗口层不
// 适用；非 Linux 主机（Windows/macOS）无法替 Linux 验证，直接跳过（PASS）。
//
// 为什么能无头实例化 LinuxApplication：构造/析构不触碰 X server（构造只是默认
// 成员初始化；析构调 Shutdown()，m_Display 为 null 时是空操作）。只有
// Initialize() 才 XOpenDisplay；无 DISPLAY 时它干净返回 false。因此 new +
// Initialize + delete 无需真实 X server 即可验证降级路径（Xvfb 下才走真窗口）。
//
// 构建（Linux，在 WSL 里）：
//   ./build_linux.sh debug ninja --target LinuxX11WindowLayerSmokeTest
//   # 无头：unset DISPLAY 后运行；真窗口：在 Xvfb 下运行（见 wsl_verify_linux.sh）
// ============================================================================

#include "Runtime/Function/Render/Platform/Linux/LinuxApplication.h"

#include <cstdio>
#include <cstdlib>

namespace
{
    int g_Failures = 0;

    void Check(bool cond, const char* msg)
    {
        if (cond)
        {
            std::printf("  [ ok ] %s\n", msg);
        }
        else
        {
            std::printf("  [FAIL] %s\n", msg);
            ++g_Failures;
        }
    }
}  // namespace

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;

#if defined(Z_PLATFORM_LINUX) && !defined(__EMSCRIPTEN__)
    const char* display = std::getenv("DISPLAY");
    const bool has_display = (display != nullptr && display[0] != '\0');

    std::printf("LinuxX11WindowLayerSmokeTest (DISPLAY=%s)\n",
                has_display ? display : "<unset>");

    // -----------------------------------------------------------------------
    // (a) 无头降级路径：无论有无 DISPLAY 都先验证——XOpenDisplay 失败时
    //     Initialize() 必须返回 false 且不崩，GetNativeDisplay() 保持 null。
    //     用一个必然不存在的 display 名强制走失败分支，保证 CI 里无 X server
    //     也能闭环（不必真的 unset DISPLAY）。
    // -----------------------------------------------------------------------
    {
        LinuxApplication app;
        Check(app.GetNativeDisplay() == nullptr,
              "headless: GetNativeDisplay() is null before init");

        // 指向一个不存在的 display，强制 XOpenDisplay 失败（:99.99 通常无 server）。
        std::setenv("DISPLAY", ":99.99", 1);
        const bool ok = app.Initialize("headless-probe", 800, 600);
        Check(!ok, "headless: Initialize() returns false when XOpenDisplay fails");
        Check(app.GetNativeDisplay() == nullptr,
              "headless: GetNativeDisplay() stays null after failed init");
        // 恢复环境变量，供下面 (b)/(c) 在 Xvfb 下走真窗口路径。
        if (has_display)
            std::setenv("DISPLAY", display, 1);
        else
            std::unsetenv("DISPLAY");
        // app 出作用域 → ~LinuxApplication() → Shutdown()（m_Display 为 null，空操作）。
    }

    // -----------------------------------------------------------------------
    // (b)/(c) 真窗口路径：仅在真的有 DISPLAY（Xvfb 或真 X server）时跑。无 DISPLAY
    //     时跳过（不算失败）——WSL 里 `unset DISPLAY` 只验证 (a)。
    // -----------------------------------------------------------------------
    if (!has_display)
    {
        std::printf("  [skip] real-window path: DISPLAY unset (run under Xvfb to exercise)\n");
    }
    else
    {
        LinuxApplication app;
        Check(app.GetNativeDisplay() == nullptr,
              "window: GetNativeDisplay() is null before init");

        const bool ok = app.Initialize("x11-smoke", 1280, 720);
        Check(ok, "window: Initialize() succeeds with a real DISPLAY");

        if (ok)
        {
            // 主窗口 + 共享 Display*（供 VulkanRHI 建 xlib surface）。
            GenericWindow* main = app.GetMainWindow();
            Check(main != nullptr, "window: GetMainWindow() returns the main window");
            Check(main->GetNativeHandle() != nullptr,
                  "window: main window has a non-null native handle (X Window)");
            Check(app.GetNativeDisplay() != nullptr,
                  "window: GetNativeDisplay() exposes the shared Display* (for xlib surface)");

            // 抽干一次事件队列（拉模型），无事件时不崩。
            app.PollEvents();
            Check(true, "window: PollEvents() drains the (empty) queue without crash");

            // (c) 多显示器枚举（XRandR）。
            const int monitor_count = app.GetMonitorCount();
            Check(monitor_count >= 1, "monitor: GetMonitorCount() >= 1");

            const auto primary = app.GetPrimaryMonitorWorkArea();
            Check(primary.width > 0 && primary.height > 0,
                  "monitor: GetPrimaryMonitorWorkArea() has positive size");

            // 按 index 查询：0 应等于主屏；越界 index 回落主屏，不崩溃。
            const auto idx0 = app.GetMonitorWorkArea(0);
            Check(idx0.width > 0 && idx0.height > 0,
                  "monitor: GetMonitorWorkArea(0) is valid");
            if (monitor_count >= 1)
            {
                Check(idx0.x == primary.x && idx0.y == primary.y &&
                          idx0.width == primary.width && idx0.height == primary.height,
                      "monitor: GetMonitorWorkArea(0) matches primary");
            }
            const auto oob = app.GetMonitorWorkArea(9999);  // 越界
            Check(oob.width > 0 || oob.height > 0,
                  "monitor: out-of-range index falls back (no crash)");

            // 窗口所在显示器查询（主窗口中心应落在某个 output 上）。
            const auto for_win = app.GetMonitorWorkAreaForWindow(main);
            Check(for_win.width > 0 && for_win.height > 0,
                  "monitor: GetMonitorWorkAreaForWindow(main) resolves an output");

            // 子窗口生命周期（编辑器浮窗语义）。
            GenericWindow* child = app.CreateChildWindow(
                "x11-smoke-child", 320, 240, /*pos_x=*/0, /*pos_y=*/0, /*decorated=*/true);
            Check(child != nullptr, "child: CreateChildWindow() returns a window");
            if (child)
            {
                Check(child->GetNativeHandle() != nullptr,
                      "child: child has a non-null native handle");
                app.DestroyChildWindow(child);
                Check(true, "child: DestroyChildWindow() returned without crash");
            }

            // 剪贴板：无其它 owner 时 GetClipboardText 应超时返回空串，不崩。
            const std::string clip = app.GetClipboardText();
            (void)clip;
            Check(true, "clipboard: GetClipboardText() returned without crash");

            app.ShowMainWindow();
            app.PollEvents();
        }

        // app 出作用域 → ~LinuxApplication() → Shutdown()：XCloseDisplay + 释放窗口。
    }

    if (g_Failures == 0)
    {
        std::printf("LinuxX11WindowLayerSmokeTest: ALL CHECKS PASSED\n");
        return 0;
    }
    std::printf("LinuxX11WindowLayerSmokeTest: %d CHECK(S) FAILED\n", g_Failures);
    return 1;
#else
    // 非原生 Linux 桌面（Windows / macOS / Emscripten / 移动）：窗口层 .cpp 未编译，
    // 无法在此实例化 LinuxApplication。这不是失败——本机替不了 Linux 真机，跳过。
    std::printf("LinuxX11WindowLayerSmokeTest: SKIP (not native Linux desktop)\n");
    return 0;
#endif
}
