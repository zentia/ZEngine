// ============================================================================
// FDpiScaleRenderer — ISlateRenderer decorator that applies a DPI scale
// ============================================================================
// Why a decorator instead of scaling the widgets themselves:
//
// Every ZSlate widget hard-codes its own metrics (STextBlock::FontSize {14.0f},
// SSearchBox::FontSize {14.0f}, STooltip::PaddingX {8.0f}, ...) across 30+
// headers, and composite widgets build inner children with their own defaults.
// Touching each field would be a large, lossy edit *and* would still miss the
// widgets we don't construct ourselves.
//
// Instead the widget tree keeps working in DPI-independent *logical* units, so
// those 14.0f defaults stay meaningful, and this decorator converts to physical
// pixels on the way out. Give the root a geometry of (physical / Scale) and
// every rect/radius/thickness/position gets multiplied by Scale here.
//
// The part that actually fixes blurry text: font_size is scaled *before* it
// reaches the font atlas, and the atlas bakes per integer pixel size and caches
// on (codepoint, rounded_size) -- see ZSlateFontAtlas::GetGlyph(). So at 150%
// a 14px label is genuinely rasterized at 21px, rather than a 14px bitmap
// stretched 1.5x. That is the difference between "sharp" and "resampled".
//
// MeasureText must round-trip: measure at the scaled size (so it matches what
// gets drawn) and divide the result back down, because the caller is a layout
// pass still thinking in logical units. Returning physical extents here would
// inflate desired sizes by Scale and cascade wrong layout everywhere.
// ============================================================================
#pragma once

#include "ZSlate/Core/SlatePaint.h"

#include <string>
#include <vector>

namespace LuaMemPreview
{

class FDpiScaleRenderer final : public ZSlate::ISlateRenderer
{
public:
    FDpiScaleRenderer(ZSlate::ISlateRenderer* inner, float scale)
        : m_Inner(inner), m_Scale(scale > 0.0f ? scale : 1.0f)
    {
    }

    void SetScale(float scale) { m_Scale = (scale > 0.0f ? scale : 1.0f); }
    float GetScale() const { return m_Scale; }
    void SetInner(ZSlate::ISlateRenderer* inner) { m_Inner = inner; }

    // ---- Primitives ------------------------------------------------------
    void DrawQuad(const ZSlate::UIRect& rect, const ZSlate::UIColor& color) override
    {
        m_Inner->DrawQuad(S(rect), color);
    }

    void DrawRect(const ZSlate::UIRect& rect, const ZSlate::UIColor& color, float thickness = 1.0f) override
    {
        // Keep hairlines at >=1 physical pixel; a scaled-down border that lands
        // below 1px would drop out entirely on some rows.
        const float t = thickness * m_Scale;
        m_Inner->DrawRect(S(rect), color, t < 1.0f ? 1.0f : t);
    }

    void DrawConvexPoly(const ZSlate::Vector2* points, int count, const ZSlate::UIColor& color) override
    {
        if (points == nullptr || count <= 0)
        {
            m_Inner->DrawConvexPoly(points, count, color);
            return;
        }
        m_Scratch.clear();
        m_Scratch.reserve(static_cast<size_t>(count));
        for (int i = 0; i < count; ++i)
            m_Scratch.push_back(ZSlate::Vector2(points[i].x * m_Scale, points[i].y * m_Scale));
        m_Inner->DrawConvexPoly(m_Scratch.data(), count, color);
    }

    void DrawRoundedRect(const ZSlate::UIRect& rect, float radius, const ZSlate::UIColor& color) override
    {
        m_Inner->DrawRoundedRect(S(rect), radius * m_Scale, color);
    }

    void DrawTexturedQuad(const ZSlate::UIRect& rect, void* texture_handle,
                          const ZSlate::UIColor& tint = ZSlate::Colors::White) override
    {
        m_Inner->DrawTexturedQuad(S(rect), texture_handle, tint);
    }

    void DrawBox(const ZSlate::UIRect& rect, const ZSlate::FMargin& margin, void* texture_handle,
                 const ZSlate::UIColor& tint) override
    {
        m_Inner->DrawBox(S(rect), S(margin), texture_handle, tint);
    }

    void DrawBorder(const ZSlate::UIRect& rect, const ZSlate::FMargin& margin, void* texture_handle,
                    const ZSlate::UIColor& tint) override
    {
        m_Inner->DrawBorder(S(rect), S(margin), texture_handle, tint);
    }

    // ---- Text ------------------------------------------------------------
    void DrawTextLabel(const ZSlate::UIRect& rect, const std::string& text, float font_size,
                       const ZSlate::UIColor& color,
                       ZSlate::TextAnchor alignment = ZSlate::TextAnchor::MiddleLeft,
                       ZSlate::TextWrapMode wrap = ZSlate::TextWrapMode::NoWrap,
                       void* font_handle = nullptr) override
    {
        m_Inner->DrawTextLabel(S(rect), text, font_size * m_Scale, color, alignment, wrap, font_handle);
    }

    void DrawTextLabel(const std::string& text, const ZSlate::Vector2& pos, float font_size,
                       const ZSlate::UIColor& color) override
    {
        m_Inner->DrawTextLabel(text, ZSlate::Vector2(pos.x * m_Scale, pos.y * m_Scale),
                               font_size * m_Scale, color);
    }

    ZSlate::Vector2 MeasureText(const std::string& text, float font_size) const override
    {
        // Measure at the size we will actually rasterize, then convert back to
        // logical units for the layout pass that asked.
        const ZSlate::Vector2 physical = m_Inner->MeasureText(text, font_size * m_Scale);
        const float inv = 1.0f / m_Scale;
        return ZSlate::Vector2(physical.x * inv, physical.y * inv);
    }

    // ---- Clipping / state ------------------------------------------------
    void PushClipRect(const ZSlate::UIRect& rect) override { m_Inner->PushClipRect(S(rect)); }
    void PopClipRect() override { m_Inner->PopClipRect(); }
    void BeginFrame() override { m_Inner->BeginFrame(); }
    void EndFrame() override { m_Inner->EndFrame(); }
    void Flush() override { m_Inner->Flush(); }

private:
    ZSlate::UIRect S(const ZSlate::UIRect& r) const
    {
        return ZSlate::UIRect(r.x * m_Scale, r.y * m_Scale, r.w * m_Scale, r.h * m_Scale);
    }
    ZSlate::FMargin S(const ZSlate::FMargin& m) const
    {
        return ZSlate::FMargin(m.Left * m_Scale, m.Top * m_Scale, m.Right * m_Scale, m.Bottom * m_Scale);
    }

    ZSlate::ISlateRenderer* m_Inner {nullptr};
    float m_Scale {1.0f};
    mutable std::vector<ZSlate::Vector2> m_Scratch;
};

}  // namespace LuaMemPreview
