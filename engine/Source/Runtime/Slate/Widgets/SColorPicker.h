#pragma once

#include "Runtime/Slate/Widgets/SLeafWidget.h"

#include <algorithm>
#include <cmath>
#include <functional>

namespace ZSlate
{
// An HSV color picker: a saturation/value square, a hue bar, and an optional
// alpha bar (UE Slate SColorPicker / ImGui ColorPicker analogue). Because the
// UIRenderer only draws solid quads, the gradients are tessellated into small
// cells -- fine for an editor inspector (one picker visible at a time).
//
// Edits report through OnColorChanged(r,g,b,a) in linear [0,1]. Seed the widget
// with SetColorRGBA() before painting; read back with GetColorRGBA().
class SColorPicker : public SLeafWidget
{
public:
    float SquareSize {160.0f};
    float BarWidth {18.0f};
    float Gap {10.0f};
    bool ShowAlpha {true};

    // Tessellation resolution (cells per axis / segments per bar).
    int SquareCells {20};
    int BarSegments {32};

    std::function<void(float, float, float, float)> OnColorChanged;

    void SetColorRGBA(float r, float g, float b, float a)
    {
        RGBtoHSV(r, g, b, m_H, m_S, m_V);
        m_A = Clamp01(a);
    }

    void GetColorRGBA(float& r, float& g, float& b, float& a) const
    {
        HSVtoRGB(m_H, m_S, m_V, r, g, b);
        a = m_A;
    }

    Vector2 ComputeDesiredSize() const override
    {
        float w = SquareSize + Gap + BarWidth;
        if (ShowAlpha)
            w += Gap + BarWidth;
        return Vector2(w, SquareSize);
    }

    void OnPaint(const FPaintContext& ctx, const FGeometry& geom) const override
    {
        if (ctx.Renderer == nullptr)
            return;
        const UIRect rect = geom.ToRect();
        UIRect sv, hue, alpha;
        ComputeRegions(rect, sv, hue, alpha);

        // --- SV square: per-cell HSV(hue, sat=x, val=1-y).
        const int n = SquareCells > 0 ? SquareCells : 1;
        const float cw = sv.width / n;
        const float ch = sv.height / n;
        for (int j = 0; j < n; ++j)
        {
            const float v = 1.0f - (j + 0.5f) / n;
            for (int i = 0; i < n; ++i)
            {
                const float s = (i + 0.5f) / n;
                float r, g, b;
                HSVtoRGB(m_H, s, v, r, g, b);
                ctx.Renderer->drawQuad(UIRect(sv.x + i * cw, sv.y + j * ch, cw + 1.0f, ch + 1.0f),
                                       UIColor(r, g, b, 1.0f));
            }
        }
        ctx.Renderer->drawRect(sv, UIColor(0.0f, 0.0f, 0.0f, 1.0f), 1.0f);
        // SV marker.
        {
            const float mx = sv.x + m_S * sv.width;
            const float my = sv.y + (1.0f - m_V) * sv.height;
            const UIColor mc = (m_V > 0.5f) ? UIColor(0.0f, 0.0f, 0.0f, 1.0f) : UIColor(1.0f, 1.0f, 1.0f, 1.0f);
            ctx.Renderer->drawRect(UIRect(mx - 4.0f, my - 4.0f, 8.0f, 8.0f), mc, 1.5f);
        }

        // --- Hue bar.
        const int m = BarSegments > 0 ? BarSegments : 1;
        const float seg_h = hue.height / m;
        for (int k = 0; k < m; ++k)
        {
            float r, g, b;
            HSVtoRGB((k + 0.5f) / m, 1.0f, 1.0f, r, g, b);
            ctx.Renderer->drawQuad(UIRect(hue.x, hue.y + k * seg_h, hue.width, seg_h + 1.0f), UIColor(r, g, b, 1.0f));
        }
        ctx.Renderer->drawRect(hue, UIColor(0.0f, 0.0f, 0.0f, 1.0f), 1.0f);
        {
            const float hy = hue.y + m_H * hue.height;
            ctx.Renderer->drawRect(UIRect(hue.x - 2.0f, hy - 2.0f, hue.width + 4.0f, 4.0f),
                                   UIColor(1.0f, 1.0f, 1.0f, 1.0f), 1.5f);
        }

        // --- Alpha bar (current color over a checker-ish gray base).
        if (ShowAlpha)
        {
            float cr, cg, cb;
            HSVtoRGB(m_H, m_S, m_V, cr, cg, cb);
            for (int k = 0; k < m; ++k)
            {
                const float t = 1.0f - (k + 0.5f) / m;  // top = opaque
                const float gray = ((k & 1) ? 0.32f : 0.20f);
                ctx.Renderer->drawQuad(UIRect(alpha.x, alpha.y + k * seg_h, alpha.width, seg_h + 1.0f),
                                       UIColor(gray, gray, gray, 1.0f));
                ctx.Renderer->drawQuad(UIRect(alpha.x, alpha.y + k * seg_h, alpha.width, seg_h + 1.0f),
                                       UIColor(cr, cg, cb, t));
            }
            ctx.Renderer->drawRect(alpha, UIColor(0.0f, 0.0f, 0.0f, 1.0f), 1.0f);
            const float ay = alpha.y + (1.0f - m_A) * alpha.height;
            ctx.Renderer->drawRect(UIRect(alpha.x - 2.0f, ay - 2.0f, alpha.width + 4.0f, 4.0f),
                                   UIColor(1.0f, 1.0f, 1.0f, 1.0f), 1.5f);
        }
    }

    FReply OnMouseButtonDown(const Vector2& pos, int button) override
    {
        if (button != 0)
            return FReply::Unhandled();
        const UIRect rect = m_CachedGeometry.ToRect();
        UIRect sv, hue, alpha;
        ComputeRegions(rect, sv, hue, alpha);
        if (sv.Contains(pos))
            m_Active = Region::SV;
        else if (hue.Contains(pos))
            m_Active = Region::Hue;
        else if (ShowAlpha && alpha.Contains(pos))
            m_Active = Region::Alpha;
        else
            return FReply::Unhandled();
        ApplyFromPos(pos);
        return FReply::Handled().CaptureMouse(this);
    }

    void OnMouseMove(const Vector2& pos) override
    {
        if (m_Active != Region::None)
            ApplyFromPos(pos);
    }

    FReply OnMouseButtonUp(const Vector2& /*pos*/, int button) override
    {
        if (button != 0)
            return FReply::Unhandled();
        m_Active = Region::None;
        return FReply::Handled().ReleaseMouseCapture();
    }

    void OnMouseCaptureLost() override { m_Active = Region::None; }

private:
    enum class Region
    {
        None,
        SV,
        Hue,
        Alpha
    };

    static float Clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

    void ComputeRegions(const UIRect& rect, UIRect& sv, UIRect& hue, UIRect& alpha) const
    {
        const float sq = std::min(SquareSize, rect.height);
        sv = UIRect(rect.x, rect.y, sq, sq);
        const float hx = rect.x + sq + Gap;
        hue = UIRect(hx, rect.y, BarWidth, sq);
        alpha = ShowAlpha ? UIRect(hx + BarWidth + Gap, rect.y, BarWidth, sq) : UIRect(0, 0, 0, 0);
    }

    void ApplyFromPos(const Vector2& pos)
    {
        const UIRect rect = m_CachedGeometry.ToRect();
        UIRect sv, hue, alpha;
        ComputeRegions(rect, sv, hue, alpha);
        switch (m_Active)
        {
            case Region::SV:
                m_S = Clamp01(sv.width > 0.0f ? (pos.x - sv.x) / sv.width : 0.0f);
                m_V = Clamp01(sv.height > 0.0f ? 1.0f - (pos.y - sv.y) / sv.height : 0.0f);
                break;
            case Region::Hue:
                m_H = Clamp01(hue.height > 0.0f ? (pos.y - hue.y) / hue.height : 0.0f);
                break;
            case Region::Alpha:
                m_A = Clamp01(alpha.height > 0.0f ? 1.0f - (pos.y - alpha.y) / alpha.height : 0.0f);
                break;
            default:
                return;
        }
        if (OnColorChanged)
        {
            float r, g, b;
            HSVtoRGB(m_H, m_S, m_V, r, g, b);
            OnColorChanged(r, g, b, m_A);
        }
    }

    static void HSVtoRGB(float h, float s, float v, float& r, float& g, float& b)
    {
        if (s <= 0.0f)
        {
            r = g = b = v;
            return;
        }
        h = h - std::floor(h);
        const float hh = h * 6.0f;
        const int i = static_cast<int>(hh);
        const float f = hh - i;
        const float p = v * (1.0f - s);
        const float q = v * (1.0f - s * f);
        const float t = v * (1.0f - s * (1.0f - f));
        switch (i % 6)
        {
            case 0: r = v; g = t; b = p; break;
            case 1: r = q; g = v; b = p; break;
            case 2: r = p; g = v; b = t; break;
            case 3: r = p; g = q; b = v; break;
            case 4: r = t; g = p; b = v; break;
            default: r = v; g = p; b = q; break;
        }
    }

    static void RGBtoHSV(float r, float g, float b, float& h, float& s, float& v)
    {
        const float mx = std::max(r, std::max(g, b));
        const float mn = std::min(r, std::min(g, b));
        v = mx;
        const float d = mx - mn;
        s = (mx <= 0.0f) ? 0.0f : (d / mx);
        if (d <= 0.0f)
        {
            h = 0.0f;
            return;
        }
        if (mx == r)
            h = (g - b) / d + (g < b ? 6.0f : 0.0f);
        else if (mx == g)
            h = (b - r) / d + 2.0f;
        else
            h = (r - g) / d + 4.0f;
        h /= 6.0f;
    }

    float m_H {0.0f};
    float m_S {0.0f};
    float m_V {1.0f};
    float m_A {1.0f};
    Region m_Active {Region::None};
};
}  // namespace ZSlate
