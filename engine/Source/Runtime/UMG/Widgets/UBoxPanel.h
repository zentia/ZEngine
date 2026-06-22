#pragma once

#include "ZSlate/Widgets/Layout/SBoxPanel.h"
#include "Runtime/UMG/Core/UPanelWidget.h"

#include <memory>
#include <vector>

namespace ZUMG
{
// UE UMG UVerticalBox / UHorizontalBox analogue -> ZSlate::SBoxPanel. Each child
// gets a slot config (size rule, fill weight, padding, alignment); use AddSlot()
// to add a child and fluently configure its slot.
class UBoxPanel : public UPanelWidget
{
public:
    struct FSlotConfig
    {
        ZSlate::ESizeRule SizeRule {ZSlate::ESizeRule::Auto};
        float FillSize {1.0f};
        ZSlate::FMargin Padding;
        ZSlate::EHorizontalAlignment HAlign {ZSlate::EHorizontalAlignment::Fill};
        ZSlate::EVerticalAlignment VAlign {ZSlate::EVerticalAlignment::Fill};

        FSlotConfig& AutoSize()
        {
            SizeRule = ZSlate::ESizeRule::Auto;
            return *this;
        }
        FSlotConfig& Fill(float weight = 1.0f)
        {
            SizeRule = ZSlate::ESizeRule::Stretch;
            FillSize = weight;
            return *this;
        }
        FSlotConfig& SetPadding(const ZSlate::FMargin& padding)
        {
            Padding = padding;
            return *this;
        }
        FSlotConfig& SetHAlign(ZSlate::EHorizontalAlignment align)
        {
            HAlign = align;
            return *this;
        }
        FSlotConfig& SetVAlign(ZSlate::EVerticalAlignment align)
        {
            VAlign = align;
            return *this;
        }
    };

    explicit UBoxPanel(ZSlate::EOrientation orientation)
        : m_Orientation(orientation) {}

    // Add a child and return its slot config for fluent setup.
    FSlotConfig& AddSlot(std::shared_ptr<UWidget> child)
    {
        AddChildWidget(std::move(child));
        m_SlotConfigs.resize(m_Children.size());
        return m_SlotConfigs.back();
    }

    void InvalidateWidget() override
    {
        UPanelWidget::InvalidateWidget();
        m_SlotConfigs.resize(m_Children.size());
    }

    // Per-child slot accessors (used by the asset serializer to round-trip
    // layout). Index must be < GetChildrenCount().
    const FSlotConfig& GetSlotConfigAt(size_t index) const { return m_SlotConfigs[index]; }
    FSlotConfig& MutableSlotConfigAt(size_t index)
    {
        m_SlotConfigs.resize(m_Children.size());
        return m_SlotConfigs[index];
    }

    const char* GetWidgetClassName() const override
    {
        return m_Orientation == ZSlate::EOrientation::Vertical ? "VerticalBox" : "HorizontalBox";
    }

protected:
    std::shared_ptr<ZSlate::SWidget> RebuildWidget() override
    {
        auto panel = std::make_shared<ZSlate::SBoxPanel>(m_Orientation);
        panel->Visibility = m_Visibility;
        m_SlotConfigs.resize(m_Children.size());
        for (size_t i = 0; i < m_Children.size(); ++i)
        {
            if (!m_Children[i])
                continue;
            const FSlotConfig& cfg = m_SlotConfigs[i];
            auto& slot = panel->AddSlot(m_Children[i]->TakeWidget());
            slot.SizeRule = cfg.SizeRule;
            slot.FillSize = cfg.FillSize;
            slot.Padding = cfg.Padding;
            slot.HAlign = cfg.HAlign;
            slot.VAlign = cfg.VAlign;
        }
        return panel;
    }

    ZSlate::EOrientation m_Orientation;
    std::vector<FSlotConfig> m_SlotConfigs;
};

class UVerticalBox : public UBoxPanel
{
public:
    UVerticalBox()
        : UBoxPanel(ZSlate::EOrientation::Vertical) {}
};

class UHorizontalBox : public UBoxPanel
{
public:
    UHorizontalBox()
        : UBoxPanel(ZSlate::EOrientation::Horizontal) {}
};
}  // namespace ZUMG
