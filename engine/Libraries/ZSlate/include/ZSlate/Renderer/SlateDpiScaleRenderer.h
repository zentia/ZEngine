// ============================================================================
// FSlateDpiScaleRenderer — ISlateRenderer decorator that applies a DPI scale
// ============================================================================
// Usage: wrap the real backend renderer, size the root widget geometry in
// logical units (physical_size / scale), paint through the decorator, and
// divide incoming mouse positions by the same scale:
//
//     FSlateDpiScaleRenderer scaled(&backend, dpi_scale);
//     root->Paint(FPaintContext {&scaled}, FGeometry({0,0}, physical / dpi_scale));
//
// Hosts that already scale on their own (the editor's EditorSlateHost via
// m_UiScale) must NOT stack this on top, or the content is scaled twice.
//
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
//
// Pixel snapping (opt-in, SetPixelSnap(true)):
//
// At fractional scales (125%, 150%) logical integers land between physical
// pixels (22 * 1.25 = 27.5). Without MSAA a solid quad then covers 0 or 1
// pixel per edge depending on the pixel-centre rule, so neighbouring boxes
// pick up 1px gaps / overlaps and a 1.25px border renders 1px on one side and
// 2px on the other. Snapping rounds each *edge* (x, x+w) independently rather
// than the size, so two rects that share an edge in logical space still share
// it after snapping -- no seams. Non-empty rects never collapse below 1px, and
// border thickness is rounded to whole pixels (min 1).
//
// Off by default: an unsnapped decorator is an exact similarity transform,
// which is what LuaMemPreview's --dump-ui check asserts. Text placement is
// unaffected either way -- ZSlateBatchedRenderer already rounds the text rect
// and every glyph origin, and the atlas bakes at the rounded pixel size.
// ============================================================================
#pragma once

#include "ZSlate/Core/SlatePaint.h"

#include <cmath>
#include <string>
#include <vector>

namespace ZSlate
{

class FSlateDpiScaleRenderer final : public ISlateRenderer
{
public:
    FSlateDpiScaleRenderer(ISlateRenderer* inner, float scale, bool pixel_snap = false)
        : m_Inner(inner), m_Scale(scale > 0.0f ? scale : 1.0f), m_PixelSnap(pixel_snap)
    {
    }

    void SetScale(float scale) { m_Scale = (scale > 0.0f ? scale : 1.0f); }
    float GetScale() const { return m_Scale; }
    void SetInner(ISlateRenderer* inner) { m_Inner = inner; }
    ISlateRenderer* GetInner() const { return m_Inner; }
    void SetPixelSnap(bool snap) { m_PixelSnap = snap; }
    bool GetPixelSnap() const { return m_PixelSnap; }

    // ---- Primitives ------------------------------------------------------
    void DrawQuad(const UIRect& rect, const UIColor& color) override
    {
        m_Inner->DrawQuad(S(rect), color);
    }

    void DrawRect(const UIRect& rect, const UIColor& color, float thickness = 1.0f) override
    {
        // Keep hairlines at >=1 physical pixel; a scaled-down border that lands
        // below 1px would drop out entirely on some rows.
        float t = thickness * m_Scale;
        if (m_PixelSnap)
            t = Round(t);
        m_Inner->DrawRect(S(rect), color, t < 1.0f ? 1.0f : t);
    }

    void DrawConvexPoly(const Vector2* points, int count, const UIColor& color) override
    {
        if (points == nullptr || count <= 0)
        {
            m_Inner->DrawConvexPoly(points, count, color);
            return;
        }
        m_Scratch.clear();
        m_Scratch.reserve(static_cast<size_t>(count));
        for (int i = 0; i < count; ++i)
            m_Scratch.push_back(Vector2(points[i].x * m_Scale, points[i].y * m_Scale));
        m_Inner->DrawConvexPoly(m_Scratch.data(), count, color);
    }

    void DrawRoundedRect(const UIRect& rect, float radius, const UIColor& color) override
    {
        m_Inner->DrawRoundedRect(S(rect), radius * m_Scale, color);
    }

    void DrawTexturedQuad(const UIRect& rect, void* texture_handle,
                          const UIColor& tint = Colors::White) override
    {
        m_Inner->DrawTexturedQuad(S(rect), texture_handle, tint);
    }

    void DrawBox(const UIRect& rect, const FMargin& margin, void* texture_handle,
                 const UIColor& tint) override
    {
        m_Inner->DrawBox(S(rect), S(margin), texture_handle, tint);
    }

    void DrawBorder(const UIRect& rect, const FMargin& margin, void* texture_handle,
                    const UIColor& tint) override
    {
        m_Inner->DrawBorder(S(rect), S(margin), texture_handle, tint);
    }

    // ---- Text ------------------------------------------------------------
    void DrawTextLabel(const UIRect& rect, const std::string& text, float font_size,
                       const UIColor& color,
                       TextAnchor alignment = TextAnchor::MiddleLeft,
                       TextWrapMode wrap = TextWrapMode::NoWrap,
                       void* font_handle = nullptr) override
    {
        m_Inner->DrawTextLabel(S(rect), text, font_size * m_Scale, color, alignment, wrap, font_handle);
    }

    void DrawTextLabel(const std::string& text, const Vector2& pos, float font_size,
                       const UIColor& color) override
    {
        Vector2 p(pos.x * m_Scale, pos.y * m_Scale);
        if (m_PixelSnap)
            p = Vector2(Round(p.x), Round(p.y));
        m_Inner->DrawTextLabel(text, p, font_size * m_Scale, color);
    }

    Vector2 MeasureText(const std::string& text, float font_size) const override
    {
        // Measure at the size we will actually rasterize, then convert back to
        // logical units for the layout pass that asked.
        const Vector2 physical = m_Inner->MeasureText(text, font_size * m_Scale);
        const float inv = 1.0f / m_Scale;
        return Vector2(physical.x * inv, physical.y * inv);
    }

    // ---- Clipping / state ------------------------------------------------
    void PushClipRect(const UIRect& rect) override { m_Inner->PushClipRect(S(rect)); }
    void PopClipRect() override { m_Inner->PopClipRect(); }
    void BeginFrame() override { m_Inner->BeginFrame(); }
    void EndFrame() override { m_Inner->EndFrame(); }
    void Flush() override { m_Inner->Flush(); }

private:
    // floor(v + 0.5) rather than std::round: translation-invariant at .5, so a
    // shared edge rounds the same way no matter which rect it belongs to.
    static float Round(float v) { return std::floor(v + 0.5f); }

    // Snap one axis by edges: [a, a+len) -> [round(a*s), round((a+len)*s)).
    void SnapSpan(float a, float len, float& out_a, float& out_len) const
    {
        const float a0 = Round(a * m_Scale);
        float a1 = Round((a + len) * m_Scale);
        if (len > 0.0f && a1 <= a0)
            a1 = a0 + 1.0f;  // a real (non-empty) rect never vanishes
        else if (len <= 0.0f)
            a1 = a0;
        out_a = a0;
        out_len = a1 - a0;
    }

    UIRect S(const UIRect& r) const
    {
        if (!m_PixelSnap)
            return UIRect(r.x * m_Scale, r.y * m_Scale, r.w * m_Scale, r.h * m_Scale);
        UIRect out;
        SnapSpan(r.x, r.w, out.x, out.w);
        SnapSpan(r.y, r.h, out.y, out.h);
        return out;
    }
    FMargin S(const FMargin& m) const
    {
        if (!m_PixelSnap)
            return FMargin(m.Left * m_Scale, m.Top * m_Scale, m.Right * m_Scale, m.Bottom * m_Scale);
        return FMargin(Round(m.Left * m_Scale), Round(m.Top * m_Scale), Round(m.Right * m_Scale),
                       Round(m.Bottom * m_Scale));
    }

    ISlateRenderer* m_Inner {nullptr};
    float m_Scale {1.0f};
    bool m_PixelSnap {false};
    mutable std::vector<Vector2> m_Scratch;
};

}  // namespace ZSlate
