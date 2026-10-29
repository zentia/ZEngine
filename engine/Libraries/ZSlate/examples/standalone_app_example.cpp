// ============================================================================
// ZSlate Standalone Example — FSlateStandaloneApp (Win32 + D3D11 host)
// ============================================================================
// This is the file to copy when starting a new ZSlate tool.
//
// It deliberately does NOT show a hand-rolled WNDCLASS / message pump / DPI
// handler / font load. Five tools used to each carry a copy of that, and the
// copies drifted (two had no DPI support at all, four hardcoded
// "C:\Windows\Fonts"). FSlateStandaloneApp is the single implementation; host
// your UI in it and only write what your tool is actually about.
//
// See ZSlate/App/SlateStandaloneApp.h for the coordinate contract (Win32 and
// the swapchain are physical, the widget tree is logical) and for the full list
// of hooks (OnFrame / OnChar / OnKey / OnDropFile / OnResize / OnDpiChanged /
// OnClose / OnWheel / OnPostPaint).

#include "ZSlate/App/SlateStandaloneApp.h"
#include "ZSlate/Application/SlateApplication.h"
#include "ZSlate/Application/SlateInput.h"
#include "ZSlate/Core/SlateGeometry.h"
#include "ZSlate/Core/SlatePaint.h"
#include "ZSlate/Widgets/Input/SButton.h"
#include "ZSlate/Widgets/Input/SCheckBox.h"
#include "ZSlate/Widgets/Input/SEditableTextBox.h"
#include "ZSlate/Widgets/Input/SSlider.h"
#include "ZSlate/Widgets/Layout/SBoxPanel.h"
#include "ZSlate/Widgets/Layout/SSpacer.h"
#include "ZSlate/Widgets/Panels/SBorder.h"
#include "ZSlate/Widgets/Text/STextBlock.h"
#include "ZSlate/Widgets/SWidget.h"

#include <cstdio>
#include <memory>
#include <string>

// ============================================================================
// Only the widget tree is application-specific. Every size below is in logical
// (96-DPI) units; the host scales it to physical pixels on paint.
// ============================================================================
std::shared_ptr<ZSlate::SWidget> BuildDemoUI()
{
    using namespace ZSlate;

    auto root = std::make_shared<SVerticalBox>();

    auto title = std::make_shared<STextBlock>();
    title->Text = "ZSlate Standalone Demo";
    title->FontSize = 24;
    title->Color = Colors::White;
    root->AddSlot(title);
    root->AddSlot(std::make_shared<SSpacer>(Vector2(0, 12)));

    auto sub = std::make_shared<STextBlock>();
    sub->Text = "Host: FSlateStandaloneApp (DPI + fonts handled for you)";
    sub->FontSize = 14;
    sub->Color = UIColor(0.6f, 0.7f, 0.9f, 1.0f);
    root->AddSlot(sub);
    root->AddSlot(std::make_shared<SSpacer>(Vector2(0, 16)));

    auto btn = std::make_shared<SButton>();
    auto lbl = std::make_shared<STextBlock>();
    lbl->Text = "Click Me";
    lbl->Color = Colors::White;
    btn->SetContent(lbl);
    btn->OnClicked = [] { printf("[ZSlate] Click!\n"); };
    root->AddSlot(btn);
    root->AddSlot(std::make_shared<SSpacer>(Vector2(0, 8)));

    auto cb = std::make_shared<SCheckBox>();
    cb->SetLabel("Enable feature");
    cb->Checked = true;
    root->AddSlot(cb);
    root->AddSlot(std::make_shared<SSpacer>(Vector2(0, 8)));

    auto box = std::make_shared<SEditableTextBox>();
    box->HintText = "type here (clipboard works too)";
    root->AddSlot(box);
    root->AddSlot(std::make_shared<SSpacer>(Vector2(0, 8)));

    auto sl = std::make_shared<SSlider>();
    sl->Value = 50;
    root->AddSlot(sl);

    return root;
}

// ============================================================================
#ifdef _WIN32

int main()
{
    printf("=== ZSlate Standalone (Win32 + D3D11) ===\n");

    ZSlate::FStandaloneAppDesc desc;
    desc.Title  = "ZSlate Standalone Demo";
    desc.Width  = 800.0f;   // logical units
    desc.Height = 600.0f;
    desc.Font   = ZSlate::Win32::ESystemFont::Sans;

    ZSlate::FSlateStandaloneApp app;
    // Declares per-monitor DPI awareness, loads the system font + CJK fallback,
    // creates the window, initialises D3D11 and sizes everything for the
    // monitor scale.
    if (!app.Initialize(desc))
        return 1;

    app.SetRootContent(BuildDemoUI());

    return app.Run();  // message pump + per-frame paint + present
}

#else
int main()
{
    printf("This example uses Win32 + D3D11.\n");
    return 0;
}
#endif
