#pragma once

#include "Editor/EditorFileService/EditorFileService.h"
#include "Editor/EditorUI/ProjectWindow/ProjectWindowContext.h"
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

// A ZSlate-rendered, dockable Project browser. Mirrors the legacy ImGui
// ProjectWindow's single-column asset tree (Assets / Scripts / Shaders / Data
// roots, expand/collapse folders, click to select, double-click .ts/.js/.json to
// open in the external editor, right-click context menu, inline rename) but emits
// ZSlate widgets instead of ImGui calls.
//
// All scene-edit / asset-pipeline work is delegated to the SAME backend modules
// the ImGui window uses (ProjectAssetActions / ProjectContextMenu /
// ProjectDragDrop) via an owned ProjectWindowContext, so behaviour stays in
// lockstep. Only the rendering + input layer is reimplemented on ZSlate.
//
// Cross-window drag: Hierarchy GameObject -> Project creates a .prefab (Unity-style).
// Project .zasset -> Scene placement is handled by ZSlateSceneWindow. OS file drop
// import is also supported.
class ZSlateProjectWindow : public EditorWindow
{
public:
    explicit ZSlateProjectWindow(EditorUI* editor_ui);
    ~ZSlateProjectWindow() override;
    void OnGUI() override;
    bool SupportsNativeHosting() const override { return true; }
    void ExecutePendingImportDialog();

private:
    ProjectWindowContext& Ctx() { return m_Context; }

    void Rebuild(float scale);
    void AddNodeRows(EditorFileNode* node, int depth, float scale, const std::shared_ptr<ZSlate::SScrollBox>& list);
    std::shared_ptr<ZSlate::SWidget> BuildToolbar(float scale);

    bool IsCollapsed(const eastl::string& path) const { return m_Collapsed.count(std::string(path.c_str())) != 0; }
    void ToggleCollapsed(const eastl::string& path);

    void SelectNode(EditorFileNode* node);
    void OpenContextMenuFor(EditorFileNode* node, const Vector2& screen_pos, float scale);
    void OpenCreateMenu(const Vector2& screen_pos, float scale);
    void ResolveSelectedFromPath();

    static ZSlateProjectWindow* s_Instance;

    // ---- Reused ProjectWindow backing state (drives the shared backend) -----
    EditorFileService m_EditorFileService;
    EditorFileNode* m_SelectedNode {nullptr};
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
    ProjectWindowContext m_Context;

    std::chrono::time_point<std::chrono::steady_clock> m_LastFileTreeUpdate;

    // ---- ZSlate view state -------------------------------------------------
    std::unordered_set<std::string> m_Collapsed;   // collapsed folder paths (default open)

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
    std::string m_BuiltRenameKey;
    uint64_t m_CollapseVersion {0};
    uint64_t m_BuiltCollapseVersion {~0ull};
    bool m_ForceRebuild {true};
};
