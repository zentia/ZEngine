// ============================================================================
// EnvCheckLog - see EnvCheckLog.h
// ============================================================================

// Before any include: this TU reads the environment with std::getenv (the same
// call LogSystem makes) and MSVC's /W4 flags it as C4996.
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "EnvCheckLog.h"

// The generated category wrapper (cat.ZEnvironmentCheck) plus the bqlog API.
// engine_log.h only pulls in <bq_log/bq_log.h>, so no engine PCH and no ZRuntime
// are needed here -- see the include dirs and the BqLog link in CMakeLists.
#include <bq_log/bq_log.h>
#include "Runtime/Core/Log/generated/engine_log.h"

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

namespace zengine::envcheck
{

namespace
{
std::unique_ptr<bq::engine_log>& Logger()
{
    // Function-local static: constructed on first use, so a tool that never
    // calls LogInitialize() never touches bqlog at all.
    static std::unique_ptr<bq::engine_log> instance;
    return instance;
}

std::string& LogPath()
{
    static std::string path;
    return path;
}

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
}  // namespace

void LogInitialize()
{
    std::unique_ptr<bq::engine_log>& logger = Logger();
    if (logger) return;

    const std::string prefix = BuildLogDirectory() + "/ZEnvironmentCollector";
    logger = std::make_unique<bq::engine_log>(bq::engine_log::create_log("ZEnvironmentCollector", BuildLogConfig(prefix).c_str()));
    LogPath() = prefix;
    // Keep early console output readable in -batchmode, before anything else
    // can consume it.
    bq::log::set_console_buffer_enable(true);
    logger->info(logger->cat.ZEnvironmentCheck, "ZEnvironmentCollector log files: {}", prefix);
}

void LogShutdown()
{
    std::unique_ptr<bq::engine_log>& logger = Logger();
    if (!logger) return;
    LogFlush();
    logger.reset();
}

void LogInfo(const std::string& message)
{
    if (const auto& logger = Logger())
        logger->info(logger->cat.ZEnvironmentCheck, "{}", message.c_str());
}

void LogWarning(const std::string& message)
{
    if (const auto& logger = Logger())
        logger->warning(logger->cat.ZEnvironmentCheck, "{}", message.c_str());
}

void LogError(const std::string& message)
{
    if (const auto& logger = Logger())
        logger->error(logger->cat.ZEnvironmentCheck, "{}", message.c_str());
}

void LogFlush()
{
    bq::log::force_flush_all_logs();
}

std::string LogFilePath()
{
    return LogPath();
}

}  // namespace zengine::envcheck
