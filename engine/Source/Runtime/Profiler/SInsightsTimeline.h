#pragma once

#include "Runtime/Profiler/InsightsTrace.h"
#include "ZSlate/Widgets/SLeafWidget.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace ZSlate
{
// The ZEngine Insights flame chart: a horizontally pannable / zoomable timeline
// with a frame ruler on top and one stacked track per OS thread (Unreal Insights
// "Timing" view analogue). Events are drawn as nested colored bars; left-drag
// pans, mouse wheel zooms about the cursor. The owning window feeds a snapshot
// each frame via SetSnapshot() and reads back the hovered-event description via
// HoverText() for its status line.
//
// Drawing-only widget: it never mutates the trace. The snapshot pointer must
// outlive Paint()+input for the frame (the window keeps it as a member).
//
// Lives in Runtime/Profiler (not Editor) so both the in-editor Insights panel
// and the standalone ZInsights.exe viewer can paint the same flame chart.
class SInsightsTimeline : public SLeafWidget
{
public:
    void SetSnapshot(const ZEngine::Insights::InsightsSnapshot* snapshot) { m_Snapshot = snapshot; }
    const std::string& HoverText() const { return m_HoverText; }

    // Reset the view so the whole captured range fits `width_px`. Called by the
    // window on first data and on the "Fit" / "Zoom to frame" actions.
    void FitView(float width_px)
    {
        if (m_Snapshot == nullptr || m_Snapshot->max_ns <= m_Snapshot->min_ns || width_px < 1.0f)
            return;
        const double span = static_cast<double>(m_Snapshot->max_ns - m_Snapshot->min_ns);
        m_ViewStartNs = static_cast<double>(m_Snapshot->min_ns);
        m_NsPerPx = span / static_cast<double>(width_px);
        if (m_NsPerPx < 1.0)
            m_NsPerPx = 1.0;
        m_ViewInit = true;
    }

    // Fit once when the first non-empty snapshot arrives.
    void EnsureInitialized(float width_px)
    {
        if (!m_ViewInit)
            FitView(width_px);
    }

    Vector2 ComputeDesiredSize() const override { return Vector2(0.0f, 0.0f); }  // stretch to fill

    void OnPaint(const FPaintContext& ctx, const FGeometry& geom) const override
    {
        if (ctx.Renderer == nullptr)
            return;
        const UIRect rect = geom.ToRect();
        ISlateRenderer* r = ctx.Renderer;

        r->drawQuad(rect, UIColor(0.09f, 0.09f, 0.11f, 1.0f));

        if (m_Snapshot == nullptr)
            return;

        // --- Frame ruler ----------------------------------------------------
        const UIRect ruler(rect.x, rect.y, rect.w, kRulerHeight);
        r->drawQuad(ruler, UIColor(0.13f, 0.13f, 0.16f, 1.0f));
        const auto& frames = m_Snapshot->frame_starts;
        for (size_t i = 0; i < frames.size(); ++i)
        {
            const float fx = XOf(static_cast<double>(frames[i]), rect);
            if (fx < rect.x - 1.0f || fx > rect.x + rect.w + 1.0f)
                continue;
            r->drawQuad(UIRect(fx, rect.y, 1.0f, rect.h), UIColor(0.30f, 0.32f, 0.40f, 0.55f));
            if (i + 1 < frames.size())
            {
                const float nx = XOf(static_cast<double>(frames[i + 1]), rect);
                const float w = nx - fx;
                if (w > 36.0f)
                {
                    const double ms = static_cast<double>(frames[i + 1] - frames[i]) / 1.0e6;
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "%.2f ms", ms);
                    r->drawText(UIRect(fx + 3.0f, rect.y, w - 4.0f, kRulerHeight), buf, 11.0f,
                                UIColor(0.70f, 0.74f, 0.82f, 1.0f), TextAnchor::MiddleLeft, TextWrapMode::NoWrap);
                }
            }
        }

        // --- Tracks ---------------------------------------------------------
        float y = rect.y + kRulerHeight + 2.0f;
        for (const ZEngine::Insights::TrackSnapshot& track : m_Snapshot->tracks)
        {
            const float rows_h = static_cast<float>(track.max_depth + 1) * kRowHeight;
            // Track header band.
            r->drawQuad(UIRect(rect.x, y, rect.w, kHeaderHeight), UIColor(0.16f, 0.17f, 0.21f, 1.0f));
            r->drawText(UIRect(rect.x + 6.0f, y, rect.w - 8.0f, kHeaderHeight), track.thread_name, 12.0f,
                        UIColor(0.85f, 0.88f, 0.94f, 1.0f), TextAnchor::MiddleLeft, TextWrapMode::NoWrap);
            const float rows_top = y + kHeaderHeight;

            for (const ZEngine::Insights::ScopeEvent& ev : track.events)
            {
                const uint64_t end = (ev.end_ns != 0) ? ev.end_ns : m_Snapshot->max_ns;
                const float x0 = XOf(static_cast<double>(ev.start_ns), rect);
                const float x1 = XOf(static_cast<double>(end), rect);
                if (x1 < rect.x || x0 > rect.x + rect.w)
                    continue;
                float bx = std::max(x0, rect.x);
                float bw = std::min(x1, rect.x + rect.w) - bx;
                if (bw < 1.0f)
                    bw = 1.0f;
                const float by = rows_top + static_cast<float>(ev.depth) * kRowHeight;
                const UIColor col = ColorForName(ev.name_id);
                r->drawQuad(UIRect(bx, by, bw, kRowHeight - 1.0f), col);
                if (bw > 28.0f)
                {
                    r->drawText(UIRect(bx + 3.0f, by, bw - 5.0f, kRowHeight - 1.0f), m_Snapshot->Name(ev.name_id),
                                10.0f, UIColor(0.04f, 0.04f, 0.06f, 1.0f), TextAnchor::MiddleLeft,
                                TextWrapMode::NoWrap);
                }
            }
            y = rows_top + rows_h + kTrackGap;
            if (y > rect.y + rect.h)
                break;
        }
    }

    FReply OnMouseButtonDown(const Vector2& pos, int button) override
    {
        // Left (0) or middle (2) drag pans.
        if (button == 0 || button == 2)
        {
            m_Panning = true;
            m_LastDrag = pos;
            return FReply::Handled().CaptureMouse(this);
        }
        return FReply::Unhandled();
    }

    void OnMouseMove(const Vector2& pos) override
    {
        const UIRect rect = m_CachedGeometry.ToRect();
        if (m_Panning)
        {
            const double dx = static_cast<double>(pos.x - m_LastDrag.x);
            m_ViewStartNs -= dx * m_NsPerPx;
            m_LastDrag = pos;
        }
        UpdateHover(pos, rect);
    }

    FReply OnMouseButtonUp(const Vector2& /*pos*/, int button) override
    {
        if (button == 0 || button == 2)
        {
            m_Panning = false;
            return FReply::Handled().ReleaseMouseCapture();
        }
        return FReply::Unhandled();
    }

    FReply OnMouseWheel(const Vector2& pos, float delta) override
    {
        const UIRect rect = m_CachedGeometry.ToRect();
        const double ns_at_cursor = NsOf(pos.x, rect);
        const double factor = (delta > 0.0f) ? (1.0 / 1.25) : 1.25;
        m_NsPerPx *= factor;
        if (m_NsPerPx < 0.25)
            m_NsPerPx = 0.25;
        if (m_NsPerPx > 1.0e9)
            m_NsPerPx = 1.0e9;
        m_ViewStartNs = ns_at_cursor - static_cast<double>(pos.x - rect.x) * m_NsPerPx;
        return FReply::Handled();
    }

    void OnMouseCaptureLost() override { m_Panning = false; }

private:
    float XOf(double ns, const UIRect& rect) const
    {
        return rect.x + static_cast<float>((ns - m_ViewStartNs) / m_NsPerPx);
    }
    double NsOf(float x, const UIRect& rect) const
    {
        return m_ViewStartNs + static_cast<double>(x - rect.x) * m_NsPerPx;
    }

    void UpdateHover(const Vector2& pos, const UIRect& rect)
    {
        m_HoverText.clear();
        if (m_Snapshot == nullptr)
            return;
        const double ns = NsOf(pos.x, rect);
        float y = rect.y + kRulerHeight + 2.0f;
        for (const ZEngine::Insights::TrackSnapshot& track : m_Snapshot->tracks)
        {
            const float rows_top = y + kHeaderHeight;
            const float rows_h = static_cast<float>(track.max_depth + 1) * kRowHeight;
            if (pos.y >= rows_top && pos.y < rows_top + rows_h)
            {
                const int row = static_cast<int>((pos.y - rows_top) / kRowHeight);
                for (const ZEngine::Insights::ScopeEvent& ev : track.events)
                {
                    if (static_cast<int>(ev.depth) != row)
                        continue;
                    const uint64_t end = (ev.end_ns != 0) ? ev.end_ns : m_Snapshot->max_ns;
                    if (ns >= static_cast<double>(ev.start_ns) && ns <= static_cast<double>(end))
                    {
                        const double ms = static_cast<double>(end - ev.start_ns) / 1.0e6;
                        char buf[64];
                        std::snprintf(buf, sizeof(buf), "  -  %.3f ms", ms);
                        m_HoverText = m_Snapshot->Name(ev.name_id) + buf;
                        return;
                    }
                }
            }
            y = rows_top + rows_h + kTrackGap;
        }
    }

    static UIColor ColorForName(uint32_t id)
    {
        // Stable pseudo-random hue from the name id; fixed saturation/value.
        const float hue = static_cast<float>((id * 2654435761u) % 1000u) / 1000.0f;
        float rr, gg, bb;
        HSVtoRGB(hue, 0.55f, 0.85f, rr, gg, bb);
        return UIColor(rr, gg, bb, 1.0f);
    }

    static void HSVtoRGB(float h, float s, float v, float& r, float& g, float& b)
    {
        const float hh = (h - std::floor(h)) * 6.0f;
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

    static constexpr float kRulerHeight {22.0f};
    static constexpr float kHeaderHeight {16.0f};
    static constexpr float kRowHeight {15.0f};
    static constexpr float kTrackGap {6.0f};

    const ZEngine::Insights::InsightsSnapshot* m_Snapshot {nullptr};
    double m_ViewStartNs {0.0};
    double m_NsPerPx {1000.0};
    bool m_ViewInit {false};
    bool m_Panning {false};
    Vector2 m_LastDrag {0.0f, 0.0f};
    std::string m_HoverText;
};
}  // namespace ZSlate
