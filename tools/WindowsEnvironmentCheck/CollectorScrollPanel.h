#pragma once

#include "ZSlate/Widgets/Layout/SScrollBox.h"
#include <algorithm>
#include <memory>

namespace zengine::envcheck::collector
{
class CollectorScrollPanel : public ZSlate::SWidget
{
public:
    CollectorScrollPanel(std::shared_ptr<ZSlate::SScrollBox> content, std::shared_ptr<ZSlate::SScrollBar> bar)
        : content_(std::move(content)), bar_(std::move(bar)) {}

    int GetChildCount() const override { return 2; }
    std::shared_ptr<ZSlate::SWidget> GetChildAt(int index) const override
    {
        if (index == 0) return content_;
        if (index == 1) return bar_;
        return nullptr;
    }
    ZSlate::Vector2 ComputeDesiredSize() const override
    {
        auto size = content_->ComputeDesiredSize();
        size.x += bar_->ComputeDesiredSize().x;
        return size;
    }
    void ArrangeChildren(const ZSlate::FGeometry& geometry, std::vector<ZSlate::FArrangedWidget>& children) const override
    {
        const float width = std::max(0.0f, geometry.LocalSize.x);
        const float height = std::max(0.0f, geometry.LocalSize.y);
        const float barWidth = std::clamp(bar_->ComputeDesiredSize().x, 0.0f, width);
        const float contentWidth = width - barWidth;
        children.push_back({content_, geometry.MakeChild(ZSlate::Vector2(0, 0), ZSlate::Vector2(contentWidth, height))});
        children.push_back({bar_, geometry.MakeChild(ZSlate::Vector2(contentWidth, 0), ZSlate::Vector2(barWidth, height))});
    }
private:
    std::shared_ptr<ZSlate::SScrollBox> content_;
    std::shared_ptr<ZSlate::SScrollBar> bar_;
};
}
