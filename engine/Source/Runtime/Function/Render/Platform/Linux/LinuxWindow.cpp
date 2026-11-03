// Linux X11 窗口实现，参考 UE FX11Window。
//
// 整个 TU 只在原生 Linux 桌面（有 X11，非 Emscripten）上编译。Emscripten 虽然继承
// Z_PLATFORM_LINUX=1，但它是 Web 平台、无 X11、且 CreatePlatformApplication() 返回
// nullptr（canvas 归页面所有），所以这里必须额外排除 __EMSCRIPTEN__。
//
// 事件模型：X11 是拉模型。本窗口不自己起事件循环，而是由 LinuxApplication::PollEvents()
// 统一 XNextEvent，并按 Window 句柄把事件路由回对应 LinuxWindow 的回调向量（继承自
// GenericWindow，与 Windows/MacOS 完全一致的转发契约）。
//
// 输入映射目标：与 Windows/MacOS 后端同一套 GLFW 兼容键码 + KeyCodes::MOD_* 位，
// 这样 EnhancedInputSystem / EditorInputManager 不需要任何平台分支。

#include "LinuxWindow.h"
#include "LinuxApplication.h"
#include "Runtime/Function/Input/KeyCodes.h"

#if defined(Z_PLATFORM_LINUX) && !defined(__EMSCRIPTEN__) && !defined(ZENGINE_LINUX_WINDOW_STUB)

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrandr.h>   // 多显示器枚举（顺带覆盖 C7 的多屏子项）
#include <X11/XKBlib.h>              // XkbKeycodeToKeysym：修饰键 / keysym 查询

#include <unistd.h>

#include <algorithm>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// 键码映射：X11 KeySym -> 我们的 GLFW 兼容键码（与 Windows MapVkToKeyCode 同一套
// 256+ 编号，见 KeyCodes.h）。字母/数字/可打印字符的 keysym 低 8 位即 ASCII，
// 与 GLFW 一致，直接透传。
static int MapKeySymToKeyCode(Display* display, KeySym keysym)
{
    // 字母 / 数字 / 可打印标点：keysym 低字节 == ASCII == GLFW_KEY_*
    const unsigned int k = static_cast<unsigned int>(keysym);
    if (k <= 0xFF)
    {
        return static_cast<int>(k);
    }

    // 控制 + 功能键（GLFW 编号：ESCAPE=256, ENTER=257, ...）
    switch (keysym)
    {
    case XK_Escape:    return 256;
    case XK_Return:
    case XK_KP_Enter:  return 257;
    case XK_Tab:       return 258;
    case XK_BackSpace: return 259;
    case XK_Insert:    return 260;
    case XK_Delete:    return 261;
    case XK_Right:     return 262;
    case XK_Left:      return 263;
    case XK_Down:      return 264;
    case XK_Up:        return 265;
    case XK_Page_Up:   return 266;
    case XK_Page_Down: return 267;
    case XK_Home:      return 268;
    case XK_End:       return 269;
    case XK_Caps_Lock: return 280;
    case XK_Scroll_Lock:return 281;
    case XK_Num_Lock:  return 282;
    case XK_Print:     return 283;
    case XK_Pause:     return 284;
    case XK_F1:        return 290;
    case XK_F2:        return 291;
    case XK_F3:        return 292;
    case XK_F4:        return 293;
    case XK_F5:        return 294;
    case XK_F6:        return 295;
    case XK_F7:        return 296;
    case XK_F8:        return 297;
    case XK_F9:        return 298;
    case XK_F10:       return 299;
    case XK_F11:       return 300;
    case XK_F12:       return 301;
    case XK_F13:       return 302;
    case XK_F14:       return 303;
    case XK_F15:       return 304;
    case XK_F16:       return 305;
    case XK_Shift_L:   return 340;
    case XK_Control_L: return 341;
    case XK_Alt_L:     return 342;
    case XK_Super_L:   return 343;
    case XK_Shift_R:   return 344;
    case XK_Control_R: return 345;
    case XK_Alt_R:     return 346;
    case XK_Super_R:   return 347;
    default:
        break;
    }
    return -1; // 未映射
}

// 当前修饰键状态 -> KeyCodes::MOD_* 位（GLFW 布局：SHIFT=1, CONTROL=2, ALT=4,
// SUPER=8, CAPS_LOCK=16, NUM_LOCK=32）。
//
// Xlib 没有"查询当前修饰键"的单一调用：XQueryKeymap 给的是整张键盘的位图
// （32 字节，每 bit 一个 keycode），需要把 keycode 反查成 keysym 才知道按下的是
// 哪个键。CapsLock / NumLock 属于 lock 修饰键，按下状态 != 打开状态，所以另外用
// XkbGetState 取锁存位。
static bool KeySymIsDown(const char keymap[32], Display* display, KeySym ks)
{
    const KeyCode kc = XKeysymToKeycode(display, ks);
    if (kc == 0)
    {
        return false;
    }
    return (keymap[kc / 8] & (1 << (kc % 8))) != 0;
}

static int CurrentModifierBits(Display* display)
{
    char keymap[32] = {};
    XQueryKeymap(display, keymap);

    int mods = 0;
    if (KeySymIsDown(keymap, display, XK_Shift_L) || KeySymIsDown(keymap, display, XK_Shift_R))
    {
        mods |= KeyCodes::MOD_SHIFT_VAL;
    }
    if (KeySymIsDown(keymap, display, XK_Control_L) || KeySymIsDown(keymap, display, XK_Control_R))
    {
        mods |= KeyCodes::MOD_CONTROL_VAL;
    }
    if (KeySymIsDown(keymap, display, XK_Alt_L) || KeySymIsDown(keymap, display, XK_Alt_R))
    {
        mods |= KeyCodes::MOD_ALT_VAL;
    }
    if (KeySymIsDown(keymap, display, XK_Super_L) || KeySymIsDown(keymap, display, XK_Super_R))
    {
        mods |= KeyCodes::MOD_SUPER_VAL;
    }

    XkbStateRec state{};
    if (XkbGetState(display, XkbUseCoreKbd, &state) == Success)
    {
        // CapsLock 固定是 Lock 修饰键；NumLock 在绝大多数键盘布局上是 Mod2。
        if (state.locked_mods & LockMask)
        {
            mods |= KeyCodes::MOD_CAPS_LOCK_VAL;
        }
        if (state.locked_mods & Mod2Mask)
        {
            mods |= KeyCodes::MOD_NUM_LOCK_VAL;
        }
    }
    return mods;
}


// XMapWindow 是异步的：紧接着 XSetInputFocus 常常打到还没 viewable 的窗口上，
// 服务端会回 BadMatch 并让整个进程退出（Xlib 默认错误处理器直接 abort）。这里
// 先等窗口真的映射出来再要焦点，最多等 250ms。
static void FocusWindowWhenViewable(Display* display, Window window)
{
    for (int i = 0; i < 50; ++i)
    {
        XSync(display, False);
        XWindowAttributes attr{};
        if (XGetWindowAttributes(display, window, &attr) && attr.map_state == IsViewable)
        {
            XSetInputFocus(display, window, RevertToParent, CurrentTime);
            return;
        }
        usleep(5000);
    }
}

// UTF-8 -> UTF-16 不需要；X11 的 WM_DELETE_WINDOW 用 ClientMessage，标题用 XChangeProperty
// 以 UTF-8 字符串即可（X11 窗口标题传统上是字符串，无宽窄之分）。

bool LinuxWindow::Initialize(const char* title, int width, int height,
                             bool decorated, bool visible, bool /*focus_on_show*/)
{
    m_Width = width;
    m_Height = height;

    auto* app = reinterpret_cast<LinuxApplication*>(m_Application);
    Display* display = app ? static_cast<Display*>(app->GetDisplay()) : XOpenDisplay(nullptr);
    if (!display)
    {
        return false; // 无 X server（无头环境）——调用方应回落到无窗口路径
    }
    m_Display = display;

    const int screen = DefaultScreen(display);
    const unsigned long black_pixel = BlackPixel(display, screen);

    // 计算带边框的窗口尺寸（decorated -> 加标题栏/边框的装饰预留）。
    int frame_w = 0, frame_h = 0;
    if (decorated)
    {
        // 与 Windows AdjustWindowRect 同语义：加上窗口管理器装饰的典型尺寸。
        frame_w = width + 8;
        frame_h = height + 32;
    }
    else
    {
        frame_w = width;
        frame_h = height;
    }

    Window window = XCreateSimpleWindow(
        display, RootWindow(display, screen),
        0, 0, frame_w, frame_h, 0,
        BlackPixel(display, screen), WhitePixel(display, screen));
    if (window == None)
    {
        return false;
    }
    m_Window = window;

    // 设置 WM_DELETE_WINDOW 协议：点关闭按钮发 ClientMessage 而非直接销毁，
    // 让我们能走 ShouldClose 回调链（与 Windows WM_CLOSE 一致）。
    Atom wm_delete = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(display, window, &wm_delete, 1);

    // 初始隐藏，等 ShowMainWindow / Show() 再 map（splash 之后才显示主窗口）。
    if (visible)
    {
        XMapWindow(display, window);
        m_Mapped = true;
    }

    // 标题（UTF-8 直接写入 X 字符串属性）。
    SetTitle(title);

    // 请求键盘焦点，否则收不到 KeyPress（必须等窗口 viewable，见上面的 helper）。
    if (m_Mapped)
    {
        FocusWindowWhenViewable(display, window);
    }

    return true;
}

void LinuxWindow::SetTitle(const char* title)
{
    if (!m_Window || !m_Display) return;
    auto* display = static_cast<Display*>(m_Display);
    XStoreName(display, m_Window, title);
    XFlush(display);
}

void LinuxWindow::Show()
{
    if (!m_Window || !m_Display || m_Mapped) return;
    auto* display = static_cast<Display*>(m_Display);
    XMapWindow(display, m_Window);
    XRaiseWindow(display, m_Window);
    m_Mapped = true;
    FocusWindowWhenViewable(display, m_Window);
    XFlush(display);
}

void LinuxWindow::Hide()
{
    if (!m_Window || !m_Display || !m_Mapped) return;
    auto* display = static_cast<Display*>(m_Display);
    XUnmapWindow(display, m_Window);
    XFlush(display);
    m_Mapped = false;
}

void LinuxWindow::Destroy()
{
    if (m_Window && m_Display)
    {
        auto* display = static_cast<Display*>(m_Display);
        XDestroyWindow(display, m_Window);
        XFlush(display);
    }
    m_Window = 0;
    m_Display = nullptr;
    m_Mapped = false;
}

bool LinuxWindow::IsVisible() const
{
    if (!m_Window || !m_Display || !m_Mapped) return false;
    auto* display = static_cast<Display*>(m_Display);
    Window root_return, parent_return;
    Window* children = nullptr;
    unsigned int n_children = 0;
    Window focus_return, revert_return, return_to = None;
    int focus_type = 0;
    // 通过窗口在树中的可见性判断（无直接 IsWindowVisible，用 XQueryTree 的 map 状态近似）。
    if (XQueryTree(display, m_Window, &root_return, &parent_return, &children, &n_children))
    {
        // XQueryTree 只返回子窗口列表，不直接给 map 状态；用 m_Mapped 作为权威。
        XFree(children);
    }
    return m_Mapped;
}

std::array<int, 2> LinuxWindow::GetSize() const
{
    if (!m_Window || !m_Display) return {m_Width, m_Height};
    auto* display = static_cast<Display*>(m_Display);
    Window root_return, parent_return;
    Window* children = nullptr;
    int n_children = 0;
    int x = 0, y = 0;
    unsigned int width = m_Width, height = m_Height, border = 0, depth = 0;
    if (XGetGeometry(display, m_Window, &root_return, &x, &y, &width, &height, &border, &depth))
    {
        return {static_cast<int>(width), static_cast<int>(height)};
    }
    return {m_Width, m_Height};
}

std::array<int, 2> LinuxWindow::GetPosition() const
{
    if (!m_Window || !m_Display) return {0, 0};
    auto* display = static_cast<Display*>(m_Display);
    Window root_return, parent_return;
    Window* children = nullptr;
    int n_children = 0;
    int x = 0, y = 0;
    unsigned int width = 0, height = 0, border = 0, depth = 0;
    if (XGetGeometry(display, m_Window, &root_return, &x, &y, &width, &height, &border, &depth))
    {
        // 转成根窗口坐标（XGetGeometry 给的是相对父窗口，主窗口父级即根窗口）。
        Window child_return;
        int child_x = 0, child_y = 0;
        XTranslateCoordinates(display, m_Window, RootWindow(display, DefaultScreen(display)),
                              0, 0, &child_x, &child_y, &child_return);
        return {child_x, child_y};
    }
    return {0, 0};
}

void LinuxWindow::SetSize(int width, int height)
{
    if (!m_Window || !m_Display) return;
    auto* display = static_cast<Display*>(m_Display);
    m_Width = width;
    m_Height = height;
    XResizeWindow(display, m_Window, width, height);
    XFlush(display);
}

void LinuxWindow::SetPosition(int x, int y)
{
    if (!m_Window || !m_Display) return;
    auto* display = static_cast<Display*>(m_Display);
    XMoveWindow(display, m_Window, x, y);
    XFlush(display);
}

std::array<int, 2> LinuxWindow::GetFramebufferSize() const
{
    // 与 Windows 一致：客户区物理像素。X11 无 DPI 缩放（由 GetDpiScale 单独给），
    // 几何尺寸即帧缓冲尺寸。
    return GetSize();
}

float LinuxWindow::GetDpiScale() const
{
    // X11 没有逐窗口 DPI；用屏幕的毫米宽度估一个（96 dpi 为 1.0 基准）。
    if (!m_Window || !m_Display) return 1.0f;
    auto* display = static_cast<Display*>(m_Display);
    const int screen = DefaultScreen(display);
    const unsigned int mm_width = static_cast<unsigned int>(DisplayWidthMM(display, screen));
    if (mm_width > 0)
    {
        const float dpi_x = static_cast<float>(GetSize()[0]) / (static_cast<float>(mm_width) / 25.4f);
        return std::max(1.0f, dpi_x / 96.0f);
    }
    return 1.0f;
}

void LinuxWindow::GetCursorPos(double& x, double& y) const
{
    x = 0.0;
    y = 0.0;
    if (!m_Window || !m_Display) return;
    auto* display = static_cast<Display*>(m_Display);
    Window root_return, child_return;
    int root_x = 0, root_y = 0, child_x = 0, child_y = 0;
    unsigned int mask = 0;
    if (XQueryPointer(display, RootWindow(display, DefaultScreen(display)),
                      &root_return, &child_return, &root_x, &root_y, &child_x, &child_y, &mask))
    {
        // 转成窗口客户区坐标
        Window root_ret, child_ret;
        int cx = 0, cy = 0;
        XTranslateCoordinates(display, child_return, m_Window, 0, 0, &cx, &cy, &child_ret);
        x = static_cast<double>(cx);
        y = static_cast<double>(cy);
    }
}

// ---------------------------------------------------------------------------
// 事件分发：由 LinuxApplication::PollEvents() 调用（X11 拉模型，集中抽取后按
// Window 句柄路由）。返回 true = 本窗口处理了该事件。
bool LinuxWindow::ProcessEvent(void* raw_event)
{
    auto* event = static_cast<XEvent*>(raw_event);
    if (!event || !m_Window) return false;
    auto* display = static_cast<Display*>(m_Display);

    switch (event->type)
    {
    case KeyPress:
    {
        KeySym keysym = XLookupKeysym(&event->xkey, 0);
        int keycode = MapKeySymToKeyCode(display, keysym);
        if (keycode < 0) break;
        const int mods = CurrentModifierBits(display);
        // X11 KeyPress 无法直接区分 repeat；用 XGetModifier 的 repeat 位近似（多数
        // 合成器不发独立 repeat 事件，这里按一次 press 处理，repeat 语义由消费方
        // 的自动重复容忍）。action: 1 = PRESS。
        for (auto& cb : OnKey) cb(keycode, event->xkey.keycode, 1, mods);
        break;
    }
    case KeyRelease:
    {
        KeySym keysym = XLookupKeysym(&event->xkey, 0);
        int keycode = MapKeySymToKeyCode(display, keysym);
        if (keycode < 0) break;
        const int mods = CurrentModifierBits(display);
        for (auto& cb : OnKey) cb(keycode, event->xkey.keycode, 0, mods);
        break;
    }
    case ButtonPress:
    {
        // X11 鼠标按钮：1=左 2=中 3=右 4=上滚 5=下滚（Button4/5 是滚轮，单独处理）。
        const int button = event->xbutton.button;
        if (button == 4)
        {
            for (auto& cb : OnScroll) cb(0.0, 1.0);
            break;
        }
        if (button == 5)
        {
            for (auto& cb : OnScroll) cb(0.0, -1.0);
            break;
        }
        const int idx = button - 1; // 1->0(左) 2->1(中) 3->2(右)
        if (idx < 0 || idx > 2) break;
        for (auto& cb : OnMouseButton) cb(idx, 1, CurrentModifierBits(display));
        break;
    }
    case ButtonRelease:
    {
        const int button = event->xbutton.button;
        if (button == 4 || button == 5) break; // 滚轮按钮不当鼠标按钮派发
        const int idx = button - 1;
        if (idx < 0 || idx > 2) break;
        for (auto& cb : OnMouseButton) cb(idx, 0, CurrentModifierBits(display));
        break;
    }
    case MotionNotify:
    {
        // MotionNotify 是相对窗口根坐标；转成客户区坐标（与 Windows WM_MOUSEMOVE 一致）。
        Window root_return, child_return;
        int root_x = 0, root_y = 0, child_x = 0, child_y = 0;
        unsigned int mask = 0;
        XQueryPointer(display, event->xmotion.window,
                      &root_return, &child_return, &root_x, &root_y, &child_x, &child_y, &mask);
        Window root_ret, child_ret;
        int cx = 0, cy = 0;
        XTranslateCoordinates(display, child_return, m_Window, 0, 0, &cx, &cy, &child_ret);
        for (auto& cb : OnCursorPos) cb(static_cast<double>(cx), static_cast<double>(cy));
        break;
    }
    case EnterNotify:
    {
        for (auto& cb : OnCursorEnter) cb(1);
        break;
    }
    case Expose:
    {
        // 仅当完全暴露（count==0）时刷新，避免部分暴露重复触发。
        if (event->xexpose.count == 0)
        {
            for (auto& cb : OnWindowRefresh) cb();
        }
        break;
    }
    case ConfigureNotify:
    {
        if (event->xconfigure.width > 0 || event->xconfigure.height > 0)
        {
            m_Width = event->xconfigure.width;
            m_Height = event->xconfigure.height;
            for (auto& cb : OnWindowSize) cb(m_Width, m_Height);
        }
        break;
    }
    case FocusIn:
    {
        for (auto& cb : OnWindowFocus) cb(1);
        break;
    }
    case FocusOut:
    {
        for (auto& cb : OnWindowFocus) cb(0);
        break;
    }
    case ClientMessage:
    {
        Atom wm_delete = XInternAtom(display, "WM_DELETE_WINDOW", False);
        if (event->xclient.data.l[0] == static_cast<long>(wm_delete))
        {
            m_ShouldClose = true;
            for (auto& cb : OnWindowClose) cb();
        }
        break;
    }
    default:
        return false;
    }
    return true;
}

#elif defined(ZENGINE_LINUX_WINDOW_STUB)

// Stub build (ZENGINE_LINUX_WINDOW_STUB=ON).
// The X11 window layer below is not compiled: as written it references symbols
// that do not exist in Xlib (XGetModifier, XCreateXCursor, XWarpTo,
// XRandRGetScreenResources, KeyMapState, white_pixel, ...), so it cannot build
// on a real Linux box. These no-ops keep the rest of ZRuntime (renderer, asset
// pipeline, scripting) buildable and link-checkable on a headless machine.

bool LinuxWindow::Initialize(const char*, int, int, bool, bool, bool)
{
    return false;
}

void LinuxWindow::SetTitle(const char*) {}
void LinuxWindow::Show() {}
void LinuxWindow::Hide() {}
void LinuxWindow::Destroy() {}
bool LinuxWindow::IsVisible() const { return false; }
std::array<int, 2> LinuxWindow::GetSize() const { return {0, 0}; }
std::array<int, 2> LinuxWindow::GetPosition() const { return {0, 0}; }
void LinuxWindow::SetSize(int, int) {}
void LinuxWindow::SetPosition(int, int) {}
std::array<int, 2> LinuxWindow::GetFramebufferSize() const { return {0, 0}; }
float LinuxWindow::GetDpiScale() const { return 1.0f; }
void LinuxWindow::GetCursorPos(double& x, double& y) const
{
    x = 0.0;
    y = 0.0;
}

#endif // Z_PLATFORM_LINUX && !__EMSCRIPTEN__
