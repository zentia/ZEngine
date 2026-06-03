// =============================================================================
// ZEngine Web (Emscripten) Launcher
// -----------------------------------------------------------------------------
// Browser host that boots the engine via the standard SystemRegistry flow:
//
//   1. RegisterRuntime() pulls in core + render systems (RegisterRuntime.cpp
//      now registers WebGL2RHI as RHI on Emscripten).
//   2. RegisterPlatform() adds the asset manager.
//   3. START_SYSTEM_WITHOUT_UI() runs InitializeAll(false) — skipping the
//      desktop SplashScreen which is GLFW/ImGui-desktop-only.
//      During this step:
//        * WindowSystem creates a canvas-backed WebGL2 context via the
//          emscripten GLFW shim (USE_GLFW=3 + OPENGL_ES_API hint).
//        * WebGL2RHI picks up that current context with
//          emscripten_webgl_get_current_context().
//        * RenderSystem builds a no-op render pipeline (Web takes the
//          stub path because Z_HAS_VULKAN / _WIN32 / __APPLE__ are all off).
//   4. emscripten_set_main_loop drives Application::TickOneFrame each
//      requestAnimationFrame.
//
// Application::Run() is intentionally NOT called: it is a desktop-style
// blocking while-loop that would deadlock the browser main thread. Instead
// the per-frame logic is driven externally — this matches the documented
// Emscripten pattern (see https://emscripten.org/docs/api_reference/emscripten.h.html#c.emscripten_set_main_loop).
// =============================================================================

#if !defined(__EMSCRIPTEN__)
    #error "WebLauncher is an Emscripten-only target."
#endif

#include "Runtime/Application/Application.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Function/Render/WindowSystem.h"
#include "Runtime/RegisterRuntime.h"

#include <chrono>
#include <cstdio>
#include <emscripten.h>
#include <emscripten/html5.h>

namespace
{
    // Cached once the engine has finished startup. emscripten_set_main_loop's
    // callback signature is parameterless, so we route through a free
    // function that pulls the system out of the registry on demand.
    bool g_engine_started = false;
    std::chrono::steady_clock::time_point g_last_tick;

    void mainLoopTick()
    {
        // Application::CalculateDeltaTime() is protected, so we maintain
        // our own steady_clock-based delta here. Application::TickOneFrame
        // accepts a frame delta directly.
        const auto now = std::chrono::steady_clock::now();
        const float delta =
            std::chrono::duration_cast<std::chrono::duration<float>>(now - g_last_tick).count();
        g_last_tick = now;

        auto app = GET_SYSTEM(Application);
        if (!app)
        {
            return;
        }
        app->TickOneFrame(delta);
    }
}  // namespace

int main(int /*argc*/, char** /*argv*/)
{
    std::printf("[ZWebLauncher] booting ZEngine for Web...\n");

    // Stage 1: register core + runtime + render systems.
    // RegisterRuntime registers WindowSystem, ConfigManager, ThreadManager,
    // PreloadManager (stubbed on Web), PhysicsManager (Jolt-stubbed on Web),
    // WorldManager, InputSystem, ParticleManager, WebGL2RHI as RHI,
    // RenderSystem, RenderDebugConfig, ModuleManager, UISystem,
    // PlayerSettings, TaskGraph, CommandSystem, ConsoleManager,
    // Application, UserPreferences, ProjectInfo, ResourceManager,
    // ScriptingManager.
    RegisterRuntime();
    RegisterPlatform();

    // Stage 2: initialize all systems in dependency-topological order.
    // We use the WITHOUT_UI variant: the desktop SplashScreen is GLFW+ImGui
    // desktop-only and has no Web bring-up.
    START_SYSTEM_WITHOUT_UI();
    g_engine_started = true;
    g_last_tick = std::chrono::steady_clock::now();

    std::printf("[ZWebLauncher] engine started; entering main loop.\n");

    // Stage 3: hand control over to the browser. fps=0 lets the browser
    // pick the cadence (typically vsync via requestAnimationFrame).
    // simulate_infinite_loop=1 ensures main() never returns from wasm so
    // static destructors do not run prematurely while the page is alive.
    emscripten_set_main_loop(mainLoopTick, 0, 1);

    return 0;
}
