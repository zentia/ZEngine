// =============================================================================
// TexPreview — standalone tool, ZSlate + D3D11, no ZRuntime
// =============================================================================
// The Win32 host, the message pump, DPI handling and font loading used to be
// ~120 lines of local code here -- a copy of what LuaMemPreview,
// ClientProjBranches and WindowsEnvironmentCheck each also carried, and the
// only copy with *no* DPI support at all (the process was DPI-virtualised, so
// the whole window was bitmap-stretched and the text came out soft).
//
// All of it now lives in ZSlate::FSlateStandaloneApp, so this file is only
// about what is actually TexPreview-specific: the texture widget and the
// command-line path.

#include "ZSlate/App/SlateStandaloneApp.h"
#include "TexPreviewWindow.h"

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    // UTF-8 argv: the CRT's __argv is in the ANSI codepage, which mangles
    // non-ASCII texture paths.
    const std::vector<std::string> args = ZSlate::Win32::CommandLineArgsUtf8();

    std::printf("=== TexPreview (ZSlate + D3D11) ===\n");

    auto window = std::make_shared<TexPreviewWindow>();
    window->Visibility = ZSlate::EVisibility::Visible;
    if (args.size() >= 2)
        window->SetTexture(std::filesystem::path(args[1]));

    ZSlate::FStandaloneAppDesc desc;
    desc.Title = "Texture Preview (ZSlate)";
    desc.Width  = 1024.0f;
    desc.Height = 768.0f;
    desc.Font   = ZSlate::Win32::ESystemFont::Sans;

    ZSlate::FSlateStandaloneApp app;
    if (!app.Initialize(desc))
        return 1;

    // Give TexPreviewWindow access to the D3D11 device so it can create GPU
    // textures. Available only after Initialize() has brought the backend up.
    window->SetD3D11Device(app.GetDevice(), app.GetContext());
    app.SetRootContent(window);

    // Set keyboard focus on left-click so ProcessChar can deliver to OnKeyChar.
    app.OnFrame = [&app, window]
    {
        if (app.IsMouseDown(0))
            app.GetInputRouter().SetKeyboardFocusWidget(window.get());
    };

    return app.Run();
}

#else
int main() { std::printf("TexPreview: Windows + D3D11 only\n"); return 1; }
#endif
