#pragma once

#include "Runtime/Core/Base/Singleton.h"
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