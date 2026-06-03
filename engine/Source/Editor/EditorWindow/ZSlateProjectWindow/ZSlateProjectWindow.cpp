#include "ZSlateProjectWindow.h"

#include "Editor/EditorAsset/EditorAssetManager.h"
#include "Editor/EditorDragDrop/EditorDragDrop.h"
#include "Editor/EditorLayout/EditorLayoutWindowIds.h"
#include "Editor/EditorSceneManager/EditorSceneManager.h"
#include "Editor/EditorUI/ProjectWindow/ProjectAssetActions.h"
#include "Editor/EditorUI/ProjectWindow/ProjectDragDrop.h"
#include "Editor/EditorUI/ProjectWindow/ProjectWindowHelpers.h"
#include "Editor/Menu/AssetsMenu.h"
#include "Editor/Platform/Interface/EditorUtility.h"
#include "Editor/ZSlate/Backend/EditorSlateHost.h"       // native input bus (P10)
#include "Editor/ZSlate/Backend/ZSlateEditorOverlay.h"  // native RHI backend (M3)
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Function/Render/WindowSystem.h"
#include "Runtime/Resource/Asset/AssetManager.h"
#include "Runtime/Slate/Application/SlateApplication.h"
#include "Runtime/Slate/Widgets/SBorder.h"
#include "Runtime/Slate/Widgets/SBoxPanel.h"
#include "Runtime/Slate/Widgets/SButton.h"
#include "Runtime/Slate/Widgets/SEditableTextBox.h"
#include "Runtime/Slate/Widgets/SMenu.h"
#include "Runtime/Slate/Widgets/SDropTarget.h"
#include "Runtime/Slate/Widgets/SScrollBox.h"
#include "Runtime/Slate/Widgets/SSpacer.h"
#include "Runtime/Slate/Widgets/STextBlock.h"

#include <algorithm>
#include <cstring>
using namespace ZSlate;

namespace
{
const UIColor kPanelColor(0.10f, 0.10f, 0.12f, 1.0f);
const UIColor kTransparent(0.0f, 0.0f, 0.0f, 0.0f);
const UIColor kHoverColor(0.25f, 0.27f, 0.32f, 0.65f);
const UIColor kSelectedColor(0.18f, 0.35f, 0.58f, 1.0f);
const UIColor kNameColor(0.86f, 0.88f, 0.92f, 1.0f);
const UIColor kFolderColor(0.95f, 0.86f, 0.55f, 1.0f);
const UIColor kSelectedTextColor(0.97f, 0.98f, 1.0f, 1.0f);
const UIColor kTypeColor(0.50f, 0.52f, 0.58f, 1.0f);
const UIColor kToggleColor(0.62f, 0.65f, 0.72f, 1.0f);

std::shared_ptr<STextBlock> MakeText(const std::string& text, float font_size, const UIColor& color,
                                     TextAnchor anchor = TextAnchor::MiddleLeft)
{
    auto t = std::make_shared<STextBlock>();
    t->Text = text;
    t->FontSize = font_size;
    t->Color = color;
    t->Alignment = anchor;
    return t;
}
}  // namespace

ZSlateProjectWindow* ZSlateProjectWindow::s_Instance = nullptr;

ZSlateProjectWindow::ZSlateProjectWindow(EditorUI* editor_ui)
    : EditorWindow(editor_ui, EditorLayoutWindowIds::kProject)
    , m_Context(m_EditorFileService,
                m_SelectedNode,
                m_NewObjectIndexMap,
                m_PendingDeletePath,
                m_HasPendingDelete,
                m_RenamingTargetPath,
                m_IsRenaming,
                m_RenameFocusPending,
                m_RenameBuffer,
                m_PendingPrefabSourceGid,
                m_PendingPrefabTargetFolder,
                m_HasPendingPrefabCreate,
                m_PendingOsDropImports,
                m_OsDropMutex,
                m_PendingMaterialShaderPath,
                m_HasPendingMaterialFromShader,
                m_PendingImportDialog,
                m_AssetTreeDirty)
{
    s_Instance = this;

    if (auto asset_manager = dynamic_cast<EditorAssetManager*>(GET_SYSTEM(AssetManager)))
    {
        m_AssetRegistryListenerHandle = asset_manager->RegisterOnAssetUpdated(
            [this, asset_manager](const AssetRegistryChangeEvent& ev) {
                (void)ev;
                if (asset_manager->getAssetRegistry().isScanning())
                    return;
                m_AssetTreeDirty = true;
            });
    }

    if (auto window_system = GET_SYSTEM(WindowSystem))
    {
        window_system->registerOnDropFunc([this](int count, const char** paths) {
            if (count <= 0 || paths == nullptr)
                return;
            std::vector<std::string> copies;
            copies.reserve(static_cast<size_t>(count));
            for (int i = 0; i < count; ++i)
            {
                if (paths[i] != nullptr)
                    copies.emplace_back(paths[i]);
            }
            if (!copies.empty())
                ProjectDragDrop::OnOsFilesDropped(Ctx(), copies);
        });
    }
}

ZSlateProjectWindow::~ZSlateProjectWindow()
{
    if (m_AssetRegistryListenerHandle != 0)
    {
        if (auto asset_manager = dynamic_cast<EditorAssetManager*>(GET_SYSTEM(AssetManager)))
            asset_manager->UnregisterOnAssetUpdated(m_AssetRegistryListenerHandle);
    }
    s_Instance = nullptr;
}

void ZSlateProjectWindow::ToggleCollapsed(const eastl::string& path)
{
    const std::string key(path.c_str());
    if (m_Collapsed.count(key) != 0)
        m_Collapsed.erase(key);
    else
        m_Collapsed.insert(key);
    ++m_CollapseVersion;
}

void ZSlateProjectWindow::ResolveSelectedFromPath()
{
    const std::filesystem::path& sel = GET_SYSTEM(EditorSceneManager)->getSelectedAssetPath();
    if (!sel.empty())
        m_SelectedNode = ProjectWindowHelpers::FindProjectNodeAcrossRoots(m_EditorFileService, sel);
    else
        m_SelectedNode = nullptr;
}

void ZSlateProjectWindow::SelectNode(EditorFileNode* node)
{
    if (node == nullptr)
        return;
    m_SelectedNode = node;
    if (!ProjectWindowHelpers::IsFolderNode(node))
        GET_SYSTEM(EditorSceneManager)->OnAssetSelected(node->m_FilePath.c_str(), node->displayTypeLabel());
}

std::shared_ptr<SWidget> ZSlateProjectWindow::BuildToolbar(float scale)
{
    const float font = 13.0f * scale;
    auto bar = std::make_shared<SHorizontalBox>();

    auto import_btn = std::make_shared<SButton>();
    import_btn->Padding = FMargin(10.0f * scale, 3.0f * scale);
    import_btn->SetContent(MakeText("Import", font, kNameColor));
    import_btn->OnClicked = [this]() { ProjectAssetActions::RequestImportDialog(Ctx()); };
    bar->AddSlot(import_btn).AutoSize().SetVAlign(EVerticalAlignment::Center);

    bar->AddSlot(std::make_shared<SSpacer>(Vector2(6.0f * scale, 0.0f))).AutoSize();

    auto add_btn = std::make_shared<SButton>();
    add_btn->Padding = FMargin(10.0f * scale, 3.0f * scale);
    add_btn->SetContent(MakeText("Add", font, kNameColor));
    std::weak_ptr<SButton> add_weak = add_btn;
    add_btn->OnClicked = [this, scale, add_weak]() {
        if (auto b = add_weak.lock())
        {
            const FGeometry& g = b->GetCachedGeometry();
            OpenCreateMenu(Vector2(g.AbsolutePosition.x, g.AbsolutePosition.y + g.LocalSize.y), scale);
        }
    };
    bar->AddSlot(add_btn).AutoSize().SetVAlign(EVerticalAlignment::Center);

    return bar;
}

void ZSlateProjectWindow::AddNodeRows(EditorFileNode* node, int depth, float scale,
                                      const std::shared_ptr<SScrollBox>& list)
{
    if (node == nullptr)
        return;

    const bool is_folder = ProjectWindowHelpers::IsFolderNode(node);
    const bool collapsed = is_folder && IsCollapsed(node->m_FilePath);
    const bool has_children = is_folder && !node->m_ChildNodes.empty();
    const bool is_selected = (m_SelectedNode == node);
    const bool renaming = ProjectAssetActions::IsNodeRenaming(Ctx(), node);

    const float font_size = 13.0f * scale;
    const float indent_unit = 14.0f * scale;
    const float toggle_w = 14.0f * scale;

    auto row = std::make_shared<SButton>();
    row->Padding = FMargin(2.0f * scale, 2.0f * scale);
    row->HAlign = EHorizontalAlignment::Fill;
    row->VAlign = EVerticalAlignment::Center;
    row->NormalColor = is_selected ? kSelectedColor : kTransparent;
    row->HoverColor = is_selected ? kSelectedColor : kHoverColor;
    row->PressedColor = kSelectedColor;
    EditorFileNode* node_ptr = node;
    if (!renaming)
    {
        row->OnClicked = [this, node_ptr]() {
            // Double-click detection (open external editor / toggle folder).
            const auto now = std::chrono::steady_clock::now();
            const bool is_double = (node_ptr == m_LastClickNode) &&
                                   (now - m_LastClickTime < std::chrono::milliseconds(350));
            m_LastClickNode = node_ptr;
            m_LastClickTime = now;

            SelectNode(node_ptr);

            if (is_double)
            {
                if (ProjectWindowHelpers::IsFolderNode(node_ptr))
                {
                    ToggleCollapsed(node_ptr->m_FilePath);
                }
                else
                {
                    const std::string& ext = node_ptr->m_FileExtension;
                    if (ext == "scene")
                    {
                        GET_SYSTEM(EditorSceneManager)->OpenSceneFromProjectPath(node_ptr->m_FilePath);
                    }
                    else if (ext == "ts" || ext == "tsx" || ext == "js" || ext == "json")
                        EditorUtility::OpenInExternalEditor(node_ptr->m_FilePath);
                }
            }
        };
        row->OnRightClicked = [this, node_ptr](const Vector2& screen_pos) {
            m_PendingContextNode = node_ptr;
            m_PendingContextPos = screen_pos;
            m_HasPendingContext = true;
        };

        // Drop target: a Hierarchy GameObject dragged here creates a .prefab under
        // the folder that owns this row (or the parent folder for asset rows).
        row->CanAcceptDrop = [](const std::shared_ptr<FDragDropOperation>& op) {
            return op != nullptr && op->PayloadType == EditorDragDrop::kZSlateAssetPayloadGObjectId &&
                   op->Id != 0;
        };
        row->OnDropHandler = [this, node_ptr](const std::shared_ptr<FDragDropOperation>& op) {
            if (op == nullptr)
                return;
            const std::filesystem::path folder = ProjectWindowHelpers::ResolveDropTargetFolder(node_ptr);
            ProjectAssetActions::RequestPrefabCreate(Ctx(), static_cast<GObjectID>(op->Id), folder);
            m_ForceRebuild = true;
        };

        // Drag source: a `.zasset` row can be dragged onto the Scene viewport to
        // instantiate it (Prefab / MeshData). The op carries the absolute path;
        // the cross-window channel (SlateInputRouter -> SetActiveDragOperation)
        // makes it visible to the Scene window even though that lives in a
        // different router. Folders / non-asset rows are not draggable.
        if (!is_folder && node->m_FileExtension == "zasset")
        {
            const std::string asset_path = std::string(node->m_FilePath.c_str());
            const std::string label = std::string(ProjectWindowHelpers::GetProjectDisplayName(node).c_str());
            row->OnDragDetectedHandler = [asset_path, label](const Vector2&) -> std::shared_ptr<FDragDropOperation> {
                auto op = std::make_shared<FAssetDragDropOp>();
                op->PayloadType = EditorDragDrop::kZSlateAssetPayloadAssetPath;
                op->DecoratorText = label;
                op->AssetPath = asset_path;
                return op;
            };
        }
    }

    auto hb = std::make_shared<SHorizontalBox>();
    if (depth > 0)
        hb->AddSlot(std::make_shared<SSpacer>(Vector2(depth * indent_unit, 0.0f))).AutoSize();

    if (has_children)
    {
        auto toggle = std::make_shared<SButton>();
        toggle->Padding = FMargin(1.0f * scale, 0.0f);
        toggle->HAlign = EHorizontalAlignment::Center;
        toggle->VAlign = EVerticalAlignment::Center;
        toggle->NormalColor = kTransparent;
        toggle->HoverColor = UIColor(0.32f, 0.34f, 0.40f, 0.85f);
        toggle->PressedColor = kTransparent;
        toggle->SetContent(MakeText(collapsed ? ">" : "v", font_size, kToggleColor));
        eastl::string toggle_path = node->m_FilePath;
        toggle->OnClicked = [this, toggle_path]() { ToggleCollapsed(toggle_path); };
        hb->AddSlot(toggle).AutoSize().SetVAlign(EVerticalAlignment::Center);
    }
    else
    {
        hb->AddSlot(std::make_shared<SSpacer>(Vector2(toggle_w, 0.0f))).AutoSize();
    }

    hb->AddSlot(std::make_shared<SSpacer>(Vector2(4.0f * scale, 0.0f))).AutoSize();

    if (renaming)
    {
        m_RenameBox = std::make_shared<SEditableTextBox>();
        m_RenameBox->FontSize = font_size;
        m_RenameBox->Text = m_RenameBuffer.data();
        m_RenameBox->MinWidth = 80.0f * scale;
        m_RenameBox->OnTextCommitted = [this](const std::string& t) {
            std::memset(m_RenameBuffer.data(), 0, m_RenameBuffer.size());
            const size_t n = std::min(t.size(), m_RenameBuffer.size() - 1);
            if (n > 0)
                std::memcpy(m_RenameBuffer.data(), t.data(), n);
            ProjectAssetActions::CommitRename(Ctx());
            m_ForceRebuild = true;
        };
        hb->AddSlot(m_RenameBox).Fill().SetVAlign(EVerticalAlignment::Center);
    }
    else
    {
        const eastl::string display = ProjectWindowHelpers::GetProjectDisplayName(node);
        hb->AddSlot(MakeText(std::string(display.c_str()), font_size,
                             is_folder ? kFolderColor : (is_selected ? kSelectedTextColor : kNameColor)))
            .AutoSize()
            .SetVAlign(EVerticalAlignment::Center);

        // Type label, pushed to the right edge.
        hb->AddSlot(std::make_shared<SSpacer>(Vector2(8.0f * scale, 0.0f))).Fill();
        if (!is_folder)
        {
            hb->AddSlot(MakeText(node->displayTypeLabel(), font_size, kTypeColor, TextAnchor::MiddleRight))
                .AutoSize()
                .SetVAlign(EVerticalAlignment::Center);
        }
    }

    row->SetContent(hb);
    list->AddChild(row);

    if (is_folder && !collapsed)
    {
        for (int i = 0; i < node->m_ChildNodes.size(); ++i)
            AddNodeRows(node->m_ChildNodes[static_cast<size_t>(i)].get(), depth + 1, scale, list);
    }
}

void ZSlateProjectWindow::Rebuild(float scale)
{
    m_RenameBox = nullptr;

    auto root = std::make_shared<SBorder>();
    root->BackgroundColor = kPanelColor;
    root->Padding = FMargin(4.0f * scale, 4.0f * scale);
    root->HAlign = EHorizontalAlignment::Fill;
    root->VAlign = EVerticalAlignment::Fill;

    auto column = std::make_shared<SVerticalBox>();
    column->AddSlot(BuildToolbar(scale)).AutoSize().SetPadding(FMargin(0.0f, 0.0f, 0.0f, 4.0f * scale));

    auto list = std::make_shared<SScrollBox>();
    for (EditorFileNode* r : m_EditorFileService.getEditorRootNodes())
        AddNodeRows(r, 0, scale, list);

    // Catch-all drop target for blank space: create the prefab under the current
    // selection's folder (or Assets/ when nothing is selected).
    auto drop = std::make_shared<SDropTarget>();
    drop->CanAcceptDrop = [](const std::shared_ptr<FDragDropOperation>& op) {
        return op != nullptr && op->PayloadType == EditorDragDrop::kZSlateAssetPayloadGObjectId &&
               op->Id != 0;
    };
    drop->OnDropHandler = [this](const std::shared_ptr<FDragDropOperation>& op) {
        if (op == nullptr)
            return;
        const std::filesystem::path folder = ProjectWindowHelpers::ResolveDropTargetFolder(m_SelectedNode);
        ProjectAssetActions::RequestPrefabCreate(Ctx(), static_cast<GObjectID>(op->Id), folder);
        m_ForceRebuild = true;
    };
    drop->SetContent(list);
    column->AddSlot(drop).Fill();

    root->SetContent(column);
    m_Root = root;
}

void ZSlateProjectWindow::OpenCreateMenu(const Vector2& screen_pos, float scale)
{
    m_Popup.Open(screen_pos, scale, [this](SMenu& menu, float s) {
        menu.MinWidth = 180.0f * s;
        auto create = [this](const char* type) {
            const std::string t = type;
            return [this, t]() { ProjectAssetActions::CreateNewAsset(Ctx(), t); };
        };
        // "Create" rolls up into a submenu now that the popup supports nesting.
        auto create_sub = menu.AddSubMenu("Create", s);
        create_sub->AddItem("Material", create("Material"), s);
        create_sub->AddItem("Shader", create("Shader"), s);
        create_sub->AddItem("Blueprint", create("Blueprint"), s);
        create_sub->AddItem("Level", create("Level"), s);
        create_sub->AddItem("Object", create("Object"), s);
        create_sub->AddSeparator(s);
        create_sub->AddItem("Folder", create("Folder"), s);
        menu.AddSeparator(s);
        menu.AddItem("Import...", [this]() { ProjectAssetActions::RequestImportDialog(Ctx()); }, s);
    });
}

void ZSlateProjectWindow::OpenContextMenuFor(EditorFileNode* node, const Vector2& screen_pos, float scale)
{
    if (node == nullptr)
        return;

    // Mirror the ImGui window: right-click selects the node first.
    SelectNode(node);

    const bool is_folder = ProjectWindowHelpers::IsFolderNode(node);
    const bool is_shader = ProjectWindowHelpers::IsShaderAssetNode(node);
    const bool can_rename = ProjectAssetActions::CanRenameNode(Ctx(), node);
    const bool is_zasset_product = ProjectWindowHelpers::IsZassetProductNode(node);
    EditorFileNode* n = node;

    m_Popup.Open(screen_pos, scale, [this, n, is_folder, is_shader, can_rename, is_zasset_product](SMenu& menu, float s) {
        menu.MinWidth = 190.0f * s;

        if (is_folder)
        {
            auto create = [this, n](const char* type) {
                const std::string t = type;
                return [this, n, t]() {
                    m_SelectedNode = n;
                    ProjectAssetActions::CreateNewAsset(Ctx(), t);
                };
            };
            auto create_sub = menu.AddSubMenu("Create", s);
            create_sub->AddItem("Material", create("Material"), s);
            create_sub->AddItem("Shader", create("Shader"), s);
            create_sub->AddItem("Blueprint", create("Blueprint"), s);
            create_sub->AddItem("Level", create("Level"), s);
            create_sub->AddItem("Object", create("Object"), s);
            create_sub->AddSeparator(s);
            create_sub->AddItem("Folder", create("Folder"), s);
            menu.AddItem("Import...", [this, n]() {
                m_SelectedNode = n;
                ProjectAssetActions::RequestImportDialog(Ctx());
            }, s);
            menu.AddSeparator(s);
        }
        else if (is_shader)
        {
            menu.AddItem("Create Material", [this, n]() { ProjectAssetActions::RequestMaterialFromShader(Ctx(), n); }, s);
            menu.AddSeparator(s);
        }

        menu.AddItem("Delete", [this, n]() { ProjectAssetActions::OnMenuItemDelete(Ctx(), n); }, s);
        if (can_rename)
        {
            menu.AddItem("Rename", [this, n]() {
                ProjectAssetActions::OnMenuItemRename(Ctx(), n);
                m_ForceRebuild = true;
            }, s);
        }
        menu.AddSeparator(s);
        menu.AddItem("Show in Explorer", [n]() { ProjectAssetActions::OnMenuItemShowInExplorer(n); }, s);
        menu.AddItem("Copy Path", [n]() { ProjectAssetActions::OnMenuItemCopyPath(n); }, s);
        if (is_zasset_product)
            menu.AddItem("Reimport", [this, n]() { ProjectAssetActions::OnMenuItemReimport(Ctx(), n); }, s);
        if (!is_folder)
        {
            menu.AddItem("Convert asset", [n]() {
                eastl::string parent_dir;
                std::filesystem::path src(n->m_FilePath.c_str());
                std::filesystem::path parent = src.parent_path();
                if (!parent.empty())
                    parent_dir = parent.generic_string().c_str();
                AssetsMenu::ConvertAsset(n->m_FilePath, parent_dir);
            }, s);
        }
    });
}

void ZSlateProjectWindow::ExecutePendingImportDialog()
{
    ProjectAssetActions::ExecutePendingImportDialog(Ctx());
}

void ZSlateProjectWindow::OnGUI()
{
    // Native import dialog is blocking; drain it before we paint anything.
    ExecutePendingImportDialog();

    // P10c: process-wide measurer installed by ZSlateEditorOverlay::BeginFrameIfEnabled.

    float ui_scale = ZSlate::EditorSlateHost::Get().GetUiScale();
    if (ui_scale < 0.5f)
        ui_scale = 1.0f;

    // Refresh the on-disk file tree (skip while a context menu is open so the
    // node pointers captured by the menu items stay valid).
    const auto now = std::chrono::steady_clock::now();
    if (!m_Popup.IsOpen() && (m_AssetTreeDirty || now - m_LastFileTreeUpdate > std::chrono::seconds(1)))
    {
        m_EditorFileService.BuildEngineFileTree();
        m_LastFileTreeUpdate = now;
        m_AssetTreeDirty = false;
        ResolveSelectedFromPath();
        m_ForceRebuild = true;
    }

    if (m_EditorFileService.getEditorRootNode() == nullptr)
        return;

    // Decide if the ZSlate tree needs rebuilding.
    const std::string sel_path = m_SelectedNode != nullptr ? std::string(m_SelectedNode->m_FilePath.c_str()) : std::string();
    const std::string rename_key = m_IsRenaming ? m_RenamingTargetPath.generic_string() : std::string();
    const bool needs_rebuild = m_ForceRebuild || m_Root == nullptr || ui_scale != m_BuiltScale ||
                               sel_path != m_BuiltSelectedPath || rename_key != m_BuiltRenameKey ||
                               m_CollapseVersion != m_BuiltCollapseVersion;
    if (needs_rebuild)
    {
        Rebuild(ui_scale);
        m_BuiltScale = ui_scale;
        m_BuiltSelectedPath = sel_path;
        m_BuiltRenameKey = rename_key;
        m_BuiltCollapseVersion = m_CollapseVersion;
        m_ForceRebuild = false;
        m_Input.Reset();
        if (m_IsRenaming && m_RenameFocusPending && m_RenameBox)
        {
            m_Input.SetKeyboardFocus(m_RenameBox.get());
            m_RenameFocusPending = false;
        }
    }

    // ---- Paint --------------------------------------------------------------
    // P10c: native-host panels source their leaf rect from EditorView::NativeRect()
    // (no ImGui::Begin / item to probe); otherwise use the ImGui content region.
    const float* native_rect = NativeRect();
    Vector2 pos(native_rect[0], native_rect[1]);
    Vector2 avail(native_rect[2], native_rect[3]);
    if (avail.x < 1.0f)
        avail.x = 1.0f;
    if (avail.y < 1.0f)
        avail.y = 1.0f;

    const UIRect region(pos.x, pos.y, avail.x, avail.y);
    const FGeometry geometry(Vector2(pos.x, pos.y), Vector2(avail.x, avail.y));

    // P9: the native RHI backend paints into the shared BatchedUIRenderer (frame managed by
    // ZSlateEditorOverlay around WindowUI::PreRender), clipped to this panel. The legacy
    // per-window SlateImGuiRenderer fallback was retired.
    auto& overlay = ZSlate::ZSlateEditorOverlay::Get();
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

    // ---- Input --------------------------------------------------------------
    // P11a: input / hover / wheel / keyboard all come from the GLFW-backed
    // EditorSlateHost (the transitional r.ZSlate.NativeInput CVar was retired in
    // P10c, so the old ImGui::GetIO() fallback branches were dead and are gone).
    ZSlate::EditorSlateHost& host = ZSlate::EditorSlateHost::Get();
    const int surface_id = ZSlate::EditorSlateHost::HashId(m_Title);
    host.BeginSurface(surface_id, region, ZSlate::ESurfaceLayer::Panels);
    const Vector2 mouse = host.GetPointerPos();
    const bool over_canvas = host.IsSurfaceHovered(surface_id, mouse);
    const bool left_down = host.IsLeftDown();
    const bool right_down = host.IsRightDown();
    const float wheel = over_canvas ? host.GetWheelDelta() : 0.0f;
    const bool right_up_edge = !right_down && m_PrevRightDown;

    if (m_Popup.IsOpen())
    {
        // Clamp the popup to the native display rect (== the editor's full-window
        // viewport work area).
        const UIRect viewport_rect(host.GetDisplayPos().x, host.GetDisplayPos().y,
                                   host.GetDisplaySize().x, host.GetDisplaySize().y);

        {
            // Append the popup into the shared batch after the panel content so it
            // draws on top (the native overlay is composited after all ImGui).
            m_Popup.Render(overlay.GetRenderer(), mouse, left_down, wheel, viewport_rect, 1);
        }
    }
    else
    {
        m_Input.ProcessMouse(m_Root, mouse, over_canvas, left_down, wheel, right_down);

        if (m_Input.HasKeyboardFocus())
        {
            for (unsigned int cp : host.GetCharsThisFrame())
                m_Input.ProcessChar(cp);
            for (EKey key : host.GetKeysThisFrame())
            {
                if (key == EKey::Backspace || key == EKey::Enter)
                    m_Input.ProcessKey(key);
                else if (key == EKey::Escape && m_IsRenaming)
                {
                    ProjectAssetActions::CancelRename(Ctx());
                    m_ForceRebuild = true;
                }
            }
        }

        if (right_up_edge && over_canvas)
        {
            if (m_HasPendingContext)
                OpenContextMenuFor(m_PendingContextNode, m_PendingContextPos, ui_scale);
            else
                OpenCreateMenu(mouse, ui_scale);
            m_HasPendingContext = false;
        }
    }

    m_PrevLeftDown = left_down;
    m_PrevRightDown = right_down;

    // ---- Drain deferred backend work (same calls as the ImGui window) -------
    ProjectAssetActions::ExecutePendingDelete(Ctx());
    ProjectAssetActions::ExecutePendingPrefabCreate(Ctx());
    ProjectDragDrop::ExecutePendingOsDropImports(Ctx());
    ProjectAssetActions::ExecutePendingMaterialFromShader(Ctx());
}
