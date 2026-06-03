#include "SplashScreen.h"

#include "Runtime/Core/Base/Macro.h"

#ifdef _WIN32
    #include <dwmapi.h>
    #pragma comment(lib, "dwmapi.lib")
#endif

// Window class name
static const wchar_t* SPLASH_CLASS_NAME = L"ZEngineSplashScreen";

// Window dimensions
static const int SPLASH_WIDTH = 500;
static const int SPLASH_HEIGHT = 280;

#ifdef _WIN32
// Colors (dark theme)
static const COLORREF Z_COLOR_BACKGROUND = RGB(25, 25, 30);
static const COLORREF Z_COLOR_TITLE = RGB(220, 220, 220);
static const COLORREF Z_COLOR_TEXT = RGB(180, 180, 180);
static const COLORREF Z_COLOR_PROGRESS_BG = RGB(50, 50, 55);
static const COLORREF Z_COLOR_PROGRESS_FILL = RGB(66, 150, 250);
static const COLORREF Z_COLOR_BORDER = RGB(60, 60, 70);
#endif

SplashScreen::SplashScreen() {}

SplashScreen::~SplashScreen()
{
    Shutdown();
}

#ifdef _WIN32

LRESULT CALLBACK SplashScreen::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    SplashScreen* splash = reinterpret_cast<SplashScreen*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));

    switch (msg)
    {
        case WM_CREATE:
        {
            LPCREATESTRUCT pCreate = reinterpret_cast<LPCREATESTRUCT>(lParam);
            splash = reinterpret_cast<SplashScreen*>(pCreate->lpCreateParams);
            SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(splash));
            return 0;
        }

        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            if (splash)
            {
                splash->Render(hdc);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;  // We handle background painting in WM_PAINT

        case WM_DESTROY:
            return 0;

        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
    }
}

bool SplashScreen::RegisterWindowClass()
{
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.cbClsExtra = 0;
    wc.cbWndExtra = 0;
    wc.hInstance = m_Hinstance;
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(COLOR_BACKGROUND);
    wc.lpszMenuName = nullptr;
    wc.lpszClassName = SPLASH_CLASS_NAME;
    wc.hIconSm = LoadIcon(nullptr, IDI_APPLICATION);

    return RegisterClassExW(&wc) != 0;
}

void SplashScreen::CreateFonts()
{
    // Title font - large and bold
    m_TitleFont = CreateFontW(36,                        // Height
                              0,                         // Width (0 = auto)
                              0,                         // Escapement
                              0,                         // Orientation
                              FW_BOLD,                   // Weight
                              FALSE,                     // Italic
                              FALSE,                     // Underline
                              FALSE,                     // StrikeOut
                              DEFAULT_CHARSET,           // CharSet
                              OUT_DEFAULT_PRECIS,        // OutPrecision
                              CLIP_DEFAULT_PRECIS,       // ClipPrecision
                              CLEARTYPE_QUALITY,         // Quality
                              DEFAULT_PITCH | FF_SWISS,  // PitchAndFamily
                              L"Segoe UI"                // FaceName
    );

    // Text font - normal size
    m_TextFont = CreateFontW(20,                        // Height
                             0,                         // Width
                             0,                         // Escapement
                             0,                         // Orientation
                             FW_NORMAL,                 // Weight
                             FALSE,                     // Italic
                             FALSE,                     // Underline
                             FALSE,                     // StrikeOut
                             DEFAULT_CHARSET,           // CharSet
                             OUT_DEFAULT_PRECIS,        // OutPrecision
                             CLIP_DEFAULT_PRECIS,       // ClipPrecision
                             CLEARTYPE_QUALITY,         // Quality
                             DEFAULT_PITCH | FF_SWISS,  // PitchAndFamily
                             L"Segoe UI"                // FaceName
    );
}

bool SplashScreen::Initialize()
{
    if (m_Initialized)
    {
        return true;
    }

    m_Hinstance = GetModuleHandle(nullptr);

    // Register window class
    if (!RegisterWindowClass())
    {
        LOG_ERROR(ZWindow, "Failed to register splash screen window class");
        return false;
    }

    // Create fonts
    CreateFonts();

    // Get screen size for centering
    int screen_width = GetSystemMetrics(SM_CXSCREEN);
    int screen_height = GetSystemMetrics(SM_CYSCREEN);
    int pos_x = (screen_width - SPLASH_WIDTH) / 2;
    int pos_y = (screen_height - SPLASH_HEIGHT) / 2;

    // Create window (borderless popup with shadow)
    m_Hwnd = CreateWindowExW(WS_EX_TOOLWINDOW,   // Extended style: no taskbar button
                             SPLASH_CLASS_NAME,  // Class name
                             L"ZEngine",         // Window title
                             WS_POPUP,           // Style: borderless popup
                             pos_x,
                             pos_y,  // Position
                             SPLASH_WIDTH,
                             SPLASH_HEIGHT,  // Size
                             nullptr,        // Parent
                             nullptr,        // Menu
                             m_Hinstance,    // Instance
                             this            // Additional data (passed to WM_CREATE)
    );

    if (!m_Hwnd)
    {
        LOG_ERROR(ZWindow, "Failed to create splash screen window");
        Cleanup();
        return false;
    }

    // Enable DWM shadow for the window
    BOOL enable_shadow = TRUE;
    DwmSetWindowAttribute(m_Hwnd, DWMWA_NCRENDERING_POLICY, &enable_shadow, sizeof(enable_shadow));

    // Set dark mode for window (Windows 10 1809+)
    BOOL use_dark_mode = TRUE;
    DwmSetWindowAttribute(m_Hwnd, 20, &use_dark_mode, sizeof(use_dark_mode));  // DWMWA_USE_IMMERSIVE_DARK_MODE

    // Show and update window
    ShowWindow(m_Hwnd, SW_SHOW);
    UpdateWindow(m_Hwnd);

    m_Initialized = true;
    m_Progress = 0.0f;
    m_StatusText = "Initializing...";

    return true;
}

void SplashScreen::PumpPendingMessages()
{
    MSG msg = {};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

void SplashScreen::Update(float progress, const std::string& status_text)
{
    if (!m_Initialized || !m_Hwnd)
    {
        return;
    }

    m_Progress = progress;
    m_StatusText = status_text;

    PumpPendingMessages();

    // Force repaint
    InvalidateRect(m_Hwnd, nullptr, FALSE);
    UpdateWindow(m_Hwnd);
}

void SplashScreen::Render(HDC hdc)
{
    RECT client_rect;
    GetClientRect(m_Hwnd, &client_rect);

    // Create double buffer to prevent flickering
    HDC mem_dc = CreateCompatibleDC(hdc);
    HBITMAP mem_bitmap = CreateCompatibleBitmap(hdc, client_rect.right, client_rect.bottom);
    HBITMAP old_bitmap = (HBITMAP)SelectObject(mem_dc, mem_bitmap);

    // Fill background
    HBRUSH bg_brush = CreateSolidBrush(COLOR_BACKGROUND);
    FillRect(mem_dc, &client_rect, bg_brush);
    DeleteObject(bg_brush);

    // Draw border
    HPEN border_pen = CreatePen(PS_SOLID, 1, Z_COLOR_BORDER);
    HPEN old_pen = (HPEN)SelectObject(mem_dc, border_pen);
    HBRUSH null_brush = (HBRUSH)GetStockObject(NULL_BRUSH);
    HBRUSH old_brush = (HBRUSH)SelectObject(mem_dc, null_brush);
    Rectangle(mem_dc, 0, 0, client_rect.right, client_rect.bottom);
    SelectObject(mem_dc, old_pen);
    SelectObject(mem_dc, old_brush);
    DeleteObject(border_pen);

    // Set text properties
    SetBkMode(mem_dc, TRANSPARENT);

    // Draw title "ZEngine"
    HFONT old_font = (HFONT)SelectObject(mem_dc, m_TitleFont);
    SetTextColor(mem_dc, Z_COLOR_TITLE);
    RECT title_rect = {0, 60, client_rect.right, 110};
    DrawTextW(mem_dc, L"ZEngine", -1, &title_rect, DT_CENTER | DT_SINGLELINE);

    // Draw status text
    SelectObject(mem_dc, m_TextFont);
    SetTextColor(mem_dc, Z_COLOR_TEXT);

    // Convert status text to wide string
    int wide_len = MultiByteToWideChar(CP_UTF8, 0, m_StatusText.c_str(), -1, nullptr, 0);
    std::wstring wide_status(wide_len, 0);
    MultiByteToWideChar(CP_UTF8, 0, m_StatusText.c_str(), -1, &wide_status[0], wide_len);

    RECT status_rect = {0, 130, client_rect.right, 170};
    DrawTextW(mem_dc, wide_status.c_str(), -1, &status_rect, DT_CENTER | DT_SINGLELINE);

    // Draw progress bar background
    int progress_left = 50;
    int progress_right = client_rect.right - 50;
    int progress_top = 180;
    int progress_bottom = 200;
    int progress_width = progress_right - progress_left;

    RECT progress_bg_rect = {progress_left, progress_top, progress_right, progress_bottom};
    HBRUSH progress_bg_brush = CreateSolidBrush(Z_COLOR_PROGRESS_BG);
    FillRect(mem_dc, &progress_bg_rect, progress_bg_brush);
    DeleteObject(progress_bg_brush);

    // Draw progress bar fill
    int fill_width = static_cast<int>(progress_width * m_Progress);
    RECT progress_fill_rect = {progress_left, progress_top, progress_left + fill_width, progress_bottom};
    HBRUSH progress_fill_brush = CreateSolidBrush(Z_COLOR_PROGRESS_FILL);
    FillRect(mem_dc, &progress_fill_rect, progress_fill_brush);
    DeleteObject(progress_fill_brush);

    // Draw percentage text
    wchar_t percent_text[32];
    swprintf_s(percent_text, L"%.0f%%", m_Progress * 100.0f);
    RECT percent_rect = {0, 210, client_rect.right, 240};
    DrawTextW(mem_dc, percent_text, -1, &percent_rect, DT_CENTER | DT_SINGLELINE);

    // Restore font
    SelectObject(mem_dc, old_font);

    // Copy buffer to screen
    BitBlt(hdc, 0, 0, client_rect.right, client_rect.bottom, mem_dc, 0, 0, SRCCOPY);

    // Cleanup
    SelectObject(mem_dc, old_bitmap);
    DeleteObject(mem_bitmap);
    DeleteDC(mem_dc);
}

void SplashScreen::Cleanup()
{
    if (m_TitleFont)
    {
        DeleteObject(m_TitleFont);
        m_TitleFont = nullptr;
    }

    if (m_TextFont)
    {
        DeleteObject(m_TextFont);
        m_TextFont = nullptr;
    }

    UnregisterClassW(SPLASH_CLASS_NAME, m_Hinstance);
}

void SplashScreen::Shutdown()
{
    if (!m_Initialized)
    {
        return;
    }

    if (m_Hwnd)
    {
        DestroyWindow(m_Hwnd);
        m_Hwnd = nullptr;
    }

    Cleanup();

    m_Initialized = false;
}

#else
// Non-Windows platforms: stub implementation
void SplashScreen::PumpPendingMessages() {}

bool SplashScreen::Initialize()
{
    // TODO: Implement for other platforms if needed
    m_Initialized = true;
    return true;
}

void SplashScreen::Update(float progress, const std::string& status_text)
{
    m_Progress = progress;
    m_StatusText = status_text;
}

void SplashScreen::Shutdown()
{
    m_Initialized = false;
}
#endif