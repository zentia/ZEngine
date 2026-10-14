#pragma once

// ============================================================================
// ZSlate/Widgets/Panels/SBorder.h
//
// A single-child decorator that fills a background and strokes an outline.
// ============================================================================

#include "../SWidget.h"
#include "../../Core/ZSlateTypes.h"

#include <memory>

namespace ZSlate
{

class SBorder : public SWidget
{
public:
    UIColor BackgroundColor { 0.10f, 0.11f, 0.13f, 1.0f };
    UIColor BorderColor     { 0.25f, 0.27f, 0.32f, 1.0f };
    float   BorderThickness = 1.0f;
    Vector2 Padding         { 4.0f, 4.0f };

    void SetContent(const std::shared_ptr<SWidget>& content)
    {
        m_Content = content;
        if (m_Content) m_Content->SetParent(this);
    }

    const std::shared_ptr<SWidget>& GetContent() const { return m_Content; }

    void AddSlot(const std::shared_ptr<SWidget>& content) { SetContent(content); }

    Vector2 ComputeDesiredSize() const override;

    void OnArrangeChildren(const FGeometry& allottedGeometry,
                           std::vector<FArrangedChild>& outChildren) const override;

    void OnPaint(const FPaintContext& context, const FGeometry& geometry) const override;

private:
    std::shared_ptr<SWidget> m_Content;
};

} // namespace ZSlate
