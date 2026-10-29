#pragma once

#include "ZSlate/Widgets/SWidget.h"

namespace ZSlate
{

// =============================================================================
// SThrobber — animated spinner/loading indicator (UE SThrobber analogue)
// =============================================================================
//
// Draws a rotating arc or a row of bouncing dots. Animates via Tick.
// Current implementation: row of 3 pulsing dots (the most universal style).
//
class SThrobber : public SWidget
{
public:
    // Number of dots in the row
    int NumPieces {3};

    // Dot radius
    float PieceRadius {4.0f};

    // Dot color
    UIColor Color {0.4f, 0.6f, 1.0f, 1.0f};

    // Spacing between dots
    float Spacing {8.0f};

    // Animation speed (radians per second)
    float AnimSpeed {4.0f};

    Vector2 ComputeDesiredSize() const override
    {
        float w = NumPieces * (PieceRadius * 2.0f + Spacing) - Spacing;
        float h = PieceRadius * 2.0f;
        return Vector2(w, h);
    }

    void Tick(const FGeometry&, float deltaSec) override
    {
        m_Time += deltaSec * AnimSpeed;
    }

    void OnPaint(const FPaintContext& ctx, const FGeometry& geom) const override
    {
        if (!ctx.Renderer) return;
        const UIRect r = geom.ToRect();
        float cx = r.x + r.w * 0.5f;
        float cy = r.y + r.h * 0.5f;

        for (int i = 0; i < NumPieces; ++i)
        {
            // Each dot oscillates with a phase offset
            float phase = static_cast<float>(i) / static_cast<float>(NumPieces);
            float alpha = 0.3f + 0.7f * (0.5f + 0.5f * std::sin(m_Time * AnimSpeed + phase * 3.14159f * 2.0f));

            float dotX = cx + (static_cast<float>(i) - (NumPieces - 1) * 0.5f) * (PieceRadius * 2.0f + Spacing);
            float rr = PieceRadius * (0.7f + 0.3f * alpha);

            ctx.Renderer->DrawQuad(
                UIRect(dotX - rr, cy - rr, rr * 2.0f, rr * 2.0f),
                UIColor(Color.x, Color.y, Color.z, Color.w * alpha));
        }
    }

private:
    mutable float m_Time {0.0f};
};

}  // namespace ZSlate
