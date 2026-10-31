#include "Application.h"

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/Base/Platform.h"
#include "Runtime/Core/Base/ScopedAutoreleasePool.h"
#include "Runtime/Core/Memory/MemoryManager.h"
#include "Runtime/Core/Time/TimeState.h"
#include "Runtime/Function/Console/RuntimeConsoleCommands.h"
#include "Runtime/Function/Framework/World/WorldManager.h"
#include "Runtime/Function/Framework/Coroutine/Coroutine.h"
#include "Runtime/Function/Input/InputSystem.h"
#include "Runtime/Function/Particle/ParticleManager.h"
#include "Runtime/Function/Physics/PhysicsManager.h"
#include "Runtime/Function/PlayerSettings/PlayerSettings.h"
#include "Runtime/Project/ProjectInfo.h"
#include "Runtime/Resource/Config/ConfigManager.h"
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

#include <thread>

bool g_isPlaying {false};
bool g_isEditor {false};

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
        // runInBackground == false：失焦就停世界，但**必须继续泵消息**。
        //
        // 不泵消息的后果不是"卡住"而是"未响应"—— OS 检测到窗口几秒没取
        // 事件就把标题栏变灰、弹"程序未响应"。所以这里 continue 之前先
        // PollEvents。
        //
        // 时间也不能不推进：CalculateDeltaTime 已经更新了 m_LastTickTimePoint
        // （下一行就调用），所以恢复焦点那一帧的 delta 是"失焦了多久"——
        // 一个巨大的值。逻辑侧看到的就表现为角色瞬移。这里先把这一帧吃掉
        // 再 continue，让返回的那一帧 delta 是正常的一帧。
        if (!m_RunInBackground && !GET_SYSTEM(WindowSystem)->IsWindowFocused())
        {
            GET_SYSTEM(WindowSystem)->PollEvents();
            CalculateDeltaTime();
            continue;
        }

        const std::chrono::steady_clock::time_point frame_start = std::chrono::steady_clock::now();
        const float delta_time = CalculateDeltaTime();
        if (!TickOneFrame(delta_time))
        {
            break;
        }

        LimitFrameRate(frame_start);
    }
}

void Application::LimitFrameRate(std::chrono::steady_clock::time_point frame_start)
{
    if (m_TargetFrameRate <= 0)
    {
        return;
    }

    // 预算按目标帧率算，减掉本帧已经花掉的时间。超预算就不睡（负值 sleep_for
    // 直接返回，不用自己 clamp）。
    const std::chrono::duration<float> budget {1.0f / static_cast<float>(m_TargetFrameRate)};
    const auto elapsed = std::chrono::steady_clock::now() - frame_start;
    const auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(budget) -
                           std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed);
    if (remaining > std::chrono::nanoseconds::zero())
    {
        std::this_thread::sleep_for(remaining);
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

    // End-of-frame coroutines: after the frame has been rendered, before the
    // next one's input is polled. This is where camera-follow belongs -- in the
    // Update phase it would chase a position that later Updates then move.
    if (auto* coroutines = GET_SYSTEM(CoroutineScheduler))
    {
        coroutines->TickEndOfFrame();
    }

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

    // 输入的帧末滚动：**必须**在 PollEvents 之前。事件是 PollEvents 投递的，
    // 滚完之后到达的事件会留到下一帧被读到；反过来的话（先 Poll 再滚）本帧
    // 投的事件刚进来就被清掉了。
    // 也不受 should_update_logic 影响：暂停时同样要滚，否则暂停期间攒下的
    // 边沿会在恢复那一帧一次性全部触发。
    if (auto* input_system = GET_SYSTEM(InputSystem))
    {
        input_system->EndFrame();
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

    // 固定步在 Update 之前。
    //
    // 顺序不能反：物理（在 Level::FixedTick 里）跑完之后，本帧的 Update 才能
    // 读到这一步之后的位姿 —— 否则角色控制器读到的永远是上一步的位置，
    // 表现为输入到位移多延迟一帧，且帧率越高延迟越明显。
    //
    // 步数由 TimeState 在 BeginFrame 里算好（累加器 + 死亡螺旋闸门），这里
    // 只是照着循环分发。步长恒为固定步长，绝不传 delta_time。
    //
    // 输入是**每帧**采样一次（InputSystem::Tick 在上面），所有固定步共用
    // 同一份输入快照。这是有意的：把输入也按步切分需要事件带时间戳，属于
    // 确定性回放的下一档，现在不做。
    {
        Z_PROFILE_SCOPE("Engine::fixedTick");
        TimeState& ts = TimeState::Get();
        const float fixed_delta = ts.GetFixedDeltaTime();
        for (int step = 0; step < ts.GetFixedStepCount(); ++step)
        {
            GET_SYSTEM(WorldManager)->FixedTick(fixed_delta);
            // Coroutines waiting on WaitForFixedUpdate resume once per *step*,
            // not once per frame -- that is the whole point of the instruction.
            if (auto* coroutines = GET_SYSTEM(CoroutineScheduler))
            {
                coroutines->TickFixedUpdate();
            }
        }
    }

    // 接触事件（OnCollisionEnter/Stay/Exit、OnTriggerEnter/Exit）在这一步派发：
    // 物理刚在固定步里跑完，事件现在才存在；而队列是**被排空**的，所以整个
    // 帧里只能有这一个调用点 —— 派发两次的第二次什么都拿不到，表现为
    // "回调偶尔不触发"。
    if (auto* physics = GET_SYSTEM(PhysicsManager))
    {
        physics->DispatchContactEvents();
    }

    GET_SYSTEM(WorldManager)->Tick(delta_time);

    // Coroutines: Update phase. `GetDeltaTime()` is the scaled step (a
    // timeScale of 0 must freeze WaitForSeconds) and `GetUnscaledDeltaTime()`
    // is wall clock, which is what WaitForSecondsRealtime asked for.
    if (auto* coroutines = GET_SYSTEM(CoroutineScheduler))
    {
        TimeState& ts = TimeState::Get();
        coroutines->TickUpdate(ts.GetDeltaTime(), ts.GetUnscaledDeltaTime());
        coroutines->TickLateUpdate();
    }
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

// =============================================================================
// 脚本门面实现
// =============================================================================
//
// 这些全是 static，靠 GET_SYSTEM() 拿当前进程里的那一个系统实例。每个 getter
// 都先判空：无工程启动、独立工具、单测里只注册了部分系统，都是合法状态，
// 返回空串比崩溃好（脚本侧拿到空串至少能自己判）。

namespace
{
    Application* App()
    {
        return SystemRegistry::GetInstance().GetSystem<Application>();
    }

    std::filesystem::path Normalize(const std::filesystem::path& path)
    {
        std::error_code ec;
        const std::filesystem::path weakly = std::filesystem::weakly_canonical(path, ec);
        return ec ? path : weakly;
    }
}  // namespace

void Application::Quit()
{
    if (Application* self = App())
    {
        self->RequestQuit();
    }
    // RequestQuit 只置 m_Isquit —— 主循环看的是 WindowSystem::ShouldClose()，
    // 所以这里必须同时通知窗口系统，否则 Quit() 是个空操作。
    if (auto* window_system = GET_SYSTEM(WindowSystem))
    {
        window_system->RequestClose();
    }
}

std::string Application::GetDataPath()
{
    if (auto* config = GET_SYSTEM(ConfigManager))
    {
        return Normalize(config->GetAssetFolder()).string();
    }
    return {};
}

std::string Application::GetProjectPath()
{
    if (auto* project = GET_SYSTEM(ProjectInfo))
    {
        return Normalize(project->GetProjectRoot()).string();
    }
    return {};
}

std::string Application::GetStreamingAssetsPath()
{
    if (auto* config = GET_SYSTEM(ConfigManager))
    {
        return Normalize(config->GetAssetFolder() / "StreamingAssets").string();
    }
    return {};
}

std::string Application::GetPersistentDataPath()
{
    auto* settings = GET_SYSTEM(PlayerSettings);
    if (settings == nullptr)
    {
        return {};
    }
    return Normalize(UserPreferences::GetUserPreferencesDirectory(settings->m_ProductName)).string();
}

std::string Application::GetProductName()
{
    if (auto* settings = GET_SYSTEM(PlayerSettings))
    {
        return settings->m_ProductName;
    }
    return {};
}

std::string Application::GetCompanyName()
{
    if (auto* settings = GET_SYSTEM(PlayerSettings))
    {
        return settings->m_CompanyName;
    }
    return {};
}

std::string Application::GetVersion()
{
    if (auto* settings = GET_SYSTEM(PlayerSettings))
    {
        return settings->m_Version;
    }
    return {};
}

std::string Application::GetEngineVersion()
{
    if (auto* project = GET_SYSTEM(ProjectInfo))
    {
        return project->engine_version;
    }
    return {};
}

RuntimePlatform Application::GetPlatform()
{
    // 用 Z_PLATFORM_IS_*（Platform.h 保证 CMake 侧恰好定义一个），不要自己拼
    // __APPLE__ / __linux__ —— iOS 与 macOS 在 clang 下都满足 __APPLE__，
    // 拿它当 macOS 会让真机上报成 OSXPlayer。
#if defined(__EMSCRIPTEN__)
    return RuntimePlatform::WebGLPlayer;
#elif Z_PLATFORM_IS_WINDOWS
    return g_isEditor ? RuntimePlatform::WindowsEditor : RuntimePlatform::WindowsPlayer;
#elif Z_PLATFORM_IS_IOS
    return RuntimePlatform::IPhonePlayer;
#elif Z_PLATFORM_IS_MACOS
    return g_isEditor ? RuntimePlatform::OSXEditor : RuntimePlatform::OSXPlayer;
#elif Z_PLATFORM_IS_ANDROID
    return RuntimePlatform::Android;
#elif Z_PLATFORM_IS_LINUX
    return g_isEditor ? RuntimePlatform::LinuxEditor : RuntimePlatform::LinuxPlayer;
#elif Z_PLATFORM_IS_OHOS
    return RuntimePlatform::HarmonyOSPlayer;
#else
    return RuntimePlatform::Unknown;
#endif
}

std::string Application::GetPlatformName()
{
    switch (GetPlatform())
    {
        case RuntimePlatform::WindowsEditor:
        case RuntimePlatform::WindowsPlayer:
            return "Windows";
        case RuntimePlatform::OSXEditor:
        case RuntimePlatform::OSXPlayer:
            return "MacOS";
        case RuntimePlatform::LinuxEditor:
        case RuntimePlatform::LinuxPlayer:
            return "Linux";
        case RuntimePlatform::Android:
            return "Android";
        case RuntimePlatform::IPhonePlayer:
            return "iOS";
        case RuntimePlatform::HarmonyOSPlayer:
            return "HarmonyOS";
        case RuntimePlatform::WebGLPlayer:
            return "Web";
        case RuntimePlatform::Unknown:
        default:
            return "Unknown";
    }
}

int Application::GetTargetFrameRate()
{
    const Application* self = App();
    return self != nullptr ? self->m_TargetFrameRate : -1;
}

void Application::SetTargetFrameRate(int frame_rate)
{
    if (Application* self = App())
    {
        self->m_TargetFrameRate = frame_rate;
    }
}

bool Application::GetRunInBackground()
{
    const Application* self = App();
    return self != nullptr ? self->m_RunInBackground : true;
}

void Application::SetRunInBackground(bool run)
{
    if (Application* self = App())
    {
        self->m_RunInBackground = run;
    }
}

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