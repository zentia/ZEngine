// ============================================================================
// SlateLog - see include/ZSlate/Core/SlateLog.h
// ============================================================================

#include "ZSlate/Core/SlateLog.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>

namespace ZSlate
{

namespace
{
    // 1024 is plenty for every message ZSlate emits (paths are the longest);
    // overflow is truncated rather than heap-allocated - a diagnostics path must
    // not be the thing that fails when the process is already in trouble.
    constexpr int kLogBufferSize = 1024;

    void DefaultSink(ELogLevel level, const char* message)
    {
        std::FILE* stream = (level == ELogLevel::Info) ? stdout : stderr;
        std::fputs(message, stream);
        std::fputc('\n', stream);
    }

    std::atomic<FSlateLogSink>& SinkSlot()
    {
        static std::atomic<FSlateLogSink> sink{&DefaultSink};
        return sink;
    }
}

void SetLogSink(FSlateLogSink sink)
{
    SinkSlot().store(sink ? sink : &DefaultSink, std::memory_order_release);
}

FSlateLogSink GetLogSink()
{
    return SinkSlot().load(std::memory_order_acquire);
}

void Log(ELogLevel level, const char* fmt, ...)
{
    if (fmt == nullptr) return;

    char buffer[kLogBufferSize];
    std::va_list args;
    va_start(args, fmt);
    const int written = std::vsnprintf(buffer, kLogBufferSize, fmt, args);
    va_end(args);
    if (written < 0) return;
    buffer[kLogBufferSize - 1] = '\0';

    GetLogSink()(level, buffer);
}

}  // namespace ZSlate
