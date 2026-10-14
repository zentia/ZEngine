#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <Windows.h>
#include <d3d11.h>
#undef DrawText  // windows.h #defines DrawText

// The Win32 host (window, message pump, DPI awareness / WM_DPICHANGED, font
// atlas, clipboard bridge, text measurer) is FSlateStandaloneApp. It used to be
// ~180 lines of local code here -- a copy of what LuaMemPreview, TexPreview and
// WindowsEnvironmentCheck each also carried.
#include "ZSlate/App/SlateStandaloneApp.h"
#include "ZSlate/Application/SlateApplication.h"

#include "StackTraceApp.h"
#include "UI/MainApp.h"
#include "UI/Tabs.h"
#include "UI/BranchesTab.h"
#include "UI/GitTab.h"
#include "UI/NotesTab.h"
#include "UI/TipsTab.h"
#include "UI/UrlsTab.h"
#include "UI/SettingsTab.h"

#include <cstdio>
#include <memory>
#include <string>

// ============================================================================
// Main Entry Point
// ============================================================================

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int nCmdShow)
{
    printf("=== ClientProjBranches (D3D11) ===\n\n");
    fflush(stdout);

    // COM init (required for IFileDialog)
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    ZSlate::FStandaloneAppDesc desc;
    desc.Title       = "ClientProjBranches";
    desc.Width       = 1024.0f;
    desc.Height      = 768.0f;
    desc.Font        = ZSlate::Win32::ESystemFont::Sans;
    desc.ShowCommand = nCmdShow;
    // InstallTextMeasurer and InstallClipboard default on: the Stack Trace tab
    // copies its output, and layout has to measure text at the size the atlas
    // will actually rasterize it at.

    ZSlate::FSlateStandaloneApp app;
    if (!app.Initialize(desc))
    {
        CoUninitialize();
        return 1;
    }

    // ---- Build multi-tab UI ----
    printf("Creating app...\n"); fflush(stdout);

    auto stackTrace = std::make_shared<StackTraceApp::StackTraceApp>();
    stackTrace->Initialize();

    auto branchesTab = std::make_shared<ClientProj::BranchesTab>();
    auto gitTab      = std::make_shared<ClientProj::GitTab>();
    auto notesTab    = std::make_shared<ClientProj::NotesTab>();
    auto tipsTab     = std::make_shared<ClientProj::TipsTab>();
    auto urlsTab     = std::make_shared<ClientProj::UrlsTab>();
    auto settingsTab = std::make_shared<ClientProj::SettingsTab>();

    ClientProj::MainApp mainApp;
    mainApp.SetInputRouter(&app.GetInputRouter());
    mainApp.AddTab("Branches",    branchesTab->GetRoot());
    mainApp.AddTab("Git",         gitTab->GetRoot());
    mainApp.AddTab("Stack Trace", stackTrace->GetRoot());
    mainApp.AddTab("TODO",        notesTab->GetRoot());
    mainApp.AddTab("Tips",        tipsTab->GetRoot());
    mainApp.AddTab("URLs",        urlsTab->GetRoot());
    mainApp.AddTab("Settings",    settingsTab->GetRoot());
    mainApp.SelectTab(0);  // Start on Branches tab
    printf("UI initialized\n"); fflush(stdout);

    // ---- Host hooks ----
    // Deferred UI rebuilds run outside of input processing.
    app.OnFrame = [&branchesTab] { branchesTab->OnFrame(); };

    // The stack-trace tab owns a native child surface, so it needs the physical
    // size and sees raw key/scroll events *in addition to* ZSlate's routing --
    // hence "return false": never consume.
    app.OnResize = [&stackTrace](int width, int height) { stackTrace->OnResize(width, height); };
    app.OnKey    = [&stackTrace](unsigned vk, bool down) -> bool
    {
        stackTrace->OnKeyboardEvent(static_cast<int>(vk), 0, down ? 1 : 0, 0);
        return false;
    };
    app.OnWheel  = [&stackTrace](float notches)
    {
        stackTrace->OnScrollEvent(0.0, static_cast<double>(notches));
    };

    printf("Entering main loop...\n"); fflush(stdout);
    const int result = app.Run();

    printf("\nCleaning up...\n"); fflush(stdout);
    stackTrace->Shutdown();
    CoUninitialize();

    printf("=== Application Exited ===\n");
    return result;
}
