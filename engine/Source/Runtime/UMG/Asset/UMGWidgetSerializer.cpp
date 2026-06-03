#include "Runtime/UMG/Asset/UMGWidgetSerializer.h"

#include "Runtime/UMG/Asset/UWidgetAsset.h"
#include "Runtime/UMG/Core/UMGProperties.h"
#include "Runtime/UMG/Core/UPanelWidget.h"
#include "Runtime/UMG/Core/UWidget.h"
#include "Runtime/UMG/Widgets/UBorder.h"
#include "Runtime/UMG/Widgets/UBoxPanel.h"
#include "Runtime/UMG/Widgets/UButton.h"
#include "Runtime/UMG/Widgets/UCheckBox.h"
#include "Runtime/UMG/Widgets/UEditableText.h"
#include "Runtime/UMG/Widgets/UImage.h"
#include "Runtime/UMG/Widgets/UOverlay.h"
#include "Runtime/UMG/Widgets/UScrollBox.h"
#include "Runtime/UMG/Widgets/USizeBox.h"
#include "Runtime/UMG/Widgets/USlider.h"
#include "Runtime/UMG/Widgets/USpacer.h"
#include "Runtime/UMG/Widgets/UTextBlock.h"

#include <vector>

namespace ZUMG
{
namespace
{
// --- Slot config <-> property bag -----------------------------------------
void WriteBoxSlotToBag(const UBoxPanel::FSlotConfig& cfg, UMGPropertyBag& bag)
{
    bag.SetInt("slot.size_rule", static_cast<int>(cfg.SizeRule));
    bag.SetFloat("slot.fill", cfg.FillSize);
    bag.SetFloat("slot.pad_l", cfg.Padding.Left);
    bag.SetFloat("slot.pad_t", cfg.Padding.Top);
    bag.SetFloat("slot.pad_r", cfg.Padding.Right);
    bag.SetFloat("slot.pad_b", cfg.Padding.Bottom);
    bag.SetInt("slot.halign", static_cast<int>(cfg.HAlign));
    bag.SetInt("slot.valign", static_cast<int>(cfg.VAlign));
}

void ReadBoxSlotFromBag(const UMGPropertyBag& bag, UBoxPanel::FSlotConfig& cfg)
{
    cfg.SizeRule = static_cast<ZSlate::ESizeRule>(bag.GetInt("slot.size_rule", static_cast<int>(cfg.SizeRule)));
    cfg.FillSize = bag.GetFloat("slot.fill", cfg.FillSize);
    cfg.Padding = ZSlate::FMargin(bag.GetFloat("slot.pad_l", 0.0f), bag.GetFloat("slot.pad_t", 0.0f),
                                  bag.GetFloat("slot.pad_r", 0.0f), bag.GetFloat("slot.pad_b", 0.0f));
    cfg.HAlign = static_cast<ZSlate::EHorizontalAlignment>(bag.GetInt("slot.halign", static_cast<int>(cfg.HAlign)));
    cfg.VAlign = static_cast<ZSlate::EVerticalAlignment>(bag.GetInt("slot.valign", static_cast<int>(cfg.VAlign)));
}

void WriteOverlaySlotToBag(const UOverlay::FSlotConfig& cfg, UMGPropertyBag& bag)
{
    bag.SetFloat("slot.pad_l", cfg.Padding.Left);
    bag.SetFloat("slot.pad_t", cfg.Padding.Top);
    bag.SetFloat("slot.pad_r", cfg.Padding.Right);
    bag.SetFloat("slot.pad_b", cfg.Padding.Bottom);
    bag.SetInt("slot.halign", static_cast<int>(cfg.HAlign));
    bag.SetInt("slot.valign", static_cast<int>(cfg.VAlign));
}

void ReadOverlaySlotFromBag(const UMGPropertyBag& bag, UOverlay::FSlotConfig& cfg)
{
    cfg.Padding = ZSlate::FMargin(bag.GetFloat("slot.pad_l", 0.0f), bag.GetFloat("slot.pad_t", 0.0f),
                                  bag.GetFloat("slot.pad_r", 0.0f), bag.GetFloat("slot.pad_b", 0.0f));
    cfg.HAlign = static_cast<ZSlate::EHorizontalAlignment>(bag.GetInt("slot.halign", static_cast<int>(cfg.HAlign)));
    cfg.VAlign = static_cast<ZSlate::EVerticalAlignment>(bag.GetInt("slot.valign", static_cast<int>(cfg.VAlign)));
}

// --- Node <-> bag flatten helpers -----------------------------------------
void FlattenBagToNode(const UMGPropertyBag& bag, FUMGWidgetNode& node)
{
    node.m_PropKeys.clear();
    node.m_PropTypes.clear();
    node.m_PropValues.clear();
    for (const FUMGProperty& p : bag.Entries())
    {
        node.m_PropKeys.push_back(eastl::string(p.Key.c_str()));
        const char tc[2] = {PropTypeToChar(p.Type), '\0'};
        node.m_PropTypes.push_back(eastl::string(tc));
        node.m_PropValues.push_back(eastl::string(p.Value.c_str()));
    }
}

UMGPropertyBag UnflattenNodeToBag(const FUMGWidgetNode& node)
{
    UMGPropertyBag bag;
    const size_t count = node.m_PropKeys.size();
    for (size_t i = 0; i < count; ++i)
    {
        const std::string key = node.m_PropKeys[i].c_str();
        const char type_char = (i < node.m_PropTypes.size() && !node.m_PropTypes[i].empty())
                                   ? node.m_PropTypes[i][0]
                                   : 's';
        const std::string value = (i < node.m_PropValues.size()) ? std::string(node.m_PropValues[i].c_str())
                                                                  : std::string();
        bag.SetRaw(key, PropTypeFromChar(type_char), value);
    }
    return bag;
}

// Depth-first emit. `parent_index` is the node index of the parent (-1 for root).
// `parent` / `child_pos` describe where `widget` sits in its parent so box /
// overlay slot config can be captured into the child node.
void EmitNode(const std::shared_ptr<UWidget>& widget,
              int parent_index,
              UWidget* parent,
              int child_pos,
              UWidgetAsset& out)
{
    if (!widget)
        return;

    FUMGWidgetNode node;
    node.m_ClassName = widget->GetWidgetClassName();
    node.m_Name = widget->Name.c_str();
    node.m_ParentIndex = parent_index;

    UMGPropertyBag bag;
    widget->SerializeProperties(bag);

    if (auto* box = dynamic_cast<UBoxPanel*>(parent))
        WriteBoxSlotToBag(box->GetSlotConfigAt(static_cast<size_t>(child_pos)), bag);
    else if (auto* overlay = dynamic_cast<UOverlay*>(parent))
        WriteOverlaySlotToBag(overlay->GetSlotConfigAt(static_cast<size_t>(child_pos)), bag);

    FlattenBagToNode(bag, node);

    const int my_index = static_cast<int>(out.m_Nodes.size());
    out.m_Nodes.push_back(std::move(node));

    if (auto* panel = dynamic_cast<UPanelWidget*>(widget.get()))
    {
        const int n = panel->GetChildrenCount();
        for (int i = 0; i < n; ++i)
            EmitNode(panel->GetChildWidgetAt(i), my_index, widget.get(), i, out);
    }
}
}  // namespace

std::shared_ptr<UWidget> CreateWidgetByClassName(const std::string& class_name)
{
    if (class_name == "TextBlock")
        return std::make_shared<UTextBlock>();
    if (class_name == "Button")
        return std::make_shared<UButton>();
    if (class_name == "Image")
        return std::make_shared<UImage>();
    if (class_name == "CheckBox")
        return std::make_shared<UCheckBox>();
    if (class_name == "Slider")
        return std::make_shared<USlider>();
    if (class_name == "EditableText")
        return std::make_shared<UEditableText>();
    if (class_name == "Spacer")
        return std::make_shared<USpacer>();
    if (class_name == "VerticalBox")
        return std::make_shared<UVerticalBox>();
    if (class_name == "HorizontalBox")
        return std::make_shared<UHorizontalBox>();
    if (class_name == "Border")
        return std::make_shared<UBorder>();
    if (class_name == "SizeBox")
        return std::make_shared<USizeBox>();
    if (class_name == "ScrollBox")
        return std::make_shared<UScrollBox>();
    if (class_name == "Overlay")
        return std::make_shared<UOverlay>();
    return nullptr;
}

void SerializeWidgetTree(const std::shared_ptr<UWidget>& root, UWidgetAsset& out_asset)
{
    out_asset.m_Nodes.clear();
    out_asset.m_SchemaVersion = UWidgetAsset::kCurrentSchemaVersion;
    EmitNode(root, -1, nullptr, 0, out_asset);
}

std::shared_ptr<UWidget> BuildWidgetTree(const UWidgetAsset& asset)
{
    const size_t count = asset.m_Nodes.size();
    if (count == 0)
        return nullptr;

    std::vector<std::shared_ptr<UWidget>> widgets(count);
    for (size_t i = 0; i < count; ++i)
    {
        const FUMGWidgetNode& node = asset.m_Nodes[i];
        std::shared_ptr<UWidget> w = CreateWidgetByClassName(node.m_ClassName.c_str());
        if (!w)
            continue;
        w->Name = node.m_Name.c_str();
        const UMGPropertyBag bag = UnflattenNodeToBag(node);
        w->DeserializeProperties(bag);
        widgets[i] = std::move(w);
    }

    // Attach in ascending node index so per-parent sibling order is preserved
    // (DFS pre-order guarantees siblings of one parent are index-ordered).
    std::shared_ptr<UWidget> root;
    for (size_t i = 0; i < count; ++i)
    {
        const FUMGWidgetNode& node = asset.m_Nodes[i];
        std::shared_ptr<UWidget>& child = widgets[i];
        if (!child)
            continue;

        if (node.m_ParentIndex < 0)
        {
            if (!root)
                root = child;
            continue;
        }

        const size_t parent_index = static_cast<size_t>(node.m_ParentIndex);
        if (parent_index >= count || !widgets[parent_index])
            continue;
        UWidget* parent = widgets[parent_index].get();

        const UMGPropertyBag slot_bag = UnflattenNodeToBag(node);
        if (auto* box = dynamic_cast<UBoxPanel*>(parent))
        {
            UBoxPanel::FSlotConfig& cfg = box->AddSlot(child);
            ReadBoxSlotFromBag(slot_bag, cfg);
        }
        else if (auto* overlay = dynamic_cast<UOverlay*>(parent))
        {
            UOverlay::FSlotConfig& cfg = overlay->AddSlot(child);
            ReadOverlaySlotFromBag(slot_bag, cfg);
        }
        else if (auto* panel = dynamic_cast<UPanelWidget*>(parent))
        {
            panel->AddChildWidget(child);
        }
    }

    return root;
}
}  // namespace ZUMG
