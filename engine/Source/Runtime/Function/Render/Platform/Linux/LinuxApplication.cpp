// Linux X11 应用实现，参考 UE FX11Application。
//
// 整个 TU 只在原生 Linux 桌面（X11，非 Emscripten）编译。见 LinuxWindow.cpp 的守卫说明。

#include "LinuxApplication.h"

#if defined(Z_PLATFORM_LINUX) && !defined(__EMSCRIPTEN__)

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/Xrandr/Xrandr.h>
#include <X11/keysym.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

LinuxApplication::LinuxApplication() = default;

LinuxApplication::~LinuxApplication()
{
    Shutdown();
}

bool LinuxApplication::Initialize(const char* title, int width, int height)
{
    // 打开 X server 连接，全应用共享一个 Display*。无头环境（无 DISPLAY）返回 false，
    // 调用方（WindowSystem 工厂）应回落到无窗口路径——Linux 上跑 headless 工具/冒烟
    // 测试不依赖窗口。
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
        XWarpTo(display, None, root, 0, 0, 0, 0, 0, 0, 0);
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
                    PropModeReplace, reinterpret_cast<const unsigned char*>(text),
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

    // 请求当前 CLIPBOARD selection 的 UTF8_STRING 内容。
    XConvertSelection(display, clipboard, utf8, XGetSelectionOwner(display, clipboard),
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
            unsigned char* prop = nullptr;
            int    nprop = 0;
            if (XGetSelectedProperty(display, &event.xselection, &actual_type,
                                     &format, &prop, &nprop) == 0 && prop)
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
        XC_arrow,        // 0
        XC_hand2,        // 1
        XC_sb_h_double_arrow, // 2
        XC_sb_v_double_arrow, // 3
        XC_fleur,        // 4
    };
    if (shape < 0 || shape > 4) shape = 0;
    if (!m_Display) return nullptr;
    auto* display = static_cast<Display*>(m_Display);
    return reinterpret_cast<void*>(XCreateXCursor(display, shapes[shape], BlackPixel(display, DefaultScreen(display))));
}

void LinuxApplication::SetCursor(void* /*window_handle*/, void* cursor)
{
    if (!m_Display || !cursor) return;
    auto* display = static_cast<Display*>(m_Display);
    XSetPointer(display, RootWindow(display, DefaultScreen(display)),
                static_cast<Cursor>(reinterpret_cast<unsigned long>(cursor)),
                0, 0, CurrentTime, XC_move_dont_follow);
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
// XRandR 顺序；越界回落主显示器。顺带覆盖 C7 的多显示器枚举子项。
int LinuxApplication::GetMonitorCount() const
{
    if (!m_Display) return 0;
    auto* display = static_cast<Display*>(m_Display);
    XRRScreenResources* res = XRandRGetScreenResources(display, RootWindow(display, DefaultScreen(display)));
    if (!res) return 1;
    const int count = res->noutput;
    XRRFreeScreenResources(res);
    return std::max(1, count);
}

GenericApplication::MonitorInfo LinuxApplication::GetPrimaryMonitorWorkArea() const
{
    if (!m_Display) return {0, 0, 0, 0};
    auto* display = static_cast<Display*>(m_Display);
    XRRScreenResources* res = XRandRGetScreenResources(display, RootWindow(display, DefaultScreen(display)));
    if (!res || res->noutput < 1)
    {
        if (res) XRRFreeScreenResources(res);
        return {0, 0, 0, 0};
    }
    // 主显示器 = XRandR 里连接且为首个 output（X11 无 MONITORINFOF_PRIMARY 等价；
    // 首个 active output 即主屏，与 NSScreen.mainScreen / Win32 主屏语义一致）。
    RROutput* out = nullptr;
    for (int i = 0; i < res->noutput; ++i)
    {
        if (res->outputs[i].connection == RR_Connected)
        {
            out = res->outputs[i];
            break;
        }
    }
    if (!out) out = res->outputs[0];

    unsigned int mm_w = 0, mm_h = 0;
    XRandRGetScreenResources(display, RootWindow(display, DefaultScreen(display)), &mm_w, &mm_h);
    const int w = static_cast<int>(out->width);
    const int h = static_cast<int>(out->height);
    const int x = static_cast<int>(out->x);
    const int y = static_cast<int>(out->y);
    XRRFreeScreenResources(res);
    // XRandR 给的是全屏工作区（X11 无任务栏概念，工作区 == 全屏）。
    return {x, y, w, h};
}

GenericApplication::MonitorInfo LinuxApplication::GetMonitorWorkArea(int monitor_index) const
{
    if (!m_Display) return {0, 0, 0, 0};
    auto* display = static_cast<Display*>(m_Display);
    XRRScreenResources* res = XRandRGetScreenResources(display, RootWindow(display, DefaultScreen(display)));
    if (!res) return GetPrimaryMonitorWorkArea();

    // 收集 connected output，主屏（首个）放 index 0。
    std::vector<RROutput*> connected;
    for (int i = 0; i < res->noutput; ++i)
    {
        if (res->outputs[i].connection == RR_Connected)
            connected.push_back(res->outputs[i]);
    }
    XRRFreeScreenResources(res);
    if (connected.empty()) return {0, 0, 0, 0};

    if (monitor_index < 0 || monitor_index >= static_cast<int>(connected.size()))
        return GetPrimaryMonitorWorkArea();

    RROutput* out = connected[static_cast<size_t>(monitor_index)];
    return {static_cast<int>(out->x), static_cast<int>(out->y),
            static_cast<int>(out->width), static_cast<int>(out->height)};
}

GenericApplication::MonitorInfo LinuxApplication::GetMonitorWorkAreaForWindow(GenericWindow* window) const
{
    if (window == nullptr) return GetPrimaryMonitorWorkArea();
    auto* lw = static_cast<LinuxWindow*>(window);
    if (!m_Display || lw->GetXWindow() == 0) return GetPrimaryMonitorWorkArea();
    auto* display = static_cast<Display*>(m_Display);

    // 窗口中心（根窗口坐标）→ 找包含它的 output。
    auto pos = lw->GetPosition();
    auto size = lw->GetSize();
    const int cx = pos[0] + size[0] / 2;
    const int cy = pos[1] + size[1] / 2;

    XRRScreenResources* res = XRandRGetScreenResources(display, RootWindow(display, DefaultScreen(display)));
    if (!res) return GetPrimaryMonitorWorkArea();
    for (int i = 0; i < res->noutput; ++i)
    {
        RROutput* out = res->outputs[i];
        if (out->connection != RR_Connected) continue;
        const int x = static_cast<int>(out->x), y = static_cast<int>(out->y);
        const int w = static_cast<int>(out->width), h = static_cast<int>(out->height);
        if (cx >= x && cx < x + w && cy >= y && cy < y + h)
        {
            XRRFreeScreenResources(res);
            return {x, y, w, h};
        }
    }
    XRRFreeScreenResources(res);
    return GetPrimaryMonitorWorkArea();
}

#endif // Z_PLATFORM_LINUX && !__EMSCRIPTEN__
