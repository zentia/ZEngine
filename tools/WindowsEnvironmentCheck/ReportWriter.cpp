#include "ReportWriter.h"

#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace zengine::envcheck
{
namespace
{
constexpr double BytesPerGiB = 1024.0 * 1024.0 * 1024.0;

const char* BoolText(bool value)
{
    return value ? "true" : "false";
}

std::string EscapeJson(const std::string& value)
{
    std::ostringstream stream;
    for (const unsigned char character : value)
    {
        switch (character)
        {
        case '\\': stream << "\\\\"; break;
        case '"': stream << "\\\""; break;
        case '\b': stream << "\\b"; break;
        case '\f': stream << "\\f"; break;
        case '\n': stream << "\\n"; break;
        case '\r': stream << "\\r"; break;
        case '\t': stream << "\\t"; break;
        default:
            if (character < 0x20)
            {
                stream << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                       << static_cast<int>(character) << std::dec;
            }
            else
            {
                stream << static_cast<char>(character);
            }
            break;
        }
    }
    return stream.str();
}

std::string HexId(std::uint32_t value, int width)
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
           << "  F16C usable: " << BoolText(snapshot.cpu.f16c) << "\n";

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
               << ", software adapter: " << BoolText(gpu.softwareAdapter) << '\n';
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
        if (!image.error.empty())
        {
            stream << "      Error: " << image.error << '\n';
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
           << "  \"schemaVersion\": 3,\n"
           << "  \"toolVersion\": \"1.2.0\",\n"
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
           << "    \"f16c\": " << BoolText(snapshot.cpu.f16c) << "\n"
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
               << "      \"softwareAdapter\": " << BoolText(gpu.softwareAdapter) << "\n"
               << "    }" << (index + 1 == snapshot.gpuAdapters.size() ? "" : ",") << '\n';
    }
    stream << "  ],\n";

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
        stream << "],\n      \"error\": \"" << EscapeJson(image.error) << "\"\n"
               << "    }" << (index + 1 == snapshot.inspectedImages.size() ? "" : ",") << '\n';
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

void WriteReportFile(const std::filesystem::path& path, const std::string& content)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        throw std::runtime_error("Failed to open report file: " + path.string());
    }
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!output)
    {
        throw std::runtime_error("Failed to write report file: " + path.string());
    }
}
} // namespace zengine::envcheck
