#include "Application.h"

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/Memory/MemoryManager.h"
#include "Runtime/Function/Framework/World/WorldManager.h"
#include "Runtime/Function/Input/InputSystem.h"
#include "Runtime/Function/Particle/ParticleManager.h"
#include "Runtime/Function/Physics/PhysicsManager.h"
#include "Runtime/Function/PlayerSettings/PlayerSettings.h"
#include "Runtime/Function/Render/DebugDraw/DebugDrawManager.h"
#include "Runtime/Function/Render/RenderSystem.h"
#include "Runtime/Function/Render/WindowSystem.h"
#include "Runtime/UI/UISystem.h"
#include "Runtime/Profiler/Profiler.h"
#include "Runtime/Resource/Preload/PreloadManager.h"
#include "Runtime/Resource/UserPreferences/UserPreferences.h"

bool g_isPlaying {false};

std::unordered_set<std::string> g_editorTickComponentTypes {};

std::vector<std::type_index> Application::GetDependencies() const
{
    // PlayerSettings 可选（独立工具可能不注册）
    if (SystemRegistry::GetInstance().isSystemRegistered<PlayerSettings>())
        return {GET_SYSTEM_TYPE(PlayerSettings)};
    return {};  // 无依赖
}

void Application::Shutdown()
{
    LOG_INFO(ZEngine, "engine shutdown");
}

bool Application::Initialize()
{
    auto* player_settings = SystemRegistry::GetInstance().GetSystem<PlayerSettings>();
    if (player_settings)
        name = player_settings->m_ProjectName.c_str();
    else
        name = "ZEngine";  // 默认名称（独立工具）
    return true;
}

void Application::Run()
{
    while (!GET_SYSTEM(WindowSystem)->ShouldClose())
    {
        const float delta_time = CalculateDeltaTime();
        if (!TickOneFrame(delta_time))
        {
            break;
        }

        GET_SYSTEM(WindowSystem)->SetTitle(std::string("Z - " + std::to_string(GetFps()) + " FPS").c_str());
    }
}

float Application::CalculateDeltaTime()
{
    float delta_time;
    {
        using namespace std::chrono;

        steady_clock::time_point tick_time_point = steady_clock::now();
        duration<float> time_span = duration_cast<duration<float>>(tick_time_point - m_LastTickTimePoint);
        delta_time = time_span.count();

        m_LastTickTimePoint = tick_time_point;
    }
    return delta_time;
}

bool Application::TickOneFrame(float delta_time)
{
    return TickOneFrame(delta_time, delta_time, true);
}

bool Application::TickOneFrame(float simulation_delta_time, float frame_delta_time, bool should_update_logic)
{
    if (should_update_logic)
    {
        LogicalTick(simulation_delta_time);
    }
    CalculateFPS(frame_delta_time);

    // Game thread: exchange logic/render data, then dispatch the frame to render + RHI workers.
    GET_SYSTEM(RenderSystem)->SwapLogicRenderData();

    RendererTick(simulation_delta_time);

#ifdef ENABLE_PHYSICS_DEBUG_RENDERER
    g_runtime_global_context.m_PhysicsManager->RenderPhysicsWorld(simulation_delta_time);
#endif

    // Drive Slate/UMG UI rendering (UISystem::PreRender() -> RenderSlateRoot).
    // This must run before PollEvents so that input events generated this frame
    // are consumed by the UI before the next frame's input poll.
    {
        auto* ui = GET_SYSTEM(UISystem);
        if (ui)
        {
            ui->PreRender();
        }
    }

    GET_SYSTEM(WindowSystem)->PollEvents();

    // Window title is owned by the editor (scene name + dirty flag). Standalone
    // runtime sets the FPS title in Application::Run() after TickOneFrame.

    // ZEngine Insights: close out the frame on the trace timeline (records a
    // frame boundary + prunes events outside the retained window). No-op-cheap
    // unless the Insights window has turned capture on.
    ZEngine::Insights::InsightsTrace::Get().EndFrame();

    return !GET_SYSTEM(WindowSystem)->ShouldClose();
}

void Application::LogicalTick(float delta_time)
{
    Z_PROFILE_SCOPE("Engine::logicalTick");
    GET_SYSTEM(PreloadManager)->Tick(delta_time);
    GET_SYSTEM(WorldManager)->Tick(delta_time);
    GET_SYSTEM(InputSystem)->Tick();
    // The runtime UI (UMG on ZSlate) is event-driven (input via GLFW callbacks)
    // and painted inside the UI render pass via UISystem::PreRender(); no
    // per-frame logic tick is required here.
}

bool Application::RendererTick(float delta_time)
{
    Z_PROFILE_FUNCTION();
    GET_SYSTEM(RenderSystem)->Tick(delta_time);
    return true;
}

const float Application::s_Fpsalpha = 1.f / 100;
void Application::CalculateFPS(float delta_time)
{
    m_FrameCount++;

    if (m_FrameCount == 1)
    {
        m_AverageDuration = delta_time;
    }
    else
    {
        m_AverageDuration = m_AverageDuration * (1 - s_Fpsalpha) + delta_time * s_Fpsalpha;
    }

    m_Fps = static_cast<int>(1.f / m_AverageDuration);
}