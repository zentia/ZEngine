// =============================================================================
// ZPlayer — 与平台无关的 Player 壳实现
//
// 启动序列照 ZGame.cpp 的 main() 抄，唯一的区别是末尾不调 GameEngine::Run()：
// 移动端 / 外部宿主自己拥有主循环，引擎只提供"推进一帧"。
// WebLauncher/Main.cpp 是同一套做法的现成样板（emscripten_set_main_loop）。
// =============================================================================

#include "PlayerShell.h"

#include <chrono>
#include <filesystem>
#include <string>

#include "CommonPCH/pch.h"

#include "Runtime/AssetBundle/AssetBundleManager.h"
#include "Runtime/Core/Base/CrashHandler.h"
#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Core/Log/LogSystem.h"
#include "Runtime/Function/Command/CommandSystem.h"
#include "Runtime/Function/Console/ConsoleManager.h"
#include "Runtime/Function/Framework/GameFramework/GameEngine.h"
#include "Runtime/Function/Render/WindowSystem.h"
#include "Runtime/Platform/Encoding/EncodingUtils.h"
#include "Runtime/Project/ProjectInfo.h"
#include "Runtime/RegisterRuntime.h"

namespace
{
    /// 宿主没给 delta 且自算结果为 0（同一时钟刻度内连调两次）时的兜底。
    constexpr float k_fallback_delta_seconds = 1.0f / 60.0f;

    bool        g_booted {false};
    bool        g_paused {false};
    GameEngine* g_game_engine {nullptr};

    void* g_native_window {nullptr};
    void* g_asset_manager {nullptr};

    std::string g_project_path;
    std::string g_level_override;
    std::string g_cvar_file;
    std::string g_writable_dir;
    bool        g_has_cvar_file {false};

    int g_surface_width {0};
    int g_surface_height {0};

    std::chrono::steady_clock::time_point g_last_tick {std::chrono::steady_clock::now()};

    PreferredRHI ToPreferredRHI(int value)
    {
        switch (value)
        {
            case ZPlayer_RHI_DX12:   return PreferredRHI::DX12;
            case ZPlayer_RHI_Vulkan: return PreferredRHI::Vulkan;
            default:                 return PreferredRHI::Default;
        }
    }

    /// project_path 可以是项目目录，也可以是 .zproject 文件本身 —— 两者都用
    /// 其所在目录作为工作目录，与 ZGame 保持一致。
    std::filesystem::path ResolveWorkingDir(const std::filesystem::path& project_path)
    {
        return std::filesystem::is_directory(project_path) ? project_path : project_path.parent_path();
    }

    /// 在工程目录里找 .zproject。
    ///
    /// ProjectInfo::Initialize() 吃的是 **.zproject 文件**的路径（它自己
    /// exists() 之后解析），而 Player 拿到的是"工程目录"。桌面端这个路径由命令行
    /// `-p` 传进来，移动端没有命令行，所以只能自己找。
    std::filesystem::path FindProjectFile(const std::filesystem::path& dir)
    {
        std::error_code ec;
        std::filesystem::directory_iterator it(dir, ec);
        const std::filesystem::directory_iterator end {};
        for (; !ec && it != end; it.increment(ec))
        {
            if (it->is_regular_file() && it->path().extension() == ".zproject")
            {
                return it->path();
            }
        }
        return {};
    }
}  // namespace

namespace zengine::player
{
    void ApplyStartupConsoleVariables(const char* cvar_file, bool has_cvar_file)
    {
        if (ConsoleManager* console = GET_SYSTEM(ConsoleManager))
        {
            console->LoadStartupVariables(has_cvar_file ? cvar_file : nullptr);
        }
    }

    void MountPackagedContent()
    {
        auto* bundles = GET_SYSTEM(AssetBundleManager);
        if (bundles == nullptr)
        {
            return;  // no bundle registry in this configuration
        }

        const ProjectInfo* project_info = GET_SYSTEM(ProjectInfo);
        const std::filesystem::path build_root =
            project_info != nullptr ? project_info->GetBuildRoot() : std::filesystem::path();
        if (build_root.empty())
        {
            return;
        }

        bundles->MountBuild(build_root);
    }

    bool OpenStartupLevel(const char* level_override)
    {
        auto* game_engine = GET_SYSTEM(GameEngine);
        if (game_engine == nullptr)
        {
            LOG_ERROR(ZEngine, "ZPlayer: GameEngine is not registered");
            return false;
        }

        eastl::string level_url;
        if (level_override != nullptr && level_override[0] != '\0')
        {
            level_url = level_override;
        }
        else if (auto* project_info = GET_SYSTEM(ProjectInfo); project_info != nullptr)
        {
            level_url = project_info->startup_level;
        }

        if (level_url.empty())
        {
            LOG_WARNING(ZEngine,
                        "ZPlayer: no startup_level configured in .zproject; launching with an empty world");
            return true;
        }

        LOG_INFO(ZEngine, "ZPlayer: opening startup level {}", level_url.c_str());
        return game_engine->LoadLevel(std::string(level_url.c_str()));
    }
}  // namespace zengine::player

// =============================================================================
// C 入口
// =============================================================================

extern "C" ZPlayerResult ZPlayer_Boot(const ZPlayerBootDesc* desc)
{
    if (g_booted)
    {
        LOG_ERROR(ZEngine, "ZPlayer: ZPlayer_Boot called twice");
        return ZPlayer_AlreadyBooted;
    }

    if (desc == nullptr || desc->project_path == nullptr || desc->project_path[0] == '\0')
    {
        LOG_ERROR(ZEngine, "ZPlayer: ZPlayer_Boot called without a project path");
        LogSystem::ForceFlush();
        return ZPlayer_BadArgument;
    }

    // 崩溃捕获优先装：后面的任何一步都可能崩，且移动端拿不到 stderr。
#ifdef ZENGINE_PRODUCT_VERSION
    InstallCrashHandler(ZENGINE_PRODUCT_VERSION);
#else
    InstallCrashHandler("0.1.0");
#endif

    Encoding::InitializeUtf8Locale();

    g_project_path   = desc->project_path;
    g_level_override = desc->level_override != nullptr ? desc->level_override : std::string();
    g_cvar_file      = desc->cvar_file != nullptr ? desc->cvar_file : std::string();
    g_has_cvar_file  = desc->cvar_file != nullptr && desc->cvar_file[0] != '\0';
    g_writable_dir   = desc->writable_dir != nullptr ? desc->writable_dir : std::string();
    g_native_window  = desc->native_window;
    g_asset_manager  = desc->asset_manager;
    g_surface_width  = desc->surface_width;
    g_surface_height = desc->surface_height;

    const std::filesystem::path project_path(g_project_path);
    if (!std::filesystem::exists(project_path))
    {
        LOG_ERROR(ZEngine, "ZPlayer: project path does not exist: {}", g_project_path);
        LogSystem::ForceFlush();
        return ZPlayer_ProjectNotFound;
    }

    const std::filesystem::path working_dir = ResolveWorkingDir(project_path);
    try
    {
        std::filesystem::current_path(working_dir);
    }
    catch (const std::filesystem::filesystem_error& e)
    {
        LOG_ERROR(ZEngine,
                  "ZPlayer: failed to set working directory to {}: {}",
                  working_dir.generic_string(),
                  Encoding::GetFilesystemErrorMessage(e));
        LogSystem::ForceFlush();
        return ZPlayer_WorkingDirFailed;
    }

    // 游戏层：Runtime 全部系统 + GameEngine（后者会接管关卡里的角色）。
    RegisterGameRuntime(ToPreferredRHI(desc->preferred_rhi));

    // 工程路径必须写回 CommandSystem，再启动系统。
    //
    // ProjectInfo（以及若干 importer）是从 CommandSystem::GetProjectPath() /
    // GetWorkingDir() 拿工程的，桌面端靠命令行 `-p` 填进去，而**移动端没有命令行**
    // —— ZPlayer_Boot 只设了进程 cwd，CommandSystem 里那两个字段一直是空的。
    // 漏这一步的表现极其没信息量：
    //     [F] Failed to initialize system 'ProjectInfo', rolling back
    // 前面没有任何一行解释为什么（LoadFromFile("") 直接返回 false）。
    //
    // 位置必须在 RegisterGameRuntime() **之后**：CommandSystem 本身是 Runtime
    // 系统，RegisterRuntime() 才 REGISTER_SYSTEM(CommandSystem)，在那之前
    // GET_SYSTEM 返回 nullptr，整个块会被静默跳过 —— 症状和"根本没写这段代码"
    // 一模一样。2026-10-30 就因为这个把块放错位置，白等了一轮"改了但没生效"。
    if (auto* command_system = GET_SYSTEM(CommandSystem); command_system != nullptr)
    {
        command_system->GetWorkingDir() = working_dir;

        const std::filesystem::path project_file = FindProjectFile(working_dir);
        if (!project_file.empty())
        {
            command_system->GetProjectPath() = project_file;
            LOG_INFO(ZEngine, "ZPlayer: project file = {}", project_file.generic_string());
        }
        else
        {
            LOG_WARNING(ZEngine,
                        "ZPlayer: no .zproject found under {} -- ProjectInfo will have no project",
                        working_dir.generic_string());
        }
    }
    else
    {
        // 不要静默：上一次就是这里返回 nullptr 而没人知道，日志里只剩一行
        // 没头没尾的 "[F] Failed to initialize system 'ProjectInfo'"。
        LOG_ERROR(ZEngine,
                  "ZPlayer: CommandSystem is not registered -- ProjectInfo will have no project");
    }

    START_SYSTEM();

    zengine::player::ApplyStartupConsoleVariables(g_cvar_file.c_str(), g_has_cvar_file);

    // Packaged content has to be mounted before anything opens an asset.
    zengine::player::MountPackagedContent();

    g_game_engine = GET_SYSTEM(GameEngine);
    if (g_game_engine == nullptr)
    {
        LOG_ERROR(ZEngine, "ZPlayer: GameEngine failed to initialize");
        LogSystem::ForceFlush();
        SHUTDOWN_SYSTEM();
        return ZPlayer_GameEngineMissing;
    }

    if (!zengine::player::OpenStartupLevel(g_level_override.c_str()))
    {
        LOG_ERROR(ZEngine, "ZPlayer: startup level could not be opened");
        LogSystem::ForceFlush();
        SHUTDOWN_SYSTEM();
        g_game_engine = nullptr;
        return ZPlayer_StartupLevelFailed;
    }

    // 移动端没有引擎自有的窗口，WindowSystem::GetFocusMode() 对这种配置返回
    // true（见 WindowSystem.h）。否则 InputSystem 每帧都会把 GameCommand::invalid
    // 位打上，MotorComponent::TickPlayerMotor 整体 return —— 表现为"角色不动，
    // 但没有任何报错"。
    if (auto* window_system = GET_SYSTEM(WindowSystem); window_system != nullptr)
    {
        LOG_INFO(ZEngine,
                 "ZPlayer: engine-owned window = {}, focus mode = {}",
                 window_system->GetApplication() != nullptr ? 1 : 0,
                 window_system->GetFocusMode() ? 1 : 0);

        // 宿主窗口推给 WindowSystem：RHI 建 surface 时从它取（移动端没有
        // GenericWindow，见 WindowSystem::GetNativeWindowHandle）。Android 上
        // ANativeWindow 通常晚于引擎启动才到，那时走 ZPlayer_SetNativeWindow。
        if (g_native_window != nullptr)
        {
            window_system->SetHostWindowHandle(g_native_window);
        }
        if (g_surface_width > 0 && g_surface_height > 0)
        {
            window_system->SetHostWindowSize(g_surface_width, g_surface_height);
        }
    }

    g_last_tick = std::chrono::steady_clock::now();
    g_paused    = false;
    g_booted    = true;

    LOG_INFO(ZEngine, "ZPlayer: booted (project = {})", g_project_path);
    return ZPlayer_OK;
}

extern "C" int ZPlayer_Tick(float delta_seconds)
{
    if (!g_booted || g_paused)
    {
        // 未启动 / 后台：宿主可以无脑每帧调，这里保证它是 no-op 且不算退出。
        return 1;
    }

    GameEngine* game_engine = g_game_engine;
    if (game_engine == nullptr)
    {
        return 0;
    }

    float delta = delta_seconds;
    if (!(delta > 0.0f))
    {
        // 宿主没给 delta（只有时间戳时常见）：自己算。
        // Application::CalculateDeltaTime() 是 protected，外部驱动只能自带时钟。
        const auto now = std::chrono::steady_clock::now();
        delta = std::chrono::duration_cast<std::chrono::duration<float>>(now - g_last_tick).count();
        if (!(delta > 0.0f))
        {
            delta = k_fallback_delta_seconds;
        }
    }
    g_last_tick = std::chrono::steady_clock::now();

    // 顺序照 GameEngine::Run()：先 TickOneFrame（输入 + 物理 + 渲染），
    // 再 GameEngine::Tick（GameInstance / LocalPlayer / GameMode）。
    if (!game_engine->TickOneFrame(delta))
    {
        return 0;
    }

    game_engine->Tick(delta);

    return game_engine->IsQuit() ? 0 : 1;
}

extern "C" void ZPlayer_SetNativeWindow(void* native_window)
{
    g_native_window = native_window;
    LOG_INFO(ZEngine, "ZPlayer: native window {}", native_window != nullptr ? "attached" : "detached");

    // 同步给 WindowSystem：RHI 是从那里取 surface 句柄的。
    // Android 上 ANativeWindow 会在旋转 / 回前台时被销毁重建，宿主必须每次
    // 回调都调这个函数，否则 Vulkan 会拿着一个已经失效的 ANativeWindow。
    if (auto* window_system = GET_SYSTEM(WindowSystem); window_system != nullptr)
    {
        window_system->SetHostWindowHandle(native_window);
    }
}

extern "C" void ZPlayer_Resize(int width, int height)
{
    g_surface_width  = width;
    g_surface_height = height;
    LOG_INFO(ZEngine, "ZPlayer: surface resized to {}x{}", width, height);

    // 尺寸进 WindowSystem::GetFramebufferSize()，交换链重建由 RenderSystem 侧
    // 按它判断（移动端没有 GenericWindow 可提供真实尺寸）。
    if (auto* window_system = GET_SYSTEM(WindowSystem); window_system != nullptr)
    {
        window_system->SetHostWindowSize(width, height);
    }
}

extern "C" void ZPlayer_Touch(int pointer_id, int action, float x, float y)
{
    if (!g_booted)
    {
        // 宿主可能在引擎起来之前就送触摸（Android 的手势在 INIT_WINDOW 之前
        // 也会到），静默丢弃比让 WindowSystem 拿到一个不存在的监听者好。
        return;
    }

    // 移动端没有 GenericWindow：WindowSystem::PushTouchEvent 是宿主唯一能把
    // 触摸喂进引擎的地方，EnhancedInputSystem 在那一头接着。
    if (auto* window_system = GET_SYSTEM(WindowSystem); window_system != nullptr)
    {
        window_system->PushTouchEvent(pointer_id, action, x, y);
    }
}

extern "C" void ZPlayer_SetFocus(int focused)
{
    // 焦点与暂停是两件事：暂停由 ZPlayer_Pause/Resume 管（停推进），
    // 焦点只影响输入（输入法弹出、系统通知栏拉下时该传 0）。
    if (auto* window_system = GET_SYSTEM(WindowSystem); window_system != nullptr)
    {
        window_system->SetFocusMode(focused != 0);
    }
}

extern "C" void ZPlayer_Pause(void)
{
    if (!g_booted || g_paused)
    {
        return;
    }
    g_paused = true;
    LOG_INFO(ZEngine, "ZPlayer: paused");
}

extern "C" void ZPlayer_Resume(void)
{
    if (!g_booted || !g_paused)
    {
        return;
    }
    g_paused = false;
    // 回到前台时重置帧计时：否则自算 delta 的第一帧会是"整个后台时长"，
    // 物理与动画会一次性推进一大步。
    g_last_tick = std::chrono::steady_clock::now();
    LOG_INFO(ZEngine, "ZPlayer: resumed");
}

extern "C" void ZPlayer_RequestQuit(void)
{
    if (g_game_engine != nullptr)
    {
        g_game_engine->RequestQuit();
    }
}

extern "C" void ZPlayer_Shutdown(void)
{
    if (!g_booted)
    {
        return;
    }

    g_game_engine = nullptr;
    SHUTDOWN_SYSTEM();
    LogSystem::ForceFlush();

    g_booted   = false;
    g_paused   = false;
    g_native_window = nullptr;
    g_asset_manager = nullptr;
}

extern "C" int ZPlayer_IsRunning(void)
{
    return g_booted ? 1 : 0;
}

extern "C" int ZPlayer_IsPaused(void)
{
    return g_paused ? 1 : 0;
}

extern "C" void* ZPlayer_GetNativeWindow(void)
{
    return g_native_window;
}

extern "C" void ZPlayer_GetSurfaceSize(int* out_width, int* out_height)
{
    if (out_width != nullptr)
    {
        *out_width = g_surface_width;
    }
    if (out_height != nullptr)
    {
        *out_height = g_surface_height;
    }
}

extern "C" void* ZPlayer_GetAssetManager(void)
{
    return g_asset_manager;
}

extern "C" const char* ZPlayer_GetWritableDir(void)
{
    return g_writable_dir.c_str();
}
