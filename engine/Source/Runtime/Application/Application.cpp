#include "Application.h"

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/Base/ScopedAutoreleasePool.h"
#include "Runtime/Core/Memory/MemoryManager.h"
#include "Runtime/Core/Time/TimeState.h"
#include "Runtime/Function/Console/RuntimeConsoleCommands.h"
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
#include "Runtime/Resource/Addressables/AsyncLoadManager.h"
#include "Runtime/Resource/UserPreferences/UserPreferences.h"

#if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
#elif defined(__linux__)
    #include <unistd.h>
#elif defined(__APPLE__)
    #include <mach-o/dyld.h>
#endif

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
    {
        name = player_settings->m_ProjectName.c_str();
    }
    else
    {
        // 使用可执行文件名（不含扩展名）作为默认名称
        std::string exe_name = "ZEngine";
#if defined(_WIN32)
        char exe_path[MAX_PATH];
        if (GetModuleFileNameA(nullptr, exe_path, MAX_PATH) > 0)
            exe_name = std::filesystem::path(exe_path).stem().string();
#elif defined(__linux__)
        char exe_path[4096];
        ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
        if (len > 0)
        {
            exe_path[len] = '\0';
            exe_name = std::filesystem::path(exe_path).stem().string();
        }
#elif defined(__APPLE__)
        char exe_path[4096];
        uint32_t size = sizeof(exe_path);
        if (_NSGetExecutablePath(exe_path, &size) == 0)
            exe_name = std::filesystem::path(exe_path).stem().string();
#endif
        name = exe_name.c_str();
    }
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
    // 每帧一个 autorelease pool（Apple；其它平台空操作）。Metal 每帧都会产生一批
    // autorelease 对象（command buffer / encoder / render pass descriptor ...），帧循环
    // 是纯 C++、主线程上没有 pool 时它们全进 runtime 的隐式 pool —— 永不 drain，
    // 每帧累积。必须是这一帧的第一条语句：渲染、UI、PollEvents 都在它的作用域里。
    ScopedAutoreleasePool frame_autorelease_pool;

    // 时间状态必须先于本帧任何可能读时间的东西更新 —— 逻辑 tick 里的
    // Time.GetDeltaTime()、渲染侧的时间驱动 shader 参数都从这里取数。
    //
    // 之后本帧一律用 logic_delta_time，不再用传入的 simulation_delta_time：
    // 两者的区别就是 timeScale（以及"本帧不推进逻辑"时前者恒为 0）。
    // timeScale 默认 1.0f 且 should_update_logic 为 true 时
    // logic_delta_time == simulation_delta_time，与改动前逐字节等价。
    TimeState& time_state = TimeState::Get();
    time_state.BeginFrame(simulation_delta_time, frame_delta_time, should_update_logic);
    const float logic_delta_time = time_state.GetDeltaTime();

    TickDeferredConsoleCommands();   // exec.after（暂停时也走：它数的是帧，不是逻辑帧）
    if (should_update_logic)
    {
        LogicalTick(logic_delta_time);
    }
    CalculateFPS(frame_delta_time);

    // Game thread: exchange logic/render data, then dispatch the frame to render + RHI workers.
    GET_SYSTEM(RenderSystem)->SwapLogicRenderData();

    RendererTick(logic_delta_time);

#ifdef ENABLE_PHYSICS_DEBUG_RENDERER
    g_runtime_global_context.m_PhysicsManager->RenderPhysicsWorld(logic_delta_time);
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
    // Drain the Addressables completion queue. This is stage 2 of every async
    // load: the worker threads only read bytes, and object hydration must run
    // here because AssetManager's tables are unsynchronised. Skipping this
    // does not crash — it just means no async load ever finishes.
    if (auto* async_load_manager = GET_SYSTEM(zengine::addressables::AsyncLoadManager))
    {
        async_load_manager->Tick(delta_time);
    }
    // 输入必须早于世界 tick。WorldManager::Tick 会驱动 Level::Tick ->
    // 组件 tick，MotorComponent 就是在那里读 InputSystem 的命令位算位移的。
    // 如果把 InputSystem::Tick() 留在后面，Motor 每帧读到的都是上一帧的
    // 命令（首帧更是恒为 0），表现就是注入了输入、配置也正确，角色却不动。
    // InputSystem::Tick() 不依赖世界状态，提前没有副作用。
    GET_SYSTEM(InputSystem)->Tick();
    GET_SYSTEM(WorldManager)->Tick(delta_time);
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