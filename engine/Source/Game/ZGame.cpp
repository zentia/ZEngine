// =============================================================================
// ZGame — 独立游戏进程（对标 UE 打包后的游戏 exe）。
//
// 与编辑器的唯一区别：这里没有编辑器 UI，只有游戏本身。引擎系统由
// RegisterGameRuntime() 注册（比 RegisterRuntime 多一个 GameEngine），
// 主循环跑在 GameEngine::Run() 上。
//
// 用法：ZGame -p <path-to-.zproject> [--rhi dx12|vulkan]
// =============================================================================

#include <filesystem>

#ifdef _WIN32
    #include <ZAppShell/AppMain.h>  // CommandLine (wide argv, owns its memory)
    #include <windows.h>
#endif

#include "Player/PlayerShell.h"
#include "Runtime/Core/Base/CrashHandler.h"
#include "Runtime/Function/Framework/GameFramework/GameEngine.h"
#include "Runtime/Platform/Encoding/EncodingUtils.h"
#include "Runtime/RegisterRuntime.h"

namespace
{
    struct LaunchOptions
    {
        std::filesystem::path project_path;
        bool has_project_argument {false};
        std::string level_override;
        PreferredRHI preferred_rhi {PreferredRHI::Default};
        // `--cvar-file <path|none>`: startup cvar file override (see
        // ConsoleManager::ResolveStartupVariablesFile); wins over ZENGINE_CVAR_FILE.
        std::string cvar_file;
        bool has_cvar_file {false};
    };

    // Startup cvar application, packaged-content mounting and startup-level
    // resolution all live in zengine::player (engine/Source/Player/PlayerShell.cpp)
    // now: the mobile Player shell boots the engine through exactly the same
    // seven steps, and a second copy here would only let the two drift apart.

#ifdef _WIN32
    // EncodingUtils exposes GBK->UTF-8 but nothing for wide argv. CommandSystem
    // does the same conversion inline; we keep a local copy so ZGame doesn't
    // depend on that system being up yet (this runs before START_SYSTEM).
    std::string WideToUtf8(const wchar_t* wide)
    {
        if (wide == nullptr)
        {
            return {};
        }

        const int needed = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
        if (needed <= 1)
        {
            return {};
        }

        std::string utf8;
        utf8.resize(static_cast<size_t>(needed) - 1);
        WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8.data(), needed, nullptr, nullptr);
        return utf8;
    }
#endif

#ifdef _WIN32
    LaunchOptions ParseCommandLine()
    {
        LaunchOptions options;
        const auto command_line = zengine::appshell::CommandLine::Acquire();
        if (!command_line.valid())
        {
            return options;
        }

        const int argc = command_line.count();
        const wchar_t* const* argv = command_line.values();
        for (int i = 1; i < argc; ++i)
        {
            const std::wstring_view arg(argv[i]);
            if ((arg == L"-p" || arg == L"--project") && i + 1 < argc)
            {
                options.project_path = std::filesystem::path(argv[i + 1]);
                options.has_project_argument = true;
                ++i;
                continue;
            }
            if ((arg == L"--level" || arg == L"-l") && i + 1 < argc)
            {
                options.level_override = WideToUtf8(argv[i + 1]);
                ++i;
                continue;
            }
            if (arg == L"--cvar-file" && i + 1 < argc)
            {
                options.cvar_file = WideToUtf8(argv[i + 1]);
                options.has_cvar_file = true;
                ++i;
                continue;
            }
            if (arg == L"--rhi" && i + 1 < argc)
            {
                const std::wstring_view value(argv[i + 1]);
                if (value == L"vulkan" || value == L"vk")
                {
                    options.preferred_rhi = PreferredRHI::Vulkan;
                }
                else if (value == L"dx12" || value == L"d3d12" || value == L"directx12")
                {
                    options.preferred_rhi = PreferredRHI::DX12;
                }
                ++i;
                continue;
            }
        }

        // No LocalFree: commandLine owns the array and frees it on scope exit.
        return options;
    }
#else
    LaunchOptions ParseCommandLine(int argc, char** argv)
    {
        LaunchOptions options;
        for (int i = 1; i < argc; ++i)
        {
            const std::string_view arg(argv[i]);
            if ((arg == "-p" || arg == "--project") && i + 1 < argc)
            {
                options.project_path = std::filesystem::path(argv[i + 1]);
                options.has_project_argument = true;
                ++i;
                continue;
            }
            if ((arg == "--level" || arg == "-l") && i + 1 < argc)
            {
                options.level_override = argv[i + 1];
                ++i;
                continue;
            }
            if (arg == "--cvar-file" && i + 1 < argc)
            {
                options.cvar_file = argv[i + 1];
                options.has_cvar_file = true;
                ++i;
                continue;
            }
            if (arg == "--rhi" && i + 1 < argc)
            {
                const std::string_view value(argv[i + 1]);
                if (value == "vulkan" || value == "vk")
                {
                    options.preferred_rhi = PreferredRHI::Vulkan;
                }
                else if (value == "dx12" || value == "d3d12" || value == "directx12")
                {
                    options.preferred_rhi = PreferredRHI::DX12;
                }
                ++i;
                continue;
            }
        }
        return options;
    }
#endif

    int ReportMissingProject()
    {
        LOG_ERROR(ZEngine, "ZGame: no project specified. Launch with -p <path-to-.zproject>.");
        LogSystem::ForceFlush();
        return 1;
    }

}  // namespace

#ifdef _WIN32
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
#ifdef ZENGINE_PRODUCT_VERSION
    InstallCrashHandler(ZENGINE_PRODUCT_VERSION);
#else
    InstallCrashHandler("0.1.0");
#endif

    Encoding::InitializeUtf8Locale();

    const LaunchOptions options = ParseCommandLine();
    if (!options.has_project_argument || options.project_path.empty() ||
        !std::filesystem::exists(options.project_path))
    {
        return ReportMissingProject();
    }

    const std::filesystem::path working_dir =
        std::filesystem::is_directory(options.project_path)
            ? options.project_path
            : options.project_path.parent_path();
    try
    {
        std::filesystem::current_path(working_dir);
    }
    catch (const std::filesystem::filesystem_error& e)
    {
        LOG_ERROR(ZEngine,
                  "ZGame: failed to set working directory to {}: {}",
                  working_dir.generic_string(),
                  Encoding::GetFilesystemErrorMessage(e));
        LogSystem::ForceFlush();
        return 1;
    }

    // 游戏层：Runtime 全部系统 + GameEngine（后者会接管关卡里的角色）。
    RegisterGameRuntime(options.preferred_rhi);
    START_SYSTEM();
    zengine::player::ApplyStartupConsoleVariables(options.cvar_file.c_str(), options.has_cvar_file);

    // Packaged content has to be mounted before anything opens an asset.
    zengine::player::MountPackagedContent();

    auto* game_engine = GET_SYSTEM(GameEngine);
    if (game_engine == nullptr)
    {
        LOG_ERROR(ZEngine, "ZGame: GameEngine failed to initialize");
        LogSystem::ForceFlush();
        SHUTDOWN_SYSTEM();
        return 1;
    }

    if (!zengine::player::OpenStartupLevel(options.level_override.c_str()))
    {
        LOG_ERROR(ZEngine, "ZGame: startup level could not be opened");
        LogSystem::ForceFlush();
        SHUTDOWN_SYSTEM();
        return 1;
    }

    const int exit_code = game_engine->Run();

    SHUTDOWN_SYSTEM();
    LogSystem::ForceFlush();
    return exit_code;
}
#else
int main(int argc, char** argv)
{
    // 崩溃捕获（#2）：非 Windows 过去是 no-op，POSIX main() 根本没装任何处理器。
    // 与 WinMain 分支保持一致：优先用构建注入的版本宏，其次退回 CMake 版本。
#ifdef ZENGINE_PRODUCT_VERSION
    InstallCrashHandler(ZENGINE_PRODUCT_VERSION);
#else
    InstallCrashHandler("0.1.0");
#endif

    Encoding::InitializeUtf8Locale();

    const LaunchOptions options = ParseCommandLine(argc, argv);
    if (!options.has_project_argument || options.project_path.empty() ||
        !std::filesystem::exists(options.project_path))
    {
        return ReportMissingProject();
    }

    const std::filesystem::path working_dir =
        std::filesystem::is_directory(options.project_path)
            ? options.project_path
            : options.project_path.parent_path();
    try
    {
        std::filesystem::current_path(working_dir);
    }
    catch (const std::filesystem::filesystem_error& e)
    {
        LOG_ERROR(ZEngine,
                  "ZGame: failed to set working directory to {}: {}",
                  working_dir.generic_string(),
                  Encoding::GetFilesystemErrorMessage(e));
        LogSystem::ForceFlush();
        return 1;
    }

    RegisterGameRuntime(options.preferred_rhi);
    START_SYSTEM();
    zengine::player::ApplyStartupConsoleVariables(options.cvar_file.c_str(), options.has_cvar_file);

    // Packaged content has to be mounted before anything opens an asset.
    zengine::player::MountPackagedContent();

    auto* game_engine = GET_SYSTEM(GameEngine);
    if (game_engine == nullptr)
    {
        LOG_ERROR(ZEngine, "ZGame: GameEngine failed to initialize");
        LogSystem::ForceFlush();
        SHUTDOWN_SYSTEM();
        return 1;
    }

    if (!zengine::player::OpenStartupLevel(options.level_override.c_str()))
    {
        LOG_ERROR(ZEngine, "ZGame: startup level could not be opened");
        LogSystem::ForceFlush();
        SHUTDOWN_SYSTEM();
        return 1;
    }

    const int exit_code = game_engine->Run();

    SHUTDOWN_SYSTEM();
    LogSystem::ForceFlush();
    return exit_code;
}
#endif
