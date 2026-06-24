#pragma once

#include "Editor/EditorWindow/EditorWindow.h"
#include "Runtime/Core/Math/Vector2.h"
#include "ZSlate/Core/SlatePaint.h"
#include "Runtime/UI/Render/UIRenderer.h"
#include "ZSlate/Application/SlateInput.h"
#include "Runtime/UI/Core/UITypes.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ZSlate
{
class SWidget;
}

class UIRenderer;

// Node pin data type (drives the pin / link colour).
enum class BpPinType
{
    Flow,
    Bool,
    Int,
    Float,
    String,
    Object
};

struct BpPin
{
    int id {0};
    BpPinType type {BpPinType::Flow};
    bool is_input {false};
    std::string name;
};

struct BpNode
{
    int id {0};
    std::string title;
    Vector2 position {0.0f, 0.0f};  // canvas-space (unscaled) coordinates
    std::vector<BpPin> inputs;
    std::vector<BpPin> outputs;
    bool selected {false};
};

struct BpLink
{
    int from_node_id {0};
    int from_pin_id {0};
    int to_node_id {0};
    int to_pin_id {0};
};

// A ZSlate-rendered, dockable Blueprint node-graph editor (dock title
// "Blueprint"). The "add node" actions live in a persistent ZSlate toolbar
// (replacing the legacy ImGui right-click popup); the node graph itself
// (grid + nodes + pins + bezier links) is a custom canvas painted directly
// through the active UIRenderer (DrawQuad / DrawRect / DrawText -- bezier links
// are sampled into short quads since the renderer has no native line/curve
// primitive) and hit-tested manually. No ImGui widgets are used.
class ZSlateBlueprintWindow : public EditorWindow
{
public:
    explicit ZSlateBlueprintWindow(EditorUI* editor_ui);
    void OnGUI() override;
    bool SupportsNativeHosting() const override { return true; }

private:
    void BuildToolbar(float scale);
    void PaintCanvas(UIRenderer& renderer, const UIRect& region, float scale);
    void HandleCanvasInput(const Vector2& mouse,
                           const Vector2& mouse_delta,
                           bool over_canvas,
                           bool left_clicked,
                           bool left_down,
                           bool left_released,
                           bool middle_down);

    // Graph ops.
    void AddNode(const std::string& title,
                 const Vector2& position,
                 const std::vector<std::pair<std::string, BpPinType>>& inputs,
                 const std::vector<std::pair<std::string, BpPinType>>& outputs);
    void AddNodeInView(const std::string& title,
                       const std::vector<std::pair<std::string, BpPinType>>& inputs,
                       const std::vector<std::pair<std::string, BpPinType>>& outputs);
    void CreateLink(int from_node_id, int from_pin_id, int to_node_id, int to_pin_id);
    void DeleteLink(int link_index);
    void DeleteSelection();
    bool CanConnectPins(const BpPin& from_pin, const BpPin& to_pin) const;

    // Geometry helpers (use the per-frame origin / scale captured in OnGUI).
    Vector2 NodeScreen(const BpNode& node) const;
    Vector2 NodeSize(const BpNode& node) const;
    Vector2 PinScreen(const BpNode& node, int pin_index, bool is_input) const;
    BpNode* FindNode(int node_id);
    std::pair<BpNode*, BpPin*> FindPinAt(const Vector2& screen_pos);

    UIColor PinColor(BpPinType type) const;

    // ---- Graph state -------------------------------------------------------
    std::vector<BpNode> m_Nodes;
    std::vector<BpLink> m_Links;
    int m_NextNodeId {1};
    int m_NextPinId {1};
    int m_AddCascade {0};

    Vector2 m_Scroll {0.0f, 0.0f};

    // Interaction.
    bool m_DraggingNode {false};
    int m_DraggingNodeId {-1};
    bool m_DraggingLink {false};
    int m_DragFromNodeId {-1};
    int m_DragFromPinId {-1};
    int m_HoveredPinNodeId {-1};
    int m_HoveredPinId {-1};
    int m_HoveredLinkIndex {-1};
    int m_SelectedLinkIndex {-1};

    // Per-frame paint context (set in OnGUI, used by geometry helpers).
    Vector2 m_CanvasOrigin {0.0f, 0.0f};
    float m_Scale {1.0f};

    // ---- ZSlate toolbar ----------------------------------------------------
    std::shared_ptr<ZSlate::SWidget> m_Toolbar;
    float m_BuiltScale {-1.0f};
    float m_ToolbarHeight {0.0f};

    ZSlate::SlateInputRouter m_Input;
};
