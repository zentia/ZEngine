#pragma once

#include "Runtime/Slate/Widgets/SOverlay.h"
#include "Runtime/UMG/Core/UPanelWidget.h"

#include <vector>

namespace ZUMG
{
// UE UMG UOverlay analogue -> ZSlate::SOverlay. Z-stacks children (later children
// on top), each aligned + padded within the panel's geometry.
class UOverlay : public UPanelWidget
{
public:
    struct FSlotConfig
    {
        ZSlate::FMargin Padding;
        ZSlate::EHorizontalAlignment HAlign {ZSlate::EHorizontalAlignment::Fill};
        ZSlate::EVerticalAlignment VAlign {ZSlate::EVerticalAlignment::Fill};

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

    const FSlotConfig& GetSlotConfigAt(size_t index) const { return m_SlotConfigs[index]; }
    FSlotConfig& MutableSlotConfigAt(size_t index)
    {
        m_SlotConfigs.resize(m_Children.size());
        return m_SlotConfigs[index];
    }

    const char* GetWidgetClassName() const override { return "Overlay"; }

protected:
    std::shared_ptr<ZSlate::SWidget> RebuildWidget() override
    {
        auto overlay = std::make_shared<ZSlate::SOverlay>();
        overlay->Visibility = m_Visibility;
        m_SlotConfigs.resize(m_Children.size());
        for (size_t i = 0; i < m_Children.size(); ++i)
        {
            if (!m_Children[i])
                continue;
            const FSlotConfig& cfg = m_SlotConfigs[i];
            auto& slot = overlay->AddSlot(m_Children[i]->TakeWidget());
            slot.Padding = cfg.Padding;
            slot.HAlign = cfg.HAlign;
            slot.VAlign = cfg.VAlign;
        }
        return overlay;
    }

    std::vector<FSlotConfig> m_SlotConfigs;
};
}  // namespace ZUMG
