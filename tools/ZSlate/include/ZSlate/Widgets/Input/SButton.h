#pragma once

// ============================================================================
// ZSlate/Widgets/Input/SButton.h
//
// Clickable container. Content is any widget (usually an STextBlock).
// ============================================================================

#include "../SWidget.h"
#include "../../Core/ZSlateTypes.h"

#include <functional>
#include <memory>

namespace ZSlate
{

class SButton : public SWidget
{
public:
    UIColor NormalColor  { 0.18f, 0.18f, 0.20f, 1.0f };
    UIColor HoverColor   { 0.25f, 0.25f, 0.28f, 1.0f };
    UIColor PressedColor { 0.13f, 0.38f, 0.70f, 1.0f };
    UIColor BorderColor  { 0.28f, 0.30f, 0.35f, 1.0f };
    Vector2 Padding      { 12.0f, 6.0f };

    std::function<void()> OnClicked;

    void SetContent(const std::shared_ptr<SWidget>& content)
    {
        m_Content = content;
        if (m_Content) m_Content->SetParent(this);
    }

    const std::shared_ptr<SWidget>& GetContent() const { return m_Content; }

    Vector2 ComputeDesiredSize() const override;

    void OnArrangeChildren(const FGeometry& allottedGeometry,
                           std::vector<FArrangedChild>& outChildren) const override;

    void OnPaint(const FPaintContext& context, const FGeometry& geometry) const override;

    FReply OnMouseButtonDown(const Vector2& absolutePos, int button) override;
    FReply OnMouseButtonUp  (const Vector2& absolutePos, int button) override;

    bool IsPressed() const { return m_Pressed; }

private:
    std::shared_ptr<SWidget> m_Content;
    bool                     m_Pressed = false;
};

} // namespace ZSlate
