#pragma once

#include "Runtime/Slate/Widgets/SCompoundWidget.h"

#include <functional>
#include <string>

namespace ZSlate
{
// A collapsible section: a clickable header bar (arrow + title + optional color
// swatch) over a single content child that is shown only while Expanded (UE
// Slate SExpandableArea / ImGui CollapsingHeader analogue).
//
// Expansion is toggled by clicking the header; the layout updates automatically
// on the next CacheDesiredSize()/Paint() pass (the host re-caches every frame),
// so no external rebuild is required to expand/collapse.
class SExpandableArea : public SCompoundWidget
{
public:
    std::string Title;
    bool Expanded {true};

    float HeaderHeight {24.0f};
    float FontSize {14.0f};
    float Indent {8.0f};
    float MinWidth {120.0f};
    FMargin ContentPadding {0.0f, 6.0f, 0.0f, 4.0f};

    UIColor HeaderColor {0.17f, 0.17f, 0.21f, 1.0f};
    UIColor HeaderHoverColor {0.21f, 0.21f, 0.26f, 1.0f};
    UIColor TitleColor {0.88f, 0.90f, 0.95f, 1.0f};
    UIColor ArrowColor {0.70f, 0.74f, 0.82f, 1.0f};

    // Optional color chip drawn at the right edge of the header (used by color
    // properties so the current value is visible without expanding).
    bool ShowHeaderSwatch {false};
    UIColor HeaderSwatchColor {1.0f, 1.0f, 1.0f, 1.0f};

    std::function<void(bool)> OnExpansionChanged;

    Vector2 ComputeDesiredSize() const override
    {
        Vector2 content(0.0f, 0.0f);
        if (m_Content && m_Content->Visibility != EVisibility::Collapsed)
            content = m_Content->GetDesiredSize();

        const float w = (content.x + ContentPadding.GetTotalHorizontal() + Indent) > MinWidth
                            ? (content.x + ContentPadding.GetTotalHorizontal() + Indent)
                            : MinWidth;
        float h = HeaderHeight;
        if (Expanded && m_Content)
            h += content.y + ContentPadding.GetTotalVertical();
        return Vector2(w, h);
    }

    void ArrangeChildren(const FGeometry& allotted, std::vector<FArrangedWidget>& out) const override
    {
        if (!Expanded || !m_Content || m_Content->Visibility == EVisibility::Collapsed)
            return;

        const float region_x = allotted.AbsolutePosition.x + ContentPadding.Left + Indent;
        const float region_y = allotted.AbsolutePosition.y + HeaderHeight + ContentPadding.Top;
        const float region_w = allotted.LocalSize.x - ContentPadding.GetTotalHorizontal() - Indent;
        const float region_h = allotted.LocalSize.y - HeaderHeight - ContentPadding.GetTotalVertical();

        const FGeometry child_geom =
            AlignChildInRegion(region_x, region_y, region_w, region_h, m_Content->GetDesiredSize(), HAlign, VAlign);
        out.push_back({m_Content, child_geom});
    }

    void OnPaint(const FPaintContext& ctx, const FGeometry& geom) const override
    {
        if (ctx.Renderer == nullptr)
            return;
        const UIRect rect = geom.ToRect();
        const UIRect header(rect.x, rect.y, rect.width, HeaderHeight);
        ctx.Renderer->drawQuad(header, m_Hovered ? HeaderHoverColor : HeaderColor);

        ctx.Renderer->drawText(UIRect(rect.x + 6.0f, rect.y, 16.0f, HeaderHeight), Expanded ? "v" : ">", FontSize,
                               ArrowColor, TextAnchor::MiddleLeft, TextWrapMode::NoWrap);

        const float swatch_reserve = ShowHeaderSwatch ? (HeaderHeight + 8.0f) : 6.0f;
        ctx.Renderer->drawText(UIRect(rect.x + 24.0f, rect.y, rect.width - 24.0f - swatch_reserve, HeaderHeight), Title,
                               FontSize, TitleColor, TextAnchor::MiddleLeft, TextWrapMode::NoWrap);

        if (ShowHeaderSwatch)
        {
            const float sw = HeaderHeight - 8.0f;
            const UIRect swatch(rect.x + rect.width - sw - 8.0f, rect.y + 4.0f, sw, sw);
            ctx.Renderer->drawQuad(swatch, HeaderSwatchColor);
            ctx.Renderer->drawRect(swatch, UIColor(0.0f, 0.0f, 0.0f, 1.0f), 1.0f);
        }
    }

    void OnMouseEnter() override { m_Hovered = true; }
    void OnMouseLeave() override { m_Hovered = false; }

    FReply OnMouseButtonDown(const Vector2& pos, int button) override
    {
        if (button != 0 || !InHeader(pos))
            return FReply::Unhandled();
        m_Pressed = true;
        return FReply::Handled();
    }

    FReply OnMouseButtonUp(const Vector2& pos, int button) override
    {
        if (button != 0 || !m_Pressed)
            return FReply::Unhandled();
        m_Pressed = false;
        if (InHeader(pos))
        {
            Expanded = !Expanded;
            if (OnExpansionChanged)
                OnExpansionChanged(Expanded);
        }
        return FReply::Handled();
    }

    void OnMouseCaptureLost() override { m_Pressed = false; }

private:
    bool InHeader(const Vector2& pos) const
    {
        const UIRect rect = m_CachedGeometry.ToRect();
        return pos.x >= rect.x && pos.x <= rect.x + rect.width && pos.y >= rect.y && pos.y <= rect.y + HeaderHeight;
    }

    bool m_Hovered {false};
    bool m_Pressed {false};
};
}  // namespace ZSlate
