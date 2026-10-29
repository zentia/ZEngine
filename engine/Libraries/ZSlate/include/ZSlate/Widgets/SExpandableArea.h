#pragma once

#include "ZSlate/Widgets/SCompoundWidget.h"

#include <functional>
#include <memory>
#include <string>

namespace ZSlate
{

// =============================================================================
// SExpandableArea — collapsible section (UE SExpandableArea analogue)
// =============================================================================
//
// A header row with an arrow toggle + title, and a body panel that
// slides in/out when expanded.
//
class SExpandableArea : public SCompoundWidget
{
public:
    std::string HeaderLabel {"Section"};

    // Header style
    float HeaderHeight {24.0f};
    float FontSize {13.0f};

    // Callback when expand state changes
    std::function<void(bool)> OnExpandedChanged;

    bool IsExpanded() const { return m_Expanded; }
    void SetExpanded(bool expanded) { m_Expanded = expanded; }

    Vector2 ComputeDesiredSize() const override
    {
        float bodyH = 0.0f;
        if (m_Expanded)
        {
            for (const auto& child : m_BodyChildren)
                if (child && child->IsVisible())
                    bodyH += child->GetDesiredSize().y;
        }
        return Vector2(200.0f, HeaderHeight + bodyH);
    }

    // Add a child to the body (visible only when expanded)
    void AddBodyChild(std::shared_ptr<SWidget> child)
    {
        if (child) m_BodyChildren.push_back(std::move(child));
    }

    void ClearBody() { m_BodyChildren.clear(); }

    void OnPaint(const FPaintContext& ctx, const FGeometry& geom) const override
    {
        if (!ctx.Renderer) return;
        const UIRect r = geom.ToRect();

        // Header background (hover highlight)
        UIColor headerBg = m_Hovered ? UIColor(0.16f, 0.17f, 0.21f, 1.0f)
                                    : UIColor(0.10f, 0.10f, 0.13f, 1.0f);
        ctx.Renderer->DrawQuad(UIRect(r.x, r.y, r.w, HeaderHeight), headerBg);
        ctx.Renderer->DrawRect(
            UIRect(r.x, r.y, r.w, HeaderHeight),
            UIColor(0.20f, 0.22f, 0.28f, 1.0f), 1.0f);

        // Expand/collapse arrow
        const char* arrow = m_Expanded ? "\xE2\x96\xBC" : "\xE2\x96\xB6";  // ▼ or ▶
        ctx.Renderer->DrawTextLabel(
            UIRect(r.x + 4.0f, r.y, 20.0f, HeaderHeight),
            arrow, FontSize, UIColor(0.6f, 0.6f, 0.65f, 1.0f),
            TextAnchor::MiddleCenter, TextWrapMode::NoWrap);

        // Header label
        ctx.Renderer->DrawTextLabel(
            UIRect(r.x + 24.0f, r.y, r.w - 28.0f, HeaderHeight),
            HeaderLabel, FontSize,
            UIColor(0.85f, 0.88f, 0.94f, 1.0f),
            TextAnchor::MiddleLeft, TextWrapMode::NoWrap);

        // Body
        if (m_Expanded && !m_BodyChildren.empty())
        {
            float curY = r.y + HeaderHeight;
            for (const auto& child : m_BodyChildren)
            {
                if (!child || !child->IsVisible()) continue;
                float childH = child->GetDesiredSize().y;
                FGeometry childGeom = geom.MakeChild(
                    Vector2(0.0f, curY - r.y), Vector2(r.w, childH));
                child->Paint(ctx, childGeom);
                curY += childH;
            }
        }
    }

    // ---- Input ---------------------------------------------------------------

    void OnMouseEnter() override { m_Hovered = true; }
    void OnMouseLeave() override { m_Hovered = false; }

    FReply OnMouseButtonDown(const Vector2& pos, int btn) override
    {
        if (btn != 0) return FReply::Unhandled();

        const UIRect r = GetCachedGeometry().ToRect();
        if (pos.y >= r.y && pos.y < r.y + HeaderHeight)
        {
            m_Expanded = !m_Expanded;
            if (OnExpandedChanged) OnExpandedChanged(m_Expanded);
            return FReply::Handled();
        }
        return FReply::Unhandled();
    }

private:
    mutable bool m_Expanded {false};
    mutable bool m_Hovered {false};
    std::vector<std::shared_ptr<SWidget>> m_BodyChildren;
};

}  // namespace ZSlate
