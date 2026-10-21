#include "ReportWriter.h"

#include "JsonEscape.h"
#include "ToolVersion.h"

#include <windows.h>

#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace zengine::envcheck
{
namespace
{
constexpr double BytesPerGiB = 1024.0 * 1024.0 * 1024.0;

const char* BoolText(bool value)
{
    return value ? "true" : "false";
}

std::string HexId(std::uint64_t value, int width)
{
    std::ostringstream stream;
    stream << "0x" << std::uppercase << std::hex << std::setw(width)
           << std::setfill('0') << value;
    return stream.str();
}

void WriteJsonStringArray(
    std::ostringstream& stream,
    const char* name,
    const std::vector<std::string>& values,
    bool trailingComma)
{
    stream << "  \"" << name << "\": [";
    for (std::size_t index = 0; index < values.size(); ++index)
    {
        if (index != 0)
        {
            stream << ", ";
        }
        stream << '"' << EscapeJson(values[index]) << '"';
    }
    stream << ']';
    if (trailingComma)
    {
        stream << ',';
    }
    stream << '\n';
}
} // namespace

std::string BuildTextReport(
    const EnvironmentSnapshot& snapshot,
    const Requirements& requirements,
    const Evaluation& evaluation)
{
    std::ostringstream stream;
    stream << "OSGame Windows Environment Check\n"
           << "================================\n"
           << "Result: " << ToString(evaluation.status) << '\n'
           << "Issue code: " << evaluation.issueCode << '\n'
           << "Exit code: " << evaluation.exitCode << "\n\n"
           << "System\n"
           << "  Windows: " << snapshot.system.windowsVersion << '\n'
           << "  64-bit OS: " << BoolText(snapshot.system.is64BitOs) << '\n'
           << "  64-bit process: " << BoolText(snapshot.system.is64BitProcess) << '\n'
           << std::fixed << std::setprecision(2)
           << "  Physical memory: "
           << static_cast<double>(snapshot.system.physicalMemoryBytes) / BytesPerGiB << " GiB\n"
           << "  Available physical memory: "
           << static_cast<double>(snapshot.system.availablePhysicalMemoryBytes) / BytesPerGiB << " GiB\n"
           << "  Page file total: "
           << static_cast<double>(snapshot.system.totalPageFileBytes) / BytesPerGiB << " GiB\n"
           << "  Page file available: "
           << static_cast<double>(snapshot.system.availablePageFileBytes) / BytesPerGiB << " GiB\n"
           << "  System uptime: " << snapshot.system.uptimeMilliseconds / 1000ull << " seconds\n\n"
           << "CPU\n"
           << "  Vendor: " << snapshot.cpu.vendor << '\n'
           << "  Name: " << snapshot.cpu.brand << '\n'
           << "  Family/Model/Stepping: " << snapshot.cpu.family << '/'
           << snapshot.cpu.model << '/' << snapshot.cpu.stepping << '\n'
           << "  Logical processors: " << snapshot.cpu.logicalProcessorCount << '\n'
           << "  SSE4.1: " << BoolText(snapshot.cpu.sse41) << '\n'
           << "  SSE4.2: " << BoolText(snapshot.cpu.sse42) << '\n'
           << "  AVX hardware: " << BoolText(snapshot.cpu.avxHardware) << '\n'
           << "  OSXSAVE: " << BoolText(snapshot.cpu.osxsave) << '\n'
           << "  AVX OS state: " << BoolText(snapshot.cpu.avxOsEnabled) << '\n'
           << "  AVX usable: " << BoolText(snapshot.cpu.avx) << '\n'
           << "  AVX2 usable: " << BoolText(snapshot.cpu.avx2) << '\n'
           << "  FMA usable: " << BoolText(snapshot.cpu.fma) << '\n'
           << "  F16C usable: " << BoolText(snapshot.cpu.f16c) << '\n'
           << "  AVX-512 hardware: " << BoolText(snapshot.cpu.avx512Hardware) << '\n'
           << "  AVX-512 OS state: " << BoolText(snapshot.cpu.avx512OsEnabled) << '\n'
           << "  AVX-512 usable: " << BoolText(snapshot.cpu.avx512) << "\n";

    stream << "\nGPU adapters (" << snapshot.gpuAdapters.size() << ")\n";
    for (std::size_t index = 0; index < snapshot.gpuAdapters.size(); ++index)
    {
        const auto& gpu = snapshot.gpuAdapters[index];
        stream << "  [" << index << "] " << gpu.name << '\n'
               << "      PCI: " << HexId(gpu.vendorId, 4) << ':' << HexId(gpu.deviceId, 4)
               << ", subsystem " << HexId(gpu.subsystemId, 8)
               << ", revision " << HexId(gpu.revision, 2) << '\n'
               << "      Driver: " << gpu.driverVersion << " (" << gpu.driverProvider
               << ", " << gpu.driverDate << ")\n"
               << "      Dedicated VRAM: "
               << static_cast<double>(gpu.dedicatedVideoMemoryBytes) / BytesPerGiB << " GiB\n"
               << "      Dedicated system memory: "
               << static_cast<double>(gpu.dedicatedSystemMemoryBytes) / BytesPerGiB << " GiB\n"
               << "      Shared system memory: "
               << static_cast<double>(gpu.sharedSystemMemoryBytes) / BytesPerGiB << " GiB\n"
               << "      Outputs: " << gpu.outputCount
               << " (" << gpu.attachedOutputCount << " attached to desktop)"
               << ", software adapter: " << BoolText(gpu.softwareAdapter) << '\n';
    }

    stream << "\nGPU auto-selection\n"
           << "  Mock regression cases: " << snapshot.gpuSelectionMockResults.size() << '\n';
    for (std::size_t index = 0; index < snapshot.gpuSelectionMockResults.size(); ++index)
    {
        const auto& mockResult = snapshot.gpuSelectionMockResults[index];
        stream << "  [" << index << "] " << (mockResult.passed ? "PASS" : "FAIL")
               << ": " << mockResult.caseName << '\n';
        for (std::size_t adapterIndex = 0; adapterIndex < mockResult.adapters.size(); ++adapterIndex)
        {
            const auto& adapter = mockResult.adapters[adapterIndex];
            stream << "      [" << adapterIndex << "] " << adapter.name
                   << " (PCI " << HexId(adapter.vendorId, 4) << ':' << HexId(adapter.deviceId, 4)
                   << ", dedicated "
                   << static_cast<double>(adapter.dedicatedVideoMemoryBytes) / BytesPerGiB
                   << " GiB, shared "
                   << static_cast<double>(adapter.sharedSystemMemoryBytes) / BytesPerGiB
                   << " GiB"
                   << (adapter.looksIntegrated ? ", looks integrated" : "")
                   << ")\n";
        }
        stream << "      chosen=" << mockResult.chosenAdapterIndex
               << ", expected=" << mockResult.expectedAdapterIndex
               << ", excludedIntegrated=" << BoolText(mockResult.excludedIntegrated) << '\n';
    }
    const auto& gpuSelection = snapshot.gpuSelectionRealProbe;
    stream << "  Real probe: factory created=" << BoolText(gpuSelection.dxgiFactoryCreated)
           << ", IDXGIFactory6=" << BoolText(gpuSelection.factory6Available)
           << ", GpuPreference pick=" << BoolText(gpuSelection.gpuPreferencePicked) << '\n';
    if (gpuSelection.gpuPreferencePicked)
    {
        const auto& preferred = gpuSelection.preferredAdapter;
        stream << "      Preferred: " << preferred.name
               << " (PCI " << HexId(preferred.vendorId, 4) << ':' << HexId(preferred.deviceId, 4)
               << ", dedicated "
               << static_cast<double>(preferred.dedicatedVideoMemoryBytes) / BytesPerGiB
               << " GiB)\n";
    }
    else if (!gpuSelection.errorMessage.empty())
    {
        stream << "      " << gpuSelection.errorMessage << '\n';
    }

    stream << "\nDisplays (" << snapshot.displays.size() << ")\n";
    for (std::size_t index = 0; index < snapshot.displays.size(); ++index)
    {
        const auto& display = snapshot.displays[index];
        stream << "  [" << index << "] " << display.deviceName << " - " << display.monitorName << '\n'
               << "      " << display.width << 'x' << display.height << " @ "
               << display.refreshRate << " Hz, " << display.bitsPerPixel << " bpp"
               << ", primary: " << BoolText(display.primary)
               << ", attached: " << BoolText(display.attachedToDesktop) << '\n';
    }

    stream << "\nPhysical disks (" << snapshot.physicalDisks.size() << ")\n";
    for (std::size_t index = 0; index < snapshot.physicalDisks.size(); ++index)
    {
        const auto& disk = snapshot.physicalDisks[index];
        stream << "  [" << index << "] " << disk.devicePath << " - "
               << disk.vendor << ' ' << disk.model << '\n'
               << "      Bus: " << disk.busType << ", capacity: "
               << static_cast<double>(disk.capacityBytes) / BytesPerGiB << " GiB"
               << ", removable: " << BoolText(disk.removable) << '\n';
    }

    stream << "\nVolumes (" << snapshot.volumes.size() << ")\n";
    for (std::size_t index = 0; index < snapshot.volumes.size(); ++index)
    {
        const auto& volume = snapshot.volumes[index];
        stream << "  [" << index << "] " << volume.rootPath << " " << volume.volumeLabel << '\n'
               << "      Type: " << volume.driveType << ", filesystem: " << volume.fileSystem << '\n'
               << "      Total: " << static_cast<double>(volume.totalBytes) / BytesPerGiB
               << " GiB, free: " << static_cast<double>(volume.freeBytes) / BytesPerGiB
               << " GiB, available: " << static_cast<double>(volume.availableBytes) / BytesPerGiB
               << " GiB\n";
    }

    stream << "\nInspected DLLs (" << snapshot.inspectedImages.size() << ")\n";
    for (std::size_t index = 0; index < snapshot.inspectedImages.size(); ++index)
    {
        const auto& image = snapshot.inspectedImages[index];
        stream << "  [" << index << "] " << image.path << '\n'
               << "      Exists: " << BoolText(image.fileExists)
               << ", valid PE: " << BoolText(image.validPeImage)
               << ", x64: " << BoolText(image.is64Bit) << '\n'
               << "      Debug directory: " << BoolText(image.hasDebugDirectory)
               << ", Debug CRT: " << BoolText(image.usesDebugRuntime)
               << ", timestamp: " << HexId(image.timeDateStamp, 8) << '\n';
        if (!image.debugRuntimeLibraries.empty())
        {
            stream << "      Debug runtimes:";
            for (const auto& library : image.debugRuntimeLibraries)
            {
                stream << ' ' << library;
            }
            stream << '\n';
        }

        const auto& instructionSets = image.instructionSets;
        stream << "      Instruction scan: attempted=" << BoolText(instructionSets.scanAttempted)
               << ", completed=" << BoolText(instructionSets.scanCompleted)
               << ", strategy=" << instructionSets.scanStrategy << '\n'
               << "      Executable sections=" << instructionSets.executableSectionCount
               << ", executable bytes=" << instructionSets.executableBytes
               << ", runtime functions=" << instructionSets.runtimeFunctionCount
               << ", scanned code bytes=" << instructionSets.scannedCodeBytes << '\n'
               << "      Decoded instructions=" << instructionSets.decodedInstructionCount
               << ", undecodable bytes=" << instructionSets.undecodableByteCount << '\n'
               << "      AVX=" << instructionSets.avxInstructionCount
               << ", AVX2=" << instructionSets.avx2InstructionCount
               << ", FMA=" << instructionSets.fmaInstructionCount
               << ", F16C=" << instructionSets.f16cInstructionCount
               << ", AVX-512=" << instructionSets.avx512InstructionCount << '\n'
               << "      Contains: AVX=" << BoolText(instructionSets.containsAvx)
               << ", AVX2=" << BoolText(instructionSets.containsAvx2)
               << ", FMA=" << BoolText(instructionSets.containsFma)
               << ", F16C=" << BoolText(instructionSets.containsF16c)
               << ", AVX-512=" << BoolText(instructionSets.containsAvx512) << '\n';
        for (const auto& sample : instructionSets.samples)
        {
            stream << "      [" << sample.category << '/' << sample.isaSet << "] RVA="
                   << HexId(sample.rva, 8) << ", file=" << HexId(sample.fileOffset, 8)
                   << ", bytes=" << sample.bytes << ", " << sample.text << '\n';
        }
        if (!instructionSets.error.empty())
        {
            stream << "      Instruction scan error: " << instructionSets.error << '\n';
        }
        stream << "      Note: static presence does not prove the instruction executes without runtime dispatch.\n";

        if (!image.error.empty())
        {
            stream << "      Error: " << image.error << '\n';
        }
    }

    stream << "\nDLL dynamic probes (" << snapshot.dllProbeResults.size() << ")\n";
    for (std::size_t index = 0; index < snapshot.dllProbeResults.size(); ++index)
    {
        const auto& probe = snapshot.dllProbeResults[index];
        stream << "  [" << index << "] " << probe.dllPath << '\n'
               << "      Process started: " << BoolText(probe.processStarted)
               << ", result available: " << BoolText(probe.resultAvailable)
               << ", timed out: " << BoolText(probe.timedOut)
               << ", duration: " << probe.durationMilliseconds << " ms\n"
               << "      Runner: " << (probe.runnerExecutable.empty() ? "native" : probe.runnerExecutable)
               << ", process exit: " << HexId(probe.processExitCode, 8) << '\n'
               << "      Runner ISA violation: " << BoolText(probe.runnerReportedIsaViolation)
               << ", runner internal error: " << BoolText(probe.runnerReportedInternalError) << '\n'
               << "      Load succeeded: " << BoolText(probe.loadSucceeded)
               << ", Win32 error: " << probe.win32Error
               << ", exception: " << HexId(probe.exceptionCode, 8) << '\n'
               << "      Export: " << probe.exportName
               << ", found: " << BoolText(probe.exportFound)
               << ", called: " << BoolText(probe.exportCalled)
               << ", result: " << probe.exportResult << '\n';
        if (!probe.runnerArguments.empty())
        {
            stream << "      Runner arguments:";
            for (const auto& argument : probe.runnerArguments)
            {
                stream << ' ' << argument;
            }
            stream << '\n';
        }
        if (!probe.error.empty())
        {
            stream << "      Error: " << probe.error << '\n';
        }
        if (!probe.runnerOutput.empty())
        {
            stream << "      Runner output:\n" << probe.runnerOutput << '\n';
        }
    }

    stream << "\nConfigured minimum\n"
           << "  AVX: " << BoolText(requirements.avx) << '\n'
           << "  AVX2: " << BoolText(requirements.avx2) << '\n'
           << "  FMA: " << BoolText(requirements.fma) << '\n'
           << "  F16C: " << BoolText(requirements.f16c) << '\n'
           << "  Reject Debug CRT: " << BoolText(requirements.rejectDebugRuntime) << '\n'
           << "  Physical memory: "
           << static_cast<double>(requirements.minimumMemoryBytes) / BytesPerGiB << " GiB\n";

    if (!evaluation.failures.empty())
    {
        stream << "\nFailures\n";
        for (const auto& failure : evaluation.failures)
        {
            stream << "  - " << failure << '\n';
        }
    }
    if (!evaluation.warnings.empty())
    {
        stream << "\nWarnings\n";
        for (const auto& warning : evaluation.warnings)
        {
            stream << "  - " << warning << '\n';
        }
    }
    return stream.str();
}

std::string BuildJsonReport(
    const EnvironmentSnapshot& snapshot,
    const Requirements& requirements,
    const Evaluation& evaluation)
{
    std::ostringstream stream;
    stream << "{\n"
           << "  \"schemaVersion\": 5,\n"
           << "  \"toolVersion\": \"" << ToolVersion << "\",\n"
           << "  \"result\": \"" << ToString(evaluation.status) << "\",\n"
           << "  \"issueCode\": \"" << EscapeJson(evaluation.issueCode) << "\",\n"
           << "  \"exitCode\": " << evaluation.exitCode << ",\n"
           << "  \"system\": {\n"
           << "    \"windowsVersion\": \"" << EscapeJson(snapshot.system.windowsVersion) << "\",\n"
           << "    \"is64BitOs\": " << BoolText(snapshot.system.is64BitOs) << ",\n"
           << "    \"is64BitProcess\": " << BoolText(snapshot.system.is64BitProcess) << ",\n"
           << "    \"physicalMemoryBytes\": " << snapshot.system.physicalMemoryBytes << ",\n"
           << "    \"availablePhysicalMemoryBytes\": " << snapshot.system.availablePhysicalMemoryBytes << ",\n"
           << "    \"totalPageFileBytes\": " << snapshot.system.totalPageFileBytes << ",\n"
           << "    \"availablePageFileBytes\": " << snapshot.system.availablePageFileBytes << ",\n"
           << "    \"uptimeMilliseconds\": " << snapshot.system.uptimeMilliseconds << "\n"
           << "  },\n"
           << "  \"cpu\": {\n"
           << "    \"vendor\": \"" << EscapeJson(snapshot.cpu.vendor) << "\",\n"
           << "    \"name\": \"" << EscapeJson(snapshot.cpu.brand) << "\",\n"
           << "    \"family\": " << snapshot.cpu.family << ",\n"
           << "    \"model\": " << snapshot.cpu.model << ",\n"
           << "    \"stepping\": " << snapshot.cpu.stepping << ",\n"
           << "    \"logicalProcessors\": " << snapshot.cpu.logicalProcessorCount << ",\n"
           << "    \"sse41\": " << BoolText(snapshot.cpu.sse41) << ",\n"
           << "    \"sse42\": " << BoolText(snapshot.cpu.sse42) << ",\n"
           << "    \"avxHardware\": " << BoolText(snapshot.cpu.avxHardware) << ",\n"
           << "    \"osxsave\": " << BoolText(snapshot.cpu.osxsave) << ",\n"
           << "    \"avxOsEnabled\": " << BoolText(snapshot.cpu.avxOsEnabled) << ",\n"
           << "    \"avx\": " << BoolText(snapshot.cpu.avx) << ",\n"
           << "    \"avx2\": " << BoolText(snapshot.cpu.avx2) << ",\n"
           << "    \"fma\": " << BoolText(snapshot.cpu.fma) << ",\n"
           << "    \"f16c\": " << BoolText(snapshot.cpu.f16c) << ",\n"
           << "    \"avx512Hardware\": " << BoolText(snapshot.cpu.avx512Hardware) << ",\n"
           << "    \"avx512OsEnabled\": " << BoolText(snapshot.cpu.avx512OsEnabled) << ",\n"
           << "    \"avx512\": " << BoolText(snapshot.cpu.avx512) << "\n"
           << "  },\n";

    stream << "  \"gpuAdapters\": [\n";
    for (std::size_t index = 0; index < snapshot.gpuAdapters.size(); ++index)
    {
        const auto& gpu = snapshot.gpuAdapters[index];
        stream << "    {\n"
               << "      \"name\": \"" << EscapeJson(gpu.name) << "\",\n"
               << "      \"vendorId\": " << gpu.vendorId << ",\n"
               << "      \"vendorIdHex\": \"" << HexId(gpu.vendorId, 4) << "\",\n"
               << "      \"deviceId\": " << gpu.deviceId << ",\n"
               << "      \"deviceIdHex\": \"" << HexId(gpu.deviceId, 4) << "\",\n"
               << "      \"subsystemId\": " << gpu.subsystemId << ",\n"
               << "      \"revision\": " << gpu.revision << ",\n"
               << "      \"driverProvider\": \"" << EscapeJson(gpu.driverProvider) << "\",\n"
               << "      \"driverVersion\": \"" << EscapeJson(gpu.driverVersion) << "\",\n"
               << "      \"driverDate\": \"" << EscapeJson(gpu.driverDate) << "\",\n"
               << "      \"dedicatedVideoMemoryBytes\": " << gpu.dedicatedVideoMemoryBytes << ",\n"
               << "      \"dedicatedSystemMemoryBytes\": " << gpu.dedicatedSystemMemoryBytes << ",\n"
               << "      \"sharedSystemMemoryBytes\": " << gpu.sharedSystemMemoryBytes << ",\n"
               << "      \"outputCount\": " << gpu.outputCount << ",\n"
               << "      \"attachedOutputCount\": " << gpu.attachedOutputCount << ",\n"
               << "      \"softwareAdapter\": " << BoolText(gpu.softwareAdapter) << "\n"
               << "    }" << (index + 1 == snapshot.gpuAdapters.size() ? "" : ",") << '\n';
    }
    stream << "  ],\n";

    // Same serializer the collector's gpu-selection stage uses, so a collected
    // ZIP and this report can never disagree about what the probe found.
    stream << "  \"gpuAutoSelection\": {\n"
           << BuildGpuSelectionJson(snapshot.gpuSelectionMockResults, snapshot.gpuSelectionRealProbe)
           << "\n  },\n";

    stream << "  \"displays\": [\n";
    for (std::size_t index = 0; index < snapshot.displays.size(); ++index)
    {
        const auto& display = snapshot.displays[index];
        stream << "    {\"deviceName\": \"" << EscapeJson(display.deviceName)
               << "\", \"monitorName\": \"" << EscapeJson(display.monitorName)
               << "\", \"width\": " << display.width
               << ", \"height\": " << display.height
               << ", \"refreshRate\": " << display.refreshRate
               << ", \"bitsPerPixel\": " << display.bitsPerPixel
               << ", \"primary\": " << BoolText(display.primary)
               << ", \"attachedToDesktop\": " << BoolText(display.attachedToDesktop)
               << '}' << (index + 1 == snapshot.displays.size() ? "" : ",") << '\n';
    }
    stream << "  ],\n";

    stream << "  \"physicalDisks\": [\n";
    for (std::size_t index = 0; index < snapshot.physicalDisks.size(); ++index)
    {
        const auto& disk = snapshot.physicalDisks[index];
        stream << "    {\"devicePath\": \"" << EscapeJson(disk.devicePath)
               << "\", \"vendor\": \"" << EscapeJson(disk.vendor)
               << "\", \"model\": \"" << EscapeJson(disk.model)
               << "\", \"busType\": \"" << EscapeJson(disk.busType)
               << "\", \"capacityBytes\": " << disk.capacityBytes
               << ", \"removable\": " << BoolText(disk.removable)
               << '}' << (index + 1 == snapshot.physicalDisks.size() ? "" : ",") << '\n';
    }
    stream << "  ],\n";

    stream << "  \"volumes\": [\n";
    for (std::size_t index = 0; index < snapshot.volumes.size(); ++index)
    {
        const auto& volume = snapshot.volumes[index];
        stream << "    {\"rootPath\": \"" << EscapeJson(volume.rootPath)
               << "\", \"volumeLabel\": \"" << EscapeJson(volume.volumeLabel)
               << "\", \"fileSystem\": \"" << EscapeJson(volume.fileSystem)
               << "\", \"driveType\": \"" << EscapeJson(volume.driveType)
               << "\", \"totalBytes\": " << volume.totalBytes
               << ", \"freeBytes\": " << volume.freeBytes
               << ", \"availableBytes\": " << volume.availableBytes
               << '}' << (index + 1 == snapshot.volumes.size() ? "" : ",") << '\n';
    }
    stream << "  ],\n";

    stream << "  \"inspectedImages\": [\n";
    for (std::size_t index = 0; index < snapshot.inspectedImages.size(); ++index)
    {
        const auto& image = snapshot.inspectedImages[index];
        stream << "    {\n"
               << "      \"path\": \"" << EscapeJson(image.path) << "\",\n"
               << "      \"fileExists\": " << BoolText(image.fileExists) << ",\n"
               << "      \"validPeImage\": " << BoolText(image.validPeImage) << ",\n"
               << "      \"is64Bit\": " << BoolText(image.is64Bit) << ",\n"
               << "      \"hasDebugDirectory\": " << BoolText(image.hasDebugDirectory) << ",\n"
               << "      \"usesDebugRuntime\": " << BoolText(image.usesDebugRuntime) << ",\n"
               << "      \"timeDateStamp\": " << image.timeDateStamp << ",\n"
               << "      \"timeDateStampHex\": \"" << HexId(image.timeDateStamp, 8) << "\",\n"
               << "      \"importedLibraries\": [";
        for (std::size_t libraryIndex = 0; libraryIndex < image.importedLibraries.size(); ++libraryIndex)
        {
            if (libraryIndex != 0)
            {
                stream << ", ";
            }
            stream << '"' << EscapeJson(image.importedLibraries[libraryIndex]) << '"';
        }
        stream << "],\n      \"debugRuntimeLibraries\": [";
        for (std::size_t libraryIndex = 0; libraryIndex < image.debugRuntimeLibraries.size(); ++libraryIndex)
        {
            if (libraryIndex != 0)
            {
                stream << ", ";
            }
            stream << '"' << EscapeJson(image.debugRuntimeLibraries[libraryIndex]) << '"';
        }
        const auto& instructionSets = image.instructionSets;
        stream << "],\n"
               << "      \"instructionSets\": {\n"
               << "        \"scanAttempted\": " << BoolText(instructionSets.scanAttempted) << ",\n"
               << "        \"scanCompleted\": " << BoolText(instructionSets.scanCompleted) << ",\n"
               << "        \"scanStrategy\": \"" << EscapeJson(instructionSets.scanStrategy) << "\",\n"
               << "        \"executableSectionCount\": " << instructionSets.executableSectionCount << ",\n"
               << "        \"executableBytes\": " << instructionSets.executableBytes << ",\n"
               << "        \"runtimeFunctionCount\": " << instructionSets.runtimeFunctionCount << ",\n"
               << "        \"scannedCodeBytes\": " << instructionSets.scannedCodeBytes << ",\n"
               << "        \"decodedInstructionCount\": " << instructionSets.decodedInstructionCount << ",\n"
               << "        \"undecodableByteCount\": " << instructionSets.undecodableByteCount << ",\n"
               << "        \"avxInstructionCount\": " << instructionSets.avxInstructionCount << ",\n"
               << "        \"avx2InstructionCount\": " << instructionSets.avx2InstructionCount << ",\n"
               << "        \"fmaInstructionCount\": " << instructionSets.fmaInstructionCount << ",\n"
               << "        \"f16cInstructionCount\": " << instructionSets.f16cInstructionCount << ",\n"
               << "        \"avx512InstructionCount\": " << instructionSets.avx512InstructionCount << ",\n"
               << "        \"containsAvx\": " << BoolText(instructionSets.containsAvx) << ",\n"
               << "        \"containsAvx2\": " << BoolText(instructionSets.containsAvx2) << ",\n"
               << "        \"containsFma\": " << BoolText(instructionSets.containsFma) << ",\n"
               << "        \"containsF16c\": " << BoolText(instructionSets.containsF16c) << ",\n"
               << "        \"containsAvx512\": " << BoolText(instructionSets.containsAvx512) << ",\n"
               << "        \"staticPresenceDoesNotProveExecution\": true,\n"
               << "        \"runtimeDispatchSafety\": \"UNKNOWN\",\n"
               << "        \"unsupportedOnCurrentCpuIsFailure\": false,\n"
               << "        \"samples\": [\n";
        for (std::size_t sampleIndex = 0; sampleIndex < instructionSets.samples.size(); ++sampleIndex)
        {
            const auto& sample = instructionSets.samples[sampleIndex];
            stream << "          {\"category\": \"" << EscapeJson(sample.category)
                   << "\", \"isaSet\": \"" << EscapeJson(sample.isaSet)
                   << "\", \"rva\": " << sample.rva
                   << ", \"rvaHex\": \"" << HexId(sample.rva, 8)
                   << "\", \"fileOffset\": " << sample.fileOffset
                   << ", \"fileOffsetHex\": \"" << HexId(sample.fileOffset, 8)
                   << "\", \"bytes\": \"" << EscapeJson(sample.bytes)
                   << "\", \"text\": \"" << EscapeJson(sample.text) << "\"}"
                   << (sampleIndex + 1 == instructionSets.samples.size() ? "" : ",") << '\n';
        }
        stream << "        ],\n"
               << "        \"error\": \"" << EscapeJson(instructionSets.error) << "\"\n"
               << "      },\n"
               << "      \"error\": \"" << EscapeJson(image.error) << "\"\n"
               << "    }" << (index + 1 == snapshot.inspectedImages.size() ? "" : ",") << '\n';
    }
    stream << "  ],\n";

    stream << "  \"dllProbeResults\": [\n";
    for (std::size_t index = 0; index < snapshot.dllProbeResults.size(); ++index)
    {
        const auto& probe = snapshot.dllProbeResults[index];
        stream << "    {\n"
               << "      \"dllPath\": \"" << EscapeJson(probe.dllPath) << "\",\n"
               << "      \"probeExecutable\": \"" << EscapeJson(probe.probeExecutable) << "\",\n"
               << "      \"runnerExecutable\": \"" << EscapeJson(probe.runnerExecutable) << "\",\n"
               << "      \"runnerArguments\": [";
        for (std::size_t argumentIndex = 0; argumentIndex < probe.runnerArguments.size(); ++argumentIndex)
        {
            if (argumentIndex != 0)
            {
                stream << ", ";
            }
            stream << '"' << EscapeJson(probe.runnerArguments[argumentIndex]) << '"';
        }
        stream << "],\n"
               << "      \"exportName\": \"" << EscapeJson(probe.exportName) << "\",\n"
               << "      \"attempted\": " << BoolText(probe.attempted) << ",\n"
               << "      \"processStarted\": " << BoolText(probe.processStarted) << ",\n"
               << "      \"timedOut\": " << BoolText(probe.timedOut) << ",\n"
               << "      \"resultAvailable\": " << BoolText(probe.resultAvailable) << ",\n"
               << "      \"loadSucceeded\": " << BoolText(probe.loadSucceeded) << ",\n"
               << "      \"exportRequested\": " << BoolText(probe.exportRequested) << ",\n"
               << "      \"exportFound\": " << BoolText(probe.exportFound) << ",\n"
               << "      \"exportCalled\": " << BoolText(probe.exportCalled) << ",\n"
               << "      \"exportResult\": " << probe.exportResult << ",\n"
               << "      \"processExitCode\": " << probe.processExitCode << ",\n"
               << "      \"processExitCodeHex\": \"" << HexId(probe.processExitCode, 8) << "\",\n"
               << "      \"win32Error\": " << probe.win32Error << ",\n"
               << "      \"exceptionCode\": " << probe.exceptionCode << ",\n"
               << "      \"exceptionCodeHex\": \"" << HexId(probe.exceptionCode, 8) << "\",\n"
               << "      \"durationMilliseconds\": " << probe.durationMilliseconds << ",\n"
               << "      \"runnerReportedIsaViolation\": " << BoolText(probe.runnerReportedIsaViolation) << ",\n"
               << "      \"runnerReportedInternalError\": " << BoolText(probe.runnerReportedInternalError) << ",\n"
               << "      \"runnerOutput\": \"" << EscapeJson(probe.runnerOutput) << "\",\n"
               << "      \"error\": \"" << EscapeJson(probe.error) << "\"\n"
               << "    }" << (index + 1 == snapshot.dllProbeResults.size() ? "" : ",") << '\n';
    }
    stream << "  ],\n";

    stream << "  \"requirements\": {\n"
           << "    \"avx\": " << BoolText(requirements.avx) << ",\n"
           << "    \"avx2\": " << BoolText(requirements.avx2) << ",\n"
           << "    \"fma\": " << BoolText(requirements.fma) << ",\n"
           << "    \"f16c\": " << BoolText(requirements.f16c) << ",\n"
           << "    \"rejectDebugRuntime\": " << BoolText(requirements.rejectDebugRuntime) << ",\n"
           << "    \"minimumMemoryBytes\": " << requirements.minimumMemoryBytes << "\n"
           << "  },\n";
    WriteJsonStringArray(stream, "failures", evaluation.failures, true);
    WriteJsonStringArray(stream, "warnings", evaluation.warnings, false);
    stream << "}\n";
    return stream.str();
}

namespace
{
// The name the report is built under before it is published. The process id is
// part of it because two runs can publish the same report at the same time --
// a launcher that starts one check per launch does exactly that -- and sharing
// one temporary would let the slower run rename away the faster one's content.
std::filesystem::path TemporaryReportPath(const std::filesystem::path& path)
{
    std::filesystem::path temporary(path);
    temporary += "." + std::to_string(::GetCurrentProcessId()) + ".tmp";
    return temporary;
}

// Errors that mean "someone still has the old copy open" rather than "this path
// can never be written". The replace below needs DELETE access to the file it
// overwrites, and an ifstream opened by MSVC's CRT shares read and write but
// not delete -- so a launcher reading the previous report at the moment we
// publish denies the rename. Waiting a moment is the whole fix.
bool IsTransientPublishError(DWORD error)
{
    switch (error)
    {
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
    case ERROR_USER_MAPPED_FILE:
        return true;
    default:
        return false;
    }
}
} // namespace

void WriteReportFile(const std::filesystem::path& path, const std::string& content)
{
    const std::filesystem::path temporary = TemporaryReportPath(path);
    // Whatever fails below, the temporary must not survive next to the report:
    // a stale one is exactly what makes a support engineer wonder which of the
    // two files is authoritative.
    struct TemporaryCleanup
    {
        std::filesystem::path path;
        bool published = false;
        ~TemporaryCleanup()
        {
            if (published) return;
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    } cleanup{temporary};

    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output)
        {
            throw std::runtime_error("Failed to open report file: " + temporary.string());
        }
        output.write(content.data(), static_cast<std::streamsize>(content.size()));
        // Close explicitly and check it: close() is where a buffered write
        // failure (disk full, removed drive) is reported, and publishing a file
        // whose bytes never landed would be worse than not publishing at all.
        output.close();
        if (!output)
        {
            throw std::runtime_error("Failed to write report file: " + temporary.string());
        }
    }

    // Publish by rename. The destination directory entry flips from the old
    // report to the new one in a single operation, so a reader that opens the
    // path either gets all of the old file or all of the new one -- never the
    // empty or half-written document the previous truncate-in-place write left
    // behind for the launcher's polling loop.
    //
    // REPLACE_EXISTING preserves the old behaviour: a report is rewritten on
    // every run, including the first one into an output directory a previous
    // run already used. (The ZIP deliberately does the opposite -- see
    // ReportArchive.cpp -- because overwriting a captured evidence package is
    // never right; a regenerated report is.)
    // One second in total. That is not a round number picked for luck: a
    // launcher polling every few hundred milliseconds holds the previous report
    // open for the few milliseconds it takes to read it, and 200 ms was enough
    // for the retry window to expire inside one of those holds -- the publish
    // then failed and the launcher kept showing yesterday's report.
    constexpr int PublishAttempts = 100;
    constexpr DWORD PublishRetryDelayMilliseconds = 10;
    for (int attempt = 0; attempt < PublishAttempts; ++attempt)
    {
        if (::MoveFileExW(temporary.c_str(), path.c_str(),
                          MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE)
        {
            cleanup.published = true;
            return;
        }
        const DWORD error = ::GetLastError();
        if (!IsTransientPublishError(error) || attempt + 1 == PublishAttempts)
        {
            throw std::runtime_error("Failed to publish report file " + path.string()
                                     + ": " + std::to_string(error));
        }
        ::Sleep(PublishRetryDelayMilliseconds);
    }
}
} // namespace zengine::envcheck
