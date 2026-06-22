#include "ZSlateMaterialEditorWindow.h"

#include "Editor/EditorLayout/EditorLayoutWindowIds.h"
#include "Editor/ZSlate/Backend/EditorSlateHost.h"
#include "Editor/ZSlate/Backend/ZSlateEditorOverlay.h"  // native RHI backend
#include "ZSlate/Application/SlateApplication.h"
#include "ZSlate/Widgets/SBorder.h"
#include "ZSlate/Widgets/SBoxPanel.h"
#include "ZSlate/Widgets/SButton.h"
#include "ZSlate/Widgets/SSpacer.h"
#include "ZSlate/Widgets/STextBlock.h"
#include "Runtime/UI/Render/UIRenderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
using namespace ZSlate;

namespace
{
const UIColor kPanelColor(0.10f, 0.10f, 0.12f, 1.0f);
const UIColor kCanvasBg(0.118f, 0.118f, 0.118f, 1.0f);
const UIColor kGridColor(0.196f, 0.196f, 0.196f, 0.6f);
const UIColor kSideBg(0.13f, 0.13f, 0.15f, 1.0f);
const UIColor kNodeBg(0.157f, 0.157f, 0.196f, 1.0f);
const UIColor kNodeBgSelected(0.235f, 0.235f, 0.314f, 1.0f);
const UIColor kNodeBorder(0.392f, 0.392f, 0.471f, 1.0f);
const UIColor kNodeBorderSel(0.39f, 0.59f, 0.78f, 1.0f);
const UIColor kTitleColor(1.0f, 1.0f, 0.5f, 1.0f);
const UIColor kLabelColor(0.85f, 0.86f, 0.90f, 1.0f);
const UIColor kDimColor(0.55f, 0.57f, 0.62f, 1.0f);
const UIColor kWhite(1.0f, 1.0f, 1.0f, 1.0f);
const UIColor kLinkHover(1.0f, 1.0f, 0.39f, 1.0f);
const UIColor kPreviewBg(0.196f, 0.196f, 0.196f, 1.0f);
const UIColor kSeparator(0.30f, 0.30f, 0.34f, 1.0f);

constexpr float kNodeWidth = 180.0f;
constexpr float kTitleH = 25.0f;
constexpr float kPinRow = 25.0f;
constexpr float kPinTopPad = 10.0f;
constexpr float kPinRadius = 6.0f;
constexpr float kGridSize = 64.0f;

std::shared_ptr<STextBlock> MakeText(const std::string& text, float font_size, const UIColor& color)
{
    auto t = std::make_shared<STextBlock>();
    t->Text = text;
    t->FontSize = font_size;
    t->Color = color;
    t->Alignment = TextAnchor::MiddleCenter;
    return t;
}

// Cubic bezier sampled into short filled quads (UIRenderer has no line/curve
// primitive). Identical approach to ZSlateBlueprintWindow.
void DrawBezier(UIRenderer& r,
                const Vector2& a,
                const Vector2& b,
                const Vector2& c,
                const Vector2& d,
                const UIColor& color,
                float thickness)
{
    const float chord = std::abs(d.x - a.x) + std::abs(d.y - a.y);
    int steps = static_cast<int>(chord / std::max(2.0f, thickness * 0.6f));
    steps = std::max(16, std::min(96, steps));
    const float half = thickness * 0.5f;

    Vector2 prev = a;
    for (int i = 1; i <= steps; ++i)
    {
        const float t = static_cast<float>(i) / static_cast<float>(steps);
        const float u = 1.0f - t;
        const float w0 = u * u * u;
        const float w1 = 3.0f * u * u * t;
        const float w2 = 3.0f * u * t * t;
        const float w3 = t * t * t;
        const Vector2 pt(w0 * a.x + w1 * b.x + w2 * c.x + w3 * d.x, w0 * a.y + w1 * b.y + w2 * c.y + w3 * d.y);
        const float seg = std::abs(pt.x - prev.x) + std::abs(pt.y - prev.y);
        const int sub = std::max(1, static_cast<int>(seg / std::max(1.0f, half)));
        for (int s = 1; s <= sub; ++s)
        {
            const float st = static_cast<float>(s) / static_cast<float>(sub);
            const float x = prev.x + (pt.x - prev.x) * st;
            const float y = prev.y + (pt.y - prev.y) * st;
            r.drawQuad(UIRect(x - half, y - half, thickness, thickness), color);
        }
        prev = pt;
    }
}
}  // namespace

ZSlateMaterialEditorWindow::ZSlateMaterialEditorWindow(EditorUI* editor_ui)
    : EditorWindow(editor_ui, EditorLayoutWindowIds::kMaterial)
{
    m_Open = false;
    // Default PBR output nodes, mirroring the legacy window's seed.
    AddNode(MatNodeType::Output_BaseColor, Vector2(600.0f, 120.0f));
    AddNode(MatNodeType::Output_Metallic, Vector2(600.0f, 230.0f));
    AddNode(MatNodeType::Output_Roughness, Vector2(600.0f, 320.0f));
    AddNode(MatNodeType::Output_Normal, Vector2(600.0f, 410.0f));
}

// ---------------------------------------------------------------------------
// Toolbar
// ---------------------------------------------------------------------------
void ZSlateMaterialEditorWindow::BuildToolbar(float scale)
{
    const float font = 13.0f * scale;

    auto root = std::make_shared<SBorder>();
    root->BackgroundColor = kPanelColor;
    root->Padding = FMargin(6.0f * scale, 4.0f * scale);
    root->HAlign = EHorizontalAlignment::Fill;
    root->VAlign = EVerticalAlignment::Top;

    auto bar = std::make_shared<SHorizontalBox>();

    auto add_button = [&](const char* label, std::function<void()> on_click) {
        auto btn = std::make_shared<SButton>();
        btn->Padding = FMargin(7.0f * scale, 3.0f * scale);
        btn->SetContent(MakeText(label, font, kLabelColor));
        btn->OnClicked = std::move(on_click);
        bar->AddSlot(btn).AutoSize().SetVAlign(EVerticalAlignment::Center);
        bar->AddSlot(std::make_shared<SSpacer>(Vector2(4.0f * scale, 0.0f))).AutoSize();
    };

    add_button("Const", [this]() { AddNodeInView(MatNodeType::Input_Constant); });
    add_button("Const3", [this]() { AddNodeInView(MatNodeType::Input_Constant3); });
    add_button("Tex2D", [this]() { AddNodeInView(MatNodeType::Input_Texture2D); });
    add_button("UV", [this]() { AddNodeInView(MatNodeType::Input_UV); });
    add_button("Sample", [this]() { AddNodeInView(MatNodeType::Texture_Sample2D); });
    add_button("Add", [this]() { AddNodeInView(MatNodeType::Math_Add); });
    add_button("Mul", [this]() { AddNodeInView(MatNodeType::Math_Multiply); });
    add_button("Lerp", [this]() { AddNodeInView(MatNodeType::Math_Lerp); });
    add_button("Saturate", [this]() { AddNodeInView(MatNodeType::Math_Saturate); });

    bar->AddSlot(std::make_shared<SSpacer>(Vector2(10.0f * scale, 0.0f))).AutoSize();
    add_button("Delete Selected", [this]() { DeleteSelection(); });

    root->SetContent(bar);
    m_Toolbar = root;
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------
float ZSlateMaterialEditorWindow::NodeContentExtra(MatNodeType type) const
{
    switch (type)
    {
        case MatNodeType::Input_Constant:
        case MatNodeType::Input_Constant2:
        case MatNodeType::Input_Constant3:
        case MatNodeType::Input_Constant4:
            return 22.0f;
        case MatNodeType::Input_Texture2D:
            return 40.0f;
        default:
            return 0.0f;
    }
}

Vector2 ZSlateMaterialEditorWindow::NodeScreen(const MatNode& node) const
{
    return Vector2(m_CanvasOrigin.x + m_Scroll.x + node.position.x * m_Scale,
                   m_CanvasOrigin.y + m_Scroll.y + node.position.y * m_Scale);
}

Vector2 ZSlateMaterialEditorWindow::NodeSize(const MatNode& node) const
{
    const float rows = static_cast<float>(std::max(node.inputs.size(), node.outputs.size()));
    const float content = rows * kPinRow;
    const float h = (kTitleH + content + NodeContentExtra(node.node_type) + kPinTopPad) * m_Scale;
    return Vector2(kNodeWidth * m_Scale, h);
}

Vector2 ZSlateMaterialEditorWindow::PinScreen(const MatNode& node, int pin_index, bool is_input) const
{
    const Vector2 sp = NodeScreen(node);
    const float y = sp.y + (kTitleH + kPinTopPad) * m_Scale + static_cast<float>(pin_index) * kPinRow * m_Scale;
    const float x = is_input ? sp.x : sp.x + NodeSize(node).x;
    return Vector2(x, y);
}

MatNode* ZSlateMaterialEditorWindow::FindNode(int node_id)
{
    for (auto& n : m_Nodes)
        if (n.id == node_id)
            return &n;
    return nullptr;
}

std::pair<MatNode*, MatPin*> ZSlateMaterialEditorWindow::FindPinAt(const Vector2& p)
{
    const float r = kPinRadius * m_Scale + 3.0f;
    for (auto& node : m_Nodes)
    {
        for (size_t i = 0; i < node.inputs.size(); ++i)
        {
            const Vector2 pp = PinScreen(node, static_cast<int>(i), true);
            if ((p.x - pp.x) * (p.x - pp.x) + (p.y - pp.y) * (p.y - pp.y) <= r * r)
                return {&node, &node.inputs[i]};
        }
        for (size_t i = 0; i < node.outputs.size(); ++i)
        {
            const Vector2 pp = PinScreen(node, static_cast<int>(i), false);
            if ((p.x - pp.x) * (p.x - pp.x) + (p.y - pp.y) * (p.y - pp.y) <= r * r)
                return {&node, &node.outputs[i]};
        }
    }
    return {nullptr, nullptr};
}

UIColor ZSlateMaterialEditorWindow::PinColor(MatPinType type) const
{
    switch (type)
    {
        case MatPinType::Float:
            return UIColor(0.576f, 0.886f, 0.29f, 1.0f);
        case MatPinType::Float2:
            return UIColor(0.29f, 0.576f, 0.886f, 1.0f);
        case MatPinType::Float3:
            return UIColor(0.886f, 0.576f, 0.29f, 1.0f);
        case MatPinType::Float4:
            return UIColor(0.886f, 0.29f, 0.576f, 1.0f);
        case MatPinType::Texture2D:
            return UIColor(1.0f, 0.784f, 0.0f, 1.0f);
        case MatPinType::TextureCube:
            return UIColor(1.0f, 0.588f, 0.0f, 1.0f);
        case MatPinType::Bool:
            return UIColor(0.863f, 0.188f, 0.188f, 1.0f);
        case MatPinType::Int:
            return UIColor(0.267f, 0.588f, 1.0f, 1.0f);
        default:
            return UIColor(0.5f, 0.5f, 0.5f, 1.0f);
    }
}

// ---------------------------------------------------------------------------
// Graph paint
// ---------------------------------------------------------------------------
void ZSlateMaterialEditorWindow::PaintGraph(UIRenderer& r, const UIRect& region, float scale)
{
    r.drawQuad(region, kCanvasBg);
    r.pushClipRect(region, true);

    const float grid = kGridSize * scale;
    const float ox = std::fmod(m_Scroll.x, grid);
    const float oy = std::fmod(m_Scroll.y, grid);
    for (float x = ox; x < region.width; x += grid)
        r.drawQuad(UIRect(region.x + x, region.y, 1.0f, region.height), kGridColor);
    for (float y = oy; y < region.height; y += grid)
        r.drawQuad(UIRect(region.x, region.y + y, region.width, 1.0f), kGridColor);

    // Links (under nodes).
    for (size_t i = 0; i < m_Links.size(); ++i)
    {
        const MatLink& link = m_Links[i];
        MatNode* from = FindNode(link.from_node_id);
        MatNode* to = FindNode(link.to_node_id);
        if (from == nullptr || to == nullptr)
            continue;
        int fi = -1, ti = -1;
        for (size_t k = 0; k < from->outputs.size(); ++k)
            if (from->outputs[k].id == link.from_pin_id)
                fi = static_cast<int>(k);
        for (size_t k = 0; k < to->inputs.size(); ++k)
            if (to->inputs[k].id == link.to_pin_id)
                ti = static_cast<int>(k);
        if (fi < 0 || ti < 0)
            continue;

        const Vector2 a = PinScreen(*from, fi, false);
        const Vector2 d = PinScreen(*to, ti, true);
        const Vector2 b(a.x + 50.0f * scale, a.y);
        const Vector2 c(d.x - 50.0f * scale, d.y);
        UIColor color = PinColor(from->outputs[fi].type);
        color.w = 0.85f;
        const bool hot = (m_HoveredLinkIndex == static_cast<int>(i) || m_SelectedLinkIndex == static_cast<int>(i));
        if (hot)
            color = kLinkHover;
        DrawBezier(r, a, b, c, d, color, (hot ? 4.0f : 3.0f) * scale);
    }

    // Link drag preview.
    if (m_DraggingLink)
    {
        MatNode* from = FindNode(m_DragFromNodeId);
        if (from != nullptr)
        {
            int fi = -1;
            for (size_t k = 0; k < from->outputs.size(); ++k)
                if (from->outputs[k].id == m_DragFromPinId)
                    fi = static_cast<int>(k);
            if (fi >= 0)
            {
                const Vector2 m = ZSlate::EditorSlateHost::Get().GetPointerPos();
                const Vector2 a = PinScreen(*from, fi, false);
                const Vector2 d(m.x, m.y);
                DrawBezier(r,
                           a,
                           Vector2(a.x + 50.0f * scale, a.y),
                           Vector2(d.x - 50.0f * scale, d.y),
                           d,
                           UIColor(1.0f, 1.0f, 1.0f, 0.6f),
                           2.0f * scale);
            }
        }
    }

    // Nodes.
    for (const MatNode& node : m_Nodes)
    {
        const Vector2 sp = NodeScreen(node);
        const Vector2 size = NodeSize(node);
        const UIRect body(sp.x, sp.y, size.x, size.y);

        r.drawQuad(body, node.selected ? kNodeBgSelected : kNodeBg);
        r.drawRect(body, node.selected ? kNodeBorderSel : kNodeBorder, node.selected ? 2.0f : 1.0f);

        r.drawText(UIRect(sp.x + 6.0f * scale, sp.y, size.x - 12.0f * scale, kTitleH * scale),
                   node.title,
                   13.0f * scale,
                   kTitleColor,
                   TextAnchor::MiddleLeft,
                   TextWrapMode::NoWrap);
        r.drawQuad(UIRect(sp.x, sp.y + kTitleH * scale, size.x, 1.0f), kNodeBorder);

        for (size_t i = 0; i < node.inputs.size(); ++i)
        {
            const MatPin& pin = node.inputs[i];
            const Vector2 pp = PinScreen(node, static_cast<int>(i), true);
            const bool hov = (m_HoveredPinNodeId == node.id && m_HoveredPinId == pin.id);
            const float pr = (hov ? kPinRadius + 1.0f : kPinRadius) * scale;
            r.drawQuad(UIRect(pp.x - pr, pp.y - pr, pr * 2.0f, pr * 2.0f), PinColor(pin.type));
            r.drawRect(UIRect(pp.x - pr, pp.y - pr, pr * 2.0f, pr * 2.0f), kWhite, 1.0f);
            r.drawText(UIRect(pp.x + pr + 3.0f * scale, pp.y - kPinRow * 0.5f * scale, size.x * 0.55f, kPinRow * scale),
                       pin.name,
                       11.0f * scale,
                       kLabelColor,
                       TextAnchor::MiddleLeft,
                       TextWrapMode::NoWrap);
        }

        for (size_t i = 0; i < node.outputs.size(); ++i)
        {
            const MatPin& pin = node.outputs[i];
            const Vector2 pp = PinScreen(node, static_cast<int>(i), false);
            const bool hov = (m_HoveredPinNodeId == node.id && m_HoveredPinId == pin.id);
            const float pr = (hov ? kPinRadius + 1.0f : kPinRadius) * scale;
            r.drawQuad(UIRect(pp.x - pr, pp.y - pr, pr * 2.0f, pr * 2.0f), PinColor(pin.type));
            r.drawRect(UIRect(pp.x - pr, pp.y - pr, pr * 2.0f, pr * 2.0f), kWhite, 1.0f);
            r.drawText(UIRect(pp.x - pr - 3.0f * scale - size.x * 0.55f,
                              pp.y - kPinRow * 0.5f * scale,
                              size.x * 0.55f,
                              kPinRow * scale),
                       pin.name,
                       11.0f * scale,
                       kLabelColor,
                       TextAnchor::MiddleRight,
                       TextWrapMode::NoWrap);
        }

        // Inline value read-out for constant / texture nodes.
        const float extra = NodeContentExtra(node.node_type);
        if (extra > 0.0f)
        {
            const float rows = static_cast<float>(std::max(node.inputs.size(), node.outputs.size()));
            const float cy = sp.y + (kTitleH + kPinTopPad + rows * kPinRow) * scale;
            char buf[96];
            switch (node.node_type)
            {
                case MatNodeType::Input_Constant:
                    std::snprintf(buf, sizeof(buf), "%.3f", node.data.float_value);
                    break;
                case MatNodeType::Input_Constant2:
                    std::snprintf(buf, sizeof(buf), "%.2f, %.2f", node.data.vec_value[0], node.data.vec_value[1]);
                    break;
                case MatNodeType::Input_Constant3:
                    std::snprintf(buf,
                                  sizeof(buf),
                                  "%.2f, %.2f, %.2f",
                                  node.data.vec_value[0],
                                  node.data.vec_value[1],
                                  node.data.vec_value[2]);
                    break;
                case MatNodeType::Input_Constant4:
                    std::snprintf(buf,
                                  sizeof(buf),
                                  "%.2f, %.2f, %.2f, %.2f",
                                  node.data.vec_value[0],
                                  node.data.vec_value[1],
                                  node.data.vec_value[2],
                                  node.data.vec_value[3]);
                    break;
                case MatNodeType::Input_Texture2D:
                    std::snprintf(buf,
                                  sizeof(buf),
                                  "Tex: %s",
                                  node.data.texture_path.empty() ? "(none)" : node.data.texture_path.c_str());
                    break;
                default:
                    buf[0] = '\0';
                    break;
            }
            r.drawText(UIRect(sp.x + 6.0f * scale, cy, size.x - 12.0f * scale, 20.0f * scale),
                       buf,
                       11.0f * scale,
                       kDimColor,
                       TextAnchor::MiddleLeft,
                       TextWrapMode::NoWrap);
        }
    }

    r.popClipRect();
}

// ---------------------------------------------------------------------------
// Side panel (preview placeholder + read-only properties)
// ---------------------------------------------------------------------------
void ZSlateMaterialEditorWindow::PaintSidePanel(UIRenderer& r, const UIRect& region, float scale)
{
    r.drawQuad(region, kSideBg);
    r.pushClipRect(region, true);

    const float pad = 10.0f * scale;
    float y = region.y + pad;
    const float x = region.x + pad;
    const float w = region.width - pad * 2.0f;

    r.drawText(UIRect(x, y, w, 18.0f * scale),
               "Preview",
               13.0f * scale,
               kLabelColor,
               TextAnchor::MiddleLeft,
               TextWrapMode::NoWrap);
    y += 22.0f * scale;

    const float preview = std::min(w, region.height * 0.4f);
    r.drawQuad(UIRect(x, y, preview, preview), kPreviewBg);
    r.drawRect(UIRect(x, y, preview, preview), kSeparator, 1.0f);
    r.drawText(UIRect(x, y, preview, preview),
               "Material Preview",
               12.0f * scale,
               kDimColor,
               TextAnchor::MiddleCenter,
               TextWrapMode::NoWrap);
    y += preview + pad;

    r.drawQuad(UIRect(region.x, y, region.width, 1.0f), kSeparator);
    y += pad;

    r.drawText(UIRect(x, y, w, 18.0f * scale),
               "Properties",
               13.0f * scale,
               kLabelColor,
               TextAnchor::MiddleLeft,
               TextWrapMode::NoWrap);
    y += 22.0f * scale;

    MatNode* sel = FindNode(m_SelectedNodeId);
    if (sel == nullptr)
    {
        r.drawText(UIRect(x, y, w, 18.0f * scale),
                   "Select a node",
                   12.0f * scale,
                   kDimColor,
                   TextAnchor::MiddleLeft,
                   TextWrapMode::NoWrap);
        r.popClipRect();
        return;
    }

    auto row = [&](const std::string& text, const UIColor& color) {
        r.drawText(UIRect(x, y, w, 18.0f * scale), text, 12.0f * scale, color, TextAnchor::MiddleLeft, TextWrapMode::NoWrap);
        y += 20.0f * scale;
    };

    row("Node: " + sel->title, kLabelColor);
    char buf[128];
    switch (sel->node_type)
    {
        case MatNodeType::Input_Constant:
            std::snprintf(buf, sizeof(buf), "Value: %.3f", sel->data.float_value);
            row(buf, kDimColor);
            break;
        case MatNodeType::Input_Constant2:
            std::snprintf(buf, sizeof(buf), "Value: %.3f, %.3f", sel->data.vec_value[0], sel->data.vec_value[1]);
            row(buf, kDimColor);
            break;
        case MatNodeType::Input_Constant3:
            std::snprintf(buf,
                          sizeof(buf),
                          "Value: %.3f, %.3f, %.3f",
                          sel->data.vec_value[0],
                          sel->data.vec_value[1],
                          sel->data.vec_value[2]);
            row(buf, kDimColor);
            break;
        case MatNodeType::Input_Constant4:
            std::snprintf(buf,
                          sizeof(buf),
                          "Value: %.3f, %.3f, %.3f, %.3f",
                          sel->data.vec_value[0],
                          sel->data.vec_value[1],
                          sel->data.vec_value[2],
                          sel->data.vec_value[3]);
            row(buf, kDimColor);
            break;
        case MatNodeType::Input_Texture2D:
            std::snprintf(buf, sizeof(buf), "Texture: %s", sel->data.texture_path.empty() ? "(none)" : sel->data.texture_path.c_str());
            row(buf, kDimColor);
            break;
        default:
            row("No editable properties", kDimColor);
            break;
    }
    std::snprintf(buf, sizeof(buf), "Inputs: %d   Outputs: %d", static_cast<int>(sel->inputs.size()), static_cast<int>(sel->outputs.size()));
    row(buf, kDimColor);

    r.popClipRect();
}

// ---------------------------------------------------------------------------
// Interaction
// ---------------------------------------------------------------------------
void ZSlateMaterialEditorWindow::HandleGraphInput(const Vector2& mouse,
                                                  const Vector2& mouse_delta,
                                                  bool over_canvas,
                                                  bool left_clicked,
                                                  bool left_down,
                                                  bool left_released,
                                                  bool middle_down)
{
    if (over_canvas && middle_down)
    {
        m_Scroll.x += mouse_delta.x;
        m_Scroll.y += mouse_delta.y;
    }

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
            for (size_t i = 0; i < m_Links.size(); ++i)
            {
                MatNode* from = FindNode(m_Links[i].from_node_id);
                MatNode* to = FindNode(m_Links[i].to_node_id);
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
                const Vector2 a = PinScreen(*from, fi, false);
                const Vector2 d = PinScreen(*to, ti, true);
                const Vector2 mid((a.x + d.x) * 0.5f, (a.y + d.y) * 0.5f);
                const float rr = 12.0f * m_Scale;
                if ((mouse.x - mid.x) * (mouse.x - mid.x) + (mouse.y - mid.y) * (mouse.y - mid.y) <= rr * rr)
                    m_HoveredLinkIndex = static_cast<int>(i);
            }
        }
    }

    if (m_DraggingLink && left_released)
    {
        const auto target = FindPinAt(mouse);
        if (target.first != nullptr && target.second != nullptr && target.second->is_input)
        {
            MatNode* from = FindNode(m_DragFromNodeId);
            MatPin* from_pin = nullptr;
            if (from != nullptr)
                for (auto& p : from->outputs)
                    if (p.id == m_DragFromPinId)
                        from_pin = &p;
            if (from != nullptr && from_pin != nullptr && CanConnectPins(*from_pin, *target.second))
            {
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

    if (m_DraggingNode && left_down)
    {
        if (MatNode* n = FindNode(m_DraggingNodeId))
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

    const auto pin_hit = FindPinAt(mouse);
    if (pin_hit.first != nullptr && pin_hit.second != nullptr && !pin_hit.second->is_input)
    {
        m_DraggingLink = true;
        m_DragFromNodeId = pin_hit.first->id;
        m_DragFromPinId = pin_hit.second->id;
        m_SelectedLinkIndex = -1;
        return;
    }
    if (pin_hit.first != nullptr)
        return;

    for (auto& node : m_Nodes)
    {
        const Vector2 sp = NodeScreen(node);
        const Vector2 size = NodeSize(node);
        if (mouse.x >= sp.x && mouse.x <= sp.x + size.x && mouse.y >= sp.y && mouse.y <= sp.y + size.y)
        {
            for (auto& n : m_Nodes)
                n.selected = false;
            node.selected = true;
            m_SelectedNodeId = node.id;
            m_DraggingNode = true;
            m_DraggingNodeId = node.id;
            m_SelectedLinkIndex = -1;
            return;
        }
    }

    if (m_HoveredLinkIndex >= 0)
    {
        m_SelectedLinkIndex = m_HoveredLinkIndex;
    }
    else
    {
        m_SelectedLinkIndex = -1;
        m_SelectedNodeId = -1;
    }
    for (auto& n : m_Nodes)
        n.selected = false;
}

// ---------------------------------------------------------------------------
// Per-frame entry point
// ---------------------------------------------------------------------------
void ZSlateMaterialEditorWindow::OnGUI()
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

    float pos_x = 0.0f;
    float pos_y = 0.0f;
    float avail_w = 0.0f;
    float avail_h = 0.0f;
    // Native dock hosting is unconditional: ReconcileNativeTreeWithOpenWindows (run
    // before any panel OnGUI) guarantees an open window is in the dock tree, so the
    // leaf rect always comes from NativeRect().
    const float* native_rect = NativeRect();
    pos_x = native_rect[0];
    pos_y = native_rect[1];
    avail_w = native_rect[2];
    avail_h = native_rect[3];
    if (avail_w < 1.0f)
        avail_w = 1.0f;
    if (avail_h < 1.0f)
        avail_h = 1.0f;

    m_Toolbar->CacheDesiredSize();
    const float toolbar_h = std::min(avail_h, std::max(m_Toolbar->GetDesiredSize().y, 26.0f * ui_scale));
    m_ToolbarHeight = toolbar_h;

    const float content_y = pos_y + toolbar_h;
    const float content_h = avail_h - toolbar_h;
    float side_w = m_SidePanelWidth * ui_scale;
    if (side_w > avail_w * 0.5f)
        side_w = avail_w * 0.5f;
    const float graph_w = std::max(1.0f, avail_w - side_w);

    const UIRect toolbar_region(pos_x, pos_y, avail_w, toolbar_h);
    const FGeometry toolbar_geom(Vector2(pos_x, pos_y), Vector2(avail_w, toolbar_h));
    const UIRect graph_region(pos_x, content_y, graph_w, content_h);
    const UIRect side_region(pos_x + graph_w, content_y, side_w, content_h);
    const UIRect panel_region(pos_x, pos_y, avail_w, avail_h);

    m_CanvasOrigin = Vector2(graph_region.x, graph_region.y);

    auto& overlay = ZSlate::ZSlateEditorOverlay::Get();

    auto paint_all = [&](UIRenderer& r, FPaintContext& ctx) {
        PaintGraph(r, graph_region, ui_scale);
        PaintSidePanel(r, side_region, ui_scale);
        r.drawQuad(UIRect(pos_x + graph_w, content_y, 1.0f, content_h), kSeparator);
        r.pushClipRect(toolbar_region, true);
        m_Toolbar->Paint(ctx, toolbar_geom);
        r.popClipRect();
    };

    {
        BatchedUIRenderer& renderer = overlay.GetRenderer();
        overlay.BeginWindowGroup(ZSlate::ZSlateEditorOverlay::kZPanel);
        FPaintContext ctx;
        ctx.Renderer = &renderer;
        ctx.LayerId = 0;
        paint_all(renderer, ctx);
    }

    ZSlate::EditorSlateHost& host = ZSlate::EditorSlateHost::Get();
    const int surface_id = ZSlate::EditorSlateHost::HashId(m_Title);
    host.BeginSurface(surface_id, panel_region, ZSlate::ESurfaceLayer::Panels);
    const Vector2 mouse = host.GetPointerPos();
    const Vector2 mouse_delta = host.GetPointerDelta();
    const bool over_item = host.IsSurfaceHovered(surface_id, mouse);

    m_Input.ProcessMouse(m_Toolbar, mouse, over_item, host.IsLeftDown(), over_item ? host.GetWheelDelta() : 0.0f);

    const bool in_graph = over_item && mouse.y >= content_y && mouse.x < pos_x + graph_w;
    const bool left_clicked = in_graph && host.WasLeftPressedThisFrame();
    const bool left_down = host.IsLeftDown();
    const bool left_released = host.WasLeftReleasedThisFrame();
    const bool middle_down = host.IsMiddleDown();

    HandleGraphInput(mouse, mouse_delta, in_graph, left_clicked, left_down, left_released, middle_down);

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
void ZSlateMaterialEditorWindow::AddNode(MatNodeType type, const Vector2& position)
{
    MatNode node;
    node.id = m_NextNodeId++;
    node.node_type = type;
    node.title = NodeTitle(type);
    node.position = position;
    SetupNodePins(node);
    m_Nodes.push_back(std::move(node));
}

void ZSlateMaterialEditorWindow::AddNodeInView(MatNodeType type)
{
    const float step = 28.0f;
    const float base_x = (-m_Scroll.x) / m_Scale + 40.0f;
    const float base_y = (-m_Scroll.y) / m_Scale + 40.0f;
    const Vector2 p(base_x + static_cast<float>(m_AddCascade % 6) * step,
                    base_y + static_cast<float>(m_AddCascade % 6) * step);
    m_AddCascade++;
    AddNode(type, p);
}

void ZSlateMaterialEditorWindow::SetupNodePins(MatNode& node)
{
    node.inputs.clear();
    node.outputs.clear();
    auto add_in = [&](MatPinType t, const char* pin_name) {
        node.inputs.push_back(MatPin {m_NextPinId++, t, true, pin_name});
    };
    auto add_out = [&](MatPinType t, const char* pin_name) {
        node.outputs.push_back(MatPin {m_NextPinId++, t, false, pin_name});
    };

    switch (node.node_type)
    {
        case MatNodeType::Input_Constant:
            add_out(MatPinType::Float, "Out");
            break;
        case MatNodeType::Input_Constant2:
            add_out(MatPinType::Float2, "Out");
            break;
        case MatNodeType::Input_Constant3:
            add_out(MatPinType::Float3, "Out");
            break;
        case MatNodeType::Input_Constant4:
            add_out(MatPinType::Float4, "Out");
            break;
        case MatNodeType::Input_Texture2D:
            add_out(MatPinType::Texture2D, "Texture");
            break;
        case MatNodeType::Input_TextureCube:
            add_out(MatPinType::TextureCube, "Cube");
            break;
        case MatNodeType::Input_UV:
            add_out(MatPinType::Float2, "UV");
            break;
        case MatNodeType::Input_Time:
            add_out(MatPinType::Float, "Time");
            break;
        case MatNodeType::Math_Add:
        case MatNodeType::Math_Subtract:
        case MatNodeType::Math_Multiply:
        case MatNodeType::Math_Divide:
            add_in(MatPinType::Float, "A");
            add_in(MatPinType::Float, "B");
            add_out(MatPinType::Float, "Out");
            break;
        case MatNodeType::Math_Lerp:
            add_in(MatPinType::Float3, "A");
            add_in(MatPinType::Float3, "B");
            add_in(MatPinType::Float, "T");
            add_out(MatPinType::Float3, "Out");
            break;
        case MatNodeType::Math_Clamp:
            add_in(MatPinType::Float, "In");
            add_in(MatPinType::Float, "Min");
            add_in(MatPinType::Float, "Max");
            add_out(MatPinType::Float, "Out");
            break;
        case MatNodeType::Math_Saturate:
            add_in(MatPinType::Float, "In");
            add_out(MatPinType::Float, "Out");
            break;
        case MatNodeType::Vector_Combine:
            add_in(MatPinType::Float, "X");
            add_in(MatPinType::Float, "Y");
            add_in(MatPinType::Float, "Z");
            add_out(MatPinType::Float3, "Out");
            break;
        case MatNodeType::Vector_Split:
            add_in(MatPinType::Float3, "In");
            add_out(MatPinType::Float, "X");
            add_out(MatPinType::Float, "Y");
            add_out(MatPinType::Float, "Z");
            break;
        case MatNodeType::Texture_Sample2D:
            add_in(MatPinType::Texture2D, "Texture");
            add_in(MatPinType::Float2, "UV");
            add_out(MatPinType::Float4, "RGBA");
            break;
        case MatNodeType::Output_BaseColor:
            add_in(MatPinType::Float3, "Base Color");
            break;
        case MatNodeType::Output_Metallic:
            add_in(MatPinType::Float, "Metallic");
            break;
        case MatNodeType::Output_Roughness:
            add_in(MatPinType::Float, "Roughness");
            break;
        case MatNodeType::Output_Normal:
            add_in(MatPinType::Float3, "Normal");
            break;
        default:
            break;
    }
}

void ZSlateMaterialEditorWindow::CreateLink(int from_node_id, int from_pin_id, int to_node_id, int to_pin_id)
{
    m_Links.push_back(MatLink {from_node_id, from_pin_id, to_node_id, to_pin_id});
}

void ZSlateMaterialEditorWindow::DeleteLink(int link_index)
{
    if (link_index >= 0 && link_index < static_cast<int>(m_Links.size()))
        m_Links.erase(m_Links.begin() + link_index);
}

void ZSlateMaterialEditorWindow::DeleteSelection()
{
    if (m_SelectedLinkIndex >= 0)
    {
        DeleteLink(m_SelectedLinkIndex);
        m_SelectedLinkIndex = -1;
        return;
    }

    for (auto it = m_Nodes.begin(); it != m_Nodes.end();)
    {
        if (it->selected)
        {
            for (int i = static_cast<int>(m_Links.size()) - 1; i >= 0; --i)
                if (m_Links[i].from_node_id == it->id || m_Links[i].to_node_id == it->id)
                    DeleteLink(i);
            if (m_SelectedNodeId == it->id)
                m_SelectedNodeId = -1;
            it = m_Nodes.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

bool ZSlateMaterialEditorWindow::CanConnectPins(const MatPin& from_pin, const MatPin& to_pin) const
{
    return from_pin.type == to_pin.type;
}

bool ZSlateMaterialEditorWindow::InputPinHasLink(int node_id, int pin_id) const
{
    for (const MatLink& link : m_Links)
        if (link.to_node_id == node_id && link.to_pin_id == pin_id)
            return true;
    return false;
}

std::string ZSlateMaterialEditorWindow::NodeTitle(MatNodeType type) const
{
    switch (type)
    {
        case MatNodeType::Input_Texture2D:
            return "Texture2D";
        case MatNodeType::Input_TextureCube:
            return "TextureCube";
        case MatNodeType::Input_Constant:
            return "Constant";
        case MatNodeType::Input_Constant2:
            return "Constant2";
        case MatNodeType::Input_Constant3:
            return "Constant3";
        case MatNodeType::Input_Constant4:
            return "Constant4";
        case MatNodeType::Input_Time:
            return "Time";
        case MatNodeType::Input_UV:
            return "UV";
        case MatNodeType::Math_Add:
            return "Add";
        case MatNodeType::Math_Subtract:
            return "Subtract";
        case MatNodeType::Math_Multiply:
            return "Multiply";
        case MatNodeType::Math_Divide:
            return "Divide";
        case MatNodeType::Math_Lerp:
            return "Lerp";
        case MatNodeType::Math_Clamp:
            return "Clamp";
        case MatNodeType::Math_Saturate:
            return "Saturate";
        case MatNodeType::Vector_Combine:
            return "Combine";
        case MatNodeType::Vector_Split:
            return "Split";
        case MatNodeType::Texture_Sample2D:
            return "Sample Texture2D";
        case MatNodeType::Output_BaseColor:
            return "Base Color";
        case MatNodeType::Output_Metallic:
            return "Metallic";
        case MatNodeType::Output_Roughness:
            return "Roughness";
        case MatNodeType::Output_Normal:
            return "Normal";
        default:
            return "Unknown";
    }
}
