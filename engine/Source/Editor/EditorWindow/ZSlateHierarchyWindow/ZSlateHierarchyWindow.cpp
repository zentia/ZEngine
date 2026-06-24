#include "ZSlateHierarchyWindow.h"

#include "Editor/EditorDragDrop/EditorDragDrop.h"
#include "Editor/EditorHierarchy/EditorHierarchyReparent.h"
#include "Editor/EditorLayout/EditorLayoutWindowIds.h"
#include "Editor/EditorSceneManager/EditorSceneManager.h"
#include "Editor/EditorUI/EditorSelection.h"
#include "Editor/ZSlate/Backend/EditorSlateHost.h"       // native input bus (P10)
#include "Editor/ZSlate/Backend/ZSlateEditorOverlay.h"  // native RHI backend (M3)
#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/Memory/MemoryManager.h"
#include "Runtime/Function/Framework/Component/Transform/Transform.h"
#include "Runtime/Function/Framework/Level/Level.h"
#include "Runtime/Function/Framework/World/WorldManager.h"
#include "ZSlate/Application/SlateApplication.h"
#include "ZSlate/Application/SlateDragDrop.h"
#include "ZSlate/Widgets/Panels/SBorder.h"
#include "ZSlate/Widgets/Layout/SBoxPanel.h"
#include "ZSlate/Widgets/Input/SButton.h"
#include "ZSlate/Widgets/SDropTarget.h"
#include "ZSlate/Widgets/SMenu.h"
#include "ZSlate/Widgets/Layout/SScrollBox.h"
#include "ZSlate/Widgets/Layout/SSpacer.h"
#include "ZSlate/Widgets/STextBlock.h"

#include <algorithm>
using namespace ZSlate;

namespace
{
const ZSlate::UIColor kSelectedColor(0.18f, 0.35f, 0.58f, 1.0f);
const ZSlate::UIColor kTransparent(0.0f, 0.0f, 0.0f, 0.0f);
const ZSlate::UIColor kHoverColor(0.25f, 0.27f, 0.32f, 0.65f);
const ZSlate::UIColor kNameColor(0.86f, 0.88f, 0.92f, 1.0f);
const ZSlate::UIColor kSelectedTextColor(0.97f, 0.98f, 1.0f, 1.0f);
const ZSlate::UIColor kToggleColor(0.62f, 0.65f, 0.72f, 1.0f);
const ZSlate::UIColor kPanelColor(0.10f, 0.10f, 0.12f, 1.0f);
const ZSlate::UIColor kDimColor(0.55f, 0.57f, 0.62f, 1.0f);

constexpr uint64_t kFnvOffset = 1469598103934665603ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

uint64_t HashU64(uint64_t value, uint64_t seed)
{
    for (int i = 0; i < 8; ++i)
    {
        seed ^= (value & 0xFF);
        seed *= kFnvPrime;
        value >>= 8;
    }
    return seed;
}

uint64_t HashBytes(const char* s, uint64_t seed)
{
    while (s != nullptr && *s != '\0')
    {
        seed ^= static_cast<unsigned char>(*s++);
        seed *= kFnvPrime;
    }
    return seed;
}

std::shared_ptr<STextBlock> MakeText(const std::string& text, float font_size, const ZSlate::UIColor& color)
{
    auto t = std::make_shared<STextBlock>();
    t->Text = text;
    t->FontSize = font_size;
    t->Color = color;
    t->Alignment = ZSlate::TextAnchor::MiddleLeft;
    return t;
}
}  // namespace

ZSlateHierarchyWindow::ZSlateHierarchyWindow(EditorUI* editor_ui)
    : EditorWindow(editor_ui, EditorLayoutWindowIds::kHierarchy)
{
}

ZSlateHierarchyWindow::TreeData ZSlateHierarchyWindow::BuildTree(Level* level) const
{
    TreeData tree;
    if (level == nullptr)
        return tree;

    const LevelObjectsMap& all = level->getAllGObjects();
    for (const auto& pair : all)
    {
        const std::shared_ptr<GameObject>& object = pair.second;
        if (object == nullptr || object->GetName().empty())
            continue;

        const GObjectID id = pair.first;
        const GObjectID parent_id = EditorHierarchyReparent::GetParentId(object);
        if (parent_id != k_invalid_gobject_id && all.count(parent_id) != 0)
            tree.children_by_parent[parent_id].push_back(id);
        else
            tree.roots.push_back(id);
    }

    const auto by_name = [level](GObjectID a, GObjectID b) {
        auto oa = level->GetGObjectByID(a).lock();
        auto ob = level->GetGObjectByID(b).lock();
        const eastl::string na = oa ? oa->GetName() : eastl::string();
        const eastl::string nb = ob ? ob->GetName() : eastl::string();
        if (na == nb)
            return a < b;
        return na < nb;
    };
    std::sort(tree.roots.begin(), tree.roots.end(), by_name);
    for (auto& kv : tree.children_by_parent)
        std::sort(kv.second.begin(), kv.second.end(), by_name);

    return tree;
}

uint64_t ZSlateHierarchyWindow::ComputeSignature(Level* level, const TreeData& tree, GObjectID selected) const
{
    uint64_t acc = kFnvOffset;
    if (level != nullptr)
    {
        // Order-independent fold over every object's (id, parent, name) so the
        // signature changes on add / remove / rename / reparent.
        for (const auto& pair : level->getAllGObjects())
        {
            const std::shared_ptr<GameObject>& object = pair.second;
            if (object == nullptr || object->GetName().empty())
                continue;
            uint64_t entry = HashU64(static_cast<uint64_t>(pair.first), kFnvOffset);
            entry = HashU64(static_cast<uint64_t>(EditorHierarchyReparent::GetParentId(object)), entry);
            entry = HashBytes(object->GetName().c_str(), entry);
            acc ^= entry;
        }
    }
    // Collapsed membership (order-independent) + the active selection.
    for (const GObjectID id : m_Collapsed)
        acc ^= HashU64(static_cast<uint64_t>(id), 0x9E3779B97F4A7C15ull);
    acc = HashU64(static_cast<uint64_t>(selected), acc);
    (void)tree;
    return acc;
}

void ZSlateHierarchyWindow::AddNodeRows(Level* level,
                                        const TreeData& tree,
                                        GObjectID object_id,
                                        GObjectID selected,
                                        int depth,
                                        float scale,
                                        const std::shared_ptr<SScrollBox>& list)
{
    auto object = level->GetGObjectByID(object_id).lock();
    if (object == nullptr)
        return;

    const auto children_it = tree.children_by_parent.find(object_id);
    const bool has_children = (children_it != tree.children_by_parent.end()) && !children_it->second.empty();
    const bool collapsed = IsCollapsed(object_id);
    const bool is_selected = (object_id == selected);

    const float font_size = 13.0f * scale;
    const float indent_unit = 14.0f * scale;
    const float toggle_w = 14.0f * scale;

    auto row = std::make_shared<SButton>();
    row->Padding = ZSlate::FMargin(2.0f * scale, 2.0f * scale);
    row->HAlign = EHorizontalAlignment::Fill;
    row->VAlign = EVerticalAlignment::Center;
    row->NormalColor = is_selected ? kSelectedColor : kTransparent;
    row->HoverColor = is_selected ? kSelectedColor : kHoverColor;
    row->PressedColor = kSelectedColor;
    row->OnClicked = [object_id]() { EditorSelection::SelectGameObject(object_id); };

    // Right-click: record the target; OnGUI opens the menu on the button-up edge.
    row->OnRightClicked = [this, object_id](const ZSlate::Vector2& screen_pos) {
        m_PendingContextObject = object_id;
        m_PendingContextPos = screen_pos;
        m_HasPendingContext = true;
    };

    // Drag source: carry this object's id so a drop target can reparent it.
    const std::string row_name(object->GetName().c_str());
    row->OnDragDetectedHandler = [object_id, row_name](const ZSlate::Vector2&) -> std::shared_ptr<FDragDropOperation> {
        auto op = std::make_shared<FDragDropOperation>();
        op->PayloadType = EditorDragDrop::kZSlateAssetPayloadGObjectId;
        op->Id = static_cast<uint64_t>(object_id);
        op->DecoratorText = row_name;
        return op;
    };

    // Drop target: accept another object (not self, not a cycle) and reparent it.
    row->CanAcceptDrop = [object_id](const std::shared_ptr<FDragDropOperation>& op) {
        if (op == nullptr || op->PayloadType != EditorDragDrop::kZSlateAssetPayloadGObjectId)
            return false;
        const auto dragged = static_cast<GObjectID>(op->Id);
        if (dragged == object_id)
            return false;
        Level* lvl = GET_SYSTEM(WorldManager)->getCurrentActiveLevel();
        if (lvl == nullptr)
            return false;
        return !EditorHierarchyReparent::WouldCreateCycle(lvl, dragged, object_id);
    };
    row->OnDropHandler = [this, object_id](const std::shared_ptr<FDragDropOperation>& op) {
        Level* lvl = GET_SYSTEM(WorldManager)->getCurrentActiveLevel();
        if (lvl != nullptr && op != nullptr &&
            EditorHierarchyReparent::Reparent(lvl, static_cast<GObjectID>(op->Id), object_id))
        {
            NotifyHierarchyStructureChanged(object_id);
        }
    };

    auto hb = std::make_shared<SHorizontalBox>();
    if (depth > 0)
        hb->AddSlot(std::make_shared<SSpacer>(ZSlate::Vector2(depth * indent_unit, 0.0f))).AutoSize();

    if (has_children)
    {
        auto toggle = std::make_shared<SButton>();
        toggle->Padding = ZSlate::FMargin(1.0f * scale, 0.0f);
        toggle->HAlign = EHorizontalAlignment::Center;
        toggle->VAlign = EVerticalAlignment::Center;
        toggle->NormalColor = kTransparent;
        toggle->HoverColor = ZSlate::UIColor(0.32f, 0.34f, 0.40f, 0.85f);
        toggle->PressedColor = kTransparent;
        toggle->SetContent(MakeText(collapsed ? ">" : "v", font_size, kToggleColor));
        toggle->OnClicked = [this, object_id]() {
            if (m_Collapsed.count(object_id) != 0)
                m_Collapsed.erase(object_id);
            else
                m_Collapsed.insert(object_id);
        };
        hb->AddSlot(toggle).AutoSize().SetVAlign(EVerticalAlignment::Center);
    }
    else
    {
        hb->AddSlot(std::make_shared<SSpacer>(ZSlate::Vector2(toggle_w, 0.0f))).AutoSize();
    }

    hb->AddSlot(std::make_shared<SSpacer>(ZSlate::Vector2(4.0f * scale, 0.0f))).AutoSize();
    hb->AddSlot(MakeText(std::string(object->GetName().c_str()),
                         font_size,
                         is_selected ? kSelectedTextColor : kNameColor))
        .Fill()
        .SetVAlign(EVerticalAlignment::Center);

    row->SetContent(hb);
    list->AddChild(row);

    if (has_children && !collapsed)
    {
        for (const GObjectID child_id : children_it->second)
            AddNodeRows(level, tree, child_id, selected, depth + 1, scale, list);
    }
}

void ZSlateHierarchyWindow::BuildMessage(const char* text, float scale)
{
    auto border = std::make_shared<SBorder>();
    border->BackgroundColor = kPanelColor;
    border->Padding = ZSlate::FMargin(10.0f * scale, 10.0f * scale);
    border->HAlign = EHorizontalAlignment::Fill;
    border->VAlign = EVerticalAlignment::Top;
    border->SetContent(MakeText(text, 13.0f * scale, kDimColor));
    m_Root = border;
}

void ZSlateHierarchyWindow::Rebuild(Level* level, const TreeData& tree, GObjectID selected, float scale)
{
    if (level == nullptr)
    {
        BuildMessage("No active level loaded. Check Console for ZWorld/ZLevel load errors.", scale);
        return;
    }
    if (tree.roots.empty())
    {
        BuildMessage("Active level has no objects.", scale);
        return;
    }

    auto border = std::make_shared<SBorder>();
    border->BackgroundColor = kPanelColor;
    border->Padding = ZSlate::FMargin(2.0f * scale, 2.0f * scale);
    border->HAlign = EHorizontalAlignment::Fill;
    border->VAlign = EVerticalAlignment::Fill;

    // Catch-all drop target behind the rows: dropping a row onto blank space
    // detaches it to the scene root (unparent). Dropping onto a row is handled
    // by the deeper row target first (reparent-under).
    auto blank_drop = std::make_shared<SDropTarget>();
    blank_drop->CanAcceptDrop = [](const std::shared_ptr<FDragDropOperation>& op) {
        if (op == nullptr || op->PayloadType != EditorDragDrop::kZSlateAssetPayloadGObjectId)
            return false;
        Level* lvl = GET_SYSTEM(WorldManager)->getCurrentActiveLevel();
        if (lvl == nullptr)
            return false;
        // No-op if the dragged object is already a root.
        auto dragged = lvl->GetGObjectByID(static_cast<GObjectID>(op->Id)).lock();
        if (dragged == nullptr)
            return false;
        return EditorHierarchyReparent::GetParentId(dragged) != k_invalid_gobject_id;
    };
    blank_drop->OnDropHandler = [this](const std::shared_ptr<FDragDropOperation>& op) {
        Level* lvl = GET_SYSTEM(WorldManager)->getCurrentActiveLevel();
        if (lvl != nullptr && op != nullptr &&
            EditorHierarchyReparent::Reparent(lvl, static_cast<GObjectID>(op->Id), k_invalid_gobject_id))
        {
            NotifyHierarchyStructureChanged(k_invalid_gobject_id);
        }
    };

    auto list = std::make_shared<SScrollBox>();
    for (const GObjectID root_id : tree.roots)
        AddNodeRows(level, tree, root_id, selected, 0, scale, list);

    blank_drop->SetContent(list);
    border->SetContent(blank_drop);
    m_Root = border;
}

GObjectID ZSlateHierarchyWindow::CreateEmpty(Level* level, GObjectID parent)
{
    if (level == nullptr)
        return k_invalid_gobject_id;

    // Unity "Create Empty": a GameObject with just a Transform.
    GameObject empty_template;
    empty_template.SetName("GameObject");
    empty_template.addComponent(MemoryManager::CreateObject<Transform>());

    const GObjectID created = level->CreateObject(empty_template);
    if (created == k_invalid_gobject_id)
        return k_invalid_gobject_id;

    if (parent != k_invalid_gobject_id)
        EditorHierarchyReparent::Reparent(level, created, parent);

    GET_SYSTEM(WorldManager)->MarkCurrentLevelDirty();
    NotifyHierarchyStructureChanged(parent);
    EditorSelection::SelectGameObject(created);
    return created;
}

void ZSlateHierarchyWindow::NotifyHierarchyStructureChanged(GObjectID expand_parent_id)
{
    m_ForceRebuild = true;
    if (expand_parent_id != k_invalid_gobject_id)
        m_Collapsed.erase(expand_parent_id);
}

void ZSlateHierarchyWindow::DeleteObject(GObjectID object_id)
{
    if (object_id == k_invalid_gobject_id)
        return;
    auto scene_manager = GET_SYSTEM(EditorSceneManager);
    if (scene_manager == nullptr)
        return;
    scene_manager->OnGObjectSelected(object_id);
    scene_manager->OnDeleteSelectedGObject();
    NotifyHierarchyStructureChanged(k_invalid_gobject_id);
}

void ZSlateHierarchyWindow::OpenContextMenu(GObjectID context_object, const ZSlate::Vector2& screen_pos, float scale)
{
    if (context_object != k_invalid_gobject_id)
    {
        // Right-click selects the target (Unity behaviour) so the Inspector follows.
        EditorSelection::SelectGameObject(context_object);
    }

    const GObjectID target = context_object;
    m_Popup.Open(screen_pos, scale, [this, target](SMenu& menu, float s) {
        menu.MinWidth = 170.0f * s;
        if (target != k_invalid_gobject_id)
        {
            menu.AddItem("Create Empty Child", [this, target]() {
                Level* lvl = GET_SYSTEM(WorldManager)->getCurrentActiveLevel();
                CreateEmpty(lvl, target);
            }, s);
            menu.AddItem("Delete", [this, target]() { DeleteObject(target); }, s);
            menu.AddSeparator(s);
        }
        menu.AddItem("Create Empty", [this]() {
            Level* lvl = GET_SYSTEM(WorldManager)->getCurrentActiveLevel();
            CreateEmpty(lvl, k_invalid_gobject_id);
        }, s);
    });
}

void ZSlateHierarchyWindow::OnGUI()
{
    // P10c: the process-wide ZSlate text measurer is installed once per frame by
    // ZSlateEditorOverlay::BeginFrameIfEnabled (renderer-backed), so panels no
    // longer install their own ImGui-font measurer.

    float ui_scale = ZSlate::EditorSlateHost::Get().GetUiScale();
    if (ui_scale < 0.5f)
        ui_scale = 1.0f;

    Level* level = GET_SYSTEM(WorldManager)->getCurrentActiveLevel();
    const TreeData tree = BuildTree(level);
    const GObjectID selected = EditorSelection::GetActiveGameObjectId();
    const uint64_t signature = ComputeSignature(level, tree, selected);
    const bool needs_rebuild = m_ForceRebuild || (m_Root == nullptr) || (ui_scale != m_BuiltScale) ||
                               (signature != m_BuiltSignature);
    if (needs_rebuild)
    {
        Rebuild(level, tree, selected, ui_scale);
        m_BuiltScale = ui_scale;
        m_BuiltSignature = signature;
        m_ForceRebuild = false;
        m_Input.Reset();
    }

    const float* native_rect = NativeRect();
    ZSlate::Vector2 pos(native_rect[0], native_rect[1]);
    ZSlate::Vector2 avail(native_rect[2], native_rect[3]);
    if (avail.x < 1.0f)
        avail.x = 1.0f;
    if (avail.y < 1.0f)
        avail.y = 1.0f;

    const ZSlate::UIRect region(pos.x, pos.y, avail.x, avail.y);
    const FGeometry geometry(ZSlate::Vector2(pos.x, pos.y), ZSlate::Vector2(avail.x, avail.y));

    auto& overlay = ZSlate::ZSlateEditorOverlay::Get();
    if (m_Root != nullptr)
    {
        BatchedUIRenderer& renderer = overlay.GetRenderer();
        overlay.BeginWindowGroup(ZSlate::ZSlateEditorOverlay::kZPanel);
        m_Root->CacheDesiredSize();
        FPaintContext ctx;
        ctx.Renderer = &renderer;
        ctx.LayerId = 0;
        renderer.pushClipRect(region, true);
        m_Root->Paint(ctx, geometry);
        renderer.popClipRect();
    }

    ZSlate::EditorSlateHost& host = ZSlate::EditorSlateHost::Get();
    const int surface_id = ZSlate::EditorSlateHost::HashId(m_Title);
    host.BeginSurface(surface_id, region, ZSlate::ESurfaceLayer::Panels);
    const ZSlate::Vector2 mouse = host.GetPointerPos();
    const bool over_canvas = host.IsSurfaceHovered(surface_id, mouse);
    const bool left_down = host.IsLeftDown();
    const bool right_down = host.IsRightDown();
    const float wheel = over_canvas ? host.GetWheelDelta() : 0.0f;
    const bool right_up_edge = !right_down && m_PrevRightDown;

    if (m_Popup.IsOpen())
    {
        const ZSlate::UIRect viewport_rect(host.GetDisplayPos().x, host.GetDisplayPos().y,
                                   host.GetDisplaySize().x, host.GetDisplaySize().y);
        m_Popup.Render(overlay.GetRenderer(), mouse, left_down, wheel, viewport_rect, 1);
    }
    else if (m_Root != nullptr)
    {
        m_Input.ProcessMouse(m_Root, mouse, over_canvas, left_down, wheel, right_down);

        if (right_up_edge && over_canvas)
        {
            if (m_HasPendingContext)
                OpenContextMenu(m_PendingContextObject, m_PendingContextPos, ui_scale);
            else
                OpenContextMenu(k_invalid_gobject_id, mouse, ui_scale);
            m_HasPendingContext = false;
        }
    }

    m_PrevLeftDown = left_down;
    m_PrevRightDown = right_down;
}
