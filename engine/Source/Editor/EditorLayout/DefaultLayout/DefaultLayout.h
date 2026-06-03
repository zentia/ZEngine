#pragma once

#include "Editor/EditorLayout/EditorLayout.h"
#include "Editor/EditorLayout/ZSlateDock/DockHost.h"
#include "Editor/EditorLayout/ZSlateDock/DockTree.h"
#include "Runtime/Slate/Application/SlateInput.h"  // P11f: native Save Layout dialog input routing

#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ZSlate
{
class SWidget;
class SEditableTextBox;
}  // namespace ZSlate

class DefaultLayout : public EditorLayout
{
public:
    explicit DefaultLayout(EditorUI* editor_ui);
    ~DefaultLayout() override;
    virtual void OnGUI() override;

    void DrawDialogs();
    void QueueBuiltinLayout(const std::string& layout_name);
    void OpenSaveLayoutDialog();
    void QueueUserLayout(const std::string& layout_name);
    void SaveCurrentLayoutToFileDialog();
    void LoadLayoutFromFileDialog();
    void DeleteLayout(const std::string& layout_name);

    void ResetAllLayouts();

    std::vector<std::string> GetBuiltinLayoutNames() const;
    std::vector<std::string> GetUserLayoutNames() const;
    const std::string& getCurrentLayoutName() const { return m_CurrentLayoutName; }
    bool IsCurrentLayout(const std::string& layout_name) const;

private:
    enum class BuiltinLayoutType
    {
        TwoByThree,
        FourSplit,
        Default,
        Tall,
        Wide,
        Unknown,
    };

    struct LayoutSnapshot
    {
        std::string name;
        // P9: native DockTree JSON is the only layout payload (empty for legacy ini-only
        // snapshots, which fall back to rebuilding the Default native tree on load).
        std::string dock_tree_json;
        std::unordered_map<std::string, bool> window_open_states;
    };

    bool ApplyPendingLayout();

    void ApplyBuiltinLayout(BuiltinLayoutType type);
    void ApplySnapshot(const LayoutSnapshot& snapshot);
    void ApplyBuiltinWindowStates(BuiltinLayoutType type);

    // Native-dock host renderer. When the native backend is on, builds the DockTree from
    // the open windows, solves geometry over the dock region, and paints the chrome
    // (splitters / tab strips / panel frames) through the shared overlay renderer. As of
    // P3 this is the live host (no ImGui DockSpace): EditorView::BeginGUI positions each
    // panel into its leaf rect via QueryNativeDockPanel. See ZSLATE_NATIVE_DOCK_PLAN.md.
    void RenderNativeDockPreview(const Vector2& origin, const Vector2& size);

    // PHASE 6 native-tree lifecycle. The native DockTree is the persistent source of
    // truth (not re-derived from the open set every frame). EnsureNativeDockTree
    // cold-initialises it (session file -> else Default builtin) and then reconciles it
    // incrementally with the open-window set each frame (adds newly-opened panels, removes
    // closed ones), so manual docking + builtin layouts + persistence stay coherent.
    void EnsureNativeDockTree();
    void ReconcileNativeTreeWithOpenWindows();
    void DockNativePanelSomewhere(const std::string& panel_id);
    void BuildNativeBuiltin(BuiltinLayoutType type);
    void ResetNativeInteractionState();

    // PHASE 7. Maximized rendering (single panel fills the host) + its inline input; and
    // closing a docked panel (close button on the active tab) which clears m_Open on the
    // window so the next reconcile removes it from the tree.
    void RenderNativeMaximized(const UIRect& host_rect, float scale);
    // Opaque content-area fill for the maximized panel. Runs in the kZDockBackground
    // z-group (before the panel paints and before RenderNativeMaximized's chrome), and
    // caches m_MaximizedContentRect so QueryNativeDockPanel can place the panel this frame.
    void RenderNativeMaximizedBackground(const UIRect& host_rect, float scale);
    void CloseNativePanel(const std::string& panel_id);

    // Session persistence for the native tree (mirrors ImGui's imgui.ini autosave).
    std::filesystem::path GetNativeSessionFilePath() const;
    bool LoadNativeSession();
    void SaveNativeSession(const std::string& json) const;
    void TickNativeSessionAutosave();

    // PHASE 4/5 input: splitter drag-resize, splitter double-click reset, tab-click
    // activation, and tab drag-to-dock (P5). Hit-tests the live tree against ImGui IO;
    // suppressed while a panel / popup / menu owns the mouse (gated on IsWindowHovered of
    // the MenuController host). Returns true if it structurally mutated the tree (a
    // completed MovePanel), so the caller re-solves geometry before painting.
    bool HandleNativeDockInput(const UIRect& host_rect);

public:
    // PHASE 3 panel-hosting query. When the native backend is on, EditorView::BeginGUI
    // asks the layout where to place each panel. Returns true and fills out_rect
    // (x,y,w,h in screen pixels = the leaf ContentRect) + out_is_active (whether `title`
    // is the leaf's active tab) when `title` is docked in the live native tree. Returns
    // false for panels not placed by the native tree (they fall back to floating).
    bool QueryNativeDockPanel(const char* title, float out_rect[4], bool& out_is_active) const;

    // --- Editor tear-off (true OS multi-window) -------------------------------
    // Mark `title` as floating (detached into its own OS window) or re-docked.
    // Floating panels are removed from the native dock tree and excluded from the
    // per-frame reconcile (ReconcileNativeTreeWithOpenWindows) so they are NOT
    // re-docked while open in a separate window. Setting floating=false drops the
    // mark; the next reconcile re-docks the (still-open) panel.
    void SetPanelFloating(const char* title, bool floating);
    bool IsPanelFloating(const char* title) const;

    // The FloatingPanelManager calls these around a floating window's panel paint
    // so QueryNativeDockPanel returns the floating window's CLIENT-LOCAL rect
    // (origin 0,0) and reports the panel active for the duration of that paint.
    // Outside this scope a floating panel is not in the tree, so QueryNativeDockPanel
    // returns false and EditorUI's main loop skips it (it is painted by the manager).
    void BeginFloatingPanelRender(const char* title, float x, float y, float width, float height);
    void EndFloatingPanelRender();

    // While a floating panel is being dragged over the main dock host, the
    // FloatingPanelManager sets this so OnGUI paints a translucent "dock here"
    // highlight over the host (a simplified version of UE's dock target overlay).
    void SetExternalDockHint(bool active) { m_ExternalDockHint = active; }

private:

    LayoutSnapshot CaptureCurrentLayout(const std::string& layout_name) const;
    bool SaveSnapshotToFile(const LayoutSnapshot& snapshot, const std::filesystem::path& file_path) const;
    bool LoadSnapshotFromFile(const std::filesystem::path& file_path, LayoutSnapshot& snapshot) const;
    bool SaveCurrentLayout(const std::string& layout_name);
    bool LoadLayoutFromPath(const std::filesystem::path& file_path);

    std::filesystem::path GetUserLayoutDirectory() const;
    std::filesystem::path GetUserLayoutFilePath(const std::string& layout_name) const;
    std::string SanitizeLayoutName(const std::string& layout_name) const;
    std::string NormalizeLayoutName(const std::string& layout_name) const;
    std::string MakeSuggestedLayoutName() const;
    BuiltinLayoutType ToBuiltinLayoutType(const std::string& layout_name) const;
    const char* ToBuiltinLayoutName(BuiltinLayoutType type) const;

private:
    std::string m_CurrentLayoutName {"Default"};
    std::string m_PendingBuiltinLayout;
    std::optional<LayoutSnapshot> m_PendingSnapshot;
    bool m_ResetAllRequested {false};
    bool m_OpenSaveDialogRequested {false};
    std::array<char, 128> m_LayoutNameBuffer {};

    // P11f: native Save Layout dialog (replaces the ImGui BeginPopupModal). Built once
    // on open, then painted + input-routed each frame in DrawDialogs through the editor
    // overlay. While open it registers a full-screen Foreground scrim surface so the
    // native dock chrome (HoveredSurfacePrev) and native panels (HoveredSurface) are
    // suppressed -- i.e. modal for everything native. ImGui-hosted islands are only
    // visually dimmed (overlay composites after ImGui); they are not input-blocked,
    // matching the existing native menu-dropdown model (IsForegroundCapturing is not
    // consumed by any island yet). Resolved when the islands migrate off ImGui.
    bool m_SaveDialogOpen {false};
    std::shared_ptr<ZSlate::SWidget> m_SaveDialogRoot;
    std::shared_ptr<ZSlate::SEditableTextBox> m_SaveDialogEdit;
    ZSlate::SlateInputRouter m_SaveDialogInput;
    // Button / Enter callbacks fire from inside SlateInputRouter::ProcessMouse/Key, which
    // keeps raw widget pointers alive across the call -- so they only flag intent here and
    // the actual commit/close (which frees the widget tree) runs after routing completes.
    bool m_SaveDialogWantCommit {false};
    bool m_SaveDialogWantClose {false};
    void BuildSaveLayoutDialog(float scale);
    void CloseSaveLayoutDialog();
    void CommitSaveLayoutDialog();

    // PHASE 2+ native-dock host state. The tree is the persistent source of truth
    // (PHASE 6); each frame it is reconciled incrementally with the open-window set, so
    // the per-frame cost is a geometry solve + paint, not a full structural rebuild.
    EditorDock::DockTree m_NativeDockTree;
    EditorDock::DockHost m_NativeDockHost;
    // Split node currently being drag-resized (null when idle). Valid only between a
    // splitter mouse-down and the matching mouse-up; the tree is never structurally
    // rebuilt mid-drag (the open-window set can't change while the button is held), but
    // HandleNativeDockInput re-validates the pointer against the tree each frame anyway.
    EditorDock::DockNode* m_DraggingSplitter {nullptr};

    // PHASE 5 drag-to-dock state. A tab press arms m_TabDragId (+ start pos); once the
    // cursor moves past a threshold m_TabDragActive flips true and each frame recomputes
    // the hovered drop target (m_DropTargetLeaf) + direction (m_DropDir). On release the
    // panel is moved via DockTree::MovePanel. All cleared on a structural rebuild.
    std::string m_TabDragId;
    bool m_TabDragActive {false};
    Vector2 m_TabDragStartMouse {};
    EditorDock::DockNode* m_DropTargetLeaf {nullptr};
    EditorDock::EDockDir m_DropDir {EditorDock::EDockDir::Center};
    bool m_HasDrop {false};

    // PHASE 6 persistence state.
    bool m_NativeTreeInitialized {false};
    bool m_NativeSessionRestoreAttempted {false};  // one-shot session-file restore guard
    std::string m_LastSavedNativeJson;  // last JSON written to the session file
    double m_NativeDirtySince {-1.0};   // ImGui::GetTime() of first unsaved change, or -1

    // PHASE 7 maximize/restore. When non-empty the named panel fills the whole dock host
    // and every other docked panel is hidden (QueryNativeDockPanel reports them inactive).
    // Double-clicking a tab toggles maximize; the restore button / a second double-click
    // exits. Cleared automatically if the panel is closed or undocked. The content rect is
    // cached each frame so the const QueryNativeDockPanel can place the maximized panel.
    std::string m_MaximizedPanelId;
    UIRect m_MaximizedContentRect {};
    UIRect m_RestoreButtonRect {};

    // Editor tear-off. Panels detached into their own OS window. Excluded from
    // the reconcile re-dock pass and removed from the native tree while floating.
    std::unordered_set<std::string> m_FloatingPanels;
    // Render override active during a floating window's panel paint (see
    // BeginFloatingPanelRender). When set, QueryNativeDockPanel returns the
    // client-local rect for m_FloatingRenderTitle and reports it active.
    std::string m_FloatingRenderTitle;
    UIRect m_FloatingRenderRect {};

    // Set by FloatingPanelManager during a floating-panel drag that hovers the host.
    bool m_ExternalDockHint {false};
};
