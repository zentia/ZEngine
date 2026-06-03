#include "Runtime/Core/Log/LogSystem.h"

#include <bq_log/bq_log.h>
#include <filesystem>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace
{
#if defined(_WIN32)
    // Resolve the project root from the process command line (-p / --project) so we can
    // co-locate logs with the open project (UE-style <Project>/Saved/Logs). This is
    // parsed directly from GetCommandLineW rather than CommandSystem because LogSystem
    // is constructed before the engine systems (and the command line is the source of
    // truth for the project either way). Returns empty when no project arg is present.
    std::filesystem::path GetProjectLogDirectoryFromCommandLine()
    {
        namespace fs = std::filesystem;

        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv == nullptr)
        {
            return {};
        }

        fs::path project_arg;
        for (int i = 1; i < argc; ++i)
        {
            const std::wstring_view arg(argv[i]);
            if ((arg == L"-p" || arg == L"--project") && i + 1 < argc)
            {
                project_arg = fs::path(argv[i + 1]);
                break;
            }
        }
        LocalFree(argv);

        if (project_arg.empty())
        {
            return {};
        }

        std::error_code ec;
        const fs::path project_root = fs::is_directory(project_arg, ec) ? project_arg : project_arg.parent_path();
        if (project_root.empty())
        {
            return {};
        }
        return project_root / "Saved" / "Logs";
    }
#endif

    // Logs are co-located with the open project at <Project>/Saved/Logs (UE-style).
    // When no project is supplied we fall back to a Unity-style per-user log folder
    // (Windows: %USERPROFILE%\AppData\LocalLow\ZEngine\Logs).
    std::string GetEngineLogDirectory()
    {
        namespace fs = std::filesystem;
        fs::path dir;

#if defined(_WIN32)
        dir = GetProjectLogDirectoryFromCommandLine();
        if (!dir.empty())
        {
            std::error_code ec;
            fs::create_directories(dir, ec);
            return dir.generic_string();
        }

        if (const char* user_profile = std::getenv("USERPROFILE"))
        {
            dir = fs::path(user_profile) / "AppData" / "LocalLow" / "ZEngine" / "Logs";
        }
        else if (const char* local_app_data = std::getenv("LOCALAPPDATA"))
        {
            dir = fs::path(local_app_data).parent_path() / "LocalLow" / "ZEngine" / "Logs";
        }
        else
        {
            dir = fs::current_path() / "cache" / "Logs";
        }
#elif defined(__APPLE__)
        if (const char* home = std::getenv("HOME"))
        {
            dir = fs::path(home) / "Library" / "Logs" / "ZEngine";
        }
        else
        {
            dir = fs::current_path() / "cache" / "Logs";
        }
#else
        if (const char* state_home = std::getenv("XDG_STATE_HOME"))
        {
            dir = fs::path(state_home) / "ZEngine" / "logs";
        }
        else if (const char* home = std::getenv("HOME"))
        {
            dir = fs::path(home) / ".local" / "state" / "ZEngine" / "logs";
        }
        else
        {
            dir = fs::current_path() / "cache" / "Logs";
        }
#endif

        std::error_code ec;
        fs::create_directories(dir, ec);
        return dir.generic_string();
    }

    std::string BuildEngineLogConfig(const std::string& log_file_prefix)
    {
        return std::string(R"(
				appenders_config.appender_0.type=console
				appenders_config.appender_0.time_zone=default local time
				appenders_config.appender_0.levels=[verbose,debug,info,warning,error,fatal]
				appenders_config.appender_0.enable=true

				appenders_config.appender_1.type=text_file
				appenders_config.appender_1.time_zone=default local time
				appenders_config.appender_1.levels=[verbose,debug,info,warning,error,fatal]
				appenders_config.appender_1.file_name=)") +
               log_file_prefix +
               R"(
				appenders_config.appender_1.max_file_size=10000000
				appenders_config.appender_1.expire_time_days=10
				appenders_config.appender_1.capacity_limit=200000000
				appenders_config.appender_1.enable=true
				log.thread_mode=async
				log.buffer_size=65535
				log.reliable_level=normal
				log.print_stack_levels=[fatal]
			)";
    }
}  // namespace

LogSystem::LogSystem()
{
    const std::string log_dir = GetEngineLogDirectory();
    const std::string log_file_prefix = log_dir + "/engine";
    const std::string engine_log_config = BuildEngineLogConfig(log_file_prefix);

    m_Logger = std::make_unique<bq::engine_log>(bq::engine_log::create_log("engine", engine_log_config.c_str()));
    // Keep early console output until the editor Console panel registers its callback.
    bq::log::set_console_buffer_enable(true);
    m_Logger->info(m_Logger->cat.ZEngine, "Engine log files: {}", log_file_prefix);
}

LogSystem::~LogSystem()
{
    // BqLog is asynchronous; if the LogSystem singleton tears down without flushing,
    // the worker thread can lose the last few hundred ms of log records. Drain
    // before our owning unique_ptr<bq::engine_log> goes away.
    ForceFlush();
}

void LogSystem::ForceFlush()
{
    bq::log::force_flush_all_logs();
}
