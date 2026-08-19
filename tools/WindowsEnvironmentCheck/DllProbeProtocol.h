#pragma once

#include <cstdint>

namespace zengine::envcheck
{
constexpr std::uint32_t DllProbeMagic = 0x5044435Au;
constexpr std::uint32_t DllProbeProtocolVersion = 1;

struct DllProbeWireResult
{
    std::uint32_t magic = DllProbeMagic;
    std::uint32_t version = DllProbeProtocolVersion;
    std::uint32_t loadSucceeded = 0;
    std::uint32_t win32Error = 0;
    std::uint32_t exceptionCode = 0;
    std::uint32_t exportRequested = 0;
    std::uint32_t exportFound = 0;
    std::uint32_t exportCalled = 0;
    std::int32_t exportResult = 0;
};
} // namespace zengine::envcheck
