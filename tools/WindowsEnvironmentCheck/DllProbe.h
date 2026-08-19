#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace zengine::envcheck
{
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
