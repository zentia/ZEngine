// =============================================================================
// ASTCPreview - Standalone ASTC Texture Preview Tool
// -----------------------------------------------------------------------------
// Bootstraps ZRuntime and hosts ASTCPreviewWindow inside SlateApplication.
//
// Architecture:
//   - RegisterRuntimeLight() registers core + WindowSystem + UISystem + RenderSystem
//     (no ProjectInfo / ScriptingManager / ResourceManager).
//   - START_SYSTEM() (true) lets WindowSystem create a GLFW window
//     and UISystem install GLFW input callbacks + create UIRenderer.
//   - We set ASTCPreviewWindow as the Slate root via SlateApplication.
//   - UISystem::PreRender() paints the Slate root each frame.
//   - ASTCPreviewWindow overrides OnMouseButtonDown/Move/Wheel for zoom/pan.
//
// Usage:  ASTCPreview.exe [path\to\texture.astc]
// =============================================================================

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Core/Base/CrashHandler.h"
#include "Runtime/RegisterRuntime.h"
#include "Runtime/Application/Application.h"
#include "Runtime/Function/Render/WindowSystem.h"
#include "Runtime/Slate/Application/SlateApplication.h"

#include "ASTCPreviewWindow.h"

#include <filesystem>
#include <memory>

#ifdef _WIN32
    #include <shellapi.h>
    #include <windows.h>
#endif

namespace
{
    std::filesystem::path g_TexturePath;
    std::shared_ptr<ASTCPreviewWindow> g_PreviewWindow;

    void InitPreviewWindow()
    {
        g_PreviewWindow = std::make_shared<ASTCPreviewWindow>();
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
    // RegisterRuntimeLight() registers:
    //   Core:   CommonStringTable, TypeTreeCache, MemoryManager,
    //           LLMTracker, FileSystem, AsyncReadManagerThreaded, ObjectManager
    //   Render:  WindowSystem, PreloadManager, PhysicsManager,
    //           WorldManager, InputSystem, ParticleManager,
    //           (DX12|Vulkan)RHI, RenderSystem,
    //           DebugDrawManager, RenderDebugConfig, ModuleManager, UISystem
    //   NO:     ProjectInfo, ScriptingManager, ResourceManager, etc.
    RegisterRuntimeLight();

    // START_SYSTEM() = InitializeAll(true).
    // The "true" lets WindowSystem create a GLFW window and UISystem
    // install input callbacks.  This is required for Slate to render.
    START_SYSTEM();

    // -------- Create & mount preview window ---------------------------------
    InitPreviewWindow();

    // -------- Show window (hidden during WindowSystem::Initialize) ------------
    GET_SYSTEM(WindowSystem)->ShowWindow();

    // Poll events once so GLFW updates the framebuffer size.
    // Without this, getDisplaySize() may return (0,0) on the first frame.
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
