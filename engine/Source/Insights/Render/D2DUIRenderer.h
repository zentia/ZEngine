#pragma once

// ----------------------------------------------------------------------------
// D2DUIRenderer -- a Direct2D / DirectWrite implementation of ZSlate's abstract
// ISlateRenderer, used ONLY by the standalone ZInsights.exe viewer.
//
// The in-editor Insights panel paints the very same SInsightsTimeline widget
// through the engine's RHI-backed ZSlate renderer. The standalone viewer has
// no RHI / swapchain / GLFW stack, so it provides this tiny Direct2D backend
// instead. Because both go through ZSlate::ISlateRenderer, the flame-chart
// widget code is shared verbatim.
//
// Windows-only by design (Direct2D). The whole ZInsights target is gated on
// WIN32 in CMake.
// ----------------------------------------------------------------------------

#include "ZSlate/Core/SlatePaint.h"

#include <map>
#include <string>

struct ID2D1HwndRenderTarget;
struct ID2D1SolidColorBrush;
struct IDWriteFactory;
struct IDWriteTextFormat;

namespace ZInsights
{
class D2DUIRenderer : public ZSlate::ISlateRenderer
{
public:
    D2DUIRenderer();
    ~D2DUIRenderer() override;

    // Bind the live render target. Called after the target is (re)created on
    // resize / device loss. The solid brush is recreated lazily against it.
    void SetRenderTarget(ID2D1HwndRenderTarget* target);

    // --- ZSlate::ISlateRenderer ---------------------------------------------
    void DrawQuad(const ZSlate::UIRect& rect, const ZSlate::UIColor& color) override;
    void DrawRect(const ZSlate::UIRect& rect, const ZSlate::UIColor& color, float thickness = 1.0f) override;
    void DrawConvexPoly(const ZSlate::Vector2* points, int count, const ZSlate::UIColor& color) override;
    void DrawRoundedRect(const ZSlate::UIRect& rect, float radius, const ZSlate::UIColor& color) override;
    void DrawTexturedQuad(const ZSlate::UIRect& rect, void* texture_handle,
                          const ZSlate::UIColor& tint = ZSlate::Colors::White) override;
    void DrawBox(const ZSlate::UIRect& rect, const ZSlate::FMargin& margin, void* texture_handle,
                 const ZSlate::UIColor& tint) override;
    void DrawBorder(const ZSlate::UIRect& rect, const ZSlate::FMargin& margin, void* texture_handle,
                    const ZSlate::UIColor& tint) override;

    void DrawTextLabel(const ZSlate::UIRect& rect, const std::string& text, float font_size,
                       const ZSlate::UIColor& color, ZSlate::TextAnchor alignment = ZSlate::TextAnchor::MiddleLeft,
                       ZSlate::TextWrapMode wrap = ZSlate::TextWrapMode::NoWrap,
                       void* font_handle = nullptr) override;
    void DrawTextLabel(const std::string& text, const ZSlate::Vector2& pos, float font_size,
                       const ZSlate::UIColor& color) override;
    ZSlate::Vector2 MeasureText(const std::string& text, float font_size) const override;

    void PushClipRect(const ZSlate::UIRect& rect) override;
    void PopClipRect() override;

    // The host owns BeginDraw / EndDraw on the D2D target, so frame hooks are no-ops.
    void BeginFrame() override {}
    void EndFrame() override {}
    void Flush() override {}

private:
    void EnsureBrush();
    IDWriteTextFormat* GetTextFormat(float font_size) const;

    ID2D1HwndRenderTarget* m_Target {nullptr};  // not owned (owned by the app)
    ID2D1SolidColorBrush* m_Brush {nullptr};
    IDWriteFactory* m_DWrite {nullptr};
    mutable std::map<int, IDWriteTextFormat*> m_Formats;  // keyed by rounded font px
    int m_ClipDepth {0};
};
}  // namespace ZInsights
