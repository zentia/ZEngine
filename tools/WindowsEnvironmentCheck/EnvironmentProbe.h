#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace zengine::envcheck
{
struct CpuFeatures
{
    std::string vendor;
    std::string brand;
    int family = 0;
    int model = 0;
    int stepping = 0;
    unsigned int logicalProcessorCount = 0;
    bool sse41 = false;
    bool sse42 = false;
    bool avxHardware = false;
    bool osxsave = false;
    bool avxOsEnabled = false;
    bool avx = false;
    bool avx2 = false;
    bool fma = false;
    bool f16c = false;
};

struct SystemInfo
{
    std::string windowsVersion;
    bool is64BitOs = false;
    bool is64BitProcess = false;
    std::uint64_t physicalMemoryBytes = 0;
    std::uint64_t availablePhysicalMemoryBytes = 0;
    std::uint64_t totalPageFileBytes = 0;
    std::uint64_t availablePageFileBytes = 0;
    std::uint64_t uptimeMilliseconds = 0;
};

struct GpuAdapterInfo
{
    std::string name;
    std::string driverProvider;
    std::string driverVersion;
    std::string driverDate;
    std::uint32_t vendorId = 0;
    std::uint32_t deviceId = 0;
    std::uint32_t subsystemId = 0;
    std::uint32_t revision = 0;
    std::uint64_t dedicatedVideoMemoryBytes = 0;
    std::uint64_t dedicatedSystemMemoryBytes = 0;
    std::uint64_t sharedSystemMemoryBytes = 0;
    unsigned int outputCount = 0;
    bool softwareAdapter = false;
};

struct DisplayInfo
{
    std::string deviceName;
    std::string monitorName;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t refreshRate = 0;
    std::uint32_t bitsPerPixel = 0;
    bool primary = false;
    bool attachedToDesktop = false;
};

struct PhysicalDiskInfo
{
    std::string devicePath;
    std::string model;
    std::string vendor;
    std::string busType;
    std::uint64_t capacityBytes = 0;
    bool removable = false;
};

struct VolumeInfo
{
    std::string rootPath;
    std::string volumeLabel;
    std::string fileSystem;
    std::string driveType;
    std::uint64_t totalBytes = 0;
    std::uint64_t freeBytes = 0;
    std::uint64_t availableBytes = 0;
};

struct EnvironmentSnapshot
{
    CpuFeatures cpu;
    SystemInfo system;
    std::vector<GpuAdapterInfo> gpuAdapters;
    std::vector<DisplayInfo> displays;
    std::vector<PhysicalDiskInfo> physicalDisks;
    std::vector<VolumeInfo> volumes;
};

struct Requirements
{
    bool avx = true;
    bool avx2 = false;
    bool fma = false;
    bool f16c = false;
    std::uint64_t minimumMemoryBytes = 4ull * 1024ull * 1024ull * 1024ull;
};

enum class CompatibilityStatus
{
    Passed,
    Warning,
    Unsupported
};

struct Evaluation
{
    CompatibilityStatus status = CompatibilityStatus::Passed;
    int exitCode = 0;
    std::string issueCode = "NONE";
    std::vector<std::string> failures;
    std::vector<std::string> warnings;
};

EnvironmentSnapshot CollectEnvironment();
Evaluation EvaluateEnvironment(const EnvironmentSnapshot& snapshot, const Requirements& requirements);
const char* ToString(CompatibilityStatus status);
} // namespace zengine::envcheck
