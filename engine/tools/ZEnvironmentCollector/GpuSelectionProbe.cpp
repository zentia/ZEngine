#include "GpuSelectionProbe.h"

#include "JsonEscape.h"

#include <Windows.h>
#include <dxgi.h>
#include <dxgi1_6.h>

#include <cstdio>
#include <sstream>

namespace zengine::envcheck
{
namespace
{
// Minimal RAII helper mirroring the one already used in HardwareProbe.cpp, kept
// local to this translation unit so this module has no dependency beyond
// GpuSelectionProbe.h + system headers.
template <typename T>
class ComPtr
{
public:
    ~ComPtr()
    {
        Reset();
    }

    T** Put()
    {
        Reset();
        return &pointer_;
    }

    T* Get() const
    {
        return pointer_;
    }

    T* operator->() const
    {
        return pointer_;
    }

    void Reset()
    {
        if (pointer_ != nullptr)
        {
            pointer_->Release();
            pointer_ = nullptr;
        }
    }

private:
    T* pointer_ = nullptr;
};

std::string WideToUtf8(const wchar_t* value)
{
    if (value == nullptr)
    {
        return {};
    }
    const int byteCount = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (byteCount <= 1)
    {
        return {};
    }
    std::string result(static_cast<std::size_t>(byteCount - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), byteCount, nullptr, nullptr);
    return result;
}
} // namespace

// ---------------------------------------------------------------------------
// Decision logic: kept byte-for-byte equivalent to the fallback branch added to
// Runtime/GfxDevice/GfxDeviceSetup.cpp. Any change to the production heuristic
// should be mirrored here so this tool keeps testing what actually ships.
// ---------------------------------------------------------------------------
GpuFallbackSelectionResult SelectBestAdapterFallback(std::vector<GpuSelectionAdapter>& adapters)
{
    int bestAdapterIndex = 0;
    std::uint64_t bestDedicatedVideoMemory = 0;
    int bestNonIntegratedAdapterIndex = -1;
    std::uint64_t bestNonIntegratedDedicatedVideoMemory = 0;

    for (std::size_t i = 0; i < adapters.size(); ++i)
    {
        GpuSelectionAdapter& adapter = adapters[i];

        const bool isSoftwareAdapter = (adapter.vendorId == 0x1414 && adapter.deviceId == 0x8c);
        const bool looksIntegrated = (adapter.vendorId == 0x8086) ||
            (adapter.dedicatedVideoMemoryBytes > 0 && adapter.sharedSystemMemoryBytes > adapter.dedicatedVideoMemoryBytes);
        adapter.isSoftwareAdapter = isSoftwareAdapter;
        adapter.looksIntegrated = looksIntegrated;

        if (!isSoftwareAdapter && adapter.dedicatedVideoMemoryBytes > bestDedicatedVideoMemory)
        {
            bestAdapterIndex = static_cast<int>(i);
            bestDedicatedVideoMemory = adapter.dedicatedVideoMemoryBytes;
        }

        if (!isSoftwareAdapter && !looksIntegrated && adapter.dedicatedVideoMemoryBytes > bestNonIntegratedDedicatedVideoMemory)
        {
            bestNonIntegratedAdapterIndex = static_cast<int>(i);
            bestNonIntegratedDedicatedVideoMemory = adapter.dedicatedVideoMemoryBytes;
        }
    }

    GpuFallbackSelectionResult result;
    result.excludedIntegrated = bestNonIntegratedAdapterIndex >= 0;
    result.chosenAdapterIndex = result.excludedIntegrated ? bestNonIntegratedAdapterIndex : bestAdapterIndex;
    return result;
}

// ---------------------------------------------------------------------------
// Mock regression cases.
// ---------------------------------------------------------------------------
namespace
{
GpuSelectionAdapter MakeAdapter(const char* name, std::uint32_t vendorId, std::uint32_t deviceId,
    std::uint64_t dedicatedMb, std::uint64_t sharedMb)
{
    GpuSelectionAdapter adapter;
    adapter.name = name;
    adapter.vendorId = vendorId;
    adapter.deviceId = deviceId;
    adapter.dedicatedVideoMemoryBytes = dedicatedMb * 1024ull * 1024ull;
    adapter.sharedSystemMemoryBytes = sharedMb * 1024ull * 1024ull;
    return adapter;
}
} // namespace

std::vector<GpuSelectionMockCase> BuildGpuSelectionMockCases()
{
    std::vector<GpuSelectionMockCase> cases;

    {
        GpuSelectionMockCase mockCase;
        mockCase.name = "Intel iGPU over-report vs real dGPU (exact bug scenario)";
        mockCase.adapters.push_back(MakeAdapter("Intel(R) Iris(R) Xe Graphics", 0x8086, 0x9a49, 4200, 8192));
        mockCase.adapters.push_back(MakeAdapter("NVIDIA GeForce RTX 4060 Laptop", 0x10de, 0x28a0, 4096, 256));
        mockCase.expectedAdapterIndex = 1;
        cases.push_back(mockCase);
    }
    {
        GpuSelectionMockCase mockCase;
        mockCase.name = "Extreme case: iGPU reports 8GB > real dGPU 6GB";
        mockCase.adapters.push_back(MakeAdapter("AMD Radeon Graphics (iGPU)", 0x1002, 0x1636, 8192, 16384));
        mockCase.adapters.push_back(MakeAdapter("NVIDIA GeForce RTX 4070 Laptop", 0x10de, 0x2820, 6144, 256));
        mockCase.expectedAdapterIndex = 1;
        cases.push_back(mockCase);
    }
    {
        GpuSelectionMockCase mockCase;
        mockCase.name = "Single dGPU only (regression check)";
        mockCase.adapters.push_back(MakeAdapter("NVIDIA GeForce RTX 5080", 0x10de, 0x2c02, 16384, 256));
        mockCase.expectedAdapterIndex = 0;
        cases.push_back(mockCase);
    }
    {
        GpuSelectionMockCase mockCase;
        mockCase.name = "Integrated-only machine (all adapters look integrated)";
        mockCase.adapters.push_back(MakeAdapter("Intel(R) UHD Graphics 630", 0x8086, 0x3e92, 128, 8192));
        mockCase.expectedAdapterIndex = 0;
        cases.push_back(mockCase);
    }
    {
        // The software-adapter branch had no case at all. It matters because the
        // Basic Render Driver can sit in the enumeration next to a real card, and
        // picking it is the one mistake here that costs the player the whole GPU.
        // It is given more dedicated memory than the real card on purpose: shared
        // stays 0 so it does not also look integrated, which leaves the software
        // flag as the only thing that can exclude it. A software adapter with no
        // memory would be excluded by the memory compare anyway and would pass
        // even with the branch deleted.
        GpuSelectionMockCase mockCase;
        mockCase.name = "Software adapter never wins, even reporting more memory than the real card";
        mockCase.adapters.push_back(MakeAdapter("Microsoft Basic Render Driver", 0x1414, 0x8c, 8192, 0));
        mockCase.adapters.push_back(MakeAdapter("NVIDIA GeForce RTX 4060 Laptop", 0x10de, 0x28a0, 4096, 256));
        mockCase.expectedAdapterIndex = 1;
        cases.push_back(mockCase);
    }
    {
        // Nothing else to pick, so the mirror falls back to index 0 rather than
        // reporting a failure. Pinned because "software only" has to stay an
        // answer the caller can act on, not an error the caller has to invent.
        GpuSelectionMockCase mockCase;
        mockCase.name = "Software renderer only (index 0, not a failure)";
        mockCase.adapters.push_back(MakeAdapter("Microsoft Basic Render Driver", 0x1414, 0x8c, 0, 0));
        mockCase.expectedAdapterIndex = 0;
        cases.push_back(mockCase);
    }
    {
        GpuSelectionMockCase mockCase;
        // Two identical cards: the compare is strictly greater, so the first one
        // wins and stays won. Ties must not be resolved by enumeration order
        // changing under a driver update.
        mockCase.name = "Two identical dGPUs: strictly-greater compare keeps the first";
        mockCase.adapters.push_back(MakeAdapter("NVIDIA GeForce RTX 5080 (slot 1)", 0x10de, 0x2c02, 16384, 256));
        mockCase.adapters.push_back(MakeAdapter("NVIDIA GeForce RTX 5080 (slot 2)", 0x10de, 0x2c02, 16384, 256));
        mockCase.expectedAdapterIndex = 0;
        cases.push_back(mockCase);
    }
    {
        GpuSelectionMockCase mockCase;
        // The software test is a conjunction. Vendor 0x1414 on its own is not the
        // Basic Render Driver, and widening it to the vendor alone would drop a real
        // card out of the running on any machine that reports one.
        mockCase.name = "Vendor 0x1414 with another device id is not the software adapter";
        mockCase.adapters.push_back(MakeAdapter("Microsoft adapter, not the Basic Render Driver", 0x1414, 0x8d, 8192, 0));
        mockCase.adapters.push_back(MakeAdapter("NVIDIA GeForce RTX 3050", 0x10de, 0x25a2, 4096, 256));
        mockCase.expectedAdapterIndex = 0;
        cases.push_back(mockCase);
    }
    {
        GpuSelectionMockCase mockCase;
        // The vendor rule is what it is: every 0x8086 part counts as integrated, so
        // a discrete Intel card loses to a smaller card from another vendor. Pinned
        // on purpose -- if that rule is ever narrowed to real integrated parts, it
        // should break this case and be changed deliberately rather than drift.
        mockCase.name = "Intel Arc discrete is classified integrated by the vendor rule (current behavior)";
        mockCase.adapters.push_back(MakeAdapter("Intel(R) Arc(TM) A770 Graphics", 0x8086, 0x56a0, 16384, 256));
        mockCase.adapters.push_back(MakeAdapter("NVIDIA GeForce RTX 3060", 0x10de, 0x2504, 12288, 256));
        mockCase.expectedAdapterIndex = 1;
        cases.push_back(mockCase);
    }

    return cases;
}

GpuSelectionMockCaseResult RunGpuSelectionMockCase(const GpuSelectionMockCase& mockCase)
{
    GpuSelectionMockCaseResult result;
    result.caseName = mockCase.name;
    result.adapters = mockCase.adapters;
    result.expectedAdapterIndex = mockCase.expectedAdapterIndex;

    const GpuFallbackSelectionResult selection = SelectBestAdapterFallback(result.adapters);
    result.chosenAdapterIndex = selection.chosenAdapterIndex;
    result.excludedIntegrated = selection.excludedIntegrated;
    result.passed = (result.chosenAdapterIndex == result.expectedAdapterIndex);
    return result;
}

std::vector<GpuSelectionMockCaseResult> RunAllGpuSelectionMockCases()
{
    std::vector<GpuSelectionMockCaseResult> results;
    for (const GpuSelectionMockCase& mockCase : BuildGpuSelectionMockCases())
    {
        results.push_back(RunGpuSelectionMockCase(mockCase));
    }
    return results;
}

// ---------------------------------------------------------------------------
// Real hardware probe: identical call sequence to the production fix in
// GfxDeviceSetup.cpp (IDXGIFactory6::EnumAdapterByGpuPreference first, no
// fallback execution here since the fallback logic is already covered by the
// mock cases above).
// ---------------------------------------------------------------------------
GpuSelectionRealProbeResult ProbeRealGpuSelection()
{
    GpuSelectionRealProbeResult result;

    ComPtr<IDXGIFactory1> factory1;
    const HRESULT createResult = CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(factory1.Put()));
    if (FAILED(createResult) || factory1.Get() == nullptr)
    {
        result.errorMessage = "CreateDXGIFactory1 failed";
        return result;
    }
    result.dxgiFactoryCreated = true;

    for (UINT adapterIndex = 0;; ++adapterIndex)
    {
        ComPtr<IDXGIAdapter> adapter;
        const HRESULT enumResult = factory1->EnumAdapters(adapterIndex, adapter.Put());
        if (enumResult == DXGI_ERROR_NOT_FOUND)
        {
            break;
        }
        if (FAILED(enumResult) || adapter.Get() == nullptr)
        {
            continue;
        }

        DXGI_ADAPTER_DESC description{};
        if (FAILED(adapter->GetDesc(&description)))
        {
            continue;
        }

        GpuSelectionAdapter info;
        info.name = WideToUtf8(description.Description);
        info.vendorId = description.VendorId;
        info.deviceId = description.DeviceId;
        info.dedicatedVideoMemoryBytes = description.DedicatedVideoMemory;
        info.sharedSystemMemoryBytes = description.SharedSystemMemory;
        info.isSoftwareAdapter = (description.VendorId == 0x1414 && description.DeviceId == 0x8c);
        result.allAdapters.push_back(info);
    }

    ComPtr<IDXGIFactory6> factory6;
    const HRESULT queryResult = factory1->QueryInterface(__uuidof(IDXGIFactory6), reinterpret_cast<void**>(factory6.Put()));
    if (FAILED(queryResult) || factory6.Get() == nullptr)
    {
        result.errorMessage = "IDXGIFactory6 unavailable on this OS; production code would use the fallback heuristic";
        return result;
    }
    result.factory6Available = true;

    ComPtr<IDXGIAdapter1> preferred;
    const HRESULT preferenceResult = factory6->EnumAdapterByGpuPreference(
        0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, __uuidof(IDXGIAdapter1), reinterpret_cast<void**>(preferred.Put()));
    if (FAILED(preferenceResult) || preferred.Get() == nullptr)
    {
        result.errorMessage = "EnumAdapterByGpuPreference failed";
        return result;
    }

    DXGI_ADAPTER_DESC1 preferredDescription{};
    if (FAILED(preferred->GetDesc1(&preferredDescription)))
    {
        result.errorMessage = "GetDesc1 failed on preferred adapter";
        return result;
    }

    result.preferredAdapter.name = WideToUtf8(preferredDescription.Description);
    result.preferredAdapter.vendorId = preferredDescription.VendorId;
    result.preferredAdapter.deviceId = preferredDescription.DeviceId;
    result.preferredAdapter.dedicatedVideoMemoryBytes = preferredDescription.DedicatedVideoMemory;
    result.preferredAdapter.sharedSystemMemoryBytes = preferredDescription.SharedSystemMemory;
    result.preferredAdapter.isSoftwareAdapter = (preferredDescription.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
    result.gpuPreferencePicked = true;
    return result;
}
std::string FormatGpuMegabytes(std::uint64_t bytes)
{
    char text[48]{};
    std::snprintf(text, sizeof(text), "%llu MB", static_cast<unsigned long long>(bytes / (1024ull * 1024ull)));
    return std::string(text);
}
std::string FormatGpuAdapterLine(std::size_t index, const GpuSelectionAdapter& adapter, bool showIntegratedFlag)
{
    char prefix[64]{};
    std::snprintf(prefix, sizeof(prefix), "  [%zu] ", index);
    char ids[64]{};
    std::snprintf(ids, sizeof(ids), "  vendor=0x%08X", adapter.vendorId);
    std::string line = prefix;
    line += adapter.name;
    line += ids;
    line += "  dedicated=" + FormatGpuMegabytes(adapter.dedicatedVideoMemoryBytes);
    line += "  shared=" + FormatGpuMegabytes(adapter.sharedSystemMemoryBytes);
    if (adapter.isSoftwareAdapter)
    {
        line += "  [software]";
    }
    if (showIntegratedFlag && adapter.looksIntegrated)
    {
        line += "  [looks integrated, filtered out]";
    }
    return line;
}
std::string FormatGpuSelectionReport(
    const std::vector<GpuSelectionMockCaseResult>& mockResults,
    const GpuSelectionRealProbeResult& realResult)
{
    std::string report = "GPU auto-select verification (offline, no engine link)\n";
    report += "Mirrors GfxDeviceSetup.cpp adapter-selection logic byte-for-byte\n\n";
    report += "== Mock regression cases ==\n";
    for (const GpuSelectionMockCaseResult& mockResult : mockResults)
    {
        report += mockResult.passed ? "[PASS] " : "[FAIL] ";
        report += mockResult.caseName;
        report += '\n';
        for (std::size_t i = 0; i < mockResult.adapters.size(); ++i)
        {
            report += FormatGpuAdapterLine(i, mockResult.adapters[i], true);
            report += '\n';
        }
        report += "  chosen=" + std::to_string(mockResult.chosenAdapterIndex)
            + "  expected=" + std::to_string(mockResult.expectedAdapterIndex)
            + "  excludedIntegrated=" + (mockResult.excludedIntegrated ? "true" : "false") + "\n";
    }
    report += AllGpuSelectionMockCasesPassed(mockResults) ? "Mock regression: ALL PASS\n\n"
                                                          : "Mock regression: HAS FAILURE\n\n";

    report += "== Real hardware probe (this machine) ==\n";
    report += "All adapters (EnumAdapters1):\n";
    for (std::size_t i = 0; i < realResult.allAdapters.size(); ++i)
    {
        report += FormatGpuAdapterLine(i, realResult.allAdapters[i], false);
        report += '\n';
    }
    if (realResult.gpuPreferencePicked)
    {
        report += "IDXGIFactory6::EnumAdapterByGpuPreference(HIGH_PERFORMANCE) picked: "
            + realResult.preferredAdapter.name
            + "  dedicated=" + FormatGpuMegabytes(realResult.preferredAdapter.dedicatedVideoMemoryBytes) + "\n";
    }
    else
    {
        report += "EnumAdapterByGpuPreference unavailable/failed: "
            + (realResult.errorMessage.empty() ? std::string("unknown reason") : realResult.errorMessage) + "\n";
    }
    return report;
}
std::string BuildGpuSelectionJson(
    const std::vector<GpuSelectionMockCaseResult>& mockResults,
    const GpuSelectionRealProbeResult& realResult)
{
    std::ostringstream stream;
    stream << "    \"allMockCasesPassed\": " << (AllGpuSelectionMockCasesPassed(mockResults) ? "true" : "false") << ",\n";
    stream << "    \"mockCases\": [\n";
    for (std::size_t index = 0; index < mockResults.size(); ++index)
    {
        const GpuSelectionMockCaseResult& mockResult = mockResults[index];
        stream << "      {\n"
               << "        \"caseName\": \"" << EscapeJson(mockResult.caseName) << "\",\n"
               << "        \"passed\": " << (mockResult.passed ? "true" : "false") << ",\n"
               << "        \"chosenAdapterIndex\": " << mockResult.chosenAdapterIndex << ",\n"
               << "        \"expectedAdapterIndex\": " << mockResult.expectedAdapterIndex << ",\n"
               << "        \"excludedIntegrated\": " << (mockResult.excludedIntegrated ? "true" : "false") << ",\n"
               << "        \"adapters\": [";
        for (std::size_t adapterIndex = 0; adapterIndex < mockResult.adapters.size(); ++adapterIndex)
        {
            if (adapterIndex != 0)
            {
                stream << ", ";
            }
            const GpuSelectionAdapter& adapter = mockResult.adapters[adapterIndex];
            stream << "{\"name\": \"" << EscapeJson(adapter.name)
                   << "\", \"vendorId\": " << adapter.vendorId
                   << ", \"deviceId\": " << adapter.deviceId
                   << ", \"dedicatedVideoMemoryBytes\": " << adapter.dedicatedVideoMemoryBytes
                   << ", \"sharedSystemMemoryBytes\": " << adapter.sharedSystemMemoryBytes
                   << ", \"looksIntegrated\": " << (adapter.looksIntegrated ? "true" : "false")
                   << "}";
        }
        stream << "]\n"
               << "      }" << (index + 1 == mockResults.size() ? "" : ",") << '\n';
    }
    stream << "    ],\n";
    stream << "    \"realProbe\": {\n"
           << "      \"dxgiFactoryCreated\": " << (realResult.dxgiFactoryCreated ? "true" : "false") << ",\n"
           << "      \"factory6Available\": " << (realResult.factory6Available ? "true" : "false") << ",\n"
           << "      \"gpuPreferencePicked\": " << (realResult.gpuPreferencePicked ? "true" : "false") << ",\n"
           << "      \"preferredAdapter\": {";
    if (realResult.gpuPreferencePicked)
    {
        const GpuSelectionAdapter& preferred = realResult.preferredAdapter;
        stream << "\"name\": \"" << EscapeJson(preferred.name)
               << "\", \"vendorId\": " << preferred.vendorId
               << ", \"deviceId\": " << preferred.deviceId
               << ", \"dedicatedVideoMemoryBytes\": " << preferred.dedicatedVideoMemoryBytes;
    }
    stream << "},\n"
           << "      \"errorMessage\": \"" << EscapeJson(realResult.errorMessage) << "\"\n"
           << "    }";
    return stream.str();
}
bool AllGpuSelectionMockCasesPassed(const std::vector<GpuSelectionMockCaseResult>& mockResults)
{
    for (const GpuSelectionMockCaseResult& mockResult : mockResults)
    {
        if (!mockResult.passed)
        {
            return false;
        }
    }
    return !mockResults.empty();
}
} // namespace zengine::envcheck
