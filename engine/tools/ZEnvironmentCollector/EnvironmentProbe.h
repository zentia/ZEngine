#pragma once

#include "DllProbe.h"
#include "GpuSelectionProbe.h"
#include "PeInspector.h"

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
    bool avx512Hardware = false;
    bool avx512OsEnabled = false;
    bool avx512 = false;
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

// One output on an adapter, i.e. one port something could be plugged into.
// `attachedToDesktop` is the part that matters: an output that exists but is not
// attached is not the one carrying the picture.
struct GpuOutputInfo
{
    std::string deviceName;
    bool attachedToDesktop = false;
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
    unsigned int attachedOutputCount = 0;
    std::vector<GpuOutputInfo> outputs;
    // False when EnumOutputs failed part-way. Without this, "no outputs" and
    // "could not ask" look the same, and the latter must never be reported as
    // the former -- that is the same trap as an empty adapter list.
    bool outputsEnumerated = false;
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
    std::vector<PeImageInfo> inspectedImages;
    std::vector<DllProbeResult> dllProbeResults;
    std::vector<GpuSelectionMockCaseResult> gpuSelectionMockResults;
    GpuSelectionRealProbeResult gpuSelectionRealProbe;
};

struct Requirements
{
    bool avx = false;
    bool avx2 = false;
    bool fma = false;
    bool f16c = false;
    bool rejectDebugRuntime = true;
    std::uint64_t minimumMemoryBytes = 4ull * 1024ull * 1024ull * 1024ull;
    // Available, not installed. The crash report we actually receive is the
    // 16 GB machine with 300 MB free, and "installed >= minimum" says nothing
    // at all about that state.
    std::uint64_t minimumAvailableMemoryBytes = 2ull * 1024ull * 1024ull * 1024ull;
    // How much page file a machine that cannot do without one needs. Windows
    // reports ullTotalPageFile as the commit limit, which is RAM plus page
    // file -- the rule subtracts to get the page file itself.
    std::uint64_t minimumPageFileBytes = 2ull * 1024ull * 1024ull * 1024ull;
    // Above this much installed RAM a small or absent page file is a deliberate
    // choice, so the rule stays quiet instead of arguing with it.
    std::uint64_t pageFileRuleMaxMemoryBytes = 16ull * 1024ull * 1024ull * 1024ull;
    // Free space on the volume the game sits on: patches and logs land there.
    std::uint64_t minimumFreeDiskSpaceBytes = 2ull * 1024ull * 1024ull * 1024ull;
    // Raw D3D_FEATURE_LEVEL number (0xb000 for 11_0), kept as a plain integer so
    // this header needs no DirectX header. 0 means "no feature level of its own":
    // device creation is still checked, but a machine that comes back with a
    // lower level is not blocked for it, because a D3D11 game running at 10_0 is
    // a game that usually runs fine.
    std::uint32_t minimumFeatureLevel = 0;
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

EnvironmentSnapshot CollectEnvironment(bool includeDevices = true);
Evaluation EvaluateEnvironment(const EnvironmentSnapshot& snapshot, const Requirements& requirements);
const char* ToString(CompatibilityStatus status);
} // namespace zengine::envcheck
