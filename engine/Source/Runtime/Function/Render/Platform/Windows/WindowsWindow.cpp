#include "WindowsWindow.h"
#include "WindowsApplication.h"
#include "Runtime/Platform/Windows/WindowsAppIcon.h"
#include "Runtime/Function/Input/KeyCodes.h"
#include <shellapi.h>  // DragAcceptFiles, DragQueryFile
#include <windowsx.h>   // GET_X_LPARAM, GET_Y_LPARAM, GET_WHEEL_DELTA_WPARAM

#include <string>
#include <vector>

static const wchar_t* WINDOW_CLASS_NAME = L"ZEngineWindow";

// 将 Win32 VK_* 映射到 GLFW 兼容的键值（与 GLFW key code 定义一致）
// 打印字符（字母/数字）直接传 ASCII；功能键用 256+ 偏移
// Current modifier state as KeyCodes::MOD_* bits (GLFW layout: SHIFT=1, CONTROL=2,
// ALT=4, SUPER=8, CAPS_LOCK=16, NUM_LOCK=32), the same encoding MacOSWindow's
// MapModifiers produces. GetKeyState reflects the state as of the message being
// processed, which is what a key / mouse event wants. This used to be a constant 0,
// so every consumer that keys off mods (EditorInputManager's Ctrl+Z / Ctrl+Y,
// ZSlate's Ctrl/Shift/Alt state) never saw a modifier on Windows.
static int CurrentModifierBits()
{
    int mods = 0;
    if ((GetKeyState(VK_SHIFT) & 0x8000) != 0)
        mods |= KeyCodes::MOD_SHIFT_VAL;
    if ((GetKeyState(VK_CONTROL) & 0x8000) != 0)
        mods |= KeyCodes::MOD_CONTROL_VAL;
    if ((GetKeyState(VK_MENU) & 0x8000) != 0)
        mods |= KeyCodes::MOD_ALT_VAL;
    if (((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) != 0)
        mods |= KeyCodes::MOD_SUPER_VAL;
    if ((GetKeyState(VK_CAPITAL) & 0x0001) != 0)
        mods |= KeyCodes::MOD_CAPS_LOCK_VAL;
    if ((GetKeyState(VK_NUMLOCK) & 0x0001) != 0)
        mods |= KeyCodes::MOD_NUM_LOCK_VAL;
    return mods;
}

// UTF-16 -> UTF-8 (drop paths). Empty on failure.
static std::string WideToUtf8(const wchar_t* wide)
{
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1)
        return {};
    std::string out(static_cast<size_t>(bytes - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), bytes, nullptr, nullptr);
    return out;
}

static int MapVkToKeyCode(WPARAM vk, LPARAM lParam)
{
    // 字母 A-Z：VK_A..VK_Z == ASCII 'A'..'Z' == GLFW_KEY_A..GLFW_KEY_Z
    if (vk >= 'A' && vk <= 'Z')
        return static_cast<int>(vk);
    // 数字 0-9
    if (vk >= '0' && vk <= '9')
        return static_cast<int>(vk);
    // 功能键映射（GLFW 定义：ESCAPE=256, ENTER=257, ...）
    switch (vk)
    {
    case VK_ESCAPE:    return 256;
    case VK_RETURN:    return 257;
    case VK_TAB:       return 258;
    case VK_BACK:       return 259;  // BACKSPACE
    case VK_INSERT:    return 260;
    case VK_DELETE:    return 261;
    case VK_RIGHT:      return 262;
    case VK_LEFT:       return 263;
    case VK_DOWN:       return 264;
    case VK_UP:         return 265;
    case VK_PRIOR:     return 266;  // PAGE_UP
    case VK_NEXT:      return 267;  // PAGE_DOWN
    case VK_HOME:      return 268;
    case VK_END:       return 269;
    case VK_CAPITAL:   return 280;  // CAPS_LOCK
    case VK_SCROLL:    return 281;  // SCROLL_LOCK
    case VK_NUMLOCK:  return 282;  // NUM_LOCK
    case VK_SNAPSHOT:  return 283;  // PRINT_SCREEN
    case VK_PAUSE:     return 284;  // PAUSE
    case VK_F1:        return 290;
    case VK_F2:        return 291;
    case VK_F3:        return 292;
    case VK_F4:        return 293;
    case VK_F5:        return 294;
    case VK_F6:        return 295;
    case VK_F7:        return 296;
    case VK_F8:        return 297;
    case VK_F9:        return 298;
    case VK_F10:       return 299;
    case VK_F11:       return 300;
    case VK_F12:       return 301;
    case VK_LSHIFT:    return 340;
    case VK_LCONTROL:  return 341;
    case VK_LMENU:     return 342;  // Left Alt
    case VK_LWIN:      return 343;
    case VK_RSHIFT:    return 344;
    case VK_RCONTROL:  return 345;
    case VK_RMENU:     return 346;  // Right Alt
    case VK_RWIN:      return 347;
    default:
        // 空格和标点
        if (vk >= 32 && vk < 256)
            return static_cast<int>(vk);
        return -1;
    }
}

bool WindowsWindow::Initialize(const char* title,
                              int         width,
                              int         height,
                              bool        decorated,
                              bool        visible,
                              bool        focus_on_show,
                              HWND        parent)
{
    m_Width = width;
    m_Height = height;

    // 构造唯一窗口类名（避免多个 ZEngine 实例冲突）
    static int s_WindowClassCounter = 0;
    wchar_t className[64];
    wsprintfW(className, L"ZEngineWindow_%d", s_WindowClassCounter++);
    m_WindowClassName = className;

    HINSTANCE hInstance = GetModuleHandleW(nullptr);

    // 注册窗口类（只注册一次 per className）
    WNDCLASSEXW wc {};
    wc.cbSize        = sizeof(WNDCLASSEXW);
    wc.style         = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc   = StaticWndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursorW(nullptr, (LPCWSTR)32512);
    wc.hbrBackground = nullptr;  // 由 DXGI swapchain 呈现，不需背景刷
    wc.lpszClassName = className;

    // 可执行文件内嵌的图标（101 = IDI_ICON1），取不到就退回 IDI_APPLICATION。
    // 为什么不能直接用 IDI_ICON1 宏，见 WindowsAppIcon.h。
    wc.hIcon   = LoadEmbeddedAppIcon(GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON));
    wc.hIconSm = LoadEmbeddedAppIcon(GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));

    if (!RegisterClassExW(&wc))
    {
        // 类已存在（多窗口共享同一类时使用）
        DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS)
            return false;
    }

    // 计算带边框的窗口尺寸
    DWORD style = WS_OVERLAPPEDWINDOW;
    if (!decorated)
        style = WS_POPUP | WS_THICKFRAME | WS_SYSMENU;  // 无装饰但可拖动缩放

    RECT rect {0, 0, width, height};
    AdjustWindowRect(&rect, style, FALSE);

    HWND hwnd = CreateWindowExW(
        0,
        className,
        reinterpret_cast<const wchar_t*>(MultiByteToWideChar(CP_UTF8, 0, title, -1, nullptr, 0) > 0
            ? nullptr : L"ZEngine"),  // placeholder
        style,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left,
        rect.bottom - rect.top,
        parent,
        nullptr,
        hInstance,
        this  // lpParam → WM_CREATE 中获取 this
    );

    // 正确设置标题（支持 UTF-8）
    int wideLen = MultiByteToWideChar(CP_UTF8, 0, title, -1, nullptr, 0);
    std::wstring wtitle(wideLen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, title, -1, &wtitle[0], wideLen);
    SetWindowTextW(hwnd, wtitle.c_str());

    if (!hwnd)
        return false;

    m_Hwnd = hwnd;

    // 将 this 指针存入 HWND 用户数据，供 StaticWndProc 取回
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

    // 接受拖放文件（等价于 GLFW 的 glfwSetDropCallback）
    DragAcceptFiles(hwnd, TRUE);

    if (visible)
        Show();
    else
        Hide();

    return true;
}

void WindowsWindow::SetTitle(const char* title)
{
    if (!m_Hwnd) return;
    int wideLen = MultiByteToWideChar(CP_UTF8, 0, title, -1, nullptr, 0);
    std::wstring wtitle(wideLen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, title, -1, &wtitle[0], wideLen);
    SetWindowTextW(m_Hwnd, wtitle.c_str());
}

void WindowsWindow::Show()
{
    if (!m_Hwnd) return;
    ShowWindow(m_Hwnd, SW_SHOW);
    UpdateWindow(m_Hwnd);
}

void WindowsWindow::Hide()
{
    if (!m_Hwnd) return;
    ShowWindow(m_Hwnd, SW_HIDE);
}

void WindowsWindow::Destroy()
{
    if (m_Hwnd)
    {
        DestroyWindow(m_Hwnd);
        m_Hwnd = nullptr;
    }
}

bool WindowsWindow::IsVisible() const
{
    return m_Hwnd && IsWindowVisible(m_Hwnd);
}

std::array<int, 2> WindowsWindow::GetSize() const
{
    // Client-area size in physical pixels (this process is Per-Monitor DPI aware,
    // so GetClientRect is exact). GetWindowRect here used to report the OUTER
    // window rect (title bar + borders), which made layout metrics disagree with
    // both WM_SIZE and GetFramebufferSize().
    if (!m_Hwnd) return {m_Width, m_Height};
    RECT rect;
    GetClientRect(m_Hwnd, &rect);
    return {rect.right, rect.bottom};
}

std::array<int, 2> WindowsWindow::GetPosition() const
{
    if (!m_Hwnd) return {0, 0};
    RECT rect;
    GetWindowRect(m_Hwnd, &rect);
    return {rect.left, rect.top};
}

void WindowsWindow::SetSize(int width, int height)
{
    if (!m_Hwnd) return;
    SetWindowPos(m_Hwnd, nullptr, 0, 0, width, height,
                 SWP_NOMOVE | SWP_NOZORDER);
}

void WindowsWindow::SetPosition(int x, int y)
{
    if (!m_Hwnd) return;
    SetWindowPos(m_Hwnd, nullptr, x, y, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER);
}

std::array<int, 2> WindowsWindow::GetFramebufferSize() const
{
    if (!m_Hwnd) return {m_Width, m_Height};
    RECT clientRect;
    GetClientRect(m_Hwnd, &clientRect);
    return {clientRect.right, clientRect.bottom};
}

float WindowsWindow::GetDpiScale() const
{
    if (!m_Hwnd) return 1.0f;
    UINT dpi = GetDpiForWindow(m_Hwnd);
    if (dpi == 0) dpi = 96;  // 默认 DPI
    return static_cast<float>(dpi) / 96.0f;
}

// ========== Win32 消息处理 ==========

LRESULT CALLBACK WindowsWindow::StaticWndProc(HWND   hwnd,
                                              UINT   msg,
                                              WPARAM wParam,
                                              LPARAM lParam)
{
    // WM_CREATE 时，lpParam 是 CreateWindowEx 传入的 this 指针
    if (msg == WM_CREATE)
    {
        CREATESTRUCT* cs = reinterpret_cast<CREATESTRUCT*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }

    WindowsWindow* self = reinterpret_cast<WindowsWindow*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (self)
    {
        LRESULT result = 0;
        if (self->ProcessMessage(msg, wParam, lParam, result))
            return result;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool WindowsWindow::ProcessMessage(UINT   msg,
                                  WPARAM wParam,
                                  LPARAM lParam,
                                  LRESULT& outResult)
{
    outResult = 0;

    switch (msg)
    {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    {
        int keycode = MapVkToKeyCode(wParam, lParam);
        int scancode = (lParam >> 16) & 0xFF;
        const int mods = CurrentModifierBits();
        // Bit 30 of lParam = key was already down: auto-repeat (GLFW_REPEAT = 2).
        const int action = ((lParam >> 30) & 1) != 0 ? 2 : 1;
        for (auto& cb : OnKey) cb(keycode, scancode, action, mods);
        // Char 消息由 WM_CHAR 处理
        break;
    }
    case WM_KEYUP:
    case WM_SYSKEYUP:
    {
        int keycode = MapVkToKeyCode(wParam, lParam);
        int scancode = (lParam >> 16) & 0xFF;
        const int mods = CurrentModifierBits();
        for (auto& cb : OnKey) cb(keycode, scancode, 0, mods);
        break;
    }
    case WM_CHAR:
    {
        unsigned int codepoint = static_cast<unsigned int>(wParam);
        for (auto& cb : OnChar) cb(codepoint);
        for (auto& cb : OnCharMods) cb(codepoint, static_cast<unsigned int>(CurrentModifierBits()));
        break;
    }
    case WM_LBUTTONDOWN:
        m_MouseButtons[0] = true;
        for (auto& cb : OnMouseButton) cb(0, 1, CurrentModifierBits());  // button=0, action=press
        break;
    case WM_LBUTTONUP:
        m_MouseButtons[0] = false;
        for (auto& cb : OnMouseButton) cb(0, 0, CurrentModifierBits());  // button=0, action=release
        break;
    case WM_RBUTTONDOWN:
        m_MouseButtons[1] = true;
        for (auto& cb : OnMouseButton) cb(1, 1, CurrentModifierBits());
        break;
    case WM_RBUTTONUP:
        m_MouseButtons[1] = false;
        for (auto& cb : OnMouseButton) cb(1, 0, CurrentModifierBits());
        break;
    case WM_MBUTTONDOWN:
        m_MouseButtons[2] = true;
        for (auto& cb : OnMouseButton) cb(2, 1, CurrentModifierBits());
        break;
    case WM_MBUTTONUP:
        m_MouseButtons[2] = false;
        for (auto& cb : OnMouseButton) cb(2, 0, CurrentModifierBits());
        break;
    case WM_MOUSEMOVE:
    {
        int x = GET_X_LPARAM(lParam);
        int y = GET_Y_LPARAM(lParam);
        // Per-Monitor DPI V2 下 WM_MOUSEMOVE 已经返回物理像素坐标，
        // 不再需要除以 DPI scale（否则会导致物理 + 逻辑混合坐标，光标与按钮位置对不齐）
        for (auto& cb : OnCursorPos) cb(static_cast<double>(x), static_cast<double>(y));
        break;
    }
    case WM_MOUSEWHEEL:
    {
        int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        float fdelta = static_cast<float>(delta) / static_cast<float>(WHEEL_DELTA);
        for (auto& cb : OnScroll) cb(0.0, fdelta);
        break;
    }
    case WM_SIZE:
    {
        m_Width  = LOWORD(lParam);
        m_Height = HIWORD(lParam);
        for (auto& cb : OnWindowSize) cb(m_Width, m_Height);
        break;
    }
    case WM_CLOSE:
        m_ShouldClose = true;
        for (auto& cb : OnWindowClose) cb();
        // 默认：不销毁窗口，由回调中调用 RequestClose 处理
        outResult = 0;
        return true;
    case WM_SETFOCUS:
        for (auto& cb : OnWindowFocus) cb(1);
        break;
    case WM_KILLFOCUS:
        for (auto& cb : OnWindowFocus) cb(0);
        break;
    case WM_PAINT:
        for (auto& cb : OnWindowRefresh) cb();
        ValidateRect(m_Hwnd, nullptr);
        break;
    case WM_SETCURSOR:
        // 参考 UE：光标被捕获时阻止系统设置光标，其余交给 DefWindowProc
        if (m_Application && m_Application->IsCursorCaptured())
            return true;
        return false;
    case WM_DPICHANGED:
    {
        // 响应 DPI 变化，更新窗口位置/大小，并触发 swapchain 重建
        const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(m_Hwnd, nullptr,
                     suggested->left, suggested->top,
                     suggested->right - suggested->left,
                     suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        // WM_SIZE 会自然触发 OnWindowSize -> swapchain 重建
        break;
    }
    case WM_DROPFILES:
    {
        HDROP hDrop = reinterpret_cast<HDROP>(wParam);
        const UINT fileCount = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
        // Every dropped path, UTF-8, then OnDrop (the GLFW drop-callback contract:
        // the strings live for the duration of the call). This used to read the
        // first path into a local buffer and drop it on the floor, so OS drag-drop
        // import (ZSlateContentBrowserWindow) never received anything on Windows.
        std::vector<std::string> paths;
        paths.reserve(fileCount);
        for (UINT i = 0; i < fileCount; ++i)
        {
            const UINT length = DragQueryFileW(hDrop, i, nullptr, 0);  // excludes terminator
            std::wstring wide(static_cast<size_t>(length) + 1, L'\0');
            DragQueryFileW(hDrop, i, wide.data(), length + 1);
            std::string utf8 = WideToUtf8(wide.c_str());
            if (!utf8.empty())
                paths.push_back(std::move(utf8));
        }
        DragFinish(hDrop);
        if (!paths.empty())
        {
            std::vector<const char*> raw;
            raw.reserve(paths.size());
            for (const std::string& path : paths)
                raw.push_back(path.c_str());
            for (auto& cb : OnDrop) cb(static_cast<int>(raw.size()), raw.data());
        }
        break;
    }
    default:
        return false;  // 未处理，交给 DefWindowProc
    }

    return true;
}

void WindowsWindow::GetCursorPos(double& x, double& y) const
{
    x = 0.0;
    y = 0.0;
    if (!m_Hwnd) return;
    POINT pt;
    if (::GetCursorPos(&pt))
    {
        if (ScreenToClient(m_Hwnd, &pt))
        {
            x = static_cast<double>(pt.x);
            y = static_cast<double>(pt.y);
        }
    }
}
