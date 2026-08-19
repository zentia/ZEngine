#include "EnvironmentProbe.h"
#include "HardwareProbe.h"

#include <Windows.h>
#include <intrin.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <sstream>

namespace zengine::envcheck
{
namespace
{
std::array<int, 4> Cpuid(int leaf, int subleaf = 0)
{
    std::array<int, 4> registers{};
    __cpuidex(registers.data(), leaf, subleaf);
    return registers;
}

std::string TrimCpuBrand(std::string value)
{
    const auto first = value.find_first_not_of(' ');
    if (first == std::string::npos)
    {
        return {};
    }

    const auto last = value.find_last_not_of(" \0", std::string::npos, 2);
    value = value.substr(first, last - first + 1);

    std::string normalized;
    normalized.reserve(value.size());
    bool previousWasSpace = false;
    for (const char character : value)
    {
        const bool isSpace = character == ' ' || character == '\t';
        if (!isSpace || !previousWasSpace)
        {
            normalized.push_back(isSpace ? ' ' : character);
        }
        previousWasSpace = isSpace;
    }
    return normalized;
}

std::string ReadWindowsVersion()
{
    using RtlGetVersionFunction = LONG(WINAPI*)(OSVERSIONINFOW*);
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr)
    {
        return "Unknown";
    }

    const auto rtlGetVersion = reinterpret_cast<RtlGetVersionFunction>(
        GetProcAddress(ntdll, "RtlGetVersion"));
    if (rtlGetVersion == nullptr)
    {
        return "Unknown";
    }

    OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    if (rtlGetVersion(&version) != 0)
    {
        return "Unknown";
    }

    std::ostringstream stream;
    stream << version.dwMajorVersion << '.' << version.dwMinorVersion
           << " (build " << version.dwBuildNumber << ')';
    return stream.str();
}

bool Is64BitOperatingSystem()
{
    SYSTEM_INFO info{};
    GetNativeSystemInfo(&info);
    return info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ||
           info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64;
}

CpuFeatures CollectCpuFeatures()
{
    CpuFeatures result;
    const auto basic = Cpuid(0);
    const int maximumBasicLeaf = basic[0];

    char vendor[13]{};
    std::memcpy(vendor, &basic[1], sizeof(int));
    std::memcpy(vendor + 4, &basic[3], sizeof(int));
    std::memcpy(vendor + 8, &basic[2], sizeof(int));
    result.vendor = vendor;

    if (maximumBasicLeaf >= 1)
    {
        const auto leaf1 = Cpuid(1);
        const unsigned int eax = static_cast<unsigned int>(leaf1[0]);
        const unsigned int ecx = static_cast<unsigned int>(leaf1[2]);
        const int baseFamily = static_cast<int>((eax >> 8) & 0x0f);
        const int baseModel = static_cast<int>((eax >> 4) & 0x0f);
        const int extendedFamily = static_cast<int>((eax >> 20) & 0xff);
        const int extendedModel = static_cast<int>((eax >> 16) & 0x0f);

        result.family = baseFamily == 0x0f ? baseFamily + extendedFamily : baseFamily;
        result.model = (baseFamily == 0x06 || baseFamily == 0x0f)
            ? baseModel | (extendedModel << 4)
            : baseModel;
        result.stepping = static_cast<int>(eax & 0x0f);
        result.sse41 = (ecx & (1u << 19)) != 0;
        result.sse42 = (ecx & (1u << 20)) != 0;
        result.fma = (ecx & (1u << 12)) != 0;
        result.osxsave = (ecx & (1u << 27)) != 0;
        result.avxHardware = (ecx & (1u << 28)) != 0;
        result.f16c = (ecx & (1u << 29)) != 0;

        if (result.osxsave)
        {
            const unsigned __int64 xcr0 = _xgetbv(0);
            result.avxOsEnabled = (xcr0 & 0x6u) == 0x6u;
        }

        result.avx = result.avxHardware && result.avxOsEnabled;
        result.fma = result.fma && result.avx;
        result.f16c = result.f16c && result.avx;
    }

    if (maximumBasicLeaf >= 7)
    {
        const auto leaf7 = Cpuid(7, 0);
        const unsigned int ebx = static_cast<unsigned int>(leaf7[1]);
        result.avx2 = result.avx && (ebx & (1u << 5)) != 0;
        result.avx512Hardware = (ebx & (1u << 16)) != 0;
        if (result.osxsave)
        {
            const unsigned __int64 xcr0 = _xgetbv(0);
            result.avx512OsEnabled = (xcr0 & 0xE6u) == 0xE6u;
        }
        result.avx512 = result.avx512Hardware && result.avx512OsEnabled;
    }

    const auto extended = Cpuid(static_cast<int>(0x80000000u));
    const unsigned int maximumExtendedLeaf = static_cast<unsigned int>(extended[0]);
    if (maximumExtendedLeaf >= 0x80000004u)
    {
        char brand[49]{};
        for (unsigned int leaf = 0; leaf < 3; ++leaf)
        {
            const auto registers = Cpuid(static_cast<int>(0x80000002u + leaf));
            std::memcpy(brand + leaf * 16, registers.data(), 16);
        }
        result.brand = TrimCpuBrand(brand);
    }

    result.logicalProcessorCount = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return result;
}
} // namespace

EnvironmentSnapshot CollectEnvironment()
{
    EnvironmentSnapshot snapshot;
    snapshot.cpu = CollectCpuFeatures();
    snapshot.system.windowsVersion = ReadWindowsVersion();
    snapshot.system.is64BitOs = Is64BitOperatingSystem();
#if defined(_WIN64)
    snapshot.system.is64BitProcess = true;
#else
    snapshot.system.is64BitProcess = false;
#endif

    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory) != FALSE)
    {
        snapshot.system.physicalMemoryBytes = memory.ullTotalPhys;
        snapshot.system.availablePhysicalMemoryBytes = memory.ullAvailPhys;
        snapshot.system.totalPageFileBytes = memory.ullTotalPageFile;
        snapshot.system.availablePageFileBytes = memory.ullAvailPageFile;
    }
    snapshot.system.uptimeMilliseconds = GetTickCount64();

    CollectGraphicsInformation(snapshot);
    CollectStorageInformation(snapshot);
    return snapshot;
}

Evaluation EvaluateEnvironment(const EnvironmentSnapshot& snapshot, const Requirements& requirements)
{
    Evaluation evaluation;

    if (!snapshot.system.is64BitOs || !snapshot.system.is64BitProcess)
    {
        evaluation.failures.emplace_back("A 64-bit Windows operating system and process are required.");
        evaluation.exitCode = 11;
        evaluation.issueCode = "WINDOWS_X64_REQUIRED";
    }

    const auto requireFeature = [&evaluation](bool required, bool available, const char* name)
    {
        if (required && !available)
        {
            evaluation.failures.emplace_back(std::string("Required CPU feature is unavailable: ") + name + '.');
            if (evaluation.exitCode == 0)
            {
                evaluation.exitCode = 10;
                evaluation.issueCode = "CPU_ISA_UNSUPPORTED";
            }
        }
    };

    requireFeature(requirements.avx, snapshot.cpu.avx, "AVX");
    requireFeature(requirements.avx2, snapshot.cpu.avx2, "AVX2");
    requireFeature(requirements.fma, snapshot.cpu.fma, "FMA");
    requireFeature(requirements.f16c, snapshot.cpu.f16c, "F16C");

    if (snapshot.system.physicalMemoryBytes != 0 &&
        snapshot.system.physicalMemoryBytes < requirements.minimumMemoryBytes)
    {
        evaluation.failures.emplace_back("Installed physical memory is below the configured minimum.");
        if (evaluation.exitCode == 0)
        {
            evaluation.exitCode = 12;
            evaluation.issueCode = "PHYSICAL_MEMORY_INSUFFICIENT";
        }
    }

    for (const auto& image : snapshot.inspectedImages)
    {
        if (!image.validPeImage)
        {
            evaluation.failures.emplace_back(
                "DLL inspection failed for " + image.path + ": " + image.error);
            if (evaluation.exitCode == 0)
            {
                evaluation.exitCode = 21;
                evaluation.issueCode = "DLL_INSPECTION_FAILED";
            }
            continue;
        }
        if (!image.is64Bit)
        {
            evaluation.failures.emplace_back("DLL is not a 64-bit image: " + image.path);
            if (evaluation.exitCode == 0)
            {
                evaluation.exitCode = 21;
                evaluation.issueCode = "DLL_ARCHITECTURE_UNSUPPORTED";
            }
        }
        if (requirements.rejectDebugRuntime && image.usesDebugRuntime)
        {
            std::string runtimes;
            for (const auto& library : image.debugRuntimeLibraries)
            {
                if (!runtimes.empty())
                {
                    runtimes += ", ";
                }
                runtimes += library;
            }
            evaluation.failures.emplace_back(
                "DLL depends on non-redistributable Debug CRT libraries: " + image.path + " [" + runtimes + "]");
            evaluation.exitCode = 20;
            evaluation.issueCode = "DLL_DEBUG_RUNTIME_UNSUPPORTED";
        }

        const auto& instructionSets = image.instructionSets;
        if (instructionSets.scanAttempted && !instructionSets.scanCompleted)
        {
            evaluation.failures.emplace_back(
                "DLL instruction scan failed for " + image.path + ": " + instructionSets.error);
            if (evaluation.exitCode == 0)
            {
                evaluation.exitCode = 21;
                evaluation.issueCode = "DLL_INSTRUCTION_SCAN_FAILED";
            }
            continue;
        }

        std::string unavailableFeatures;
        const auto addUnavailableFeature = [&unavailableFeatures](bool present, bool available, const char* name)
        {
            if (!present || available)
            {
                return;
            }
            if (!unavailableFeatures.empty())
            {
                unavailableFeatures += ", ";
            }
            unavailableFeatures += name;
        };
        addUnavailableFeature(instructionSets.containsAvx, snapshot.cpu.avx, "AVX");
        addUnavailableFeature(instructionSets.containsAvx2, snapshot.cpu.avx2, "AVX2");
        addUnavailableFeature(instructionSets.containsFma, snapshot.cpu.fma, "FMA");
        addUnavailableFeature(instructionSets.containsF16c, snapshot.cpu.f16c, "F16C");
        addUnavailableFeature(instructionSets.containsAvx512, snapshot.cpu.avx512, "AVX-512");
        if (!unavailableFeatures.empty())
        {
            evaluation.warnings.emplace_back(
                "DLL contains instructions unavailable on this CPU/OS: " + image.path + " [" +
                unavailableFeatures + "]. Static presence is informational because runtime CPU dispatch may protect these paths.");
        }
    }

    for (const auto& probe : snapshot.dllProbeResults)
    {
        if (!probe.processStarted)
        {
            evaluation.failures.emplace_back(
                "DLL probe infrastructure failed for " + probe.dllPath + ": " + probe.error);
            if (evaluation.exitCode == 0)
            {
                evaluation.exitCode = 24;
                evaluation.issueCode = "DLL_PROBE_INFRASTRUCTURE_FAILED";
            }
            continue;
        }
        if (probe.timedOut)
        {
            evaluation.failures.emplace_back("DLL probe timed out: " + probe.dllPath);
            if (evaluation.exitCode == 0)
            {
                evaluation.exitCode = 23;
                evaluation.issueCode = "DLL_PROBE_TIMEOUT";
            }
            continue;
        }
        if (probe.runnerReportedIsaViolation)
        {
            evaluation.failures.emplace_back(
                "CPU emulator reported an executed instruction unsupported by the selected target CPU while probing " +
                probe.dllPath + '.');
            if (evaluation.exitCode == 0)
            {
                evaluation.exitCode = 23;
                evaluation.issueCode = "DLL_PROBE_ILLEGAL_INSTRUCTION";
            }
            continue;
        }
        if (!probe.resultAvailable)
        {
            evaluation.failures.emplace_back(
                "DLL probe crashed or produced no result for " + probe.dllPath + ": " + probe.error);
            if (evaluation.exitCode == 0)
            {
                evaluation.exitCode = 23;
                evaluation.issueCode = "DLL_PROBE_CRASHED";
            }
            continue;
        }
        if (probe.exceptionCode != 0)
        {
            std::ostringstream message;
            message << "DLL probe raised exception 0x" << std::hex << std::uppercase
                    << probe.exceptionCode << " while loading or invoking " << probe.dllPath << '.';
            evaluation.failures.push_back(message.str());
            if (evaluation.exitCode == 0)
            {
                evaluation.exitCode = 23;
                evaluation.issueCode = probe.exceptionCode == 0xC000001Du
                    ? "DLL_PROBE_ILLEGAL_INSTRUCTION"
                    : "DLL_PROBE_EXCEPTION";
            }
            continue;
        }
        if (!probe.loadSucceeded)
        {
            evaluation.failures.emplace_back(
                "LoadLibrary failed for " + probe.dllPath + " with Win32 error " +
                std::to_string(probe.win32Error) + '.');
            if (evaluation.exitCode == 0)
            {
                evaluation.exitCode = 23;
                evaluation.issueCode = "DLL_LOAD_FAILED";
            }
            continue;
        }
        if (probe.exportRequested && !probe.exportFound)
        {
            evaluation.failures.emplace_back(
                "Requested compatibility export was not found in " + probe.dllPath + ": " +
                probe.exportName + '.');
            if (evaluation.exitCode == 0)
            {
                evaluation.exitCode = 23;
                evaluation.issueCode = "DLL_PROBE_EXPORT_NOT_FOUND";
            }
            continue;
        }
        if (probe.exportCalled && probe.exportResult == 0)
        {
            evaluation.warnings.emplace_back(
                "Compatibility export returned 0 for " + probe.dllPath +
                "; the DLL loaded successfully but this optional capability is unavailable.");
        }
        if (probe.runnerReportedInternalError)
        {
            evaluation.warnings.emplace_back(
                "The CPU emulator reported an internal assertion after producing a valid DLL probe result for " +
                probe.dllPath + "; target DLL load results were retained, but emulator stability should be reviewed.");
        }
    }

    if (!snapshot.cpu.avx2)
    {
        evaluation.warnings.emplace_back("AVX2 is unavailable; optimized local prediction paths may be disabled or incompatible.");
    }
    if (!snapshot.cpu.fma)
    {
        evaluation.warnings.emplace_back("FMA is unavailable.");
    }
    if (!snapshot.cpu.f16c)
    {
        evaluation.warnings.emplace_back("F16C is unavailable.");
    }
    if (!snapshot.cpu.avx)
    {
        evaluation.warnings.emplace_back(
            "This machine matches the known GameCore.dll 0xC000001D illegal-instruction risk profile.");
    }

    if (!evaluation.failures.empty())
    {
        evaluation.status = CompatibilityStatus::Unsupported;
    }
    else if (!evaluation.warnings.empty())
    {
        evaluation.status = CompatibilityStatus::Warning;
    }
    return evaluation;
}

const char* ToString(CompatibilityStatus status)
{
    switch (status)
    {
    case CompatibilityStatus::Passed:
        return "PASSED";
    case CompatibilityStatus::Warning:
        return "WARNING";
    case CompatibilityStatus::Unsupported:
        return "UNSUPPORTED";
    }
    return "UNKNOWN";
}
} // namespace zengine::envcheck
