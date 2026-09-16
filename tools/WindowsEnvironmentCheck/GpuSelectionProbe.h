#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace zengine::envcheck
{
// One physical DXGI adapter, as seen either via EnumAdapters1() or via a
// synthetic mock case used to unit-test the selection heuristic offline.
struct GpuSelectionAdapter
{
    std::string name;
    std::uint32_t vendorId = 0;
    std::uint32_t deviceId = 0;
    std::uint64_t dedicatedVideoMemoryBytes = 0;
    std::uint64_t sharedSystemMemoryBytes = 0;
    bool isSoftwareAdapter = false;

    // Diagnostics filled in by SelectBestAdapterFallback(), not by the enumerator.
    bool looksIntegrated = false;
};

// Result of running the fallback heuristic (DedicatedVideoMemory compare with an
// integrated-GPU filter) against a list of adapters. Mirrors the logic in
// Runtime/GfxDevice/GfxDeviceSetup.cpp's automatic-GPU-selection fallback branch
// byte-for-byte, so this is the same decision the game would make on a system
// without IDXGIFactory6.
struct GpuFallbackSelectionResult
{
    int chosenAdapterIndex = 0;
    bool excludedIntegrated = false;
};

// Runs the exact fallback selection heuristic used by GfxDeviceSetup.cpp when
// IDXGIFactory6::EnumAdapterByGpuPreference is unavailable. Mutates each
// adapter's looksIntegrated field as a side effect (for display purposes).
GpuFallbackSelectionResult SelectBestAdapterFallback(std::vector<GpuSelectionAdapter>& adapters);

// A single named regression case for the offline mock test: a fixed list of
// synthetic adapters plus the adapter index the heuristic is expected to pick.
struct GpuSelectionMockCase
{
    std::string name;
    std::vector<GpuSelectionAdapter> adapters;
    int expectedAdapterIndex = 0;
};

// The same four regression cases exercised by the standalone command-line
// verification tool used to validate the GfxDeviceSetup.cpp fix (see
// G:\OSGame\doc\...\20260915_PC双显卡自动选卡选中集显导致启动白屏_分析\分析.md):
//   1. Exact bug scenario: Intel iGPU over-reports VRAM above a real dGPU.
//   2. Extreme over-report: iGPU reports 8GB vs a real 6GB dGPU.
//   3. Single dGPU only (regression check, no iGPU present).
//   4. Integrated-only machine (must still pick something, not fail open).
std::vector<GpuSelectionMockCase> BuildGpuSelectionMockCases();

// Result of running one mock case: whether the heuristic picked the expected
// adapter, plus the fully annotated adapter list for display in a report or UI.
struct GpuSelectionMockCaseResult
{
    std::string caseName;
    std::vector<GpuSelectionAdapter> adapters; // annotated with looksIntegrated
    int chosenAdapterIndex = 0;
    int expectedAdapterIndex = 0;
    bool excludedIntegrated = false;
    bool passed = false;
};

GpuSelectionMockCaseResult RunGpuSelectionMockCase(const GpuSelectionMockCase& mockCase);
std::vector<GpuSelectionMockCaseResult> RunAllGpuSelectionMockCases();

// Outcome of probing real hardware on the current machine via DXGI.
struct GpuSelectionRealProbeResult
{
    bool dxgiFactoryCreated = false;
    bool factory6Available = false;         // IDXGIFactory6 obtained via QueryInterface
    bool gpuPreferencePicked = false;       // EnumAdapterByGpuPreference succeeded
    std::vector<GpuSelectionAdapter> allAdapters;    // via plain EnumAdapters1
    GpuSelectionAdapter preferredAdapter;   // populated when gpuPreferencePicked is true
    std::string errorMessage;               // human-readable, empty on full success
};

// Enumerates real DXGI adapters on this machine and, if IDXGIFactory6 is
// available, calls EnumAdapterByGpuPreference(DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE)
// exactly like the production code path in GfxDeviceSetup.cpp does. Safe to call
// on any Windows machine, including single-GPU systems.
GpuSelectionRealProbeResult ProbeRealGpuSelection();
} // namespace zengine::envcheck
