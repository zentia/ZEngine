#include "ZSlateBlueprintWindow.h"

#include "Editor/EditorLayout/EditorLayoutWindowIds.h"
#include "Editor/ZSlate/Backend/EditorSlateHost.h"
#include "Editor/ZSlate/Backend/ZSlateEditorOverlay.h"  // native RHI backend
#include "ZSlate/Application/SlateApplication.h"
#include "ZSlate/Widgets/Panels/SBorder.h"
#include "ZSlate/Widgets/Layout/SBoxPanel.h"
#include "ZSlate/Widgets/Input/SButton.h"
#include "ZSlate/Widgets/Layout/SSpacer.h"
#include "ZSlate/Widgets/Text/STextBlock.h"
#include "Runtime/UI/Render/UIRenderer.h"

#include <algorithm>
#include <cmath>
#include <functional>

using namespace ZSlate;

namespace
{
const ZSlate::UIColor kPanelColor(0.10f, 0.10f, 0.12f, 1.0f);
const ZSlate::UIColor kCanvasBg(0.118f, 0.118f, 0.118f, 1.0f);
const ZSlate::UIColor kGridColor(0.196f, 0.196f, 0.196f, 0.6f);
const ZSlate::UIColor kNodeBg(0.157f, 0.157f, 0.196f, 1.0f);
const ZSlate::UIColor kNodeBgSelected(0.235f, 0.235f, 0.314f, 1.0f);
const ZSlate::UIColor kNodeBorder(0.392f, 0.392f, 0.471f, 1.0f);
const ZSlate::UIColor kTitleColor(1.0f, 1.0f, 0.5f, 1.0f);
const ZSlate::UIColor kLabelColor(0.85f, 0.86f, 0.90f, 1.0f);
const ZSlate::UIColor kWhite(1.0f, 1.0f, 1.0f, 1.0f);
const ZSlate::UIColor kLinkHover(1.0f, 1.0f, 0.39f, 1.0f);

// Layout constants (unscaled px).
constexpr float kNodeWidth = 160.0f;
constexpr float kTitleH = 24.0f;
constexpr float kPinRow = 20.0f;
constexpr float kPinTopPad = 10.0f;
constexpr float kPinRadius = 7.0f;
constexpr float kGridSize = 64.0f;

std::shared_ptr<STextBlock> MakeText(const std::string& text, float font_size, const ZSlate::UIColor& color)
{
    auto t = std::make_shared<STextBlock>();
    t->Text = text;
    t->FontSize = font_size;
    t->Color = color;
    t->Alignment = ZSlate::TextAnchor::MiddleCenter;
    return t;
}
}  // namespace

ZSlateBlueprintWindow::ZSlateBlueprintWindow(EditorUI* editor_ui)
    : EditorWindow(editor_ui, EditorLayoutWindowIds::kBlueprint)
{
    m_Open = false;

    // Seed a few example nodes so the graph isn't empty on first open.
    AddNode("Start", ZSlate::Vector2(80.0f, 80.0f), {}, {{"Out", BpPinType::Flow}});
    AddNode("Print",
            ZSlate::Vector2(360.0f, 80.0f),
            {{"In", BpPinType::Flow}, {"Message", BpPinType::String}},
            {{"Out", BpPinType::Flow}});
    AddNode("Add",
            ZSlate::Vector2(360.0f, 280.0f),
            {{"A", BpPinType::Float}, {"B", BpPinType::Float}},
            {{"Result", BpPinType::Float}});
}

// ---------------------------------------------------------------------------
// Toolbar (ZSlate widget tree)
// ---------------------------------------------------------------------------
void ZSlateBlueprintWindow::BuildToolbar(float scale)
{
    const float font = 13.0f * scale;

    auto root = std::make_shared<SBorder>();
    root->BackgroundColor = kPanelColor;
    root->Padding = ZSlate::FMargin(6.0f * scale, 4.0f * scale);
    root->HAlign = EHorizontalAlignment::Fill;
    root->VAlign = EVerticalAlignment::Top;

    auto bar = std::make_shared<SHorizontalBox>();

    auto add_button = [&](const char* label, std::function<void()> on_click) {
        auto btn = std::make_shared<SButton>();
        btn->Padding = ZSlate::FMargin(8.0f * scale, 3.0f * scale);
        btn->SetContent(MakeText(label, font, kLabelColor));
        btn->OnClicked = std::move(on_click);
        bar->AddSlot(btn).AutoSize().SetVAlign(EVerticalAlignment::Center);
        bar->AddSlot(std::make_shared<SSpacer>(ZSlate::Vector2(5.0f * scale, 0.0f))).AutoSize();
    };

    add_button("+ Start", [this]() { AddNodeInView("Start", {}, {{"Out", BpPinType::Flow}}); });
    add_button("+ Print", [this]() {
        AddNodeInView("Print", {{"In", BpPinType::Flow}, {"Message", BpPinType::String}}, {{"Out", BpPinType::Flow}});
    });
    add_button("+ Add", [this]() {
        AddNodeInView("Add", {{"A", BpPinType::Float}, {"B", BpPinType::Float}}, {{"Result", BpPinType::Float}});
    });
    add_button("+ Branch", [this]() {
        AddNodeInView("Branch",
                      {{"In", BpPinType::Flow}, {"Condition", BpPinType::Bool}},
                      {{"True", BpPinType::Flow}, {"False", BpPinType::Flow}});
    });

    bar->AddSlot(std::make_shared<SSpacer>(ZSlate::Vector2(10.0f * scale, 0.0f))).AutoSize();
    add_button("Delete Selected", [this]() { DeleteSelection(); });

    root->SetContent(bar);
    m_Toolbar = root;
}

// ---------------------------------------------------------------------------
// Geometry helpers (depend on per-frame m_CanvasOrigin / m_Scale / m_Scroll)
// ---------------------------------------------------------------------------
ZSlate::Vector2 ZSlateBlueprintWindow::NodeScreen(const BpNode& node) const
{
    return ZSlate::Vector2(m_CanvasOrigin.x + m_Scroll.x + node.position.x * m_Scale,
                   m_CanvasOrigin.y + m_Scroll.y + node.position.y * m_Scale);
}

ZSlate::Vector2 ZSlateBlueprintWindow::NodeSize(const BpNode& node) const
{
    const float rows = static_cast<float>(std::max(node.inputs.size(), node.outputs.size()));
    const float content = rows * kPinRow * m_Scale;
    return ZSlate::Vector2(kNodeWidth * m_Scale, (kTitleH + kPinTopPad) * m_Scale + content + kPinTopPad * m_Scale);
}

ZSlate::Vector2 ZSlateBlueprintWindow::PinScreen(const BpNode& node, int pin_index, bool is_input) const
{
    const ZSlate::Vector2 sp = NodeScreen(node);
    const float y = sp.y + (kTitleH + kPinTopPad) * m_Scale + static_cast<float>(pin_index) * kPinRow * m_Scale;
    const float x = is_input ? sp.x : sp.x + NodeSize(node).x;
    return ZSlate::Vector2(x, y);
}

BpNode* ZSlateBlueprintWindow::FindNode(int node_id)
{
    for (auto& n : m_Nodes)
        if (n.id == node_id)
            return &n;
    return nullptr;
}

std::pair<BpNode*, BpPin*> ZSlateBlueprintWindow::FindPinAt(const ZSlate::Vector2& p)
{
    const float r = kPinRadius * m_Scale + 2.0f;
    for (auto& node : m_Nodes)
    {
        for (size_t i = 0; i < node.inputs.size(); ++i)
        {
            const ZSlate::Vector2 pp = PinScreen(node, static_cast<int>(i), true);
            if ((p.x - pp.x) * (p.x - pp.x) + (p.y - pp.y) * (p.y - pp.y) <= r * r)
                return {&node, &node.inputs[i]};
        }
        for (size_t i = 0; i < node.outputs.size(); ++i)
        {
            const ZSlate::Vector2 pp = PinScreen(node, static_cast<int>(i), false);
            if ((p.x - pp.x) * (p.x - pp.x) + (p.y - pp.y) * (p.y - pp.y) <= r * r)
                return {&node, &node.outputs[i]};
        }
    }
    return {nullptr, nullptr};
}

ZSlate::UIColor ZSlateBlueprintWindow::PinColor(BpPinType type) const
{
    switch (type)
    {
        case BpPinType::Flow:
            return ZSlate::UIColor(1.0f, 1.0f, 1.0f, 1.0f);
        case BpPinType::Bool:
            return ZSlate::UIColor(0.86f, 0.19f, 0.19f, 1.0f);
        case BpPinType::Int:
            return ZSlate::UIColor(0.27f, 0.59f, 1.0f, 1.0f);
        case BpPinType::Float:
            return ZSlate::UIColor(0.58f, 0.89f, 0.29f, 1.0f);
        case BpPinType::String:
            return ZSlate::UIColor(1.0f, 0.19f, 0.19f, 1.0f);
        case BpPinType::Object:
            return ZSlate::UIColor(0.20f, 0.59f, 0.84f, 1.0f);
        default:
            return ZSlate::UIColor(0.5f, 0.5f, 0.5f, 1.0f);
    }
}

// ---------------------------------------------------------------------------
// Canvas paint
// ---------------------------------------------------------------------------
namespace
{
// Draw a cubic bezier as a chain of short filled quads (the UIRenderer has no
// native line/curve primitive; dense sampling reads as a continuous curve and
// works identically on both the native and ImGui-fallback backends).
void DrawBezier(UIRenderer& r, const ZSlate::Vector2& a, const ZSlate::Vector2& b, const ZSlate::Vector2& c, const ZSlate::Vector2& d,
                const ZSlate::UIColor& color, float thickness)
{
    const float chord = std::abs(d.x - a.x) + std::abs(d.y - a.y);
    int steps = static_cast<int>(chord / std::max(2.0f, thickness * 0.6f));
    steps = std::max(16, std::min(96, steps));
    const float half = thickness * 0.5f;

    ZSlate::Vector2 prev = a;
    for (int i = 1; i <= steps; ++i)
    {
        const float t = static_cast<float>(i) / static_cast<float>(steps);
        const float u = 1.0f - t;
        const float w0 = u * u * u;
        const float w1 = 3.0f * u * u * t;
        const float w2 = 3.0f * u * t * t;
        const float w3 = t * t * t;
        const ZSlate::Vector2 pt(w0 * a.x + w1 * b.x + w2 * c.x + w3 * d.x,
                         w0 * a.y + w1 * b.y + w2 * c.y + w3 * d.y);
        // Sub-sample between prev and pt so there are no gaps on steep segments.
        const float seg = std::abs(pt.x - prev.x) + std::abs(pt.y - prev.y);
        const int sub = std::max(1, static_cast<int>(seg / std::max(1.0f, half)));
        for (int s = 1; s <= sub; ++s)
        {
            const float st = static_cast<float>(s) / static_cast<float>(sub);
            const float x = prev.x + (pt.x - prev.x) * st;
            const float y = prev.y + (pt.y - prev.y) * st;
            r.DrawQuad(ZSlate::UIRect(x - half, y - half, thickness, thickness), color);
        }
        prev = pt;
    }
}
}  // namespace

void ZSlateBlueprintWindow::PaintCanvas(UIRenderer& r, const ZSlate::UIRect& region, float scale)
{
    r.DrawQuad(region, kCanvasBg);
    r.PushClipRect(region, true);

    // Grid.
    const float grid = kGridSize * scale;
    const float ox = std::fmod(m_Scroll.x, grid);
    const float oy = std::fmod(m_Scroll.y, grid);
    for (float x = ox; x < region.width; x += grid)
        r.DrawQuad(ZSlate::UIRect(region.x + x, region.y, 1.0f, region.height), kGridColor);
    for (float y = oy; y < region.height; y += grid)
        r.DrawQuad(ZSlate::UIRect(region.x, region.y + y, region.width, 1.0f), kGridColor);

    // Links.
    for (size_t i = 0; i < m_Links.size(); ++i)
    {
        const BpLink& link = m_Links[i];
        BpNode* from = FindNode(link.from_node_id);
        BpNode* to = FindNode(link.to_node_id);
        if (from == nullptr || to == nullptr)
            continue;

        int from_idx = -1;
        for (size_t k = 0; k < from->outputs.size(); ++k)
            if (from->outputs[k].id == link.from_pin_id)
                from_idx = static_cast<int>(k);
        int to_idx = -1;
        for (size_t k = 0; k < to->inputs.size(); ++k)
            if (to->inputs[k].id == link.to_pin_id)
                to_idx = static_cast<int>(k);
        if (from_idx < 0 || to_idx < 0)
            continue;

        const ZSlate::Vector2 a = PinScreen(*from, from_idx, false);
        const ZSlate::Vector2 d = PinScreen(*to, to_idx, true);
        const ZSlate::Vector2 b(a.x + 50.0f * scale, a.y);
        const ZSlate::Vector2 c(d.x - 50.0f * scale, d.y);

        ZSlate::UIColor color = PinColor(from->outputs[from_idx].type);
        color.w = 0.85f;
        const bool hot = (m_HoveredLinkIndex == static_cast<int>(i) || m_SelectedLinkIndex == static_cast<int>(i));
        if (hot)
            color = kLinkHover;
        DrawBezier(r, a, b, c, d, color, (hot ? 4.0f : 3.0f) * scale);
    }

    // Link drag preview.
    if (m_DraggingLink)
    {
        BpNode* from = FindNode(m_DragFromNodeId);
        if (from != nullptr)
        {
            int from_idx = -1;
            for (size_t k = 0; k < from->outputs.size(); ++k)
                if (from->outputs[k].id == m_DragFromPinId)
                    from_idx = static_cast<int>(k);
            if (from_idx >= 0)
            {
                const ZSlate::Vector2 m = ZSlate::EditorSlateHost::Get().GetPointerPos();
                const ZSlate::Vector2 a = PinScreen(*from, from_idx, false);
                const ZSlate::Vector2 d = m;
                DrawBezier(r, a, ZSlate::Vector2(a.x + 50.0f * scale, a.y), ZSlate::Vector2(d.x - 50.0f * scale, d.y), d,
                           ZSlate::UIColor(1.0f, 1.0f, 1.0f, 0.6f), 2.0f * scale);
            }
        }
    }

    // Nodes.
    for (const BpNode& node : m_Nodes)
    {
        const ZSlate::Vector2 sp = NodeScreen(node);
        const ZSlate::Vector2 size = NodeSize(node);
        const ZSlate::UIRect body(sp.x, sp.y, size.x, size.y);

        r.DrawQuad(body, node.selected ? kNodeBgSelected : kNodeBg);
        r.DrawRect(body, kNodeBorder, 1.0f);

        // Title.
        r.DrawText(ZSlate::UIRect(sp.x + 6.0f * scale, sp.y, size.x - 12.0f * scale, kTitleH * scale),
                   node.title,
                   13.0f * scale,
                   kTitleColor,
                   ZSlate::TextAnchor::MiddleLeft,
                   ZSlate::TextWrapMode::NoWrap);
        // Title separator.
        r.DrawQuad(ZSlate::UIRect(sp.x, sp.y + kTitleH * scale, size.x, 1.0f), kNodeBorder);

        // Input pins + labels.
        for (size_t i = 0; i < node.inputs.size(); ++i)
        {
            const BpPin& pin = node.inputs[i];
            const ZSlate::Vector2 pp = PinScreen(node, static_cast<int>(i), true);
            const bool hov = (m_HoveredPinNodeId == node.id && m_HoveredPinId == pin.id);
            const float pr = (hov ? kPinRadius + 1.0f : kPinRadius) * scale;
            r.DrawQuad(ZSlate::UIRect(pp.x - pr, pp.y - pr, pr * 2.0f, pr * 2.0f), PinColor(pin.type));
            r.DrawRect(ZSlate::UIRect(pp.x - pr, pp.y - pr, pr * 2.0f, pr * 2.0f), kWhite, 1.0f);
            r.DrawText(ZSlate::UIRect(pp.x + pr + 3.0f * scale, pp.y - kPinRow * 0.5f * scale, size.x * 0.5f, kPinRow * scale),
                       pin.name,
                       11.0f * scale,
                       kLabelColor,
                       ZSlate::TextAnchor::MiddleLeft,
                       ZSlate::TextWrapMode::NoWrap);
        }

        // Output pins + labels.
        for (size_t i = 0; i < node.outputs.size(); ++i)
        {
            const BpPin& pin = node.outputs[i];
            const ZSlate::Vector2 pp = PinScreen(node, static_cast<int>(i), false);
            const bool hov = (m_HoveredPinNodeId == node.id && m_HoveredPinId == pin.id);
            const float pr = (hov ? kPinRadius + 1.0f : kPinRadius) * scale;
            r.DrawQuad(ZSlate::UIRect(pp.x - pr, pp.y - pr, pr * 2.0f, pr * 2.0f), PinColor(pin.type));
            r.DrawRect(ZSlate::UIRect(pp.x - pr, pp.y - pr, pr * 2.0f, pr * 2.0f), kWhite, 1.0f);
            r.DrawText(ZSlate::UIRect(pp.x - pr - 3.0f * scale - size.x * 0.5f,
                              pp.y - kPinRow * 0.5f * scale,
                              size.x * 0.5f,
                              kPinRow * scale),
                       pin.name,
                       11.0f * scale,
                       kLabelColor,
                       ZSlate::TextAnchor::MiddleRight,
                       ZSlate::TextWrapMode::NoWrap);
        }
    }

    r.PopClipRect();
}

// ---------------------------------------------------------------------------
// Canvas interaction (manual hit-testing against io.MousePos)
// ---------------------------------------------------------------------------
void ZSlateBlueprintWindow::HandleCanvasInput(const ZSlate::Vector2& mouse,
                                              const ZSlate::Vector2& mouse_delta,
                                              bool over_canvas,
                                              bool left_clicked,
                                              bool left_down,
                                              bool left_released,
                                              bool middle_down)
{
    // Middle-drag pans the canvas.
    if (over_canvas && middle_down)
    {
        m_Scroll.x += mouse_delta.x;
        m_Scroll.y += mouse_delta.y;
    }

    // Hover detection for pins + links (only meaningful over the canvas).
    m_HoveredPinNodeId = -1;
    m_HoveredPinId = -1;
    m_HoveredLinkIndex = -1;
    if (over_canvas)
    {
        const auto hovered = FindPinAt(mouse);
        if (hovered.first != nullptr && hovered.second != nullptr)
        {
            m_HoveredPinNodeId = hovered.first->id;
            m_HoveredPinId = hovered.second->id;
        }
        else
        {
            // Link hover via midpoint proximity (matches the legacy heuristic).
            for (size_t i = 0; i < m_Links.size(); ++i)
            {
                BpNode* from = FindNode(m_Links[i].from_node_id);
                BpNode* to = FindNode(m_Links[i].to_node_id);
                if (from == nullptr || to == nullptr)
                    continue;
                int fi = -1, ti = -1;
                for (size_t k = 0; k < from->outputs.size(); ++k)
                    if (from->outputs[k].id == m_Links[i].from_pin_id)
                        fi = static_cast<int>(k);
                for (size_t k = 0; k < to->inputs.size(); ++k)
                    if (to->inputs[k].id == m_Links[i].to_pin_id)
                        ti = static_cast<int>(k);
                if (fi < 0 || ti < 0)
                    continue;
                const ZSlate::Vector2 a = PinScreen(*from, fi, false);
                const ZSlate::Vector2 d = PinScreen(*to, ti, true);
                const ZSlate::Vector2 mid((a.x + d.x) * 0.5f, (a.y + d.y) * 0.5f);
                if ((mouse.x - mid.x) * (mouse.x - mid.x) + (mouse.y - mid.y) * (mouse.y - mid.y) <= (12.0f * m_Scale) * (12.0f * m_Scale))
                    m_HoveredLinkIndex = static_cast<int>(i);
            }
        }
    }

    // Finish a link drag on release.
    if (m_DraggingLink && left_released)
    {
        const auto target = FindPinAt(mouse);
        if (target.first != nullptr && target.second != nullptr && target.second->is_input)
        {
            BpNode* from = FindNode(m_DragFromNodeId);
            BpPin* from_pin = nullptr;
            if (from != nullptr)
                for (auto& p : from->outputs)
                    if (p.id == m_DragFromPinId)
                        from_pin = &p;

            if (from != nullptr && from_pin != nullptr && CanConnectPins(*from_pin, *target.second))
            {
                // Input pins accept a single connection: drop any existing one.
                for (size_t i = 0; i < m_Links.size(); ++i)
                {
                    if (m_Links[i].to_node_id == target.first->id && m_Links[i].to_pin_id == target.second->id)
                    {
                        DeleteLink(static_cast<int>(i));
                        break;
                    }
                }
                CreateLink(from->id, from_pin->id, target.first->id, target.second->id);
            }
        }
        m_DraggingLink = false;
        m_DragFromNodeId = -1;
        m_DragFromPinId = -1;
    }

    // Drag a node body.
    if (m_DraggingNode && left_down)
    {
        if (BpNode* n = FindNode(m_DraggingNodeId))
        {
            n->position.x += mouse_delta.x / m_Scale;
            n->position.y += mouse_delta.y / m_Scale;
        }
    }
    if (left_released)
    {
        m_DraggingNode = false;
        m_DraggingNodeId = -1;
    }

    if (!over_canvas || !left_clicked)
        return;

    // 1. Output pin -> start a link drag.
    const auto pin_hit = FindPinAt(mouse);
    if (pin_hit.first != nullptr && pin_hit.second != nullptr && !pin_hit.second->is_input)
    {
        m_DraggingLink = true;
        m_DragFromNodeId = pin_hit.first->id;
        m_DragFromPinId = pin_hit.second->id;
        m_SelectedLinkIndex = -1;
        return;
    }
    if (pin_hit.first != nullptr)  // clicked an input pin: ignore (no drag from inputs)
        return;

    // 2. Node body -> select + begin node drag.
    for (auto& node : m_Nodes)
    {
        const ZSlate::Vector2 sp = NodeScreen(node);
        const ZSlate::Vector2 size = NodeSize(node);
        if (mouse.x >= sp.x && mouse.x <= sp.x + size.x && mouse.y >= sp.y && mouse.y <= sp.y + size.y)
        {
            for (auto& n : m_Nodes)
                n.selected = false;
            node.selected = true;
            m_DraggingNode = true;
            m_DraggingNodeId = node.id;
            m_SelectedLinkIndex = -1;
            return;
        }
    }

    // 3. Empty space: select a hovered link, else clear selection.
    if (m_HoveredLinkIndex >= 0)
    {
        m_SelectedLinkIndex = m_HoveredLinkIndex;
        for (auto& n : m_Nodes)
            n.selected = false;
    }
    else
    {
        m_SelectedLinkIndex = -1;
        for (auto& n : m_Nodes)
            n.selected = false;
    }
}

// ---------------------------------------------------------------------------
// Per-frame entry point
// ---------------------------------------------------------------------------
void ZSlateBlueprintWindow::OnGUI()
{
    float ui_scale = ZSlate::EditorSlateHost::Get().GetUiScale();
    if (ui_scale < 0.5f)
        ui_scale = 1.0f;
    m_Scale = ui_scale;

    if (m_Toolbar == nullptr || ui_scale != m_BuiltScale)
    {
        m_BuiltScale = ui_scale;
        BuildToolbar(ui_scale);
        m_Input.Reset();
    }

    // Native dock hosting is unconditional for editor panels: the dock chrome's
    // ReconcileNativeTreeWithOpenWindows (run before any panel OnGUI) guarantees an
    // open window is placed in the dock tree, so EditorView::BeginGUI always took the
    // native-host path here. Source the leaf rect from NativeRect().
    const float* r = NativeRect();
    float pos_x = r[0];
    float pos_y = r[1];
    float avail_w = r[2];
    float avail_h = r[3];
    if (avail_w < 1.0f)
        avail_w = 1.0f;
    if (avail_h < 1.0f)
        avail_h = 1.0f;

    m_Toolbar->CacheDesiredSize();
    const float toolbar_h = std::min(avail_h, std::max(m_Toolbar->GetDesiredSize().y, 26.0f * ui_scale));
    m_ToolbarHeight = toolbar_h;

    const ZSlate::UIRect toolbar_region(pos_x, pos_y, avail_w, toolbar_h);
    const FGeometry toolbar_geom(ZSlate::Vector2(pos_x, pos_y), ZSlate::Vector2(avail_w, toolbar_h));
    const ZSlate::UIRect canvas_region(pos_x, pos_y + toolbar_h, avail_w, avail_h - toolbar_h);
    const ZSlate::UIRect panel_region(pos_x, pos_y, avail_w, avail_h);

    m_CanvasOrigin = ZSlate::Vector2(canvas_region.x, canvas_region.y);

    // ---- Paint --------------------------------------------------------------
    auto& overlay = ZSlate::ZSlateEditorOverlay::Get();
    {
        BatchedUIRenderer& renderer = overlay.GetRenderer();
        overlay.BeginWindowGroup(ZSlate::ZSlateEditorOverlay::kZPanel);

        FPaintContext ctx;
        ctx.Renderer = &renderer;
        ctx.LayerId = 0;

        PaintCanvas(renderer, canvas_region, ui_scale);

        renderer.PushClipRect(toolbar_region, true);
        m_Toolbar->Paint(ctx, toolbar_geom);
        renderer.PopClipRect();
    }

    // ---- Input --------------------------------------------------------------
    ZSlate::EditorSlateHost& host = ZSlate::EditorSlateHost::Get();
    const int surface_id = ZSlate::EditorSlateHost::HashId(m_Title);
    host.BeginSurface(surface_id, panel_region, ZSlate::ESurfaceLayer::Panels);
    const ZSlate::Vector2 mouse = host.GetPointerPos();
    const ZSlate::Vector2 mouse_delta = host.GetPointerDelta();
    const bool over_item = host.IsSurfaceHovered(surface_id, mouse);

    m_Input.ProcessMouse(m_Toolbar, mouse, over_item, host.IsLeftDown(), over_item ? host.GetWheelDelta() : 0.0f);

    const bool in_canvas = over_item && mouse.y >= canvas_region.y;
    const bool left_clicked = in_canvas && host.WasLeftPressedThisFrame();
    const bool left_down = host.IsLeftDown();
    const bool left_released = host.WasLeftReleasedThisFrame();
    const bool middle_down = host.IsMiddleDown();

    HandleCanvasInput(mouse, mouse_delta, in_canvas, left_clicked, left_down, left_released, middle_down);

    if (over_item)
    {
        for (EKey key : host.GetKeysThisFrame())
        {
            if (key == EKey::Delete)
                DeleteSelection();
        }
    }
}

// ---------------------------------------------------------------------------
// Graph ops
// ---------------------------------------------------------------------------
void ZSlateBlueprintWindow::AddNode(const std::string& title,
                                    const ZSlate::Vector2& position,
                                    const std::vector<std::pair<std::string, BpPinType>>& inputs,
                                    const std::vector<std::pair<std::string, BpPinType>>& outputs)
{
    BpNode node;
    node.id = m_NextNodeId++;
    node.title = title;
    node.position = position;
    for (const auto& in : inputs)
        node.inputs.push_back(BpPin {m_NextPinId++, in.second, true, in.first});
    for (const auto& out : outputs)
        node.outputs.push_back(BpPin {m_NextPinId++, out.second, false, out.first});
    m_Nodes.push_back(std::move(node));
}

void ZSlateBlueprintWindow::AddNodeInView(const std::string& title,
                                          const std::vector<std::pair<std::string, BpPinType>>& inputs,
                                          const std::vector<std::pair<std::string, BpPinType>>& outputs)
{
    // Cascade new nodes near the top-left of the currently visible canvas area.
    const float cx = (-m_Scroll.x + 60.0f + static_cast<float>(m_AddCascade % 6) * 28.0f) / m_Scale;
    const float cy = (-m_Scroll.y + 60.0f + static_cast<float>(m_AddCascade % 6) * 28.0f) / m_Scale;
    ++m_AddCascade;
    AddNode(title, ZSlate::Vector2(cx, cy), inputs, outputs);
}

void ZSlateBlueprintWindow::CreateLink(int from_node_id, int from_pin_id, int to_node_id, int to_pin_id)
{
    m_Links.push_back(BpLink {from_node_id, from_pin_id, to_node_id, to_pin_id});
}

void ZSlateBlueprintWindow::DeleteLink(int link_index)
{
    if (link_index >= 0 && link_index < static_cast<int>(m_Links.size()))
        m_Links.erase(m_Links.begin() + link_index);
}

void ZSlateBlueprintWindow::DeleteSelection()
{
    // Selected link first.
    if (m_SelectedLinkIndex >= 0 && m_SelectedLinkIndex < static_cast<int>(m_Links.size()))
    {
        DeleteLink(m_SelectedLinkIndex);
        m_SelectedLinkIndex = -1;
        return;
    }

    // Selected nodes + their links.
    for (auto it = m_Nodes.begin(); it != m_Nodes.end();)
    {
        if (it->selected)
        {
            for (int i = static_cast<int>(m_Links.size()) - 1; i >= 0; --i)
            {
                if (m_Links[i].from_node_id == it->id || m_Links[i].to_node_id == it->id)
                    DeleteLink(i);
            }
            it = m_Nodes.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

bool ZSlateBlueprintWindow::CanConnectPins(const BpPin& from_pin, const BpPin& to_pin) const
{
    return from_pin.type == to_pin.type;
}
