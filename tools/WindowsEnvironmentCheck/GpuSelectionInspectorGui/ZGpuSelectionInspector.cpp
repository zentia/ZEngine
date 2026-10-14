// ============================================================================
// ZGpuSelectionInspector — standalone ZSlate GUI for the dual-GPU adapter
// selection heuristic verification (Win32 + D3D11, mirrors the pattern used by
// ZSlate/examples/standalone_app_example.cpp).
//
// This tool does not touch or link against the game/engine build. It is a
// thin, self-contained visual front-end over GpuSelectionProbe.h/.cpp, whose
// selection logic is kept byte-for-byte identical to the fallback branch added
// to Runtime/GfxDevice/GfxDeviceSetup.cpp. See:
//   G:\OSGame\doc\双周_20260912-20260925\第1周_20260912-20260918\
//     20260915_PC双显卡自动选卡选中集显导致启动白屏_分析\分析.md
//
// Shows two sections:
//   1. Mock regression cases — synthetic dual-GPU scenarios (including the
//      exact bug scenario), run offline against the decision logic.
//   2. Real hardware probe — what this machine's DXGI adapters actually are,
//      and what IDXGIFactory6::EnumAdapterByGpuPreference picks here.
// A "Re-run probe" button re-executes both sections (useful for revalidating
// after a GPU/driver change, e.g. toggling the iGPU in BIOS then rebooting).
// ============================================================================

#include "GpuSelectionProbe.h"

#include "ZSlate/App/SlateStandaloneApp.h"
#include "ZSlate/Application/SlateApplication.h"
#include "ZSlate/Application/SlateInput.h"
#include "ZSlate/Core/SlatePaint.h"
#include "ZSlate/Core/SlateGeometry.h"
#include "ZSlate/Widgets/Layout/SBoxPanel.h"
#include "ZSlate/Widgets/Input/SButton.h"
#include "ZSlate/Widgets/Layout/SSpacer.h"
#include "ZSlate/Widgets/Text/STextBlock.h"
#include "ZSlate/Widgets/SWidget.h"

#include <chrono>
#include <cstdio>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#undef DrawText // windows.h #defines DrawText -> DrawTextA/W

using namespace zengine::envcheck;

namespace
{
// ---------------------------------------------------------------------------
// Formatting helpers
// ---------------------------------------------------------------------------
std::string FormatMegabytes(std::uint64_t bytes)
{
    std::ostringstream stream;
    stream << (bytes / (1024ull * 1024ull)) << " MB";
    return stream.str();
}

std::string FormatAdapterLine(std::size_t index, const GpuSelectionAdapter& adapter, bool showIntegratedFlag)
{
    std::ostringstream stream;
    stream << "  [" << index << "] " << adapter.name
        << "  vendor=0x" << std::hex << adapter.vendorId << std::dec
        << "  dedicated=" << FormatMegabytes(adapter.dedicatedVideoMemoryBytes)
        << "  shared=" << FormatMegabytes(adapter.sharedSystemMemoryBytes);
    if (adapter.isSoftwareAdapter)
    {
        stream << "  [software]";
    }
    if (showIntegratedFlag && adapter.looksIntegrated)
    {
        stream << "  [looks integrated, filtered out]";
    }
    return stream.str();
}

// ---------------------------------------------------------------------------
// The inspector application: owns the probe results and rebuilds the widget
// tree whenever the user asks for a re-run.
//
// The Win32 host (window, message pump, DPI awareness, font atlas) is
// ZSlate::FSlateStandaloneApp. This tool previously had no DPI support at all,
// so it was DPI-virtualised: rendered at 96 DPI and bitmap-stretched to the
// monitor scale.
// ---------------------------------------------------------------------------
class InspectorApp
{
public:
    ZSlate::FSlateStandaloneApp Host;

    void RunProbesAndBuildUi()
    {
        const std::vector<GpuSelectionMockCaseResult> mockResults = RunAllGpuSelectionMockCases();
        const GpuSelectionRealProbeResult realResult = ProbeRealGpuSelection();

        auto root = std::make_shared<ZSlate::SVerticalBox>();

        auto title = std::make_shared<ZSlate::STextBlock>();
        title->Text = "GPU Auto-Select Verification (offline, no engine link)";
        title->FontSize = 20;
        title->Color = ZSlate::Colors::White;
        root->AddSlot(title);
        root->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(0, 4)));

        auto subtitle = std::make_shared<ZSlate::STextBlock>();
        subtitle->Text = "Mirrors GfxDeviceSetup.cpp adapter-selection logic byte-for-byte";
        subtitle->FontSize = 13;
        subtitle->Color = ZSlate::UIColor(0.6f, 0.7f, 0.9f, 1.0f);
        root->AddSlot(subtitle);
        root->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(0, 12)));

        // ---- Section 1: mock regression cases ----
        bool allMockPass = true;
        for (const GpuSelectionMockCaseResult& mockResult : mockResults)
        {
            if (!mockResult.passed)
            {
                allMockPass = false;
            }

            auto caseHeader = std::make_shared<ZSlate::STextBlock>();
            std::ostringstream headerStream;
            headerStream << (mockResult.passed ? "[PASS] " : "[FAIL] ") << mockResult.caseName;
            caseHeader->Text = headerStream.str();
            caseHeader->FontSize = 14;
            caseHeader->Color = mockResult.passed ? ZSlate::UIColor(0.4f, 0.85f, 0.4f, 1.0f)
                                                   : ZSlate::UIColor(0.9f, 0.35f, 0.35f, 1.0f);
            root->AddSlot(caseHeader);

            for (std::size_t i = 0; i < mockResult.adapters.size(); ++i)
            {
                auto line = std::make_shared<ZSlate::STextBlock>();
                line->Text = FormatAdapterLine(i, mockResult.adapters[i], true);
                line->FontSize = 12;
                line->Color = ZSlate::UIColor(0.85f, 0.85f, 0.85f, 1.0f);
                root->AddSlot(line);
            }

            auto resultLine = std::make_shared<ZSlate::STextBlock>();
            std::ostringstream resultStream;
            resultStream << "  chosen=" << mockResult.chosenAdapterIndex
                << "  expected=" << mockResult.expectedAdapterIndex
                << "  excludedIntegrated=" << (mockResult.excludedIntegrated ? "true" : "false");
            resultLine->Text = resultStream.str();
            resultLine->FontSize = 12;
            resultLine->Color = ZSlate::UIColor(0.6f, 0.6f, 0.6f, 1.0f);
            root->AddSlot(resultLine);
            root->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(0, 8)));
        }

        auto mockSummary = std::make_shared<ZSlate::STextBlock>();
        mockSummary->Text = allMockPass ? "Mock regression: ALL PASS" : "Mock regression: HAS FAILURE";
        mockSummary->FontSize = 15;
        mockSummary->Color = allMockPass ? ZSlate::UIColor(0.4f, 0.85f, 0.4f, 1.0f) : ZSlate::UIColor(0.9f, 0.35f, 0.35f, 1.0f);
        root->AddSlot(mockSummary);
        root->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(0, 16)));

        // ---- Section 2: real hardware probe ----
        auto realTitle = std::make_shared<ZSlate::STextBlock>();
        realTitle->Text = "Real hardware probe (this machine)";
        realTitle->FontSize = 16;
        realTitle->Color = ZSlate::Colors::White;
        root->AddSlot(realTitle);
        root->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(0, 4)));

        auto adaptersHeader = std::make_shared<ZSlate::STextBlock>();
        adaptersHeader->Text = "All adapters (EnumAdapters1):";
        adaptersHeader->FontSize = 13;
        adaptersHeader->Color = ZSlate::UIColor(0.8f, 0.8f, 0.8f, 1.0f);
        root->AddSlot(adaptersHeader);

        for (std::size_t i = 0; i < realResult.allAdapters.size(); ++i)
        {
            auto line = std::make_shared<ZSlate::STextBlock>();
            line->Text = FormatAdapterLine(i, realResult.allAdapters[i], false);
            line->FontSize = 12;
            line->Color = ZSlate::UIColor(0.85f, 0.85f, 0.85f, 1.0f);
            root->AddSlot(line);
        }
        root->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(0, 8)));

        auto preferredLine = std::make_shared<ZSlate::STextBlock>();
        std::ostringstream preferredStream;
        if (realResult.gpuPreferencePicked)
        {
            preferredStream << "IDXGIFactory6::EnumAdapterByGpuPreference(HIGH_PERFORMANCE) picked: "
                << realResult.preferredAdapter.name
                << "  dedicated=" << FormatMegabytes(realResult.preferredAdapter.dedicatedVideoMemoryBytes);
            preferredLine->Color = ZSlate::UIColor(0.4f, 0.85f, 0.4f, 1.0f);
        }
        else
        {
            preferredStream << "EnumAdapterByGpuPreference unavailable/failed: " << realResult.errorMessage;
            preferredLine->Color = ZSlate::UIColor(0.9f, 0.7f, 0.3f, 1.0f);
        }
        preferredLine->Text = preferredStream.str();
        preferredLine->FontSize = 13;
        root->AddSlot(preferredLine);
        root->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(0, 16)));

        // ---- Re-run button ----
        auto rerunButton = std::make_shared<ZSlate::SButton>();
        auto rerunLabel = std::make_shared<ZSlate::STextBlock>();
        rerunLabel->Text = "Re-run probe";
        rerunLabel->Color = ZSlate::Colors::White;
        rerunButton->SetContent(rerunLabel);
        rerunButton->OnClicked = [this]
        {
            printf("[ZGpuSelectionInspector] Re-run requested\n");
            RunProbesAndBuildUi();
        };
        root->AddSlot(rerunButton);

        // NOTE: deliberately NOT wrapping root in SBorder/SScrollBox here.
        // SWidget::CacheDesiredSize() recurses via the virtual
        // GetChildCount()/GetChildAt() pair, but neither SCompoundWidget (the
        // SBorder/SButton base) nor SBoxPanel (the SVerticalBox/SHorizontalBox
        // base) override those two functions -- they only override
        // ComputeDesiredSize()/ArrangeChildren(). That means wrapping this
        // content in an SBorder/SScrollBox breaks desired-size caching for
        // every descendant (all STextBlock children end up with a cached
        // (0,0) size, so SBoxPanel::ArrangeChildren never advances its cursor
        // and every line paints stacked on top of the others). This is a real
        // gap in the ZSlate widget base classes, not something fixable from
        // this call site; examples/standalone_app_example.cpp avoids it only
        // because its root IS the SVerticalBox with leaf-widget slots (no
        // extra container layer to recurse through). Setting root directly as
        // SetRootContent mirrors that known-good structure.
        Host.SetRootContent(root);
    }
};
} // namespace

int main()
{
    printf("=== ZGpuSelectionInspector (ZSlate GUI) ===\n");

    InspectorApp app;

    ZSlate::FStandaloneAppDesc desc;
    desc.Title  = "GPU Auto-Select Verification Tool";
    desc.Width  = 980.0f;
    desc.Height = 720.0f;
    desc.Font   = ZSlate::Win32::ESystemFont::Mono;

    if (!app.Host.Initialize(desc))
    {
        printf("ZSlate host init failed\n");
        return 1;
    }

    app.RunProbesAndBuildUi();
    return app.Host.Run();
}
