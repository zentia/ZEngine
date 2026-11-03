// Linux X11 应用实现，参考 UE FX11Application。
//
// 整个 TU 只在原生 Linux 桌面（X11，非 Emscripten）编译。见 LinuxWindow.cpp 的守卫说明。

#include "LinuxApplication.h"

#if defined(Z_PLATFORM_LINUX) && !defined(__EMSCRIPTEN__) && !defined(ZENGINE_LINUX_WINDOW_STUB)

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrandr.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

LinuxApplication::LinuxApplication() = default;

LinuxApplication::~LinuxApplication()
{
    Shutdown();
}

namespace
{
    // Xlib 的默认错误处理器会 terminate 整个进程：一个无害的 BadMatch（例如窗口
    // 还没 viewable 就 SetInputFocus，或者客户端和 WM 抢焦点）就能让编辑器直接
    // 死掉。这里改成记日志继续跑——和 Windows 的消息泵对 WM_ 错误码的宽容度一致。
    int HandleXError(Display* display, XErrorEvent* event)
    {
        char text[256] = {};
        XGetErrorText(display, event->error_code, text, sizeof(text));
        std::fprintf(stderr, "[X11] error: %s (request %d, minor %d)\n", text,
                     static_cast<int>(event->request_code), static_cast<int>(event->minor_code));
        return 0;
    }
}



bool LinuxApplication::Initialize(const char* title, int width, int height)
{
    // 打开 X server 连接，全应用共享一个 Display*。无头环境（无 DISPLAY）返回 false，
    // 调用方（WindowSystem 工厂）应回落到无窗口路径——Linux 上跑 headless 工具/冒烟
    // 测试不依赖窗口。
    // 必须在任何请求之前装上，否则第一次 X 错误就直接 abort。
    XSetErrorHandler(HandleXError);

    Display* display = XOpenDisplay(nullptr);
    if (!display)
    {
        return false;
    }
    m_Display = display;

    m_MainWindow = std::make_unique<LinuxWindow>();
    if (!m_MainWindow->Initialize(title, width, height,
                                   true,    // decorated
                                   false,   // 初始隐藏（ShowMainWindow 在 splash 后）
                                   true))
    {
        XCloseDisplay(display);
        m_Display = nullptr;
        return false;
    }
    m_MainWindow->SetParentApplication(this);
    m_AllWindows.push_back(m_MainWindow.get());
    return true;
}

void LinuxApplication::Shutdown()
{
    m_OwnedChildren.clear();
    m_MainWindow.reset();
    m_AllWindows.clear();
    if (m_Display)
    {
        XCloseDisplay(static_cast<Display*>(m_Display));
        m_Display = nullptr;
    }
}

void LinuxApplication::PollEvents()
{
    if (!m_Display) return;
    auto* display = static_cast<Display*>(m_Display);

    // 拉模型：抽干当前队列里所有待处理事件，按 Window 句柄路由到对应窗口。
    while (XPending(display) > 0)
    {
        XEvent event;
        XNextEvent(display, &event);

        // 找到事件所属窗口（按 X Window 句柄匹配）。
        LinuxWindow* target = nullptr;
        for (GenericWindow* w : m_AllWindows)
        {
            auto* lw = static_cast<LinuxWindow*>(w);
            if (lw->GetXWindow() == event.xany.window)
            {
                target = lw;
                break;
            }
        }
        // 事件可能发给子窗口/根窗口；找不到归属时尝试用 map_window 反查（简化：忽略）。
        if (target)
        {
            target->ProcessEvent(&event);
            // 主窗口的 ShouldClose 反映到应用级关闭。
            if (m_MainWindow && m_MainWindow->ShouldClose())
            {
                m_ShouldClose = true;
            }
        }
    }
}

void LinuxApplication::SetTitle(const char* title)
{
    if (m_MainWindow)
        m_MainWindow->SetTitle(title);
}

void LinuxApplication::ShowMainWindow()
{
    if (m_MainWindow)
        m_MainWindow->Show();
}

void LinuxApplication::SetCursorMode(bool capture)
{
    if (m_CursorCaptured == capture)
        return;
    m_CursorCaptured = capture;

    if (!m_Display || !m_MainWindow) return;
    auto* display = static_cast<Display*>(m_Display);
    const unsigned long root = RootWindow(display, DefaultScreen(display));

    if (capture)
    {
        // 把鼠标限制在主窗口客户区内（等价 Windows ClipCursor / GLFW_CURSOR_DISABLED）。
        XWarpPointer(display, None, m_MainWindow->GetXWindow(), 0, 0, 0, 0, 0, 0);
        // X11 无引用计数隐藏光标的等价原语；用 XFlush + 依赖窗口管理器的指针捕获。
        // 真正的指针锁定需 _NET_WMPointerGrab（合成器扩展），此处保守只做 warp。
    }
    else
    {
        // 释放：X11 无全局 ClipCursor(nullptr) 等价；warp 回窗口中心即可。
    }
    XFlush(display);
}

GenericWindow* LinuxApplication::CreateChildWindow(const char* title,
                                                   int        width,
                                                   int        height,
                                                   int        pos_x,
                                                   int        pos_y,
                                                   bool       decorated)
{
    auto child = std::make_unique<LinuxWindow>();
    if (!child->Initialize(title, width, height, decorated, false, true))
    {
        return nullptr;
    }
    child->SetPosition(pos_x, pos_y);
    child->SetParentApplication(this);

    GenericWindow* result = child.get();
    m_OwnedChildren.push_back(std::move(child));
    m_AllWindows.push_back(result);
    return result;
}

void LinuxApplication::DestroyChildWindow(GenericWindow* window)
{
    if (!window) return;
    auto it = std::find(m_AllWindows.begin(), m_AllWindows.end(), window);
    if (it != m_AllWindows.end())
        m_AllWindows.erase(it);

    for (auto cit = m_OwnedChildren.begin(); cit != m_OwnedChildren.end(); ++cit)
    {
        if (cit->get() == window)
        {
            cit->get()->Destroy();
            m_OwnedChildren.erase(cit);
            break;
        }
    }
}

GenericWindow* LinuxApplication::GetMainWindow() const
{
    return m_MainWindow.get();
}

std::array<int, 2> LinuxApplication::GetFramebufferSize() const
{
    if (!m_MainWindow) return {0, 0};
    return m_MainWindow->GetFramebufferSize();
}

// ---------------------------------------------------------------------------
// 剪贴板：X11 的 CLIPBOARD selection（UTF8_STRING 目标）。
// 简化实现：Set 走 XSetSelectionOwner；Get 走 XConvertSelection + 阻塞等
// SelectionNotify。完整的剪贴板协议（selection 请求方、增量传输）超出本层
// 需要，这里覆盖"复制一段文本 / 读回一段文本"的常见路径。
void LinuxApplication::SetClipboardText(const char* text)
{
    if (!m_Display || !m_MainWindow) return;
    auto* display = static_cast<Display*>(m_Display);
    Atom clipboard = XInternAtom(display, "CLIPBOARD", False);
    Atom utf8 = XInternAtom(display, "UTF8_STRING", False);

    // 确保窗口接收 selection 请求（成为 selection owner 的前提）。
    XSetSelectionOwner(display, clipboard, m_MainWindow->GetXWindow(), CurrentTime);

    // 把文本作为 UTF8_STRING 挂到 CLIPBOARD selection。
    const size_t len = std::strlen(text);
    XChangeProperty(display, m_MainWindow->GetXWindow(), clipboard, utf8,
                    8, PropModeReplace, reinterpret_cast<const unsigned char*>(text),
                    static_cast<int>(len));
    XSetSelectionOwner(display, clipboard, m_MainWindow->GetXWindow(), CurrentTime);
    XFlush(display);
}

std::string LinuxApplication::GetClipboardText()
{
    if (!m_Display || !m_MainWindow) return {};
    auto* display = static_cast<Display*>(m_Display);
    Atom clipboard = XInternAtom(display, "CLIPBOARD", False);
    Atom utf8 = XInternAtom(display, "UTF8_STRING", False);

    // 请求当前 CLIPBOARD selection 的 UTF8_STRING 内容。第 4 个参数是"把结果写回
    // 哪个属性"，必须是一个 Atom（原来的 XGetSelectionOwner(...) 是 Owner 窗口 id，
    // 不是属性，服务端会直接丢弃 SelectionNotify）。
    const Atom sel_property = XInternAtom(display, "ZSEL_CLIPBOARD", False);
    XConvertSelection(display, clipboard, utf8, sel_property,
                      m_MainWindow->GetXWindow(), CurrentTime);

    // 阻塞等 SelectionNotify（带超时保护，避免死等）。
    std::string result;
    for (int i = 0; i < 50; ++i)
    {
        XEvent event;
        if (!XPending(display))
        {
            XFlush(display);
            return result;
        }
        XNextEvent(display, &event);
        if (event.type == SelectionNotify &&
            event.xselection.selection == clipboard &&
            event.xselection.requestor == m_MainWindow->GetXWindow())
        {
            Atom actual_type = None;
            int  format = 0;
            unsigned long nprop = 0;
            unsigned long bytes_after = 0;
            unsigned char* prop = nullptr;
            if (XGetWindowProperty(display, event.xselection.requestor, event.xselection.property,
                                   0, LONG_MAX / 4, False, AnyPropertyType,
                                   &actual_type, &format, &nprop, &bytes_after, &prop) == Success && prop)
            {
                if (actual_type == utf8)
                {
                    result.assign(reinterpret_cast<char*>(prop), static_cast<size_t>(nprop));
                }
                XFree(prop);
            }
            break;
        }
    }
    return result;
}

void* LinuxApplication::CreateStandardCursor(int shape)
{
    // X11 标准光标形状（XCursorShape 枚举）：
    //   0=arrow 1=hand 2=horizontal resize 3=vertical resize 4=diagonal(nwse)
    const int shapes[] = {
        XC_left_ptr,     // 0
        XC_hand2,        // 1
        XC_sb_h_double_arrow, // 2
        XC_sb_v_double_arrow, // 3
        XC_fleur,        // 4
    };
    if (shape < 0 || shape > 4) shape = 0;
    if (!m_Display) return nullptr;
    auto* display = static_cast<Display*>(m_Display);
    return reinterpret_cast<void*>(XCreateFontCursor(display, static_cast<unsigned int>(shapes[shape])));
}

void LinuxApplication::SetCursor(void* window_handle, void* cursor)
{
    if (!m_Display || !cursor) return;
    auto* display = static_cast<Display*>(m_Display);
    Window target = window_handle
        ? static_cast<Window>(reinterpret_cast<unsigned long>(window_handle))
        : (m_MainWindow ? m_MainWindow->GetXWindow() : RootWindow(display, DefaultScreen(display)));
    XDefineCursor(display, target, static_cast<Cursor>(reinterpret_cast<unsigned long>(cursor)));
    XFlush(display);
}

double LinuxApplication::GetTime()
{
    // 单调时钟秒（与 Windows QPC 同语义）。X11 的 TimeConvert 依赖 server 时间，
    // 不如 steady_clock 可靠，故用标准库。
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------------------
// 显示器枚举：XRandR。索引序与 Windows/macOS 后端一致——0 = 主显示器，其余按
// XRandR 顺序；越界回落主显示器。
//
// 注意 XRandR 的 output 只是"端口"，位置和尺寸挂在它当前绑定的 crtc 上，所以
// 要先 XRRGetOutputInfo 拿到 crtc，再 XRRGetCrtcInfo 拿几何。
namespace
{
    struct MonitorRect
    {
        int x, y, w, h;
    };

    std::vector<MonitorRect> CollectMonitors(Display* display)
    {
        std::vector<MonitorRect> monitors;
        const Window root = RootWindow(display, DefaultScreen(display));
        XRRScreenResources* res = XRRGetScreenResourcesCurrent(display, root);
        if (!res)
        {
            return monitors;
        }

        for (int i = 0; i < res->noutput; ++i)
        {
            XRROutputInfo* output = XRRGetOutputInfo(display, res, res->outputs[i]);
            if (!output)
            {
                continue;
            }

            if (output->connection == RR_Connected && output->crtc != None)
            {
                XRRCrtcInfo* crtc = XRRGetCrtcInfo(display, res, output->crtc);
                if (crtc)
                {
                    if (crtc->width > 0 && crtc->height > 0)
                    {
                        monitors.push_back({static_cast<int>(crtc->x), static_cast<int>(crtc->y),
                                            static_cast<int>(crtc->width), static_cast<int>(crtc->height)});
                    }
                    XRRFreeCrtcInfo(crtc);
                }
            }
            XRRFreeOutputInfo(output);
        }
        XRRFreeScreenResources(res);
        return monitors;
    }
} // namespace

int LinuxApplication::GetMonitorCount() const
{
    if (!m_Display) return 0;
    const auto monitors = CollectMonitors(static_cast<Display*>(m_Display));
    return monitors.empty() ? 1 : static_cast<int>(monitors.size());
}

GenericApplication::MonitorInfo LinuxApplication::GetPrimaryMonitorWorkArea() const
{
    if (!m_Display) return {0, 0, 0, 0};
    auto* display = static_cast<Display*>(m_Display);
    const auto monitors = CollectMonitors(display);
    if (!monitors.empty())
    {
        // 首个 active output 即主屏（X11 没有 MONITORINFOF_PRIMARY 等价物）。
        const MonitorRect& m = monitors.front();
        return {m.x, m.y, m.w, m.h};
    }

    // 没有 XRandR / 没有 connected output：退回整个屏幕。
    const int screen = DefaultScreen(display);
    return {0, 0, DisplayWidth(display, screen), DisplayHeight(display, screen)};
}

GenericApplication::MonitorInfo LinuxApplication::GetMonitorWorkArea(int monitor_index) const
{
    if (!m_Display) return {0, 0, 0, 0};
    const auto monitors = CollectMonitors(static_cast<Display*>(m_Display));
    if (monitor_index < 0 || monitor_index >= static_cast<int>(monitors.size()))
    {
        return GetPrimaryMonitorWorkArea();
    }
    const MonitorRect& m = monitors[static_cast<size_t>(monitor_index)];
    return {m.x, m.y, m.w, m.h};
}

GenericApplication::MonitorInfo LinuxApplication::GetMonitorWorkAreaForWindow(GenericWindow* window) const
{
    if (window == nullptr) return GetPrimaryMonitorWorkArea();
    auto* lw = static_cast<LinuxWindow*>(window);
    if (!m_Display || lw->GetXWindow() == 0) return GetPrimaryMonitorWorkArea();

    // 窗口中心（根窗口坐标）落在哪个 output 里，就是哪个显示器。
    const auto pos = lw->GetPosition();
    const auto size = lw->GetSize();
    const int cx = pos[0] + size[0] / 2;
    const int cy = pos[1] + size[1] / 2;

    const auto monitors = CollectMonitors(static_cast<Display*>(m_Display));
    for (const MonitorRect& m : monitors)
    {
        if (cx >= m.x && cx < m.x + m.w && cy >= m.y && cy < m.y + m.h)
        {
            return {m.x, m.y, m.w, m.h};
        }
    }
    return GetPrimaryMonitorWorkArea();
}

#elif defined(ZENGINE_LINUX_WINDOW_STUB)

// Stub build (ZENGINE_LINUX_WINDOW_STUB=ON) - see LinuxWindow.cpp.

LinuxApplication::LinuxApplication() = default;
LinuxApplication::~LinuxApplication() = default;

bool LinuxApplication::Initialize(const char*, int, int) { return false; }
void LinuxApplication::Shutdown() {}
void LinuxApplication::PollEvents() {}
void LinuxApplication::SetTitle(const char*) {}
void LinuxApplication::ShowMainWindow() {}
void LinuxApplication::SetCursorMode(bool) {}
void LinuxApplication::SetClipboardText(const char*) {}
std::string LinuxApplication::GetClipboardText() { return {}; }
void* LinuxApplication::CreateStandardCursor(int) { return nullptr; }
void LinuxApplication::SetCursor(void*, void*) {}
double LinuxApplication::GetTime() { return 0.0; }

GenericWindow* LinuxApplication::CreateChildWindow(const char*, int, int, int, int, bool)
{
    return nullptr;
}

void LinuxApplication::DestroyChildWindow(GenericWindow*) {}

GenericWindow* LinuxApplication::GetMainWindow() const { return nullptr; }
std::array<int, 2> LinuxApplication::GetFramebufferSize() const { return {0, 0}; }

int LinuxApplication::GetMonitorCount() const { return 0; }
GenericApplication::MonitorInfo LinuxApplication::GetPrimaryMonitorWorkArea() const { return {}; }
GenericApplication::MonitorInfo LinuxApplication::GetMonitorWorkArea(int) const { return {}; }
GenericApplication::MonitorInfo LinuxApplication::GetMonitorWorkAreaForWindow(GenericWindow*) const { return {}; }

#endif // Z_PLATFORM_LINUX && !__EMSCRIPTEN__
