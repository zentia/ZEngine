#include "ZSlateUMGDesignerWindow.h"

#include "Editor/EditorLayout/EditorLayoutWindowIds.h"
#include "Editor/ZSlate/Backend/EditorSlateHost.h"
#include "Editor/ZSlate/Backend/ZSlateEditorOverlay.h"
#include "Runtime/Core/Base/Macro.h"
#include "ZSlate/Application/SlateApplication.h"
#include "ZSlate/Application/SlateDragDrop.h"
#include "ZSlate/Widgets/Panels/SBorder.h"
#include "ZSlate/Widgets/Layout/SBox.h"
#include "ZSlate/Widgets/SBoxPanel.h"
#include "ZSlate/Widgets/Input/SButton.h"
#include "ZSlate/Widgets/SCheckBox.h"
#include "ZSlate/Widgets/SColorPicker.h"
#include "ZSlate/Widgets/SDragFloat.h"
#include "ZSlate/Widgets/SEditableTextBox.h"
#include "ZSlate/Widgets/SScrollBox.h"
#include "ZSlate/Widgets/STextBlock.h"
#include "Runtime/UI/Render/BatchedUIRenderer.h"
#include "Runtime/UI/Render/UIRenderer.h"
#include "Runtime/UMG/Asset/UMGAssetIO.h"
#include "Runtime/UMG/Asset/UMGWidgetSerializer.h"
#include "Runtime/UMG/Core/UMGProperties.h"
#include "Runtime/UMG/Core/UPanelWidget.h"
#include "Runtime/UMG/Core/UWidget.h"
#include "Runtime/UMG/Widgets/UBoxPanel.h"
#include "Runtime/UMG/Widgets/UOverlay.h"

#include <cmath>
#include <vector>

using namespace ZSlate;
using namespace ZUMG;

namespace
{
// Creatable widget classes for the type cycler (matches UWidget::GetWidgetClassName).
const char* const kCreatableTypes[] = {
    "VerticalBox", "HorizontalBox", "Overlay", "Border", "SizeBox", "ScrollBox",
    "Button", "TextBlock", "Image", "CheckBox", "Slider", "EditableText", "Spacer",
};
constexpr int kCreatableCount = static_cast<int>(sizeof(kCreatableTypes) / sizeof(kCreatableTypes[0]));

// Drag payload for a hierarchy reparent gesture (UMGDesignerWindow's ImGui
// "UMG_NODE" payload equivalent, carried through SButton's drag hooks).
struct UMGDragOp : public FDragDropOperation
{
    std::weak_ptr<UWidget> Node;
};

void InvalidateTree(UWidget* w)
{
    if (w == nullptr)
        return;
    w->InvalidateWidget();
    if (auto* panel = dynamic_cast<UPanelWidget*>(w))
    {
        const int n = panel->GetChildrenCount();
        for (int i = 0; i < n; ++i)
            InvalidateTree(panel->GetChildWidgetAt(i).get());
    }
}

std::shared_ptr<STextBlock> MakeLabel(const std::string& text, float font, const ZSlate::UIColor& color)
{
    auto t = std::make_shared<STextBlock>();
    t->Text = text;
    t->FontSize = font;
    t->Color = color;
    return t;
}

std::shared_ptr<SButton> MakeButton(const std::string& label, float font, std::function<void()> on_click)
{
    auto btn = std::make_shared<SButton>();
    btn->Padding = ZSlate::FMargin(8.0f, 4.0f);
    btn->SetContent(MakeLabel(label, font, ZSlate::UIColor(0.90f, 0.91f, 0.94f, 1.0f)));
    btn->OnClicked = std::move(on_click);
    return btn;
}
}  // namespace

ZSlateUMGDesignerWindow::ZSlateUMGDesignerWindow(EditorUI* editor_ui)
    : EditorWindow(editor_ui, EditorLayoutWindowIds::kUMGDesigner)
{
}

void ZSlateUMGDesignerWindow::EnsureRoot()
{
    if (!m_Root)
    {
        auto root = std::make_shared<UVerticalBox>();
        root->Name = "Root";
        m_Root = root;
        m_Selected = m_Root;
        MarkUiDirty();
    }
}

std::shared_ptr<UWidget> ZSlateUMGDesignerWindow::FindParent(const std::shared_ptr<UWidget>& node) const
{
    if (!m_Root || m_Root == node)
        return nullptr;
    std::vector<std::shared_ptr<UWidget>> stack {m_Root};
    while (!stack.empty())
    {
        std::shared_ptr<UWidget> cur = stack.back();
        stack.pop_back();
        if (auto* panel = dynamic_cast<UPanelWidget*>(cur.get()))
        {
            const int n = panel->GetChildrenCount();
            for (int i = 0; i < n; ++i)
            {
                std::shared_ptr<UWidget> child = panel->GetChildWidgetAt(i);
                if (child == node)
                    return cur;
                if (child)
                    stack.push_back(child);
            }
        }
    }
    return nullptr;
}

bool ZSlateUMGDesignerWindow::IsDescendant(const std::shared_ptr<UWidget>& root, const UWidget* node) const
{
    if (!root || node == nullptr)
        return false;
    std::vector<UWidget*> stack {root.get()};
    while (!stack.empty())
    {
        UWidget* cur = stack.back();
        stack.pop_back();
        if (cur == node)
            return true;
        if (auto* panel = dynamic_cast<UPanelWidget*>(cur))
            for (int i = 0; i < panel->GetChildrenCount(); ++i)
                if (auto c = panel->GetChildWidgetAt(i))
                    stack.push_back(c.get());
    }
    return false;
}

std::shared_ptr<UWidget> ZSlateUMGDesignerWindow::ResolveByRaw(const UWidget* raw) const
{
    if (!m_Root || raw == nullptr)
        return nullptr;
    std::vector<std::shared_ptr<UWidget>> stack {m_Root};
    while (!stack.empty())
    {
        std::shared_ptr<UWidget> cur = stack.back();
        stack.pop_back();
        if (cur.get() == raw)
            return cur;
        if (auto* panel = dynamic_cast<UPanelWidget*>(cur.get()))
            for (int i = 0; i < panel->GetChildrenCount(); ++i)
                if (auto c = panel->GetChildWidgetAt(i))
                    stack.push_back(c);
    }
    return nullptr;
}

void ZSlateUMGDesignerWindow::AddWidgetToSelection(const std::string& class_name)
{
    EnsureRoot();
    std::shared_ptr<UWidget> sel = SelectedShared();
    std::shared_ptr<UWidget> target = sel ? sel : m_Root;

    UPanelWidget* panel = dynamic_cast<UPanelWidget*>(target.get());
    if (panel == nullptr)
    {
        std::shared_ptr<UWidget> parent = FindParent(target);
        target = parent ? parent : m_Root;
        panel = dynamic_cast<UPanelWidget*>(target.get());
    }
    if (panel == nullptr)
        return;

    std::shared_ptr<UWidget> child = CreateWidgetByClassName(class_name);
    if (!child)
        return;
    child->Name = class_name;

    if (auto* box = dynamic_cast<UBoxPanel*>(panel))
        box->AddSlot(child).AutoSize().SetPadding(ZSlate::FMargin(0.0f, 0.0f, 0.0f, 4.0f));
    else if (auto* overlay = dynamic_cast<UOverlay*>(panel))
        overlay->AddSlot(child);
    else
        panel->AddChildWidget(child);

    m_Selected = child;
    MarkUiDirty();
}

void ZSlateUMGDesignerWindow::DeleteSelected()
{
    std::shared_ptr<UWidget> sel = SelectedShared();
    if (!sel || sel == m_Root)
        return;
    std::shared_ptr<UWidget> parent_w = FindParent(sel);
    auto* panel = dynamic_cast<UPanelWidget*>(parent_w.get());
    if (panel == nullptr)
        return;
    const int n = panel->GetChildrenCount();
    for (int i = 0; i < n; ++i)
    {
        if (panel->GetChildWidgetAt(i) == sel)
        {
            panel->RemoveChildAt(i);
            break;
        }
    }
    m_Selected = parent_w;
    MarkUiDirty();
}

void ZSlateUMGDesignerWindow::ApplyPendingReparent()
{
    if (!m_PendingReparentNode || !m_PendingReparentTarget)
        return;
    std::shared_ptr<UWidget> node = m_PendingReparentNode;
    std::shared_ptr<UPanelWidget> target = m_PendingReparentTarget;
    m_PendingReparentNode.reset();
    m_PendingReparentTarget.reset();

    if (std::shared_ptr<UWidget> old_parent = FindParent(node))
    {
        if (auto* op = dynamic_cast<UPanelWidget*>(old_parent.get()))
        {
            for (int i = 0; i < op->GetChildrenCount(); ++i)
            {
                if (op->GetChildWidgetAt(i) == node)
                {
                    op->RemoveChildAt(i);
                    break;
                }
            }
        }
    }
    if (auto* box = dynamic_cast<UBoxPanel*>(target.get()))
        box->AddSlot(node).AutoSize().SetPadding(ZSlate::FMargin(0.0f, 0.0f, 0.0f, 4.0f));
    else if (auto* overlay = dynamic_cast<UOverlay*>(target.get()))
        overlay->AddSlot(node);
    else
        target->AddChildWidget(node);
    m_Selected = node;
    MarkUiDirty();
}

void ZSlateUMGDesignerWindow::BuildHierarchyRows(const std::shared_ptr<SScrollBox>& box,
                                                 const std::shared_ptr<UWidget>& widget,
                                                 int depth,
                                                 float scale)
{
    if (!widget)
        return;

    const float font = 14.0f * scale;
    std::string label = widget->GetWidgetClassName();
    if (!widget->Name.empty() && widget->Name != label)
        label += "  (" + widget->Name + ")";

    const bool is_selected = (SelectedShared() == widget);
    UWidget* raw = widget.get();
    std::weak_ptr<UWidget> wnode = widget;

    auto row = std::make_shared<SButton>();
    row->Padding = ZSlate::FMargin(6.0f + depth * 14.0f * scale, 3.0f, 6.0f, 3.0f);
    row->HAlign = EHorizontalAlignment::Left;
    if (is_selected)
    {
        row->NormalColor = ZSlate::UIColor(0.20f, 0.36f, 0.58f, 1.0f);
        row->HoverColor = ZSlate::UIColor(0.24f, 0.42f, 0.66f, 1.0f);
    }
    else
    {
        row->NormalColor = ZSlate::UIColor(0.15f, 0.15f, 0.17f, 1.0f);
        row->HoverColor = ZSlate::UIColor(0.22f, 0.22f, 0.26f, 1.0f);
    }
    row->SetContent(MakeLabel(label, font, ZSlate::UIColor(0.90f, 0.91f, 0.94f, 1.0f)));
    row->OnClicked = [this, wnode]() {
        m_Selected = wnode;
        MarkUiDirty();
    };

    // Drag SOURCE: non-root nodes can be dragged onto a panel to reparent.
    if (widget != m_Root)
    {
        row->OnDragDetectedHandler = [wnode, label](const ZSlate::Vector2&) -> std::shared_ptr<FDragDropOperation> {
            auto op = std::make_shared<UMGDragOp>();
            op->PayloadType = "UMG_NODE";
            op->DecoratorText = label;
            op->Node = wnode;
            return op;
        };
    }
    // Drop TARGET: any panel widget accepts a reparent that is not a cycle.
    if (dynamic_cast<UPanelWidget*>(raw) != nullptr)
    {
        row->CanAcceptDrop = [this, raw](const std::shared_ptr<FDragDropOperation>& op) -> bool {
            if (!op || op->PayloadType != "UMG_NODE")
                return false;
            auto* drag = static_cast<UMGDragOp*>(op.get());
            std::shared_ptr<UWidget> dragged = drag->Node.lock();
            if (!dragged || dragged.get() == raw || dragged == m_Root)
                return false;
            // Reject dropping a node into one of its own descendants.
            return !IsDescendant(dragged, raw);
        };
        row->OnDropHandler = [this, raw](const std::shared_ptr<FDragDropOperation>& op) {
            if (!op || op->PayloadType != "UMG_NODE")
                return;
            auto* drag = static_cast<UMGDragOp*>(op.get());
            std::shared_ptr<UWidget> dragged = drag->Node.lock();
            std::shared_ptr<UWidget> target = ResolveByRaw(raw);
            if (!dragged || !target || dragged.get() == raw || dragged == m_Root)
                return;
            if (IsDescendant(dragged, raw))
                return;
            m_PendingReparentNode = dragged;
            m_PendingReparentTarget = std::dynamic_pointer_cast<UPanelWidget>(target);
        };
    }

    box->AddChild(row);

    if (auto* panel = dynamic_cast<UPanelWidget*>(raw))
    {
        const int n = panel->GetChildrenCount();
        for (int i = 0; i < n; ++i)
            BuildHierarchyRows(box, panel->GetChildWidgetAt(i), depth + 1, scale);
    }
}

void ZSlateUMGDesignerWindow::BuildPropertyRows(const std::shared_ptr<SScrollBox>& box, float scale)
{
    const float font = 14.0f * scale;
    const float label_w = 96.0f * scale;
    const ZSlate::UIColor label_color(0.72f, 0.74f, 0.80f, 1.0f);

    std::shared_ptr<UWidget> sel = SelectedShared();
    if (!sel)
    {
        box->AddChild(MakeLabel("No widget selected", font, ZSlate::UIColor(0.55f, 0.56f, 0.62f, 1.0f)));
        return;
    }

    box->AddChild(MakeLabel(std::string("Class: ") + sel->GetWidgetClassName(), font, label_color));

    // A labelled row: fixed-width label column + an editor that fills the rest.
    auto make_row = [&](const std::string& key, const std::shared_ptr<SWidget>& editor) {
        auto row = std::make_shared<SHorizontalBox>();
        auto label_box = std::make_shared<SBox>();
        label_box->WidthOverride = label_w;
        label_box->VAlign = EVerticalAlignment::Center;
        label_box->SetContent(MakeLabel(key, font, label_color));
        row->AddSlot(label_box).AutoSize().SetVAlign(EVerticalAlignment::Center);
        row->AddSlot(editor).Fill(1.0f).SetVAlign(EVerticalAlignment::Center).SetPadding(ZSlate::FMargin(4.0f, 0.0f, 0.0f, 0.0f));
        box->AddChild(row);
    };

    std::weak_ptr<UWidget> wsel = sel;

    // Name (UWidget::Name, not part of the property bag).
    {
        auto name_box = std::make_shared<SEditableTextBox>();
        name_box->Text = sel->Name;
        name_box->FontSize = font;
        name_box->OnTextCommitted = [this, wsel](const std::string& t) {
            if (auto s = wsel.lock())
            {
                s->Name = t;
                MarkUiDirty();
            }
        };
        make_row("name", name_box);
    }

    // Generic property binding via the widget's typed bag.
    UMGPropertyBag bag;
    sel->SerializeProperties(bag);

    // Writes one field back into the live widget (re-serialising the whole bag so
    // we never clobber other fields). Preview-only dirty -> the chrome (and thus
    // this editor) survives a continuous drag.
    auto apply = [this, wsel](std::function<void(UMGPropertyBag&)> set) {
        std::shared_ptr<UWidget> s = wsel.lock();
        if (!s)
            return;
        UMGPropertyBag b;
        s->SerializeProperties(b);
        set(b);
        s->DeserializeProperties(b);
        MarkPreviewDirty();
    };

    for (const FUMGProperty& entry : bag.Entries())
    {
        const std::string key = entry.Key;
        switch (entry.Type)
        {
            case EPropType::Float:
            {
                auto df = std::make_shared<SDragFloat>();
                df->Value = bag.GetFloat(key);
                df->Speed = 0.05f;
                df->Format = "%.3f";
                df->FontSize = font;
                df->OnValueChanged = [apply, key](float v) { apply([&](UMGPropertyBag& b) { b.SetFloat(key, v); }); };
                make_row(key, df);
                break;
            }
            case EPropType::Int:
            {
                auto df = std::make_shared<SDragFloat>();
                df->Value = static_cast<float>(bag.GetInt(key));
                df->Speed = 0.2f;
                df->Format = "%.0f";
                df->FontSize = font;
                df->OnValueChanged = [apply, key](float v) {
                    apply([&](UMGPropertyBag& b) { b.SetInt(key, static_cast<int>(std::lround(v))); });
                };
                make_row(key, df);
                break;
            }
            case EPropType::Bool:
            {
                auto cb = std::make_shared<SCheckBox>();
                cb->Checked = bag.GetBool(key);
                cb->BoxSize = 16.0f * scale;
                cb->OnCheckStateChanged = [apply, key](bool v) {
                    apply([&](UMGPropertyBag& b) { b.SetBool(key, v); });
                };
                make_row(key, cb);
                break;
            }
            case EPropType::String:
            {
                auto tb = std::make_shared<SEditableTextBox>();
                tb->Text = bag.GetString(key);
                tb->FontSize = font;
                tb->OnTextCommitted = [apply, key](const std::string& t) {
                    apply([&](UMGPropertyBag& b) { b.SetString(key, t); });
                };
                make_row(key, tb);
                break;
            }
            case EPropType::Vec2:
            {
                const ZSlate::Vector2 v = bag.GetVec2(key);
                auto pair = std::make_shared<SHorizontalBox>();
                auto dx = std::make_shared<SDragFloat>();
                dx->Value = v.x;
                dx->Speed = 0.05f;
                dx->Format = "%.2f";
                dx->FontSize = font;
                dx->OnValueChanged = [apply, key](float nv) {
                    apply([&](UMGPropertyBag& b) {
                        ZSlate::Vector2 cur = b.GetVec2(key);
                        cur.x = nv;
                        b.SetVec2(key, cur);
                    });
                };
                auto dy = std::make_shared<SDragFloat>();
                dy->Value = v.y;
                dy->Speed = 0.05f;
                dy->Format = "%.2f";
                dy->FontSize = font;
                dy->OnValueChanged = [apply, key](float nv) {
                    apply([&](UMGPropertyBag& b) {
                        ZSlate::Vector2 cur = b.GetVec2(key);
                        cur.y = nv;
                        b.SetVec2(key, cur);
                    });
                };
                pair->AddSlot(dx).Fill(1.0f);
                pair->AddSlot(dy).Fill(1.0f).SetPadding(ZSlate::FMargin(4.0f, 0.0f, 0.0f, 0.0f));
                make_row(key, pair);
                break;
            }
            case EPropType::Color:
            {
                const ZSlate::UIColor c = bag.GetColor(key);
                auto picker = std::make_shared<SColorPicker>();
                picker->SquareSize = 130.0f * scale;
                picker->SetColorRGBA(c.x, c.y, c.z, c.w);
                picker->OnColorChanged = [apply, key](float r, float g, float bl, float a) {
                    apply([&](UMGPropertyBag& b) { b.SetColor(key, ZSlate::UIColor(r, g, bl, a)); });
                };
                // The picker is tall; give it a full-width row with the key above it.
                box->AddChild(MakeLabel(key, font, label_color));
                box->AddChild(picker);
                break;
            }
        }
    }
}

void ZSlateUMGDesignerWindow::BuildChrome(float scale)
{
    EnsureRoot();

    const float font = 14.0f * scale;
    const ZSlate::UIColor panel_bg(0.10f, 0.10f, 0.12f, 1.0f);
    const ZSlate::UIColor header_color(0.78f, 0.80f, 0.86f, 1.0f);

    auto root = std::make_shared<SVerticalBox>();

    // ---- Toolbar -----------------------------------------------------------
    auto toolbar = std::make_shared<SHorizontalBox>();
    auto pad = [](SBoxPanel::FSlot& slot) -> SBoxPanel::FSlot& {
        return slot.AutoSize().SetVAlign(EVerticalAlignment::Center).SetPadding(ZSlate::FMargin(0.0f, 0.0f, 4.0f, 0.0f));
    };

    pad(toolbar->AddSlot(MakeButton("New", font, [this]() {
        m_Root.reset();
        m_Selected.reset();
        EnsureRoot();
        MarkUiDirty();
    })));

    // Type cycler: clicking advances to the next creatable class.
    pad(toolbar->AddSlot(MakeButton(std::string("Type: ") + kCreatableTypes[m_AddTypeIndex], font, [this]() {
        m_AddTypeIndex = (m_AddTypeIndex + 1) % kCreatableCount;
        MarkUiDirty();
    })));
    pad(toolbar->AddSlot(MakeButton("Add", font, [this]() {
        AddWidgetToSelection(kCreatableTypes[m_AddTypeIndex]);
    })));
    pad(toolbar->AddSlot(MakeButton("Delete", font, [this]() { DeleteSelected(); })));

    m_UrlBox = std::make_shared<SEditableTextBox>();
    m_UrlBox->Text = m_AssetUrl;
    m_UrlBox->FontSize = font;
    m_UrlBox->MinWidth = 200.0f * scale;
    m_UrlBox->OnTextChanged = [this](const std::string& t) { m_AssetUrl = t; };
    m_UrlBox->OnTextCommitted = [this](const std::string& t) { m_AssetUrl = t; };
    pad(toolbar->AddSlot(m_UrlBox)).SetPadding(ZSlate::FMargin(12.0f, 0.0f, 4.0f, 0.0f));

    pad(toolbar->AddSlot(MakeButton("Save", font, [this]() {
        EnsureRoot();
        if (ZUMG::SaveWidgetTreeAsset(m_Root, m_AssetUrl))
        {
            LOG_INFO(ZEditor, "UMG Designer: saved '{}'", m_AssetUrl);
        }
        else
        {
            LOG_ERROR(ZEditor, "UMG Designer: save failed '{}'", m_AssetUrl);
        }
    })));
    pad(toolbar->AddSlot(MakeButton("Load", font, [this]() {
        if (auto loaded = ZUMG::LoadWidgetTreeAsset(m_AssetUrl))
        {
            m_Root = loaded;
            m_Selected = m_Root;
            MarkUiDirty();
            LOG_INFO(ZEditor, "UMG Designer: loaded '{}'", m_AssetUrl);
        }
        else
        {
            LOG_ERROR(ZEditor, "UMG Designer: load failed '{}'", m_AssetUrl);
        }
    })));

    root->AddSlot(toolbar).AutoSize().SetPadding(ZSlate::FMargin(4.0f, 4.0f, 4.0f, 6.0f));

    // ---- Body: hierarchy | properties --------------------------------------
    auto body = std::make_shared<SHorizontalBox>();

    auto make_panel = [&](const std::string& title, const std::shared_ptr<SScrollBox>& scroll) {
        auto border = std::make_shared<SBorder>();
        border->BackgroundColor = panel_bg;
        border->Padding = ZSlate::FMargin(6.0f, 6.0f, 6.0f, 6.0f);
        auto col = std::make_shared<SVerticalBox>();
        col->AddSlot(MakeLabel(title, font, header_color)).AutoSize().SetPadding(ZSlate::FMargin(0.0f, 0.0f, 0.0f, 6.0f));
        col->AddSlot(scroll).Fill(1.0f);
        border->SetContent(col);
        return border;
    };

    auto hierarchy = std::make_shared<SScrollBox>();
    BuildHierarchyRows(hierarchy, m_Root, 0, scale);

    auto properties = std::make_shared<SScrollBox>();
    BuildPropertyRows(properties, scale);

    body->AddSlot(make_panel("Hierarchy", hierarchy)).Fill(1.0f).SetPadding(ZSlate::FMargin(2.0f, 0.0f, 3.0f, 0.0f));
    body->AddSlot(make_panel("Properties", properties)).Fill(1.0f).SetPadding(ZSlate::FMargin(3.0f, 0.0f, 2.0f, 0.0f));

    root->AddSlot(body).Fill(1.0f);

    m_Chrome = root;
}

void ZSlateUMGDesignerWindow::RebuildPreviewIfDirty()
{
    if (!m_PreviewDirty)
        return;
    m_PreviewDirty = false;
    if (m_Root)
    {
        InvalidateTree(m_Root.get());
        m_PreviewSlate = m_Root->TakeWidget();
    }
    else
    {
        m_PreviewSlate.reset();
    }
    m_PreviewInput.Reset();
}

void ZSlateUMGDesignerWindow::OnGUI()
{
    float ui_scale = ZSlate::EditorSlateHost::Get().GetUiScale();
    if (ui_scale < 0.5f)
        ui_scale = 1.0f;

    const bool rebuild_chrome = (m_Chrome == nullptr) || m_UiDirty || (ui_scale != m_BuiltScale);
    if (rebuild_chrome)
    {
        m_UiDirty = false;
        BuildChrome(ui_scale);
        m_BuiltScale = ui_scale;
        m_ChromeInput.Reset();
    }

    RebuildPreviewIfDirty();

    float pos_x = 0.0f;
    float pos_y = 0.0f;
    float avail_w = 0.0f;
    float avail_h = 0.0f;
    // Native dock hosting is unconditional: ReconcileNativeTreeWithOpenWindows (run
    // before any panel OnGUI) guarantees an open window is in the dock tree, so the
    // leaf rect always comes from NativeRect().
    const float* native_rect = NativeRect();
    pos_x = native_rect[0];
    pos_y = native_rect[1];
    avail_w = native_rect[2];
    avail_h = native_rect[3];
    if (avail_w < 1.0f)
        avail_w = 1.0f;
    if (avail_h < 1.0f)
        avail_h = 1.0f;

    float preview_w = avail_w * 0.42f;
    if (preview_w < 120.0f)
        preview_w = std::min(avail_w * 0.5f, 120.0f);
    const float chrome_w = avail_w - preview_w;
    const float split_x = pos_x + chrome_w;

    const ZSlate::UIRect chrome_region(pos_x, pos_y, chrome_w, avail_h);
    const FGeometry chrome_geom(ZSlate::Vector2(pos_x, pos_y), ZSlate::Vector2(chrome_w, avail_h));
    const ZSlate::UIRect preview_region(split_x, pos_y, preview_w, avail_h);
    const FGeometry preview_geom(ZSlate::Vector2(split_x, pos_y), ZSlate::Vector2(preview_w, avail_h));
    const ZSlate::UIRect panel_region(pos_x, pos_y, avail_w, avail_h);
    const ZSlate::UIColor preview_bg(0.11f, 0.11f, 0.13f, 1.0f);

    auto& overlay = ZSlate::ZSlateEditorOverlay::Get();

    auto paint_all = [&](UIRenderer& renderer) {
        FPaintContext ctx;
        ctx.Renderer = &renderer;
        ctx.LayerId = 0;

        if (m_Chrome)
        {
            m_Chrome->CacheDesiredSize();
            renderer.pushClipRect(chrome_region, true);
            m_Chrome->Paint(ctx, chrome_geom);
            renderer.popClipRect();
        }

        renderer.drawQuad(preview_region, preview_bg);
        if (m_PreviewSlate)
        {
            m_PreviewSlate->CacheDesiredSize();
            renderer.pushClipRect(preview_region, true);
            m_PreviewSlate->Paint(ctx, preview_geom);
            renderer.popClipRect();
        }
    };

    {
        BatchedUIRenderer& renderer = overlay.GetRenderer();
        overlay.BeginWindowGroup(ZSlate::ZSlateEditorOverlay::kZPanel);
        paint_all(renderer);
    }

    // ---- Input -------------------------------------------------------------
    ZSlate::EditorSlateHost& host = ZSlate::EditorSlateHost::Get();
    const int surface_id = ZSlate::EditorSlateHost::HashId(m_Title);
    host.BeginSurface(surface_id, panel_region, ZSlate::ESurfaceLayer::Panels);
    const ZSlate::Vector2 mouse = host.GetPointerPos();
    const bool over_canvas = host.IsSurfaceHovered(surface_id, mouse);
    const bool left_down = host.IsLeftDown();
    const float wheel = over_canvas ? host.GetWheelDelta() : 0.0f;

    m_ChromeInput.ProcessMouse(m_Chrome, mouse, over_canvas, left_down, wheel);
    if (m_PreviewSlate)
        m_PreviewInput.ProcessMouse(m_PreviewSlate, mouse, over_canvas, left_down, wheel);

    SlateInputRouter* focus_router = nullptr;
    if (m_ChromeInput.HasKeyboardFocus())
        focus_router = &m_ChromeInput;
    else if (m_PreviewInput.HasKeyboardFocus())
        focus_router = &m_PreviewInput;
    if (focus_router != nullptr)
    {
        for (unsigned int cp : host.GetCharsThisFrame())
            focus_router->ProcessChar(cp);
        for (EKey key : host.GetKeysThisFrame())
        {
            if (key == EKey::Backspace || key == EKey::Enter)
                focus_router->ProcessKey(key);
        }
    }

    // Apply deferred reparent after routing (avoids mutating the tree mid-walk).
    ApplyPendingReparent();
}
