#pragma once

// =============================================================================
// WindowsAppIcon — load the icon embedded in the running executable
// -----------------------------------------------------------------------------
// Two window classes in ZRuntime show the app icon: the main window
// (Platform/Windows/WindowsWindow.cpp) and the splash screen
// (SplashScreen.cpp). Both used to hardcode IDI_APPLICATION, i.e. the generic
// system icon, even for executables that do carry one.
//
// The icon is resource 101 (IDI_ICON1), attached by z_embed_app_icon() --
// see engine/cmake/AppIcon.rc.in.
//
// It is loaded from the executable module by number rather than through the
// IDI_ICON1 macro, because that macro is only defined on the targets that call
// z_embed_app_icon(). This header is compiled into ZRuntime, which is linked
// into executables both with and without an embedded icon, so the #ifdef can
// never be true here.
// =============================================================================

#ifdef _WIN32

    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>

/// Icon baked into this executable at the requested size, or IDI_APPLICATION
/// when the executable carries none (which is the behaviour both callers had).
inline HICON LoadEmbeddedAppIcon(int width, int height)
{
    const HICON from_exe = static_cast<HICON>(
        LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101), IMAGE_ICON, width, height, 0));

    return from_exe != nullptr ? from_exe : LoadIconW(nullptr, (LPCWSTR)32512);  // IDI_APPLICATION
}

#endif  // _WIN32
