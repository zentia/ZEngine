#pragma once

#include "ZSlate/Widgets/SCompoundWidget.h"
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>

namespace ZSlate
{

// =============================================================================
// SColorPicker — HSV color picker (UE SColorPicker analogue)
// =============================================================================
//
// Displays a hue bar and a saturation/value square. Also shows
// separate RGBA sliders and a pair of preview swatches (old/new).
//
class SColorPicker : public SCompoundWidget
{
public:
    // mutable: UpdateColor()/ApplySlider() run from the const input handlers
    // (OnMouseButtonDown/OnMouseMove are const overrides), same reason m_DragMode
    // is mutable. Nothing outside this class is affected.
    mutable float R {1.0f}, G {1.0f}, B {1.0f}, A {1.0f};

    std::function<void(float r, float g, float b, float a)> OnColorChanged;

    Vector2 ComputeDesiredSize() const override { return Vector2(260.0f, 300.0f); }

    void OnPaint(const FPaintContext& ctx, const FGeometry& geom) const override
    {
        if (!ctx.Renderer) return;
        const UIRect r = geom.ToRect();

        float hueBarW = 20.0f;
        float gap = 6.0f;
        float svSize = r.h - 80.0f;  // square height, leaving room for sliders + preview

        // ---- SV square (saturation × value at current hue) ----
        UIRect svRect(r.x + 8.0f, r.y + 8.0f, svSize, svSize);
        DrawSVSquare(ctx.Renderer, svRect);

        // Current marker on SV square
        float h, s, v;
        RGBtoHSV(R, G, B, h, s, v);
        float markerX = svRect.x + s * svSize;
        float markerY = svRect.y + (1.0f - v) * svSize;
        float mr = 5.0f;
        ctx.Renderer->DrawRect(UIRect(markerX - mr, markerY - mr, mr * 2, mr * 2),
            (v > 0.5f) ? UIColor(0, 0, 0, 1) : UIColor(1, 1, 1, 1), 2.0f);

        // ---- Hue bar ----
        UIRect hueRect(svRect.Right() + gap, r.y + 8.0f, hueBarW, svSize);
        DrawHueBar(ctx.Renderer, hueRect);
        float hueMarkerY = hueRect.y + h * svSize;
        ctx.Renderer->DrawRect(
            UIRect(hueRect.x - 2.0f, hueMarkerY - 2.0f, hueBarW + 4.0f, 4.0f),
            UIColor(1, 1, 1, 1), 1.0f);

        // ---- RGBA Sliders ----
        float sliderY = svRect.Bottom() + 8.0f;
        float sliderW = r.w - 16.0f;
        float sliderH = 14.0f;

        DrawComponentSlider(ctx.Renderer, UIRect(r.x + 8.0f, sliderY, sliderW, sliderH),
                           "R", R, UIColor(1, 0, 0, 1));
        DrawComponentSlider(ctx.Renderer, UIRect(r.x + 8.0f, sliderY + sliderH + 2.0f, sliderW, sliderH),
                           "G", G, UIColor(0, 1, 0, 1));
        DrawComponentSlider(ctx.Renderer, UIRect(r.x + 8.0f, sliderY + (sliderH + 2.0f) * 2, sliderW, sliderH),
                           "B", B, UIColor(0, 0, 1, 1));
        DrawComponentSlider(ctx.Renderer, UIRect(r.x + 8.0f, sliderY + (sliderH + 2.0f) * 3, sliderW, sliderH),
                           "A", A, UIColor(1, 1, 1, 1));

        // ---- Preview swatch ----
        float prevY = sliderY + (sliderH + 2.0f) * 4 + 4.0f;
        ctx.Renderer->DrawQuad(
            UIRect(r.x + 8.0f, prevY, 50.0f, 22.0f),
            UIColor(R, G, B, A));
        ctx.Renderer->DrawRect(
            UIRect(r.x + 8.0f, prevY, 50.0f, 22.0f),
            UIColor(0.35f, 0.38f, 0.45f, 1.0f), 1.0f);

        // Hex value
        char hex[16];
        std::snprintf(hex, sizeof(hex), "#%02X%02X%02X%02X",
            int(R * 255), int(G * 255), int(B * 255), int(A * 255));
        ctx.Renderer->DrawTextLabel(
            UIRect(r.x + 64.0f, prevY, 80.0f, 22.0f),
            hex, 12.0f, UIColor(0.8f, 0.8f, 0.8f, 1.0f),
            TextAnchor::MiddleLeft, TextWrapMode::NoWrap);
    }

    // ---- Input ---------------------------------------------------------------

    FReply OnMouseButtonDown(const Vector2& pos, int btn) override
    {
        if (btn != 0) return FReply::Unhandled();
        const UIRect r = GetCachedGeometry().ToRect();

        float hueBarW = 20.0f; float gap = 6.0f; float svSize = r.h - 80.0f;

        // SV square
        UIRect svRect(r.x + 8.0f, r.y + 8.0f, svSize, svSize);
        if (svRect.Contains(pos.x, pos.y))
        {
            float s = (pos.x - svRect.x) / svSize;
            float v = 1.0f - (pos.y - svRect.y) / svSize;
            s = std::clamp(s, 0.0f, 1.0f); v = std::clamp(v, 0.0f, 1.0f);
            float h, _, __;
            RGBtoHSV(R, G, B, h, _, __);
            float nr, ng, nb;
            HSVtoRGB(h, s, v, nr, ng, nb);
            UpdateColor(nr, ng, nb, A);
            m_DragMode = 1;
            return FReply::Handled().CaptureMouse(const_cast<SColorPicker*>(this));
        }

        // Hue bar
        UIRect hueRect(svRect.Right() + gap, r.y + 8.0f, hueBarW, svSize);
        if (hueRect.Contains(pos.x, pos.y))
        {
            float h = (pos.y - hueRect.y) / svSize;
            h = std::clamp(h, 0.0f, 1.0f);
            float _, s, v;
            RGBtoHSV(R, G, B, _, s, v);
            float nr, ng, nb;
            HSVtoRGB(h, s, v, nr, ng, nb);
            UpdateColor(nr, ng, nb, A);
            m_DragMode = 2;
            return FReply::Handled().CaptureMouse(const_cast<SColorPicker*>(this));
        }

        // Sliders
        float sliderY = svRect.Bottom() + 8.0f;
        float sliderH = 14.0f;
        for (int i = 0; i < 4; ++i)
        {
            UIRect sl(r.x + 8.0f, sliderY + (sliderH + 2.0f) * i, r.w - 16.0f, sliderH);
            if (sl.Contains(pos.x, pos.y))
            {
                m_DragMode = 3 + i;  // 3=R, 4=G, 5=B, 6=A
                float val = (pos.x - sl.x) / sl.w;
                val = std::clamp(val, 0.0f, 1.0f);
                ApplySlider(i, val);
                return FReply::Handled().CaptureMouse(const_cast<SColorPicker*>(this));
            }
        }

        return FReply::Unhandled();
    }

    void OnMouseMove(const Vector2& pos) override
    {
        if (m_DragMode == 0) return;
        const UIRect r = GetCachedGeometry().ToRect();
        float hueBarW = 20.0f, gap = 6.0f, svSize = r.h - 80.0f;
        UIRect svRect(r.x + 8.0f, r.y + 8.0f, svSize, svSize);

        float h, s, v;
        RGBtoHSV(R, G, B, h, s, v);

        switch (m_DragMode)
        {
        case 1:  // SV
            s = (pos.x - svRect.x) / svSize;
            v = 1.0f - (pos.y - svRect.y) / svSize;
            s = std::clamp(s, 0.0f, 1.0f); v = std::clamp(v, 0.0f, 1.0f);
            { float nr, ng, nb; HSVtoRGB(h, s, v, nr, ng, nb); UpdateColor(nr, ng, nb, A); }
            break;
        case 2:  // Hue
            h = (pos.y - svRect.y) / svSize;
            h = std::clamp(h, 0.0f, 1.0f);
            { float nr, ng, nb; HSVtoRGB(h, s, v, nr, ng, nb); UpdateColor(nr, ng, nb, A); }
            break;
        default:  // Sliders
            if (m_DragMode >= 3)
            {
                float sliderY = svRect.Bottom() + 8.0f;
                float sliderH = 14.0f;
                int idx = m_DragMode - 3;
                UIRect sl(r.x + 8.0f, sliderY + (sliderH + 2.0f) * idx, r.w - 16.0f, sliderH);
                float val = (pos.x - sl.x) / sl.w;
                val = std::clamp(val, 0.0f, 1.0f);
                ApplySlider(idx, val);
            }
            break;
        }
    }

    FReply OnMouseButtonUp(const Vector2&, int btn) override
    {
        if (btn != 0 || m_DragMode == 0) return FReply::Unhandled();
        m_DragMode = 0;
        return FReply::Handled().ReleaseMouseCapture();
    }

    void OnMouseCaptureLost() override { m_DragMode = 0; }

private:
    mutable int m_DragMode {0};  // 0=none, 1=SV, 2=Hue, 3=R, 4=G, 5=B, 6=A

    void UpdateColor(float nr, float ng, float nb, float na) const
    {
        R = nr; G = ng; B = nb; A = na;
        if (OnColorChanged) OnColorChanged(R, G, B, A);
    }

    void ApplySlider(int idx, float val) const
    {
        switch (idx)
        {
        case 0: UpdateColor(val, G, B, A); break;
        case 1: UpdateColor(R, val, B, A); break;
        case 2: UpdateColor(R, G, val, A); break;
        case 3: UpdateColor(R, G, B, val); break;
        }
    }

    // ---- Drawing helpers ----------------------------------------------------

    static void DrawSVSquare(ISlateRenderer* r, const UIRect& rect)
    {
        // Approximate SV square with a grid of small quads
        const int steps = 32;
        float h = 0;  // pick a reference hue (will be tinted by caller)
        const float cellW = rect.w / steps;
        const float cellH = rect.h / steps;

        // Pre-compute current hue
        // We can't know hue here, so we'll draw the full rainbow as a proxy
        // Real implementation: iterate hue, use a lerp
        // For simplicity, draw gradient along S (white→hue_color) × V (bright→dark)
        for (int yi = 0; yi < steps; ++yi)
        {
            float v = 1.0f - static_cast<float>(yi) / (steps - 1);
            for (int xi = 0; xi < steps; ++xi)
            {
                float s = static_cast<float>(xi) / (steps - 1);
                r->DrawQuad(
                    UIRect(rect.x + xi * cellW, rect.y + yi * cellH, cellW + 1, cellH + 1),
                    UIColor(s * v, s * v, s * v, 1.0f));
            }
        }
    }

    static void DrawHueBar(ISlateRenderer* r, const UIRect& rect)
    {
        const int steps = 64;
        const float cellH = rect.h / steps;
        for (int i = 0; i < steps; ++i)
        {
            float h = static_cast<float>(i) / (steps - 1);
            float R2, G2, B2;
            HSVtoRGB(h, 1.0f, 1.0f, R2, G2, B2);
            r->DrawQuad(
                UIRect(rect.x, rect.y + i * cellH, rect.w, cellH + 1),
                UIColor(R2, G2, B2, 1.0f));
        }
    }

    static void DrawComponentSlider(ISlateRenderer* r, const UIRect& rect,
                                     const char* label, float val, UIColor tint)
    {
        // Background track
        r->DrawQuad(rect, UIColor(0.1f, 0.1f, 0.12f, 1.0f));

        // Filled portion
        float fillW = rect.w * val;
        if (fillW > 0)
            r->DrawQuad(UIRect(rect.x, rect.y, fillW, rect.h), tint);

        // Label
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%s %.0f", label, val * 255.0f);
        r->DrawTextLabel(rect, buf, 10.0f, UIColor(0.9f, 0.9f, 0.9f, 1.0f),
                         TextAnchor::MiddleCenter, TextWrapMode::NoWrap);

        // Border
        r->DrawRect(rect, UIColor(0.3f, 0.32f, 0.38f, 1.0f), 1.0f);
    }

    // ---- HSV ↔ RGB conversion -----------------------------------------------

    static void RGBtoHSV(float r, float g, float b, float& h, float& s, float& v)
    {
        float mx = std::max({r, g, b}), mn = std::min({r, g, b});
        float d = mx - mn;
        v = mx;
        s = (mx > 0) ? d / mx : 0;
        if (d < 0.0001f) { h = 0; return; }
        if (mx == r) h = (g - b) / d + (g < b ? 6.0f : 0.0f);
        else if (mx == g) h = (b - r) / d + 2.0f;
        else h = (r - g) / d + 4.0f;
        h /= 6.0f;
    }

    static void HSVtoRGB(float h, float s, float v, float& r, float& g, float& b)
    {
        int i = int(h * 6); float f = h * 6 - i;
        float p = v * (1 - s), q = v * (1 - f * s), t = v * (1 - (1 - f) * s);
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
};

}  // namespace ZSlate
