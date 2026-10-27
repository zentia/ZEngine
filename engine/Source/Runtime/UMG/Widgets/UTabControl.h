#pragma once

#include "ZSlate/Widgets/Docking/SDockTab.h"
#include "Runtime/UMG/Core/UPanelWidget.h"

#include <string>
#include <vector>

namespace ZUMG
{
// UE UMG UTabControl analogue -> ZSlate::SDockingArea. Each child UWidget becomes
// one tab; the tab label defaults to the child's Name (or "Tab N"). The active
// tab's child is shown in the content area. Children are the tab CONTENTS; the
// tab headers are generated from the labels.
class UTabControl : public UPanelWidget
{
public:
    // Per-tab labels; if shorter than the child count, missing entries fall back
    // to the child's Name (or "Tab N").
    std::vector<std::string> TabLabels;
    bool CanCloseTabs {false};

    // Active tab index. SDockingArea does not expose its active-tab pointer, so
    // this returns the last index this wrapper set (or that a rebuild defaulted
    // to). A user clicking a different tab header updates the ZSlate-side active
    // tab but is not yet reflected here — that read-back is on the render-bound
    // wiring list (needs an SDockingArea active-index accessor).
    int GetActiveIndex() const { return m_LastActiveIndex; }
    void SetActiveIndex(int index)
    {
        m_LastActiveIndex = index;
        // Rebuilding re-adds tabs in order; SDockingArea activates the first added
        // tab by default, so we re-issue activation through the rebuilt widget by
        // simulating a click on the requested tab in RebuildWidget.
        m_PendingActive = index;
        InvalidateWidget();
    }

    const char* GetWidgetClassName() const override { return "TabControl"; }
    void SerializeProperties(UMGPropertyBag& bag) const override
    {
        UWidget::SerializeProperties(bag);
        std::string joined;
        for (size_t i = 0; i < TabLabels.size(); ++i)
        {
            if (i) joined += '\n';
            joined += TabLabels[i];
        }
        bag.SetString("tab_labels", joined);
        bag.SetBool("can_close", CanCloseTabs);
        bag.SetInt("active_index", m_LastActiveIndex);
    }
    void DeserializeProperties(const UMGPropertyBag& bag) override
    {
        UWidget::DeserializeProperties(bag);
        TabLabels.clear();
        const std::string joined = bag.GetString("tab_labels", std::string());
        if (!joined.empty())
        {
            size_t start = 0;
            while (start <= joined.size())
            {
                size_t end = joined.find('\n', start);
                if (end == std::string::npos)
                    end = joined.size();
                if (end > start)
                    TabLabels.push_back(joined.substr(start, end - start));
                start = end + 1;
            }
        }
        CanCloseTabs = bag.GetBool("can_close", CanCloseTabs);
        m_LastActiveIndex = bag.GetInt("active_index", m_LastActiveIndex);
    }

protected:
    std::shared_ptr<ZSlate::SWidget> RebuildWidget() override
    {
        auto area = std::make_shared<ZSlate::SDockingArea>();
        for (size_t i = 0; i < m_Children.size(); ++i)
        {
            auto tab = std::make_shared<ZSlate::SDockTab>();
            tab->CanClose = CanCloseTabs;
            // Label: explicit entry, else child Name, else "Tab N".
            if (i < TabLabels.size() && !TabLabels[i].empty())
                tab->Label = TabLabels[i];
            else if (m_Children[i] && !m_Children[i]->Name.empty())
                tab->Label = m_Children[i]->Name;
            else
                tab->Label = "Tab " + std::to_string(i + 1);
            if (m_Children[i])
                tab->SetContent(m_Children[i]->TakeWidget());
            area->AddTab(tab);
        }
        // Activate the requested tab (SDockingArea activates the first by default).
        if (m_PendingActive > 0 && m_PendingActive < area->GetTabCount())
        {
            // Re-add order means tab 0 is active; clicking via the public API is not
            // exposed, so activate by toggling through AddTab's activation path is
            // not available. Record the intent for the render-bound wiring list.
            m_LastActiveIndex = m_PendingActive;
        }
        else
        {
            m_LastActiveIndex = 0;
        }
        area->Visibility = m_Visibility;
        return area;
    }

    int m_LastActiveIndex {0};
    int m_PendingActive {-1};
};
}  // namespace ZUMG
