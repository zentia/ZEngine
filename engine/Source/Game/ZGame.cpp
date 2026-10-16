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
        PreferredRHI preferred_rhi {PreferredRHI::Default};
    };

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

    auto* game_engine = GET_SYSTEM(GameEngine);
    if (game_engine == nullptr)
    {
        LOG_ERROR(ZEngine, "ZGame: GameEngine failed to initialize");
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

    auto* game_engine = GET_SYSTEM(GameEngine);
    if (game_engine == nullptr)
    {
        LOG_ERROR(ZEngine, "ZGame: GameEngine failed to initialize");
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
