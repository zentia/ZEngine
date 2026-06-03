#include "ZSlateContentBrowserWindow.h"

#include "Editor/EditorAsset/EditorAssetManager.h"
#include "Editor/EditorDragDrop/EditorDragDrop.h"
#include "Editor/EditorLayout/EditorLayoutWindowIds.h"
#include "Editor/EditorProjectPrefs/EditorProjectPrefs.h"
#include "Editor/EditorSceneManager/EditorSceneManager.h"
#include "Editor/EditorUI/ContentBrowser/ContentBrowserAssetActions.h"
#include "Editor/EditorUI/ContentBrowser/ContentBrowserDragDrop.h"
#include "Editor/EditorUI/ContentBrowser/ContentBrowserHelpers.h"
#include "Editor/EditorUI/ContentBrowser/ContentBrowserThumbnailCache.h"
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
#include "Runtime/Slate/Widgets/SImage.h"
#include "Runtime/Slate/Widgets/SMenu.h"
#include "Runtime/Slate/Widgets/SDropTarget.h"
#include "Runtime/Slate/Widgets/SScrollBox.h"
#include "Runtime/Slate/Widgets/SSplitter.h"
#include "Runtime/Slate/Widgets/SSpacer.h"
#include "Runtime/Slate/Widgets/STextBlock.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
using namespace ZSlate;

namespace
{
const UIColor kTreePanelColor(0.08f, 0.08f, 0.10f, 1.0f);
const UIColor kListPanelColor(0.10f, 0.10f, 0.12f, 1.0f);
const UIColor kTransparent(0.0f, 0.0f, 0.0f, 0.0f);
const UIColor kHoverColor(0.25f, 0.27f, 0.32f, 0.65f);
const UIColor kSelectedColor(0.18f, 0.35f, 0.58f, 1.0f);
const UIColor kNameColor(0.86f, 0.88f, 0.92f, 1.0f);
const UIColor kFolderColor(0.95f, 0.86f, 0.55f, 1.0f);
const UIColor kSelectedTextColor(0.97f, 0.98f, 1.0f, 1.0f);
const UIColor kTypeColor(0.50f, 0.52f, 0.58f, 1.0f);
const UIColor kToggleColor(0.62f, 0.65f, 0.72f, 1.0f);
const UIColor kToolbarActiveColor(0.22f, 0.38f, 0.62f, 1.0f);
const UIColor kThumbFolderColor(0.72f, 0.62f, 0.28f, 1.0f);
const UIColor kThumbAssetColor(0.32f, 0.38f, 0.48f, 1.0f);
const UIColor kThumbSceneColor(0.28f, 0.52f, 0.38f, 1.0f);
const UIColor kThumbDefaultColor(0.28f, 0.30f, 0.34f, 1.0f);

float ParsePrefFloat(const std::string& text, float default_value)
{
    if (text.empty())
        return default_value;
    char* end = nullptr;
    const float value = std::strtof(text.c_str(), &end);
    if (end == text.c_str())
        return default_value;
    return value;
}

UIColor ThumbColorForNode(const EditorFileNode* node)
{
    if (node == nullptr)
        return kThumbDefaultColor;
    if (ContentBrowserHelpers::IsFolderNode(node))
        return kThumbFolderColor;
    if (node->m_FileExtension == "scene")
        return kThumbSceneColor;
    if (node->m_FileExtension == "zasset")
        return kThumbAssetColor;
    return kThumbDefaultColor;
}

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

ZSlateContentBrowserWindow* ZSlateContentBrowserWindow::s_Instance = nullptr;

ZSlateContentBrowserWindow::ZSlateContentBrowserWindow(EditorUI* editor_ui)
    : EditorWindow(editor_ui, EditorLayoutWindowIds::kContentBrowser)
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
    LoadViewPrefs();

    if (auto asset_manager = dynamic_cast<EditorAssetManager*>(GET_SYSTEM(AssetManager)))
    {
        m_AssetRegistryListenerHandle = asset_manager->RegisterOnAssetUpdated(
            [this, asset_manager](const AssetRegistryChangeEvent& ev) {
                (void)ev;
                if (asset_manager->getAssetRegistry().isScanning())
                    return;
                ContentBrowserThumbnailCache::InvalidateAll();
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
                ContentBrowserDragDrop::OnOsFilesDropped(Ctx(), copies);
        });
    }
}

ZSlateContentBrowserWindow::~ZSlateContentBrowserWindow()
{
    if (m_AssetRegistryListenerHandle != 0)
    {
        if (auto asset_manager = dynamic_cast<EditorAssetManager*>(GET_SYSTEM(AssetManager)))
            asset_manager->UnregisterOnAssetUpdated(m_AssetRegistryListenerHandle);
    }
    s_Instance = nullptr;
}

void ZSlateContentBrowserWindow::LoadViewPrefs()
{
    m_PathViewWidth =
        std::clamp(ParsePrefFloat(EditorProjectPrefs::GetString(EditorProjectPrefKeys::ContentBrowserPathViewWidth, "230"),
                                  230.0f),
                   120.0f,
                   800.0f);

    const std::string view_mode =
        EditorProjectPrefs::GetString(EditorProjectPrefKeys::ContentBrowserViewMode, "Tile");
    m_ViewMode = (view_mode == "List") ? EContentBrowserViewMode::List : EContentBrowserViewMode::Tile;
}

void ZSlateContentBrowserWindow::SavePathViewWidth(float width)
{
    m_PathViewWidth = std::clamp(width, 120.0f, 800.0f);
    EditorProjectPrefs::SetString(EditorProjectPrefKeys::ContentBrowserPathViewWidth,
                                  std::to_string(static_cast<int>(m_PathViewWidth + 0.5f)));
}

void ZSlateContentBrowserWindow::SetViewMode(EContentBrowserViewMode mode)
{
    if (m_ViewMode == mode)
        return;
    m_ViewMode = mode;
    EditorProjectPrefs::SetString(EditorProjectPrefKeys::ContentBrowserViewMode,
                                  mode == EContentBrowserViewMode::List ? "List" : "Tile");
    m_ForceRebuild = true;
}

void ZSlateContentBrowserWindow::ToggleCollapsed(const eastl::string& path)
{
    const std::string key(path.c_str());
    if (m_Collapsed.count(key) != 0)
        m_Collapsed.erase(key);
    else
        m_Collapsed.insert(key);
    ++m_CollapseVersion;
}

void ZSlateContentBrowserWindow::ExpandAncestorsForFolder(const EditorFileNode* folder)
{
    if (folder == nullptr || folder->m_FilePath.empty())
    {
        return;
    }

    const std::string target = ContentBrowserHelpers::NormalizeContentBrowserPath(folder->m_FilePath.c_str())
                                   .generic_string();
    for (auto iter = m_Collapsed.begin(); iter != m_Collapsed.end();)
    {
        const std::string collapsed_key =
            ContentBrowserHelpers::NormalizeContentBrowserPath(*iter).generic_string();
        if (!collapsed_key.empty() && target.size() > collapsed_key.size() &&
            target.compare(0, collapsed_key.size(), collapsed_key) == 0 &&
            (target[collapsed_key.size()] == '/' || target[collapsed_key.size()] == '\\'))
        {
            iter = m_Collapsed.erase(iter);
        }
        else
        {
            ++iter;
        }
    }
    ++m_CollapseVersion;
}

void ZSlateContentBrowserWindow::NavigateToFolder(EditorFileNode* folder)
{
    if (folder == nullptr || !ContentBrowserHelpers::IsFolderNode(folder))
    {
        return;
    }

    ExpandAncestorsForFolder(folder);
    m_BrowsedFolderNode = folder;
    m_SelectedNode = folder;
}

void ZSlateContentBrowserWindow::EnsureBrowsedFolder()
{
    if (m_BrowsedFolderNode != nullptr && ContentBrowserHelpers::IsFolderNode(m_BrowsedFolderNode))
    {
        return;
    }

    if (m_SelectedNode != nullptr)
    {
        if (ContentBrowserHelpers::IsFolderNode(m_SelectedNode))
        {
            m_BrowsedFolderNode = m_SelectedNode;
            return;
        }

        m_BrowsedFolderNode = ContentBrowserHelpers::FindParentFolderNode(m_EditorFileService, m_SelectedNode);
        if (m_BrowsedFolderNode != nullptr)
        {
            return;
        }
    }

    const std::vector<EditorFileNode*>& roots = m_EditorFileService.getEditorRootNodes();
    m_BrowsedFolderNode = roots.empty() ? nullptr : roots.front();
}

void ZSlateContentBrowserWindow::RebindSelectionAfterTreeRebuild(const std::string& selected_path,
                                                                const std::string& browsed_path)
{
    if (!selected_path.empty())
    {
        m_SelectedNode =
            ContentBrowserHelpers::FindContentBrowserNodeAcrossRoots(m_EditorFileService, selected_path);
    }
    else
    {
        m_SelectedNode = nullptr;
    }

    if (!browsed_path.empty())
    {
        m_BrowsedFolderNode =
            ContentBrowserHelpers::FindContentBrowserNodeAcrossRoots(m_EditorFileService, browsed_path);
        if (m_BrowsedFolderNode != nullptr && !ContentBrowserHelpers::IsFolderNode(m_BrowsedFolderNode))
        {
            m_BrowsedFolderNode =
                ContentBrowserHelpers::FindParentFolderNode(m_EditorFileService, m_BrowsedFolderNode);
        }
    }
    else
    {
        m_BrowsedFolderNode = nullptr;
    }

    EnsureBrowsedFolder();
}

void ZSlateContentBrowserWindow::SelectNode(EditorFileNode* node)
{
    if (node == nullptr)
        return;
    m_SelectedNode = node;
    if (ContentBrowserHelpers::IsFolderNode(node))
    {
        m_BrowsedFolderNode = node;
    }
    if (!ContentBrowserHelpers::IsFolderNode(node))
        GET_SYSTEM(EditorSceneManager)->OnAssetSelected(node->m_FilePath.c_str(), node->displayTypeLabel());
}

void ZSlateContentBrowserWindow::HandleNodeActivated(EditorFileNode* node, EContentBrowserRowPanel panel)
{
    if (node == nullptr)
    {
        return;
    }

    if (ContentBrowserHelpers::IsFolderNode(node))
    {
        if (panel == EContentBrowserRowPanel::FolderTree)
        {
            NavigateToFolder(node);
        }
        else
        {
            SelectNode(node);
            NavigateToFolder(node);
        }
        m_ForceRebuild = true;
        return;
    }

    const std::string& ext = node->m_FileExtension;
    if (ext == "scene")
    {
        GET_SYSTEM(EditorSceneManager)->OpenSceneFromContentBrowserPath(node->m_FilePath);
    }
    else if (ext == "ts" || ext == "tsx" || ext == "js" || ext == "json")
    {
        EditorUtility::OpenInExternalEditor(node->m_FilePath);
    }
}

std::shared_ptr<SWidget> ZSlateContentBrowserWindow::BuildToolbar(float scale)
{
    const float font = 13.0f * scale;
    auto bar = std::make_shared<SHorizontalBox>();

    auto import_btn = std::make_shared<SButton>();
    import_btn->Padding = FMargin(10.0f * scale, 3.0f * scale);
    import_btn->SetContent(MakeText("Import", font, kNameColor));
    import_btn->OnClicked = [this]() { ContentBrowserAssetActions::RequestImportDialog(Ctx()); };
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

    bar->AddSlot(std::make_shared<SSpacer>(Vector2(0.0f, 0.0f))).Fill();

    auto make_view_btn = [this, font, scale](const char* label, EContentBrowserViewMode mode) {
        auto btn = std::make_shared<SButton>();
        btn->Padding = FMargin(8.0f * scale, 3.0f * scale);
        const bool active = (m_ViewMode == mode);
        btn->NormalColor = active ? kToolbarActiveColor : UIColor(0.20f, 0.20f, 0.22f, 1.0f);
        btn->HoverColor = active ? kToolbarActiveColor : kHoverColor;
        btn->PressedColor = kSelectedColor;
        btn->SetContent(MakeText(label, font, active ? kSelectedTextColor : kNameColor));
        btn->OnClicked = [this, mode]() { SetViewMode(mode); };
        return btn;
    };
    bar->AddSlot(make_view_btn("List", EContentBrowserViewMode::List))
        .AutoSize()
        .SetVAlign(EVerticalAlignment::Center);
    bar->AddSlot(std::make_shared<SSpacer>(Vector2(4.0f * scale, 0.0f))).AutoSize();
    bar->AddSlot(make_view_btn("Grid", EContentBrowserViewMode::Tile))
        .AutoSize()
        .SetVAlign(EVerticalAlignment::Center);

    return bar;
}

std::shared_ptr<SWidget> ZSlateContentBrowserWindow::BuildNavigationBar(float scale)
{
    auto bar = std::make_shared<SHorizontalBox>();
    if (m_BrowsedFolderNode == nullptr)
        return bar;

    const float font = 12.0f * scale;
    const std::vector<EditorFileNode*> chain =
        ContentBrowserHelpers::CollectFolderBreadcrumbChain(m_EditorFileService, m_BrowsedFolderNode);

    for (size_t i = 0; i < chain.size(); ++i)
    {
        EditorFileNode* folder = chain[i];
        if (folder == nullptr)
            continue;

        if (i > 0)
        {
            bar->AddSlot(MakeText(">", font, kToggleColor, TextAnchor::MiddleCenter))
                .AutoSize()
                .SetPadding(FMargin(4.0f * scale, 0.0f))
                .SetVAlign(EVerticalAlignment::Center);
        }

        const eastl::string label = ContentBrowserHelpers::GetContentBrowserDisplayName(folder);
        const bool is_current = (i + 1 == chain.size());
        if (is_current)
        {
            bar->AddSlot(MakeText(std::string(label.c_str()), font, kNameColor))
                .AutoSize()
                .SetVAlign(EVerticalAlignment::Center);
            continue;
        }

        auto crumb = std::make_shared<SButton>();
        crumb->Padding = FMargin(2.0f * scale, 1.0f * scale);
        crumb->NormalColor = kTransparent;
        crumb->HoverColor = kHoverColor;
        crumb->PressedColor = kSelectedColor;
        EditorFileNode* folder_ptr = folder;
        crumb->OnClicked = [this, folder_ptr]() {
            NavigateToFolder(folder_ptr);
            m_ForceRebuild = true;
        };
        crumb->SetContent(MakeText(std::string(label.c_str()), font, kFolderColor));
        bar->AddSlot(crumb).AutoSize().SetVAlign(EVerticalAlignment::Center);
    }

    auto border = std::make_shared<SBorder>();
    border->BackgroundColor = UIColor(0.11f, 0.11f, 0.13f, 1.0f);
    border->Padding = FMargin(4.0f * scale, 3.0f * scale);
    border->HAlign = EHorizontalAlignment::Fill;
    border->VAlign = EVerticalAlignment::Center;
    border->SetContent(bar);
    return border;
}

std::shared_ptr<SWidget> ZSlateContentBrowserWindow::BuildThumbnailWidget(EditorFileNode* node,
                                                                          float scale,
                                                                          float thumb_size)
{
    if (void* texture = ContentBrowserThumbnailCache::ResolveForNode(node))
    {
        auto image = std::make_shared<SImage>();
        image->Texture = texture;
        image->DesiredSize = Vector2(thumb_size, thumb_size);
        image->Tint = UIColor(1.0f, 1.0f, 1.0f, 1.0f);

        auto frame = std::make_shared<SBorder>();
        frame->BackgroundColor = UIColor(0.12f, 0.13f, 0.16f, 1.0f);
        frame->Padding = FMargin(2.0f * scale, 2.0f * scale);
        frame->HAlign = EHorizontalAlignment::Center;
        frame->VAlign = EVerticalAlignment::Center;
        frame->SetContent(image);
        return frame;
    }

    auto placeholder = std::make_shared<SBorder>();
    placeholder->BackgroundColor = ThumbColorForNode(node);
    placeholder->Padding = FMargin(2.0f * scale, 2.0f * scale);
    placeholder->HAlign = EHorizontalAlignment::Center;
    placeholder->VAlign = EVerticalAlignment::Center;

    auto inner = std::make_shared<SBorder>();
    inner->BackgroundColor = UIColor(0.12f, 0.13f, 0.16f, 1.0f);
    inner->SetContent(std::make_shared<SSpacer>(Vector2(thumb_size, thumb_size)));
    placeholder->SetContent(inner);
    return placeholder;
}

void ZSlateContentBrowserWindow::AddItemRow(EditorFileNode* node,
                                            int depth,
                                            float scale,
                                            EContentBrowserRowPanel panel,
                                            const std::shared_ptr<SScrollBox>& list)
{
    if (node == nullptr)
        return;

    const bool is_folder = ContentBrowserHelpers::IsFolderNode(node);
    if (panel == EContentBrowserRowPanel::FolderTree && !is_folder)
        return;

    const bool collapsed = is_folder && IsCollapsed(node->m_FilePath);
    const bool has_children = is_folder && !node->m_ChildNodes.empty();
    const bool is_selected = (m_SelectedNode == node);
    const bool is_browsed = is_folder && (m_BrowsedFolderNode == node);
    const bool renaming = ContentBrowserAssetActions::IsNodeRenaming(Ctx(), node);

    const float font_size = 13.0f * scale;
    const float indent_unit = panel == EContentBrowserRowPanel::FolderTree ? 14.0f * scale : 0.0f;
    const float toggle_w = 14.0f * scale;

    UIColor row_color = kTransparent;
    bool row_highlighted = false;
    if (panel == EContentBrowserRowPanel::FolderTree && is_browsed)
    {
        row_color = kSelectedColor;
        row_highlighted = true;
    }
    else if (panel == EContentBrowserRowPanel::AssetList && is_selected)
    {
        row_color = kSelectedColor;
        row_highlighted = true;
    }

    auto row = std::make_shared<SButton>();
    row->Padding = FMargin(2.0f * scale, 2.0f * scale);
    row->HAlign = EHorizontalAlignment::Fill;
    row->VAlign = EVerticalAlignment::Center;
    row->NormalColor = row_color;
    row->HoverColor = row_highlighted ? row_color : kHoverColor;
    row->PressedColor = kSelectedColor;
    EditorFileNode* node_ptr = node;
    if (!renaming)
    {
        row->OnClicked = [this, node_ptr, panel]() {
            const auto now = std::chrono::steady_clock::now();
            const bool is_double = (node_ptr == m_LastClickNode) &&
                                   (now - m_LastClickTime < std::chrono::milliseconds(350));
            m_LastClickNode = node_ptr;
            m_LastClickTime = now;

            SelectNode(node_ptr);

            if (is_double)
            {
                HandleNodeActivated(node_ptr, panel);
            }
            else if (panel == EContentBrowserRowPanel::FolderTree &&
                     ContentBrowserHelpers::IsFolderNode(node_ptr))
            {
                NavigateToFolder(node_ptr);
                m_ForceRebuild = true;
            }
        };
        row->OnRightClicked = [this, node_ptr](const Vector2& screen_pos) {
            m_PendingContextNode = node_ptr;
            m_PendingContextPos = screen_pos;
            m_HasPendingContext = true;
        };

        row->CanAcceptDrop = [](const std::shared_ptr<FDragDropOperation>& op) {
            return op != nullptr && op->PayloadType == EditorDragDrop::kZSlateAssetPayloadGObjectId &&
                   op->Id != 0;
        };
        row->OnDropHandler = [this, node_ptr](const std::shared_ptr<FDragDropOperation>& op) {
            if (op == nullptr)
                return;
            const std::filesystem::path folder = ContentBrowserHelpers::ResolveDropTargetFolder(node_ptr);
            ContentBrowserAssetActions::RequestPrefabCreate(Ctx(), static_cast<GObjectID>(op->Id), folder);
            m_ForceRebuild = true;
        };

        if (!is_folder && node->m_FileExtension == "zasset")
        {
            const std::string asset_path = std::string(node->m_FilePath.c_str());
            const std::string label = std::string(ContentBrowserHelpers::GetContentBrowserDisplayName(node).c_str());
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

    if (panel == EContentBrowserRowPanel::FolderTree && has_children)
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
    else if (panel == EContentBrowserRowPanel::FolderTree)
    {
        hb->AddSlot(std::make_shared<SSpacer>(Vector2(toggle_w, 0.0f))).AutoSize();
    }

    hb->AddSlot(std::make_shared<SSpacer>(Vector2(4.0f * scale, 0.0f))).AutoSize();

    if (panel == EContentBrowserRowPanel::AssetList && !is_folder)
    {
        hb->AddSlot(BuildThumbnailWidget(node, scale, 20.0f * scale))
            .AutoSize()
            .SetVAlign(EVerticalAlignment::Center);
        hb->AddSlot(std::make_shared<SSpacer>(Vector2(6.0f * scale, 0.0f))).AutoSize();
    }

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
            ContentBrowserAssetActions::CommitRename(Ctx());
            m_ForceRebuild = true;
        };
        hb->AddSlot(m_RenameBox).Fill().SetVAlign(EVerticalAlignment::Center);
    }
    else
    {
        const eastl::string display = ContentBrowserHelpers::GetContentBrowserDisplayName(node);
        hb->AddSlot(MakeText(std::string(display.c_str()), font_size,
                             is_folder ? kFolderColor
                                       : (is_selected ? kSelectedTextColor : kNameColor)))
            .AutoSize()
            .SetVAlign(EVerticalAlignment::Center);

        if (panel == EContentBrowserRowPanel::AssetList)
        {
            hb->AddSlot(std::make_shared<SSpacer>(Vector2(8.0f * scale, 0.0f))).Fill();
            if (!is_folder)
            {
                hb->AddSlot(MakeText(node->displayTypeLabel(), font_size, kTypeColor, TextAnchor::MiddleRight))
                    .AutoSize()
                    .SetVAlign(EVerticalAlignment::Center);
            }
        }
    }

    row->SetContent(hb);
    list->AddChild(row);
}

void ZSlateContentBrowserWindow::AddFolderTreeRows(EditorFileNode* node,
                                                   int depth,
                                                   float scale,
                                                   const std::shared_ptr<SScrollBox>& list)
{
    if (node == nullptr || !ContentBrowserHelpers::IsFolderNode(node))
        return;

    AddItemRow(node, depth, scale, EContentBrowserRowPanel::FolderTree, list);

    if (IsCollapsed(node->m_FilePath))
        return;

    for (int i = 0; i < node->m_ChildNodes.size(); ++i)
    {
        EditorFileNode* child = node->m_ChildNodes[static_cast<size_t>(i)].get();
        if (child != nullptr && ContentBrowserHelpers::IsFolderNode(child))
            AddFolderTreeRows(child, depth + 1, scale, list);
    }
}

void ZSlateContentBrowserWindow::AddAssetListRows(float scale, const std::shared_ptr<SScrollBox>& list)
{
    if (m_BrowsedFolderNode == nullptr)
        return;

    for (int i = 0; i < m_BrowsedFolderNode->m_ChildNodes.size(); ++i)
    {
        EditorFileNode* child = m_BrowsedFolderNode->m_ChildNodes[static_cast<size_t>(i)].get();
        AddItemRow(child, 0, scale, EContentBrowserRowPanel::AssetList, list);
    }
}

std::shared_ptr<SWidget> ZSlateContentBrowserWindow::BuildAssetTile(EditorFileNode* node,
                                                                    float scale,
                                                                    float tile_w,
                                                                    float tile_h,
                                                                    float thumb_size)
{
    if (node == nullptr)
        return nullptr;

    const bool is_folder = ContentBrowserHelpers::IsFolderNode(node);
    const bool is_selected = (m_SelectedNode == node);
    const bool renaming = ContentBrowserAssetActions::IsNodeRenaming(Ctx(), node);
    const float font_size = 11.0f * scale;
    EditorFileNode* node_ptr = node;

    auto tile = std::make_shared<SButton>();
    tile->Padding = FMargin(4.0f * scale, 4.0f * scale);
    tile->HAlign = EHorizontalAlignment::Fill;
    tile->VAlign = EVerticalAlignment::Fill;
    tile->NormalColor = is_selected ? kSelectedColor : kTransparent;
    tile->HoverColor = is_selected ? kSelectedColor : kHoverColor;
    tile->PressedColor = kSelectedColor;

    if (!renaming)
    {
        tile->OnClicked = [this, node_ptr, is_folder]() {
            const auto now = std::chrono::steady_clock::now();
            const bool is_double = (node_ptr == m_LastClickNode) &&
                                   (now - m_LastClickTime < std::chrono::milliseconds(350));
            m_LastClickNode = node_ptr;
            m_LastClickTime = now;

            SelectNode(node_ptr);

            if (is_double)
                HandleNodeActivated(node_ptr, EContentBrowserRowPanel::AssetList);
            else if (is_folder)
                NavigateToFolder(node_ptr);
            if (is_double || is_folder)
                m_ForceRebuild = true;
        };
        tile->OnRightClicked = [this, node_ptr](const Vector2& screen_pos) {
            m_PendingContextNode = node_ptr;
            m_PendingContextPos = screen_pos;
            m_HasPendingContext = true;
        };

        tile->CanAcceptDrop = [](const std::shared_ptr<FDragDropOperation>& op) {
            return op != nullptr && op->PayloadType == EditorDragDrop::kZSlateAssetPayloadGObjectId &&
                   op->Id != 0;
        };
        tile->OnDropHandler = [this, node_ptr](const std::shared_ptr<FDragDropOperation>& op) {
            if (op == nullptr)
                return;
            const std::filesystem::path folder = ContentBrowserHelpers::ResolveDropTargetFolder(node_ptr);
            ContentBrowserAssetActions::RequestPrefabCreate(Ctx(), static_cast<GObjectID>(op->Id), folder);
            m_ForceRebuild = true;
        };

        if (!is_folder && node->m_FileExtension == "zasset")
        {
            const std::string asset_path = std::string(node->m_FilePath.c_str());
            const std::string label = std::string(ContentBrowserHelpers::GetContentBrowserDisplayName(node).c_str());
            tile->OnDragDetectedHandler = [asset_path, label](const Vector2&) -> std::shared_ptr<FDragDropOperation> {
                auto op = std::make_shared<FAssetDragDropOp>();
                op->PayloadType = EditorDragDrop::kZSlateAssetPayloadAssetPath;
                op->DecoratorText = label;
                op->AssetPath = asset_path;
                return op;
            };
        }
    }

    auto column = std::make_shared<SVerticalBox>();

    column->AddSlot(BuildThumbnailWidget(node, scale, thumb_size))
        .AutoSize()
        .SetHAlign(EHorizontalAlignment::Center)
        .SetPadding(FMargin(0.0f, 0.0f, 0.0f, 4.0f * scale));

    if (renaming)
    {
        m_RenameBox = std::make_shared<SEditableTextBox>();
        m_RenameBox->FontSize = font_size;
        m_RenameBox->Text = m_RenameBuffer.data();
        m_RenameBox->MinWidth = tile_w - 8.0f * scale;
        m_RenameBox->OnTextCommitted = [this](const std::string& t) {
            std::memset(m_RenameBuffer.data(), 0, m_RenameBuffer.size());
            const size_t n = std::min(t.size(), m_RenameBuffer.size() - 1);
            if (n > 0)
                std::memcpy(m_RenameBuffer.data(), t.data(), n);
            ContentBrowserAssetActions::CommitRename(Ctx());
            m_ForceRebuild = true;
        };
        column->AddSlot(m_RenameBox).Fill().SetVAlign(EVerticalAlignment::Top);
    }
    else
    {
        const eastl::string display = ContentBrowserHelpers::GetContentBrowserDisplayName(node);
        auto label = MakeText(std::string(display.c_str()), font_size,
                              is_folder ? kFolderColor : (is_selected ? kSelectedTextColor : kNameColor),
                              TextAnchor::MiddleCenter);
        column->AddSlot(label).AutoSize().SetHAlign(EHorizontalAlignment::Center);
    }

    tile->SetContent(column);
    return tile;
}

void ZSlateContentBrowserWindow::AddAssetTileGrid(float scale,
                                                  float asset_area_width,
                                                  const std::shared_ptr<SScrollBox>& list)
{
    if (m_BrowsedFolderNode == nullptr)
        return;

    const float tile_w = 96.0f * scale;
    const float tile_h = 112.0f * scale;
    const float thumb_size = 64.0f * scale;
    const int cols = std::max(1, static_cast<int>((asset_area_width - 8.0f * scale) / tile_w));

    auto grid = std::make_shared<SVerticalBox>();
    auto row = std::make_shared<SHorizontalBox>();
    int col = 0;

    for (int i = 0; i < m_BrowsedFolderNode->m_ChildNodes.size(); ++i)
    {
        EditorFileNode* child = m_BrowsedFolderNode->m_ChildNodes[static_cast<size_t>(i)].get();
        if (auto tile = BuildAssetTile(child, scale, tile_w, tile_h, thumb_size))
        {
            row->AddSlot(tile).AutoSize().SetVAlign(EVerticalAlignment::Top);
            ++col;
        }

        if (col >= cols)
        {
            grid->AddSlot(row).AutoSize();
            row = std::make_shared<SHorizontalBox>();
            col = 0;
        }
    }

    if (col > 0)
        grid->AddSlot(row).AutoSize();

    list->AddChild(grid);
}

void ZSlateContentBrowserWindow::Rebuild(float scale, float content_width)
{
    m_RenameBox = nullptr;
    EnsureBrowsedFolder();

    auto root = std::make_shared<SBorder>();
    root->BackgroundColor = kListPanelColor;
    root->Padding = FMargin(4.0f * scale, 4.0f * scale);
    root->HAlign = EHorizontalAlignment::Fill;
    root->VAlign = EVerticalAlignment::Fill;

    auto column = std::make_shared<SVerticalBox>();
    column->AddSlot(BuildToolbar(scale)).AutoSize().SetPadding(FMargin(0.0f, 0.0f, 0.0f, 4.0f * scale));

    // Left path view | drag handle | right asset view (UE SSplitter).
    auto splitter = std::make_shared<SSplitter>();
    splitter->LeftPanelWidth = m_PathViewWidth;
    splitter->HandleWidth = 4.0f * scale;
    splitter->MinLeftPanelWidth = 120.0f * scale;
    splitter->MinRightPanelWidth = 160.0f * scale;
    splitter->OnLeftPanelResizeFinished = [this](float width) {
        SavePathViewWidth(width);
        m_ForceRebuild = true;
    };

    auto tree_border = std::make_shared<SBorder>();
    tree_border->BackgroundColor = kTreePanelColor;
    tree_border->Padding = FMargin(2.0f * scale, 2.0f * scale);
    tree_border->HAlign = EHorizontalAlignment::Fill;
    tree_border->VAlign = EVerticalAlignment::Fill;
    auto tree_list = std::make_shared<SScrollBox>();
    for (EditorFileNode* root_node : m_EditorFileService.getEditorRootNodes())
        AddFolderTreeRows(root_node, 0, scale, tree_list);
    tree_border->SetContent(tree_list);
    splitter->SetLeftContent(tree_border);

    auto list_border = std::make_shared<SBorder>();
    list_border->BackgroundColor = kListPanelColor;
    list_border->Padding = FMargin(2.0f * scale, 2.0f * scale);
    list_border->HAlign = EHorizontalAlignment::Fill;
    list_border->VAlign = EVerticalAlignment::Fill;

    auto list_column = std::make_shared<SVerticalBox>();
    if (m_BrowsedFolderNode != nullptr)
    {
        list_column->AddSlot(BuildNavigationBar(scale))
            .AutoSize()
            .SetPadding(FMargin(0.0f, 0.0f, 0.0f, 4.0f * scale));
    }

    auto asset_list = std::make_shared<SScrollBox>();
    const float asset_area_width =
        std::max(160.0f * scale, content_width - m_PathViewWidth - splitter->HandleWidth - 16.0f * scale);
    if (m_ViewMode == EContentBrowserViewMode::Tile)
        AddAssetTileGrid(scale, asset_area_width, asset_list);
    else
        AddAssetListRows(scale, asset_list);

    auto drop = std::make_shared<SDropTarget>();
    drop->CanAcceptDrop = [](const std::shared_ptr<FDragDropOperation>& op) {
        return op != nullptr && op->PayloadType == EditorDragDrop::kZSlateAssetPayloadGObjectId &&
               op->Id != 0;
    };
    drop->OnDropHandler = [this](const std::shared_ptr<FDragDropOperation>& op) {
        if (op == nullptr)
            return;
        const std::filesystem::path folder = ContentBrowserHelpers::ResolveDropTargetFolder(m_BrowsedFolderNode);
        ContentBrowserAssetActions::RequestPrefabCreate(Ctx(), static_cast<GObjectID>(op->Id), folder);
        m_ForceRebuild = true;
    };
    drop->SetContent(asset_list);
    list_column->AddSlot(drop).Fill();
    list_border->SetContent(list_column);
    splitter->SetRightContent(list_border);

    column->AddSlot(splitter).Fill();
    root->SetContent(column);
    m_Root = root;
    m_PathViewWidth = splitter->LeftPanelWidth;
}

void ZSlateContentBrowserWindow::OpenCreateMenu(const Vector2& screen_pos, float scale)
{
    m_Popup.Open(screen_pos, scale, [this](SMenu& menu, float s) {
        menu.MinWidth = 180.0f * s;
        auto create = [this](const char* type) {
            const std::string t = type;
            return [this, t]() { ContentBrowserAssetActions::CreateNewAsset(Ctx(), t); };
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
        menu.AddItem("Import...", [this]() { ContentBrowserAssetActions::RequestImportDialog(Ctx()); }, s);
    });
}

void ZSlateContentBrowserWindow::OpenContextMenuFor(EditorFileNode* node, const Vector2& screen_pos, float scale)
{
    if (node == nullptr)
        return;

    // Mirror the ImGui window: right-click selects the node first.
    SelectNode(node);

    const bool is_folder = ContentBrowserHelpers::IsFolderNode(node);
    const bool is_shader = ContentBrowserHelpers::IsShaderAssetNode(node);
    const bool can_rename = ContentBrowserAssetActions::CanRenameNode(Ctx(), node);
    const bool is_zasset_product = ContentBrowserHelpers::IsZassetProductNode(node);
    EditorFileNode* n = node;

    m_Popup.Open(screen_pos, scale, [this, n, is_folder, is_shader, can_rename, is_zasset_product](SMenu& menu, float s) {
        menu.MinWidth = 190.0f * s;

        if (is_folder)
        {
            auto create = [this, n](const char* type) {
                const std::string t = type;
                return [this, n, t]() {
                    m_SelectedNode = n;
                    ContentBrowserAssetActions::CreateNewAsset(Ctx(), t);
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
                ContentBrowserAssetActions::RequestImportDialog(Ctx());
            }, s);
            menu.AddSeparator(s);
        }
        else if (is_shader)
        {
            menu.AddItem("Create Material", [this, n]() { ContentBrowserAssetActions::RequestMaterialFromShader(Ctx(), n); }, s);
            menu.AddSeparator(s);
        }

        menu.AddItem("Delete", [this, n]() { ContentBrowserAssetActions::OnMenuItemDelete(Ctx(), n); }, s);
        if (can_rename)
        {
            menu.AddItem("Rename", [this, n]() {
                ContentBrowserAssetActions::OnMenuItemRename(Ctx(), n);
                m_ForceRebuild = true;
            }, s);
        }
        menu.AddSeparator(s);
        menu.AddItem("Show in Explorer", [n]() { ContentBrowserAssetActions::OnMenuItemShowInExplorer(n); }, s);
        menu.AddItem("Copy Path", [n]() { ContentBrowserAssetActions::OnMenuItemCopyPath(n); }, s);
        if (is_zasset_product)
            menu.AddItem("Reimport", [this, n]() { ContentBrowserAssetActions::OnMenuItemReimport(Ctx(), n); }, s);
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

void ZSlateContentBrowserWindow::ExecutePendingImportDialog()
{
    ContentBrowserAssetActions::ExecutePendingImportDialog(Ctx());
}

void ZSlateContentBrowserWindow::OnGUI()
{
    // Native import dialog is blocking; drain it before we paint anything.
    ExecutePendingImportDialog();

    // P10c: process-wide measurer installed by ZSlateEditorOverlay::BeginFrameIfEnabled.

    float ui_scale = ZSlate::EditorSlateHost::Get().GetUiScale();
    if (ui_scale < 0.5f)
        ui_scale = 1.0f;

    // Refresh the on-disk file tree when the asset registry reports changes.
    // Rebind selection by path afterwards; do NOT pull from EditorSceneManager
    // (folder picks live only in the Content Browser and would jump every tick).
    if (!m_Popup.IsOpen() && m_AssetTreeDirty)
    {
        const std::string selected_path =
            m_SelectedNode != nullptr ? std::string(m_SelectedNode->m_FilePath.c_str()) : m_BuiltSelectedPath;
        const std::string browsed_path =
            m_BrowsedFolderNode != nullptr ? std::string(m_BrowsedFolderNode->m_FilePath.c_str())
                                           : m_BuiltBrowsedPath;

        m_EditorFileService.BuildEngineFileTree();
        m_AssetTreeDirty = false;
        RebindSelectionAfterTreeRebuild(selected_path, browsed_path);
        m_ForceRebuild = true;
    }

    if (m_EditorFileService.getEditorRootNode() == nullptr)
        return;

    if (ContentBrowserThumbnailCache::Tick(4))
        m_ForceRebuild = true;

    const float* native_rect = NativeRect();
    Vector2 avail(native_rect[2], native_rect[3]);
    if (avail.x < 1.0f)
        avail.x = 1.0f;
    if (avail.y < 1.0f)
        avail.y = 1.0f;

    // Decide if the ZSlate tree needs rebuilding.
    const std::string sel_path = m_SelectedNode != nullptr ? std::string(m_SelectedNode->m_FilePath.c_str()) : std::string();
    const std::string browsed_path =
        m_BrowsedFolderNode != nullptr ? std::string(m_BrowsedFolderNode->m_FilePath.c_str()) : std::string();
    const std::string rename_key = m_IsRenaming ? m_RenamingTargetPath.generic_string() : std::string();
    const bool width_changed =
        std::abs(avail.x - m_BuiltContentWidth) > 48.0f && m_ViewMode == EContentBrowserViewMode::Tile;
    const bool needs_rebuild = m_ForceRebuild || m_Root == nullptr || ui_scale != m_BuiltScale ||
                               sel_path != m_BuiltSelectedPath || browsed_path != m_BuiltBrowsedPath ||
                               rename_key != m_BuiltRenameKey || m_CollapseVersion != m_BuiltCollapseVersion ||
                               m_ViewMode != m_BuiltViewMode || width_changed;
    if (needs_rebuild)
    {
        Rebuild(ui_scale, avail.x);
        m_BuiltScale = ui_scale;
        m_BuiltSelectedPath = sel_path;
        m_BuiltBrowsedPath = browsed_path;
        m_BuiltRenameKey = rename_key;
        m_BuiltCollapseVersion = m_CollapseVersion;
        m_BuiltViewMode = m_ViewMode;
        m_BuiltContentWidth = avail.x;
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
    Vector2 pos(native_rect[0], native_rect[1]);

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
                    ContentBrowserAssetActions::CancelRename(Ctx());
                    m_ForceRebuild = true;
                }
            }
        }

        if (right_up_edge && over_canvas)
        {
            if (m_HasPendingContext)
                OpenContextMenuFor(m_PendingContextNode, m_PendingContextPos, ui_scale);
            else
                OpenContextMenuFor(m_BrowsedFolderNode, mouse, ui_scale);
            m_HasPendingContext = false;
        }
    }

    m_PrevLeftDown = left_down;
    m_PrevRightDown = right_down;

    // ---- Drain deferred backend work (same calls as the ImGui window) -------
    ContentBrowserAssetActions::ExecutePendingDelete(Ctx());
    ContentBrowserAssetActions::ExecutePendingPrefabCreate(Ctx());
    ContentBrowserDragDrop::ExecutePendingOsDropImports(Ctx());
    ContentBrowserAssetActions::ExecutePendingMaterialFromShader(Ctx());
}
