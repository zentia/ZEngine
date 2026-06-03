#pragma once

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <Windows.h>
#endif

#include "Runtime/Core/Base/Singleton.h"

#include <string>

class SplashScreen : public Singleton<SplashScreen>
{
public:
    SplashScreen();
    ~SplashScreen();

    // Initialize the splash screen (standalone, no dependency on main window)
    bool Initialize();

    // Update the splash screen (call this in a loop)
    void Update(float progress, const std::string& status_text = "");

    // Close the splash screen
    void Shutdown();

    // Drain pending Win32 messages (no-op off Windows). Call after each splash
    // Update() and before long-running system inits so DXGI/D3D callbacks run.
    static void PumpPendingMessages();

    // Check if splash screen should be shown
    bool isInitialized() const { return m_Initialized; }

private:
    bool m_Initialized {false};
    float m_Progress {0.0f};
    std::string m_StatusText;

#ifdef _WIN32
    HWND m_Hwnd {nullptr};
    HINSTANCE m_Hinstance {nullptr};
    HFONT m_TitleFont {nullptr};
    HFONT m_TextFont {nullptr};

    // Win32 window procedure
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    // Render the splash screen using GDI
    void Render(HDC hdc);

    // Register window class
    bool RegisterWindowClass();

    // Create fonts
    void CreateFonts();

    // Cleanup resources
    void Cleanup();
#endif
};