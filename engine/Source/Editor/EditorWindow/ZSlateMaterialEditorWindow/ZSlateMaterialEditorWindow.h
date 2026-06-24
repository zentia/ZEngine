#pragma once

#include "Editor/EditorWindow/EditorWindow.h"
#include "Runtime/Core/Math/Vector2.h"
#include "ZSlate/Core/SlatePaint.h"
#include "Runtime/UI/Render/UIRenderer.h"
#include "ZSlate/Application/SlateInput.h"
#include "Runtime/UI/Core/UITypes.h"

#include <array>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ZSlate
{
class SWidget;
}

class UIRenderer;

// Material node pin data type (drives the pin / link colour and connect rules).
enum class MatPinType
{
    Float,
    Float2,
    Float3,
    Float4,
    Texture2D,
    TextureCube,
    Bool,
    Int
};

enum class MatNodeType
{
    Input_Texture2D,
    Input_TextureCube,
    Input_Constant,
    Input_Constant2,
    Input_Constant3,
    Input_Constant4,
    Input_Time,
    Input_UV,
    Math_Add,
    Math_Subtract,
    Math_Multiply,
    Math_Divide,
    Math_Lerp,
    Math_Clamp,
    Math_Saturate,
    Vector_Combine,
    Vector_Split,
    Texture_Sample2D,
    Output_BaseColor,
    Output_Metallic,
    Output_Roughness,
    Output_Normal
};

struct MatPin
{
    int id {0};
    MatPinType type {MatPinType::Float};
    bool is_input {false};
    std::string name;
};

struct MatNodeData
{
    float float_value {0.0f};
    std::array<float, 4> vec_value {{0.0f, 0.0f, 0.0f, 0.0f}};
    std::string texture_path;
};

struct MatNode
{
    int id {0};
    MatNodeType node_type {MatNodeType::Input_Constant};
    std::string title;
    Vector2 position {0.0f, 0.0f};  // canvas-space (unscaled)
    std::vector<MatPin> inputs;
    std::vector<MatPin> outputs;
    MatNodeData data;
    bool selected {false};
};

struct MatLink
{
    int from_node_id {0};
    int from_pin_id {0};
    int to_node_id {0};
    int to_pin_id {0};
};

// A ZSlate-rendered, dockable Material node-graph editor (dock title "Material
// Editor"). Layout mirrors the legacy ImGui window: a persistent ZSlate toolbar
// of "add node" actions (replacing the right-click submenu palette), a node
// graph custom-canvas on the left (grid + nodes + typed square pins + bezier
// links, painted through the active UIRenderer and hit-tested manually), and a
// right-hand panel (material-preview placeholder + read-only properties for the
// selected node). No ImGui widgets are used. Live constant editing from the
// legacy in-node DragFloat widgets is presented read-only here (the graph is a
// visual prototype not yet wired to a real material compile).
class ZSlateMaterialEditorWindow : public EditorWindow
{
public:
    explicit ZSlateMaterialEditorWindow(EditorUI* editor_ui);
    void OnGUI() override;
    bool SupportsNativeHosting() const override { return true; }

private:
    void BuildToolbar(float scale);

    void PaintGraph(UIRenderer& r, const UIRect& region, float scale);
    void PaintSidePanel(UIRenderer& r, const UIRect& region, float scale);
    void HandleGraphInput(const Vector2& mouse,
                          const Vector2& mouse_delta,
                          bool over_canvas,
                          bool left_clicked,
                          bool left_down,
                          bool left_released,
                          bool middle_down);

    // Graph ops.
    void AddNode(MatNodeType type, const Vector2& position);
    void AddNodeInView(MatNodeType type);
    void SetupNodePins(MatNode& node);
    void CreateLink(int from_node_id, int from_pin_id, int to_node_id, int to_pin_id);
    void DeleteLink(int link_index);
    void DeleteSelection();
    bool CanConnectPins(const MatPin& from_pin, const MatPin& to_pin) const;
    bool InputPinHasLink(int node_id, int pin_id) const;

    // Geometry (per-frame origin / scale / scroll captured in OnGUI).
    Vector2 NodeScreen(const MatNode& node) const;
    Vector2 NodeSize(const MatNode& node) const;
    float NodeContentExtra(MatNodeType type) const;
    Vector2 PinScreen(const MatNode& node, int pin_index, bool is_input) const;
    MatNode* FindNode(int node_id);
    std::pair<MatNode*, MatPin*> FindPinAt(const Vector2& screen_pos);

    std::string NodeTitle(MatNodeType type) const;
    ZSlate::UIColor PinColor(MatPinType type) const;

    // ---- Graph state -------------------------------------------------------
    std::vector<MatNode> m_Nodes;
    std::vector<MatLink> m_Links;
    int m_NextNodeId {1};
    int m_NextPinId {1};
    int m_AddCascade {0};
    int m_SelectedNodeId {-1};

    Vector2 m_Scroll {0.0f, 0.0f};

    bool m_DraggingNode {false};
    int m_DraggingNodeId {-1};
    bool m_DraggingLink {false};
    int m_DragFromNodeId {-1};
    int m_DragFromPinId {-1};
    int m_HoveredPinNodeId {-1};
    int m_HoveredPinId {-1};
    int m_HoveredLinkIndex {-1};
    int m_SelectedLinkIndex {-1};

    // Per-frame paint context.
    Vector2 m_CanvasOrigin {0.0f, 0.0f};
    float m_Scale {1.0f};
    float m_SidePanelWidth {300.0f};

    // ---- ZSlate toolbar ----------------------------------------------------
    std::shared_ptr<ZSlate::SWidget> m_Toolbar;
    float m_BuiltScale {-1.0f};
    float m_ToolbarHeight {0.0f};

    ZSlate::SlateInputRouter m_Input;
};
