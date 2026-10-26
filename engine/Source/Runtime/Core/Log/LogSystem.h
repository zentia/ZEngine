#pragma once

#include "Runtime/Core/Base/Singleton.h"
#include "Runtime/Core/Log/CrashLogTail.h"
#include "generated/engine_log.h"

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

#if __has_include(<format>)
    #include <format>
#endif

#if defined(__cpp_lib_format) && (__cpp_lib_format >= 201907L)
    #define Z_LOG_HAS_STD_FORMAT 1
#else
    #define Z_LOG_HAS_STD_FORMAT 0
#endif

#if Z_LOG_HAS_STD_FORMAT && __has_include(<EASTL/string.h>)
    #include <EASTL/string.h>

    // eastl::string is the engine's primary string type and is frequently passed to
    // LOG_* macros. LogSystem::log instantiates its fatal branch (fatalCallback ->
    // std::vformat) for every log level, so eastl::string must be std::formattable
    // regardless of the actual level. BqLog already serializes eastl::string on its
    // own; this closes the equivalent std::format gap. The __has_include guard keeps
    // this header usable by targets (e.g. render_doc) that do not have EASTL on their
    // include path and never log eastl::string.
    template <>
    struct std::formatter<eastl::string> : std::formatter<std::string_view>
    {
        template <class FormatContext>
        auto format(const eastl::string& s, FormatContext& ctx) const
        {
            return std::formatter<std::string_view>::format(
                std::string_view(s.c_str(), s.size()), ctx);
        }
    };
#endif

class LogSystem final : public Singleton<LogSystem>
{
public:
    LogSystem();
    ~LogSystem();

    // Force-Flush BqLog's asynchronous buffer to disk. Safe to call from anywhere
    // (idempotent, internally just signals the worker thread to drain). Call this
    // from any "the program is about to die" path -- crash handlers, graceful
    // shutdown, watchdog kills -- so the tail of the log isn't lost.
    static void ForceFlush();

    // Directory + file-name prefix the text-file appender writes to, i.e. the
    // argument the tool-facing C API hands back from ZRuntime_LogFilePath() so a
    // tool can print "logs are here" into its own report. Empty until the
    // singleton is constructed (which happens on the first log call).
    //
    // It is a member rather than a free function because the value is computed
    // once, in the constructor, from the command line / USERPROFILE -- asking
    // for it must not re-run that resolution.
    std::string m_LogFilePrefix;
    static const std::string& LogFilePath() { return GetInstance().m_LogFilePrefix; }

    enum class LogLevel : uint8_t
    {
        verbose,
        debug,
        info,
        warning,
        error,
        fatal
    };

    template<typename CAT_INDEX, typename STR, typename... TARGS>
    void log(LogLevel level, const CAT_INDEX& category, const STR& log_content_cstr, TARGS&&... args)
    {
        // 同步写入崩溃安全环形缓冲（#11）：独立于 BqLog 的异步缓冲，是
        // force_flush_all_logs() 失败时的兜底——崩溃前最后 N 条预渲染日志不丢。
        // 渲染与 BqLog 各自独立，渲染失败会退化为只记格式串（见 recordTail）。
        recordTail(level, log_content_cstr, std::forward<TARGS>(args)...);
        switch (level)
        {
            case LogLevel::verbose:
                m_Logger->verbose(category, log_content_cstr, std::forward<TARGS>(args)...);
                break;
            case LogLevel::debug:
                m_Logger->debug(category, log_content_cstr, std::forward<TARGS>(args)...);
                break;
            case LogLevel::info:
                m_Logger->info(category, log_content_cstr, std::forward<TARGS>(args)...);
                break;
            case LogLevel::warning:
                m_Logger->warning(category, log_content_cstr, std::forward<TARGS>(args)...);
                break;
            case LogLevel::error:
                m_Logger->error(category, log_content_cstr, std::forward<TARGS>(args)...);
                break;
            case LogLevel::fatal:
                m_Logger->fatal(category, log_content_cstr, std::forward<TARGS>(args)...);
                fatalCallback(log_content_cstr, std::forward<TARGS>(args)...);
                break;
            default:
                break;
        }
    }

    // 为字符串字面量特化（编译时已知）
    template<typename... TARGS>
    void fatalCallback(const char* format_str, TARGS&&... args)
    {
#if Z_LOG_HAS_STD_FORMAT
        const std::string message = std::vformat(format_str, std::make_format_args(args...));
        throw std::runtime_error(message);
#else
        (void)sizeof...(args);
        throw std::runtime_error(format_str);
#endif
    }

    // 为 std::string 特化（运行时）
    template<typename... TARGS>
    void fatalCallback(const std::string& format_str, TARGS&&... args)
    {
#if Z_LOG_HAS_STD_FORMAT
        const std::string message =
            std::vformat(format_str.c_str(), std::make_format_args(std::forward<TARGS>(args)...));
        throw std::runtime_error(message);
#else
        (void)sizeof...(args);
        throw std::runtime_error(format_str);
#endif
    }

    // 把一条日志渲染进崩溃安全环形缓冲（#11）。与上面的 fatalCallback 一样要处理
    // 「有 std::format」与「没有」两条路；这里不抛异常——记录失败就退化为只记格式串，
    // 因为崩溃路径上抛异常比丢一条记录糟得多。level 映射到 logtail::kLevel* 常量
    // （数值与 LogLevel 声明顺序一致）。
    template<typename STR, typename... TARGS>
    void recordTail(LogLevel level, const STR& format_str, TARGS&&... args)
    {
        char buf[zengine::logtail::kMaxTextLen];
        const uint8_t lv = toTailLevel(level);

#if Z_LOG_HAS_STD_FORMAT
        try
        {
            const std::string rendered = std::vformat(format_str, std::make_format_args(args...));
            std::snprintf(buf, sizeof(buf), "%s", rendered.c_str());
        }
        catch (...)
        {
            // 渲染失败（如参数类型不可格式化）——至少把格式串本身记下来。
            std::snprintf(buf, sizeof(buf), "%s", std::string_view(format_str).data());
        }
#else
        // Referencing the pack via sizeof(...) is valid even when it is empty
        // and requires no expansion -- works on MSVC and clang (emcc). (A bare
        // `(void)args...` is parsed as a C-cast `(void)args` plus an ellipsis.)
        (void)sizeof...(args);
        std::snprintf(buf, sizeof(buf), "%s", std::string_view(format_str).data());
#endif
        zengine::logtail::Record(lv, buf);
    }

    static uint8_t toTailLevel(LogLevel level)
    {
        switch (level)
        {
            case LogLevel::verbose: return zengine::logtail::kLevelVerbose;
            case LogLevel::debug:   return zengine::logtail::kLevelDebug;
            case LogLevel::info:    return zengine::logtail::kLevelInfo;
            case LogLevel::warning: return zengine::logtail::kLevelWarning;
            case LogLevel::error:   return zengine::logtail::kLevelError;
            case LogLevel::fatal:   return zengine::logtail::kLevelFatal;
            default:                return zengine::logtail::kLevelInfo;
        }
    }

    void registerCallback(bq::type_func_ptr_console_callback callback)
    {
        m_Logger->register_console_callback(callback);
    }

    void unregisterCallback(bq::type_func_ptr_console_callback callback)
    {
        m_Logger->unregister_console_callback(callback);
    }

    std::unique_ptr<bq::engine_log> m_Logger;
};

// Free-function bridge to LogSystem::ForceFlush(), so low-level headers (notably
// CrashHandler.h) can flush the log on a fatal path without pulling in this
// header's EASTL / <format> dependencies. Safe to call before the LogSystem
// singleton exists; no-op in that case.
void ZEngineForceFlushLogs();

// Free-function bridge to the crash-safe log-tail dump (#11). Writes the ring
// buffer's most recent N pre-rendered log records to <dir>/crash_log_tail.txt.
// Like ZEngineForceFlushLogs, this must work on a fatal path with the process
// already compromised: it touches no singleton, allocates nothing on the heap,
// and tolerates concurrent writes (torn slots are skipped). Returns the number
// of records written. Empty dir => no-op, returns 0.
uint32_t ZEngineDumpCrashLogTail(const std::string& dir);