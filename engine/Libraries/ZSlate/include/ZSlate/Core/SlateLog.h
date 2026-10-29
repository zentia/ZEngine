#pragma once

// ============================================================================
// SlateLog - one process-wide diagnostics sink for ZSlate
// ============================================================================
// ZSlate is deliberately dependency-free: it carries its own stb_truetype, its
// own math and its own platform backends, and it must keep linking into tools
// that never see the engine (this is why the LOG_* macros were originally
// replanted as plain fprintf). That same property makes its diagnostics
// invisible where it matters most: the hosts are WIN32-subsystem GUI apps with
// no console, so a font failure scrolls past a stderr nobody owns.
//
// The compromise is a sink, not a dependency: ZSlate still formats and emits on
// its own (default: stderr/stdout, byte-identical to the old behaviour), but a
// host may install a callback and redirect every message into its own logging
// stack - the engine's LogSystem / bqlog (扁鹊) for ZEditor and the engine
// tools, or a file appender for a standalone one.
//
// Thread-safety: the sink pointer is atomic and read once per call. Emitting
// from several threads at once is safe as long as the installed sink is
// itself thread-safe (bqlog and fprintf both are).

namespace ZSlate
{

enum class ELogLevel
{
    Info,
    Warning,
    Error,
};

// nullptr restores the default stderr/stdout sink.
using FSlateLogSink = void (*)(ELogLevel level, const char* message);

void SetLogSink(FSlateLogSink sink);
FSlateLogSink GetLogSink();

// printf-style. `message` handed to the sink is already formatted and NUL
// terminated; it is NOT kept alive after the call returns.
void Log(ELogLevel level, const char* fmt, ...);

}  // namespace ZSlate
