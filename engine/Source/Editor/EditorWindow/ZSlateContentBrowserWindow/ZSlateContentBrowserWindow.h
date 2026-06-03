#pragma once

#include "Editor/EditorFileService/EditorFileService.h"
#include "Editor/EditorUI/ContentBrowser/ContentBrowserContext.h"
#include "Editor/EditorWindow/EditorWindow.h"
#include "Editor/Menu/ZSlatePopupMenu.h"  // reusable context-menu overlay
#include "Runtime/Slate/Core/SlatePaint.h"
#include "Runtime/UI/Render/UIRenderer.h"
#include "Runtime/Slate/Application/SlateInput.h"
#include "Runtime/Slate/Core/SlateGeometry.h"  // Vector2
#include "function/framework/Object/ObjectIdAllocator.h"

#include <EASTL/string.h>
#include <EASTL/unordered_map.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace ZSlate
{
class SWidget;
class SScrollBox;
class SEditableTextBox;
class SMenu;
}  // namespace ZSlate

// UE-style Content Browser: folder tree (left) + asset list (right).
class ZSlateContentBrowserWindow : public EditorWindow
{
public:
    explicit ZSlateContentBrowserWindow(EditorUI* editor_ui);
    ~ZSlateContentBrowserWindow() override;
    void OnGUI() override;
    bool SupportsNativeHosting() const override { return true; }
    void ExecutePendingImportDialog();

private:
    enum class EContentBrowserRowPanel
    {
        FolderTree,
        AssetList,
    };

    enum class EContentBrowserViewMode
    {
        List,
        Tile,
    };

    ContentBrowserContext& Ctx() { return m_Context; }

    void Rebuild(float scale, float content_width);
    void AddFolderTreeRows(EditorFileNode* node, int depth, float scale, const std::shared_ptr<ZSlate::SScrollBox>& list);
    void AddAssetListRows(float scale, const std::shared_ptr<ZSlate::SScrollBox>& list);
    void AddAssetTileGrid(float scale, float asset_area_width, const std::shared_ptr<ZSlate::SScrollBox>& list);
    std::shared_ptr<ZSlate::SWidget> BuildAssetTile(EditorFileNode* node, float scale, float tile_w, float tile_h,
                                                    float thumb_size);
    void AddItemRow(EditorFileNode* node,
                    int depth,
                    float scale,
                    EContentBrowserRowPanel panel,
                    const std::shared_ptr<ZSlate::SScrollBox>& list);
    std::shared_ptr<ZSlate::SWidget> BuildToolbar(float scale);
    std::shared_ptr<ZSlate::SWidget> BuildNavigationBar(float scale);
    std::shared_ptr<ZSlate::SWidget> BuildThumbnailWidget(EditorFileNode* node, float scale, float thumb_size);
    void LoadViewPrefs();
    void SavePathViewWidth(float width);
    void SetViewMode(EContentBrowserViewMode mode);

    bool IsCollapsed(const eastl::string& path) const { return m_Collapsed.count(std::string(path.c_str())) != 0; }
    void ToggleCollapsed(const eastl::string& path);
    void ExpandAncestorsForFolder(const EditorFileNode* folder);

    void SelectNode(EditorFileNode* node);
    void NavigateToFolder(EditorFileNode* folder);
    void EnsureBrowsedFolder();
    void OpenContextMenuFor(EditorFileNode* node, const Vector2& screen_pos, float scale);
    void OpenCreateMenu(const Vector2& screen_pos, float scale);
    void RebindSelectionAfterTreeRebuild(const std::string& selected_path, const std::string& browsed_path);
    void HandleNodeActivated(EditorFileNode* node, EContentBrowserRowPanel panel);

    static ZSlateContentBrowserWindow* s_Instance;

    // ---- Content Browser backing state (drives the shared backend) -----
    EditorFileService m_EditorFileService;
    EditorFileNode* m_SelectedNode {nullptr};
    EditorFileNode* m_BrowsedFolderNode {nullptr};
    eastl::unordered_map<eastl::string, unsigned int> m_NewObjectIndexMap;
    std::filesystem::path m_PendingDeletePath;
    bool m_HasPendingDelete {false};
    std::filesystem::path m_RenamingTargetPath;
    bool m_IsRenaming {false};
    bool m_RenameFocusPending {false};
    std::array<char, 512> m_RenameBuffer {};
    GObjectID m_PendingPrefabSourceGid {k_invalid_gobject_id};
    std::filesystem::path m_PendingPrefabTargetFolder;
    bool m_HasPendingPrefabCreate {false};
    std::vector<std::string> m_PendingOsDropImports;
    std::mutex m_OsDropMutex;
    std::filesystem::path m_PendingMaterialShaderPath;
    bool m_HasPendingMaterialFromShader {false};
    bool m_PendingImportDialog {false};
    bool m_AssetTreeDirty {false};
    uint32_t m_AssetRegistryListenerHandle {0};
    ContentBrowserContext m_Context;

    // ---- ZSlate view state -------------------------------------------------
    std::unordered_set<std::string> m_Collapsed;   // collapsed folder paths (default open)
    float m_PathViewWidth {230.0f};
    EContentBrowserViewMode m_ViewMode {EContentBrowserViewMode::Tile};

    // Double-click detection (open external editor / toggle folder).
    EditorFileNode* m_LastClickNode {nullptr};
    std::chrono::time_point<std::chrono::steady_clock> m_LastClickTime;

    std::shared_ptr<ZSlate::SWidget> m_Root;
    std::shared_ptr<ZSlate::SEditableTextBox> m_RenameBox;  // live during inline rename

    // Per-window context-menu overlay (reusable anchored popup w/ submenu stack).
    ZSlate::ZSlatePopupMenu m_Popup;

    // Right-click target captured during routing, opened on the up edge.
    EditorFileNode* m_PendingContextNode {nullptr};
    Vector2 m_PendingContextPos {0.0f, 0.0f};
    bool m_HasPendingContext {false};

    ZSlate::SlateInputRouter m_Input;

    bool m_PrevLeftDown {false};
    bool m_PrevRightDown {false};

    // Rebuild bookkeeping.
    float m_BuiltScale {-1.0f};
    std::string m_BuiltSelectedPath;
    std::string m_BuiltBrowsedPath;
    std::string m_BuiltRenameKey;
    uint64_t m_CollapseVersion {0};
    uint64_t m_BuiltCollapseVersion {~0ull};
    EContentBrowserViewMode m_BuiltViewMode {EContentBrowserViewMode::Tile};
    float m_BuiltContentWidth {-1.0f};
    bool m_ForceRebuild {true};
};
