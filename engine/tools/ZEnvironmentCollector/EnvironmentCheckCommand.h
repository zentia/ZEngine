#pragma once
// ============================================================================
// The "plain" environment check (hardware snapshot + PE inspection + isolated
// DLL probe) as a reusable command instead of a wmain() body.
// ============================================================================
// Before the three tools were merged this pipeline lived verbatim inside
// main.cpp's wmain(), so only the console executable could run it. The merged
// shell needs the exact same pipeline twice:
//   * `-batchmode`                -> text on stdout, exit code to the caller
//   * the "compatibility" tab     -> text in a scroll view, no console
// Both go through RunEnvironmentCheck() so the two front-ends cannot drift.
// ============================================================================

#include "EnvironmentProbe.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace zengine::envcheck
{
struct EnvironmentCheckOptions
{
    std::filesystem::path outputDirectory = std::filesystem::current_path();
    std::vector<std::filesystem::path> dllPaths;
    std::vector<std::filesystem::path> probeDllPaths;
    std::filesystem::path probeRunner;
    std::vector<std::wstring> probeRunnerArguments;
    std::string probeExport;
    std::uint32_t probeTimeoutMilliseconds = 60000;
    Requirements requirements;
    bool quiet = false;
    bool showHelp = false;
    // GUI-only switch: the recursive DLL scan plus the isolated load probe can
    // take minutes on a build tree, and the compatibility tab is usually about
    // the machine, not about the DLLs next to the tool.
    bool includeDllChecks = true;
};

struct EnvironmentCheckOutcome
{
    int exitCode = 0;
    std::string textReport;
    std::string jsonReport;
    std::filesystem::path textPath;
    std::filesystem::path jsonPath;
    Evaluation evaluation;
};

// Progress reporting; the same shape the collector uses so the shell can share
// one status label. `message` is a human-readable stage name.
using EnvironmentCheckProgress = std::function<void(const std::string& message)>;

EnvironmentCheckOptions ParseEnvironmentCheckCommandLine(int argumentCount, wchar_t** arguments);
void PrintEnvironmentCheckUsage();

// Parses a comma-separated feature list ("none", "avx,avx2,fma,f16c") into
// `requirements`. Shared by --require and the compatibility tab so the GUI cannot
// accept a spelling the CLI would reject. Throws std::invalid_argument.
void ApplyRequiredFeatures(Requirements& requirements, const std::string& commaSeparatedFeatures);

// Runs the whole pipeline. `cancel` is polled between stages; progress is called
// from the calling thread. Throws std::exception on tool errors -- callers turn
// that into their own reporting (30 for the shell's exit code, a status label
// for the GUI).
EnvironmentCheckOutcome RunEnvironmentCheck(
    const EnvironmentCheckOptions& options,
    const std::atomic_bool& cancel,
    const EnvironmentCheckProgress& progress = {});

EnvironmentCheckOutcome RunEnvironmentCheck(const EnvironmentCheckOptions& options);

// Parse + run + print. Returns the compatibility exit code, or 30 on a tool
// error (already reported on stderr).
int RunEnvironmentCheckCommand(int argumentCount, wchar_t** arguments);

// Prints the GPU auto-select verification report. Returns 0 when every mock
// regression case passes, 1 when the selection heuristic regressed, 30 on a tool
// error. A machine that cannot run EnumAdapterByGpuPreference is NOT a failure:
// that is reported in the text and still exits 0.
int RunGpuSelectionCommand(int argumentCount, wchar_t** arguments);
} // namespace zengine::envcheck
