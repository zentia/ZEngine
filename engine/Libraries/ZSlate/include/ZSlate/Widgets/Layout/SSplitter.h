#pragma once

#include "ZSlate/Widgets/SWidget.h"

#include <functional>
#include <memory>

namespace ZSlate
{
class SSplitterHandle;

// Horizontal two-pane splitter (UE Slate SSplitter analogue for Path | Assets).
// Left pane uses a fixed pixel width (resizable via drag handle); right fills remainder.
class SSplitter : public SWidget
{
public:
    SSplitter();

    float LeftPanelWidth {230.0f};
    float HandleWidth {4.0f};
    float MinLeftPanelWidth {120.0f};
    float MinRightPanelWidth {160.0f};

    UIColor HandleColor {0.18f, 0.18f, 0.20f, 1.0f};
    UIColor HandleHoverColor {0.28f, 0.45f, 0.72f, 1.0f};

    std::function<void(float)> OnLeftPanelResized;
    std::function<void(float)> OnLeftPanelResizeFinished;

    void SetLeftContent(std::shared_ptr<SWidget> content);
    void SetRightContent(std::shared_ptr<SWidget> content);

    Vector2 ComputeDesiredSize() const override;
    int GetChildCount() const override;
    std::shared_ptr<SWidget> GetChildAt(int index) const override;
    void ArrangeChildren(const FGeometry& allotted, std::vector<FArrangedWidget>& out) const override;

    float ClampLeftWidth(float width, float total_width) const;

private:
    std::shared_ptr<SWidget> m_Left;
    std::shared_ptr<SSplitterHandle> m_Handle;
    std::shared_ptr<SWidget> m_Right;
};

}  // namespace ZSlate
