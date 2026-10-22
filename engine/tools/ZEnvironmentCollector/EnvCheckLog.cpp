// ============================================================================
// EnvCheckLog - see EnvCheckLog.h
// ============================================================================

// Before any include: this TU reads the environment with std::getenv (the same
// call LogSystem makes) and MSVC's /W4 flags it as C4996.
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "EnvCheckLog.h"

// Two back-ends, one interface (see EnvCheckLog.h):
//
//   ZENVCHECK_USE_ZRUNTIME -- the tool links ZRuntimeShared and logs through the
//   engine's LogSystem via the C ABI in Runtime/ZEngineApi/ZEngineToolApi.h.
//   The DLL owns bqlog, so there is exactly one log instance in the process and
//   the file lands next to the engine's own logs.
//
//   otherwise (the standalone configure, which has no ZRuntimeShared target) --
//   bqlog is linked statically, as it always was.
#if defined(ZENVCHECK_USE_ZRUNTIME)
    #include "Runtime/ZEngineApi/ZEngineToolApi.h"
#else
    // The generated category wrapper (cat.ZEnvironmentCheck) plus the bqlog API.
    // engine_log.h only pulls in <bq_log/bq_log.h>, so no engine PCH and no ZRuntime
    // are needed here -- see the include dirs and the BqLog link in CMakeLists.
    #include <bq_log/bq_log.h>
    #include "Runtime/Core/Log/generated/engine_log.h"
#endif

#include "ToolVersion.h"

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

namespace zengine::envcheck
{

namespace
{
#if defined(ZENVCHECK_USE_ZRUNTIME)
bool& EngineStarted()
{
    // Function-local static so a tool that never calls LogInitialize() never
    // starts the engine slice at all.
    static bool started = false;
    return started;
}
#else
std::unique_ptr<bq::engine_log>& Logger()
{
    // Function-local static: constructed on first use, so a tool that never
    // calls LogInitialize() never touches bqlog at all.
    static std::unique_ptr<bq::engine_log> instance;
    return instance;
}
#endif

std::string& LogPath()
{
    static std::string path;
    return path;
}

#if !defined(ZENVCHECK_USE_ZRUNTIME)
// Only the statically-linked-bqlog build resolves the log location itself; the
// ZRuntimeShared build asks LogSystem, which already does exactly this (and
// additionally honours -p/--project).
//
// Mirrors LogSystem: co-locate with the engine's own per-user logs so support
// finds both in one directory.
std::string BuildLogDirectory()
{
    namespace fs = std::filesystem;
    fs::path directory;
    if (const char* userProfile = std::getenv("USERPROFILE"))
        directory = fs::path(userProfile) / "AppData" / "LocalLow" / "ZEngine" / "Logs";
    else
        directory = fs::current_path() / "cache" / "Logs";

    std::error_code ec;
    fs::create_directories(directory, ec);
    return directory.generic_string();
}

// bqlog is handed a prefix, not a file: the file it opens is
// <prefix>_<date>_<index>.log and it hands only the prefix back. Resolve the
// newest match so the report can name a file that exists -- a "logs are here"
// pointer that opens nothing is worse than no pointer at all. Falls back to the
// prefix when the appender has not produced a file yet.
std::string ResolveLogFile(const std::string& prefix)
{
    namespace fs = std::filesystem;
    const fs::path directory = fs::path(prefix).parent_path();
    const std::string stem = fs::path(prefix).filename().string() + "_";
    std::error_code ec;
    fs::path newest;
    fs::file_time_type newestTime{};
    for (fs::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec))
    {
        const fs::path path = it->path();
        const std::string name = path.filename().string();
        if (name.size() < stem.size() || name.compare(0, stem.size(), stem) != 0) continue;
        if (path.extension() != ".log") continue;
        std::error_code timeError;
        const auto time = fs::last_write_time(path, timeError);
        if (timeError) continue;
        if (newest.empty() || time > newestTime)
        {
            newest = path;
            newestTime = time;
        }
    }
    return newest.empty() ? prefix : newest.generic_string();
}

std::string BuildLogConfig(const std::string& filePrefix)
{
    return std::string(R"(
			appenders_config.appender_0.type=console
			appenders_config.appender_0.time_zone=localtime
			appenders_config.appender_0.levels=[verbose,debug,info,warning,error,fatal]
			appenders_config.appender_0.enable=true

			appenders_config.appender_1.type=text_file
			appenders_config.appender_1.time_zone=localtime
			appenders_config.appender_1.levels=[verbose,debug,info,warning,error,fatal]
			appenders_config.appender_1.file_name=)") +
           filePrefix +
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
#endif  // !ZENVCHECK_USE_ZRUNTIME
}  // namespace

void LogInitialize()
{
#if defined(ZENVCHECK_USE_ZRUNTIME)
    if (EngineStarted()) return;
    // Starts LogSystem + FileSystem + ObjectManager + AssetManager -- and
    // nothing else. A tool that exists to diagnose broken machines must not
    // create a window or a GPU device on one.
    if (ZRuntime_ToolInit() == 0) return;
    EngineStarted() = true;
    LogPath() = ZRuntime_LogFilePath();
#else
    std::unique_ptr<bq::engine_log>& logger = Logger();
    if (logger) return;

    const std::string prefix = BuildLogDirectory() + "/ZEnvironmentCollector";
    logger = std::make_unique<bq::engine_log>(bq::engine_log::create_log("ZEnvironmentCollector", BuildLogConfig(prefix).c_str()));
    LogPath() = ResolveLogFile(prefix);
    // Keep early console output readable in -batchmode, before anything else
    // can consume it.
    bq::log::set_console_buffer_enable(true);
#endif
    // ToolVersion on the first line: support must be able to tell which build a
    // user's log came from -- "the log looks fine but the UI is broken" is only
    // debuggable if we know the exe matches the source we are reading.
#if defined(ZENVCHECK_USE_ZRUNTIME)
    ZRuntime_LogInfo((std::string("ZEnvironmentCollector ") + ToolVersion + " log files: " + LogPath()).c_str());
#else
    logger->info(logger->cat.ZEnvironmentCheck, "ZEnvironmentCollector {} log files: {}",
                 ToolVersion, LogPath());
#endif
}

void LogShutdown()
{
#if defined(ZENVCHECK_USE_ZRUNTIME)
    if (!EngineStarted()) return;
    LogFlush();
    // Also drops any loaded localization table -- it lives in the engine slice.
    ZRuntime_ToolShutdown();
    EngineStarted() = false;
#else
    std::unique_ptr<bq::engine_log>& logger = Logger();
    if (!logger) return;
    LogFlush();
    logger.reset();
#endif
}

void LogInfo(const std::string& message)
{
#if defined(ZENVCHECK_USE_ZRUNTIME)
    ZRuntime_LogInfo(message.c_str());
#else
    if (const auto& logger = Logger())
        logger->info(logger->cat.ZEnvironmentCheck, "{}", message.c_str());
#endif
}

void LogWarning(const std::string& message)
{
#if defined(ZENVCHECK_USE_ZRUNTIME)
    ZRuntime_LogWarning(message.c_str());
#else
    if (const auto& logger = Logger())
        logger->warning(logger->cat.ZEnvironmentCheck, "{}", message.c_str());
#endif
}

void LogError(const std::string& message)
{
#if defined(ZENVCHECK_USE_ZRUNTIME)
    ZRuntime_LogError(message.c_str());
#else
    if (const auto& logger = Logger())
        logger->error(logger->cat.ZEnvironmentCheck, "{}", message.c_str());
#endif
}

void LogFlush()
{
#if defined(ZENVCHECK_USE_ZRUNTIME)
    ZRuntime_LogFlush();
#else
    bq::log::force_flush_all_logs();
#endif
}

std::string LogFilePath()
{
    return LogPath();
}

}  // namespace zengine::envcheck
