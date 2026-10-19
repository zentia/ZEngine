#pragma once

// ============================================================================
// EnvCheckLog - 扁鹊 (bqlog) front-end for ZEnvironmentCollector
// ============================================================================
// Every other ZEngine executable logs through bqlog, and this tool is the one
// that most needs it: it is a WINDOWS-subsystem binary, so stderr goes nowhere
// when it is double-clicked, and the failures it exists to report (a missing
// font, a failed WER restore, a ProcDump that never produced a dump) are exactly
// the ones a player cannot describe. Logging through bqlog puts them in the
// same per-executable log file the engine uses, which is what support asks for.
//
// Two implementations behind this one interface, selected by the
// ZENVCHECK_USE_ZRUNTIME define (see CMakeLists.txt):
//
//   * defined  -- the tool links ZRuntimeShared and logs through the engine's
//     LogSystem, reached via the C ABI in Runtime/ZEngineApi/ZEngineToolApi.h.
//     The DLL owns bqlog, so the process has exactly one log instance and the
//     file sits beside the engine's own. The same ZRuntime_ToolInit() call is
//     what makes the localization DataTable loadable, so logging and strings
//     come up together.
//   * undefined -- the standalone configure, which has no ZRuntimeShared target:
//     bqlog is linked statically, exactly as before.
//
// Log file: %USERPROFILE%\AppData\LocalLow\ZEngine\Logs\ZEnvironmentCollector_*.log
// (same rule as LogSystem; -batchmode keeps printing to stdout as well, so CI
// captures are unaffected).
//
// NOT for the report itself: the collection output (JSON / TXT / ZIP manifest)
// stays on stdout / on disk - that is the product, not a log.

#include <string>

namespace zengine::envcheck
{

// Creates the log. Safe to call more than once; later calls are no-ops.
void LogInitialize();

// Drains the async worker and releases the log. Safe to call without init.
void LogShutdown();

// RAII form of the pair above: `ScopedLog log;` at the top of a scope covers
// every return path out of it, including the throwing ones.
struct ScopedLog
{
    ScopedLog() { LogInitialize(); }
    ~ScopedLog() { LogShutdown(); }
    ScopedLog(const ScopedLog&) = delete;
    ScopedLog& operator=(const ScopedLog&) = delete;
};

void LogInfo(const std::string& message);
void LogWarning(const std::string& message);
void LogError(const std::string& message);

// bqlog is asynchronous: a short-lived -batchmode run has to drain explicitly
// or the tail is lost.
void LogFlush();

// Empty until LogInitialize(); meant for a "logs are here" line in the report.
std::string LogFilePath();

}  // namespace zengine::envcheck
