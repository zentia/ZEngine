// =============================================================================
// TexPreview - Standalone Block-Compressed Texture Preview Tool
// -----------------------------------------------------------------------------
// Bootstraps ZRuntime and hosts TexPreviewWindow inside SlateApplication.
//
// Architecture:
//   - RegisterRuntimeLight() registers core + WindowSystem + UISystem + RenderSystem
//     (no ProjectInfo / ScriptingManager / ResourceManager).
//   - START_SYSTEM() (true) lets WindowSystem create a GLFW window
//     and UISystem install GLFW input callbacks + create UIRenderer.
//   - We set TexPreviewWindow as the Slate root via SlateApplication.
//   - UISystem::PreRender() paints the Slate root each frame.
//   - TexPreviewWindow overrides OnMouseButtonDown/Move/Wheel for zoom/pan.
//
// Supported formats: ASTC (all block sizes), BC7
//
// Usage:  TexPreview.exe [path\to\texture.{astc,bc7}]
// =============================================================================

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Core/Base/CrashHandler.h"
#include "Runtime/RegisterRuntime.h"
#include "Runtime/Application/Application.h"
#include "Runtime/Function/Render/WindowSystem.h"
#include "ZSlate/Application/SlateApplication.h"

#include "TexPreviewWindow.h"

#include <filesystem>
#include <memory>

#ifdef _WIN32
    #include <shellapi.h>
    #include <windows.h>
#endif

namespace
{
    std::filesystem::path g_TexturePath;
    std::shared_ptr<TexPreviewWindow> g_PreviewWindow;

    void InitPreviewWindow()
    {
        g_PreviewWindow = std::make_shared<TexPreviewWindow>();
        if (!g_TexturePath.empty())
            g_PreviewWindow->SetTexture(g_TexturePath);

        ZSlate::SlateApplication::Get().SetRootContent(g_PreviewWindow);
    }

    void ShutdownPreviewWindow()
    {
        ZSlate::SlateApplication::Get().SetRootContent(nullptr);
        g_PreviewWindow.reset();
    }
}  // namespace

#ifdef _WIN32
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
#else
int main(int argc, char** argv)
#endif
{
    InstallCrashHandler();

    // -------- Parse command line -----------------------------------------------
#ifdef _WIN32
    {
        int argc = 0;
        LPWSTR* argv_w = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv_w && argc >= 2)
            g_TexturePath = std::filesystem::path(argv_w[1]);
        if (argv_w) LocalFree(argv_w);
    }
#else
    if (argc >= 2)
        g_TexturePath = std::filesystem::path(argv[1]);
#endif

    // -------- Boot ZRuntime (light: core + rendering, no editor) -----
    RegisterRuntimeLight();

    // START_SYSTEM() = InitializeAll(true).
    START_SYSTEM_WITHOUT_UI();

    // -------- Create & mount preview window ---------------------------------
    InitPreviewWindow();

    // -------- Show window (hidden during WindowSystem::Initialize) ------------
    GET_SYSTEM(WindowSystem)->ShowWindow();

    // Poll events once so GLFW updates the framebuffer size.
    GET_SYSTEM(WindowSystem)->PollEvents();

    // -------- Main loop ----------------------------------------------------
    auto* app = GET_SYSTEM(Application);
    if (app)
    {
        app->Run();
    }

    // -------- Shutdown ----------------------------------------------------
    ShutdownPreviewWindow();
    SHUTDOWN_SYSTEM();

    return 0;
}
