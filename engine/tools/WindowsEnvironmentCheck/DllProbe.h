#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace zengine::envcheck
{
// The probe ships as its own file on purpose -- it is a separate *process*, not
// a code path inside the collector, because loading a suspect DLL in-process
// holds the Windows loader lock while DllMain runs: a hang there cannot be
// aborted (FreeLibrary deadlocks, the thread cannot be terminated) and a fault
// there takes the whole collector and the in-flight report down with it. It is
// also built with /NODEFAULTLIB and no CRT, which a single PE image cannot mix
// with the collector's C++ runtime. See README.md, "产物".
constexpr const wchar_t* ProbeExecutableName = L"ZDllLoadProbe.exe";

struct DllProbeOptions
{
    std::filesystem::path probeExecutable;
    std::filesystem::path runnerExecutable;
    std::vector<std::wstring> runnerArguments;
    std::string exportName;
    std::uint32_t timeoutMilliseconds = 60000;
};

struct DllProbeResult
{
    std::string dllPath;
    std::string probeExecutable;
    std::string runnerExecutable;
    std::vector<std::string> runnerArguments;
    std::string exportName;
    bool attempted = false;
    bool processStarted = false;
    bool timedOut = false;
    bool resultAvailable = false;
    bool loadSucceeded = false;
    bool exportRequested = false;
    bool exportFound = false;
    bool exportCalled = false;
    std::uint32_t processExitCode = 0;
    std::uint32_t win32Error = 0;
    std::uint32_t exceptionCode = 0;
    std::int32_t exportResult = 0;
    std::uint64_t durationMilliseconds = 0;
    bool runnerReportedIsaViolation = false;
    bool runnerReportedInternalError = false;
    std::string runnerOutput;
    std::string error;
};

DllProbeResult RunDllProbe(
    const std::filesystem::path& dllPath,
    const DllProbeOptions& options);
} // namespace zengine::envcheck
