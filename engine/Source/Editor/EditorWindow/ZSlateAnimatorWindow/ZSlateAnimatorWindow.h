#pragma once

// =============================================================================
// ZSlateAnimatorWindow -- the authoring surface for AnimatorController (PR-SK3).
//
// PR-SK3 landed the state machine runtime, but a controller could only be built
// in code or by hand, which made the feature unusable in practice. This is
// Unity's Animator window: three editable tables (parameters / states /
// transitions) over the live controller asset.
//
// Built entirely from ZSlate widgets (no ImGui), following the proven
// ZSlateUMGDesignerWindow pattern: a retained chrome widget tree rebuilt when
// the model or the UI scale changes, painted through ZSlateEditorOverlay, fed by
// one SlateInputRouter.
//
// Editing rules worth knowing before touching this file:
//   * transitions / conditions address parameters BY NAME and states BY INDEX,
//     so deleting a state must renumber every transition (see RemoveState);
//   * numeric fields are SEditableTextBox + OnTextCommitted parsing -- a bad
//     value keeps the previous one instead of writing NaN into the asset;
//   * text commits do NOT rebuild the tree (that would drop keyboard focus);
//     structural edits and enum cycles do.
// =============================================================================

#include "Editor/EditorWindow/EditorWindow.h"
#include "Runtime/Resource/ResType/Data/AnimatorController.h"
#include "ZSlate/Application/SlateInput.h"

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ZSlate
{
class SWidget;
class SButton;
class SVerticalBox;
class SScrollBox;
struct FDragDropOperation;
class SEditableTextBox;
class STextBlock;
}  // namespace ZSlate

// A ZSlate-rendered, dockable Animator controller editor (dock title "Animator").
class ZSlateAnimatorWindow : public EditorWindow
{
public:
    explicit ZSlateAnimatorWindow(EditorUI* editor_ui);
    void OnGUI() override;
    bool SupportsNativeHosting() const override { return true; }

    /// Creates an empty controller bound to `asset_path` (does not write).
    bool NewController(const std::string& asset_path, std::string& out_error);
    /// Loads an existing controller. Accepts an absolute path or one relative to
    /// the project content root.
    bool OpenController(const std::string& asset_path, std::string& out_error);
    /// Writes the controller to `m_AssetPath`.
    bool SaveController(std::string& out_error);

    AnimatorController* GetController() const { return m_Controller; }
    const std::string& GetAssetPath() const { return m_AssetPath; }
    bool HasUnsavedChanges() const { return m_ModelDirty; }
    /// Opens the window and loads `asset_path` (used by console / other panels).
    bool ShowAndOpen(const std::string& asset_path, std::string& out_error);

private:
    void BuildChrome(float scale);
    void BuildParameterRows(const std::shared_ptr<ZSlate::SScrollBox>& in_box, float scale);
    void BuildStateRows(const std::shared_ptr<ZSlate::SScrollBox>& in_box, float scale);
    void BuildTransitionRows(const std::shared_ptr<ZSlate::SScrollBox>& in_box, float scale);
    /// One state's rows; shared by the States tab and the canvas strip.
    void BuildStateBody(const std::shared_ptr<ZSlate::SScrollBox>& in_box, float scale, int captured);
    /// One transition's rows; shared by the Transitions tab and the canvas strip.
    void BuildTransitionBody(const std::shared_ptr<ZSlate::SScrollBox>& in_box, float scale, int captured);
    /// Strip under the canvas for the node selected on the graph.
    void BuildSelectedStateEditor(const std::shared_ptr<ZSlate::SVerticalBox>& in_parent, float scale);

    // ---- Clip / anim-skel-map picker ---------------------------------------
    /// Which field of a state the picker is choosing. `None` = closed.
    enum class PickField
    {
        None,
        Clip,
        SkelMap,
    };

    /// Opens the picker over the right pane. `state_index` is captured here
    /// rather than read from `m_SelectedState`, because the picker is also
    /// reachable from the States tab (which has no selection).
    void OpenPicker(PickField field, int state_index);
    void ClosePicker();
    /// Collects the candidates from the Content Browser's own asset tree.
    void RefreshPickCandidates();
    void ApplyPick(const std::string& content_relative_path);
    void BuildPickerPanel(const std::shared_ptr<ZSlate::SVerticalBox>& in_parent, float scale);
    /// Re-filters the built rows every frame by toggling `Visibility`, so typing
    /// in the filter never rebuilds the tree (a rebuild drops keyboard focus).
    void UpdatePickFilter();

    // ---- Cross-window drop target (Content Browser -> canvas) ---------------
    /// Applies the cached drag op to whatever is under `mouse`: an animation
    /// clip / anim-skel-map onto a node, or a controller onto blank canvas.
    void ApplyAssetDrop(const ZSlate::Vector2& mouse);

    // ---- Drag SOURCE (canvas -> other windows) ------------------------------
    /// Publishes this state's clip as an `FAssetDragDropOp`. The canvas is
    /// hand-painted, so it has no widget to answer `OnDragDetected`: it starts
    /// the gesture itself -- and must clear the channel itself on release,
    /// because no router owns the op.
    void StartAssetDragOut(const ZSlate::Vector2& mouse);
    void EndAssetDragOut();
    /// Node whose asset line is under `point` (-1 = none), reporting which line
    /// in `out_slot`. Only lines that actually hold a path qualify -- there is
    /// nothing to drag out of an empty one.
    int HitAssetChip(const ZSlate::UIRect& region, const ZSlate::Vector2& point, int& out_slot) const;
    /// Runtime asset type of a path, lowercased with any "Res" suffix stripped
    /// ("AnimationAsset" -> "animationasset"); empty when it cannot be read.
    static std::string NormalizeAssetType(const std::string& path);
    /// Strip under the canvas for the arrow selected on the graph.
    void BuildSelectedTransitionEditor(const std::shared_ptr<ZSlate::SVerticalBox>& in_parent, float scale);

    // ---- Graph canvas ------------------------------------------------------
    /// The right-hand pane is tabbed: the graph is hand-painted (nodes + edges
    /// + drag gestures), the other two are the plain widget tables.
    enum class ViewTab
    {
        Graph,
        States,
        Transitions,
    };

    /// Painted straight through ISlateRenderer: the chrome's `UIRenderer` has
    /// no DrawTextLabel and its DrawConvexPoly takes the engine's own Vector2,
    /// which is a different type from ZSlate::Vector2.
    void PaintGraph(ZSlate::ISlateRenderer& renderer, const ZSlate::UIRect& region, float scale) const;
    /// Returns true when the click was consumed (so it must not reach the chrome).
    bool HandleGraphInput(const ZSlate::UIRect& region, const ZSlate::Vector2& mouse, bool hovered);
    void AutoLayout();
    int HitNode(const ZSlate::UIRect& region, const ZSlate::Vector2& point) const;
    bool HitPort(const ZSlate::UIRect& region, const ZSlate::Vector2& point, int state_index) const;
    /// Nearest arrow within a few px of `point`, or -1. Later arrows win (they
    /// are painted last, so they are on top).
    int HitTransition(const ZSlate::UIRect& region, const ZSlate::Vector2& point) const;
    /// Screen-space endpoints of an arrow; shared by painting and hit-testing so
    /// the two can never disagree about where an arrow is.
    bool GetTransitionEndpoints(const ZSlate::UIRect& region,
                                float scale,
                                const AnimatorTransition& transition,
                                ZSlate::Vector2& out_from,
                                ZSlate::Vector2& out_to) const;

    std::shared_ptr<ZSlate::STextBlock> MakeLabel(const std::string& text, float font);
    std::shared_ptr<ZSlate::SButton> MakeButton(const std::string& label, float font, std::function<void()> on_click);
    std::shared_ptr<ZSlate::SEditableTextBox> MakeTextBox(const std::string& in_text,
                                                          float font,
                                                          float min_width,
                                                          const std::string& in_hint,
                                                          std::function<void(const std::string&)> on_commit);

    /// Absolute path for a user-typed path (relative input resolves against the
    /// project content root).
    std::string ResolvePath(const std::string& in_path) const;
    void UpdateStatus();

    // ---- Model ops (each marks the chrome dirty) ---------------------------
    void AddParameter();
    void RemoveParameter(int index);
    void AddState();
    void RemoveState(int index);
    void AddTransition();
    void RemoveTransition(int index);
    void AddCondition(int transition_index);
    void RemoveCondition(int transition_index, int condition_index);
    void AddEvent(int state_index);
    void RemoveEvent(int state_index, int event_index);

    // ---- Model -------------------------------------------------------------
    std::string m_AssetPath;
    /// Owned by the asset system (loaded) or ObjectManager (produced); the
    /// window must never free it.
    AnimatorController* m_Controller {nullptr};

    bool m_UiDirty {true};
    bool m_ModelDirty {false};

    // ---- Chrome (retained widget tree) -------------------------------------
    std::shared_ptr<ZSlate::SWidget> m_Chrome;
    std::shared_ptr<ZSlate::SEditableTextBox> m_PathBox;
    std::shared_ptr<ZSlate::STextBlock> m_StatusText;
    float m_BuiltScale {-1.0f};

    // ---- Graph state -------------------------------------------------------
    ViewTab m_Tab {ViewTab::Graph};
    /// Placeholder occupying the right pane in Graph mode; its cached geometry
    /// after painting IS the canvas the graph is drawn into, so the hand-painted
    /// region can never drift from the widget layout.
    std::shared_ptr<ZSlate::SWidget> m_GraphSlot;
    ZSlate::UIRect m_GraphRect {0.0f, 0.0f, 0.0f, 0.0f};
    /// Node and arrow selection are mutually exclusive: picking one clears the
    /// other, and only one strip is ever built under the canvas.
    int m_SelectedState {-1};
    int m_SelectedTransition {-1};
    /// EditorSlateHost exposes no "right pressed this frame", so the edge is
    /// latched here.
    bool m_RightWasDown {false};
    /// Node drag.
    bool m_DraggingNode {false};
    int m_DragNode {-1};
    ZSlate::Vector2 m_DragGrabOffset {0.0f, 0.0f};
    /// Drag-to-connect: from a node's bottom port onto another node.
    bool m_Connecting {false};
    int m_ConnectFrom {-1};
    ZSlate::Vector2 m_ConnectCursor {0.0f, 0.0f};

    // ---- Picker ------------------------------------------------------------
    PickField m_PickField {PickField::None};
    int m_PickStateIndex {-1};
    std::string m_PickFilter;
    /// Content-root-relative paths (what the runtime's loadAsset expects).
    std::vector<std::string> m_PickCandidates;
    /// Built row -> lowered path, for the per-frame visibility filter.
    std::vector<std::pair<std::shared_ptr<ZSlate::SWidget>, std::string>> m_PickRows;

    // ---- Cross-window drag-drop --------------------------------------------
    /// Cached copy of the in-flight drag. On the release frame the source router
    /// may already have cleared the global channel (window draw order), so the
    /// drop acts on this copy -- same reason as ZSlateSceneWindow.
    std::shared_ptr<ZSlate::FDragDropOperation> m_PendingDragOp;
    /// Node under an incoming drag (-1 = none): painted as the drop target.
    int m_DropHoverState {-1};
    ZSlate::Vector2 m_DropCursor {0.0f, 0.0f};

    /// Press landed on a clip line ("armed"), and the gesture turned into a
    /// published drag once the pointer moved far enough.
    bool m_DragOutArmed {false};
    int m_DragOutState {-1};
    /// Which of the node's asset lines is being dragged (see AssetSlot).
    int m_DragOutSlot {0};
    ZSlate::Vector2 m_DragOutOrigin {0.0f, 0.0f};
    bool m_DragOutActive {false};
    /// Asset line under the cursor, highlighted so it reads as a handle.
    int m_HoverChipState {-1};
    int m_HoverChipSlot {0};

    ZSlate::SlateInputRouter m_Input;
};
