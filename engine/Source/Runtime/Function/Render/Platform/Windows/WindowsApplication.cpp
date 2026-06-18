#include "WindowsApplication.h"
#include <shellapi.h>
#include <utility>  // std::pair

WindowsApplication::WindowsApplication() = default;

WindowsApplication::~WindowsApplication()
{
    Shutdown();
}

bool WindowsApplication::Initialize(const char* title, int width, int height)
{
    m_MainWindow = std::make_unique<WindowsWindow>();
    if (!m_MainWindow->Initialize(title, width, height,
                                   true,   // decorated
                                   false,  // not visible initially (ShowMainWindow after splash)
                                   true,   // focus_on_show
                                   nullptr))
    {
        return false;
    }
    m_AllWindows.push_back(m_MainWindow.get());
    return true;
}

void WindowsApplication::Shutdown()
{
    // 子窗口先销毁
    m_OwnedChildren.clear();
    m_MainWindow.reset();
    m_AllWindows.clear();
}

void WindowsApplication::PollEvents()
{
    MSG msg;
    // 等价 glfwPollEvents()：泵走线程消息队列中所有待处理消息
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
    {
        // 快捷键（Alt+菜单等）需 TranslateMessage
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

void WindowsApplication::SetTitle(const char* title)
{
    if (m_MainWindow)
        m_MainWindow->SetTitle(title);
}

void WindowsApplication::ShowMainWindow()
{
    if (m_MainWindow)
        m_MainWindow->Show();
}

void WindowsApplication::SetCursorMode(bool capture)
{
    m_CursorCaptured = capture;
    if (capture)
    {
        // 捕获鼠标（等价 glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED)）
        RECT clipRect;
        GetClientRect(m_MainWindow->GetHwnd(), &clipRect);
        ClientToScreen(m_MainWindow->GetHwnd(), (POINT*)&clipRect.left);
        ClientToScreen(m_MainWindow->GetHwnd(), (POINT*)&clipRect.right);
        ClipCursor(&clipRect);
        ShowCursor(FALSE);
    }
    else
    {
        ClipCursor(nullptr);
        ShowCursor(TRUE);
    }
}

GenericWindow* WindowsApplication::CreateChildWindow(const char* title,
                                                   int        width,
                                                   int        height,
                                                   int        pos_x,
                                                   int        pos_y,
                                                   bool       decorated)
{
    auto child = std::make_unique<WindowsWindow>();
    if (!child->Initialize(title, width, height,
                          decorated,
                          false,  // hidden initially
                          true,   // focus_on_show
                          m_MainWindow ? m_MainWindow->GetHwnd() : nullptr))
    {
        return nullptr;
    }
    child->SetPosition(pos_x, pos_y);

    GenericWindow* result = child.get();
    m_OwnedChildren.push_back(std::move(child));
    m_AllWindows.push_back(result);
    return result;
}

void WindowsApplication::DestroyChildWindow(GenericWindow* window)
{
    if (!window) return;
    // 从 m_AllWindows 移除弱引用
    auto it = std::find(m_AllWindows.begin(), m_AllWindows.end(), window);
    if (it != m_AllWindows.end())
        m_AllWindows.erase(it);

    // 从 m_OwnedChildren 销毁（unique_ptr 自动调用 WindowsWindow::Destroy）
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

GenericWindow* WindowsApplication::GetMainWindow() const
{
    return m_MainWindow.get();
}

std::array<int, 2> WindowsApplication::GetFramebufferSize() const
{
    if (!m_MainWindow) return {0, 0};
    return m_MainWindow->GetFramebufferSize();
}

void WindowsApplication::SetClipboardText(const char* text)
{
    if (!OpenClipboard(nullptr)) return;
    EmptyClipboard();
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, (strlen(text) + 1) * sizeof(char));
    if (hMem)
    {
        char* pMem = static_cast<char*>(GlobalLock(hMem));
        strcpy(pMem, text);
        GlobalUnlock(hMem);
        SetClipboardData(CF_TEXT, hMem);
    }
    CloseClipboard();
}

std::string WindowsApplication::GetClipboardText()
{
    if (!IsClipboardFormatAvailable(CF_TEXT) || !OpenClipboard(nullptr))
        return "";
    HANDLE hMem = GetClipboardData(CF_TEXT);
    if (!hMem) { CloseClipboard(); return ""; }
    char* pMem = static_cast<char*>(GlobalLock(hMem));
    std::string result = pMem ? pMem : "";
    GlobalUnlock(hMem);
    CloseClipboard();
    return result;
}

void* WindowsApplication::CreateStandardCursor(int shape)
{
    // shape: 0=arrow, 1=hand, 2=hresize, 3=vresize, 4=nwse-resize
    // IDC_* 是 MAKEINTRESOURCE 宏（LPSTR），需转为 LPCWSTR 以匹配 LoadCursorW
    static const wchar_t* cursor_ids[] = {
        (LPCWSTR)32512,   // IDC_ARROW
        (LPCWSTR)32649,   // IDC_HAND
        (LPCWSTR)32644,   // IDC_SIZEWE
        (LPCWSTR)32645,   // IDC_SIZENS
        (LPCWSTR)32642,   // IDC_SIZENWSE
    };
    if (shape < 0 || shape > 4) shape = 0;
    return LoadCursorW(nullptr, cursor_ids[shape]);
}

void WindowsApplication::SetCursor(void* window_handle, void* cursor)
{
    // Win32 SetCursor is system-wide; window_handle is unused
    (void)window_handle;
    ::SetCursor(static_cast<HCURSOR>(cursor));
}

double WindowsApplication::GetTime()
{
    static LARGE_INTEGER s_Freq = {};
    static BOOL s_GotFreq = QueryPerformanceFrequency(&s_Freq);
    LARGE_INTEGER now;
    if (s_GotFreq && QueryPerformanceCounter(&now))
        return static_cast<double>(now.QuadPart) / static_cast<double>(s_Freq.QuadPart);
    return 0.0;
}

int WindowsApplication::GetMonitorCount() const
{
    int count = 0;
    EnumDisplayMonitors(nullptr, nullptr,
        [](HMONITOR, HDC, LPRECT, LPARAM dwData) -> BOOL {
            (*reinterpret_cast<int*>(dwData))++;
            return TRUE;
        }, reinterpret_cast<LPARAM>(&count));
    return count;
}

GenericApplication::MonitorInfo WindowsApplication::GetPrimaryMonitorWorkArea() const
{
    RECT workArea;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    return {workArea.left, workArea.top,
            workArea.right - workArea.left, workArea.bottom - workArea.top};
}

GenericApplication::MonitorInfo WindowsApplication::GetMonitorWorkArea(int monitor_index) const
{
    // TODO: implement proper multi-monitor enumeration
    // For now, all monitor_index values return primary monitor work area
    (void)monitor_index;  // Suppress unused parameter warning
    return GetPrimaryMonitorWorkArea();
}

GenericApplication::MonitorInfo WindowsApplication::GetMonitorWorkAreaForWindow(GenericWindow* window) const
{
    if (window == nullptr) return GetPrimaryMonitorWorkArea();
    HWND hwnd = static_cast<HWND>(window->GetNativeHandle());
    if (hwnd == nullptr) return GetPrimaryMonitorWorkArea();

    // 计算窗口中心点
    RECT windowRect;
    GetWindowRect(hwnd, &windowRect);
    int cx = (windowRect.left + windowRect.right) / 2;
    int cy = (windowRect.top + windowRect.bottom) / 2;

    HMONITOR hMon = MonitorFromPoint({cx, cy}, MONITOR_DEFAULTTOPRIMARY);
    if (hMon == nullptr) return GetPrimaryMonitorWorkArea();

    MONITORINFO mi;
    mi.cbSize = sizeof(MONITORINFO);
    if (GetMonitorInfoW(hMon, &mi))
    {
        return {mi.rcWork.left, mi.rcWork.top,
                mi.rcWork.right - mi.rcWork.left, mi.rcWork.bottom - mi.rcWork.top};
    }
    return GetPrimaryMonitorWorkArea();
}
