#pragma once

// ----------------------------------------------------------------------------
// D2DUIRenderer -- a Direct2D / DirectWrite implementation of the engine's
// abstract UIRenderer, used ONLY by the standalone ZInsights.exe viewer.
//
// The in-editor Insights panel paints the very same SInsightsTimeline widget
// through the engine's RHI-backed BatchedUIRenderer. The standalone viewer has
// no RHI / swapchain / GLFW stack, so it provides this tiny Direct2D backend
// instead. Because both go through the UIRenderer interface, the flame-chart
// widget code is shared verbatim.
//
// Windows-only by design (Direct2D). The whole ZInsights target is gated on
// WIN32 in CMake.
// ----------------------------------------------------------------------------

#include "Runtime/UI/Render/UIRenderer.h"

#include <map>

struct ID2D1HwndRenderTarget;
struct ID2D1SolidColorBrush;
struct IDWriteFactory;
struct IDWriteTextFormat;

namespace ZInsights
{
class D2DUIRenderer : public UIRenderer
{
public:
    D2DUIRenderer();
    ~D2DUIRenderer() override;

    // Bind the live render target. Called after the target is (re)created on
    // resize / device loss. The solid brush is recreated lazily against it.
    void SetRenderTarget(ID2D1HwndRenderTarget* target);

    // --- UIRenderer ---------------------------------------------------------
    void PushClipRect(const UIRect& clip_rect, bool intersect_with_current = true) override;
    void PopClipRect() override;

    void DrawQuad(const UIRect& rect, const ZSlate::UIColor& color) override;
    void DrawRect(const UIRect& rect, const ZSlate::UIColor& color, float thickness = 1.0f) override;
    void DrawTexturedQuad(const UIRect& rect, void* texture_id, const ZSlate::UIColor& color = ZSlate::UIColor(1, 1, 1, 1),
                          const Vector2& uv0 = Vector2(0.0f, 0.0f),
                          const Vector2& uv1 = Vector2(1.0f, 1.0f)) override;
    void DrawText(const UIRect& rect, const std::string& text, float font_size, const ZSlate::UIColor& color,
                  TextAnchor alignment = TextAnchor::MiddleCenter, TextWrapMode wrap = TextWrapMode::Wrap,
                  Font* font = nullptr) override;
    Vector2 MeasureText(const std::string& text, float font_size, TextWrapMode wrap = TextWrapMode::Wrap,
                        float wrap_width = 0.0f, Font* font = nullptr) const override;
    Vector2 getDisplaySize() const override;

private:
    void EnsureBrush();
    IDWriteTextFormat* GetTextFormat(float font_size);

    ID2D1HwndRenderTarget* m_Target {nullptr};  // not owned (owned by the app)
    ID2D1SolidColorBrush* m_Brush {nullptr};
    IDWriteFactory* m_DWrite {nullptr};
    std::map<int, IDWriteTextFormat*> m_Formats;  // keyed by rounded font px
    int m_ClipDepth {0};
};
}  // namespace ZInsights
