#pragma once

#include "Runtime/UI/Render/UIAffine2D.h"
#include "Runtime/UI/Core/UITypes.h"

#include <cstdint>
#include <vector>

struct UIVertex
{
    float pos[2] {};
    float uv[2] {0.0f, 0.0f};
    float color[4] {1.0f, 1.0f, 1.0f, 1.0f};
};

struct UiDrawCommand
{
    void* texture_id {nullptr};
    uint32_t index_offset {0};
    uint32_t index_count {0};
    // Clip rect active when this command was recorded (UI-space pixels). Consumers
    // that support GPU scissor (e.g. the editor native overlay) use it for per-
    // command clipping; the runtime UIPass currently ignores it (its quads are
    // already CPU-clamped in AppendTexturedQuad). has_clip=false => unclipped.
    ZSlate::UIRect clip_rect {0.0f, 0.0f, 0.0f, 0.0f};
    bool has_clip {false};
};

// CPU-side geometry collected by BatchedUIRenderer during UISystem::PreRender().
class UIRenderBatch
{
public:
    void Clear();
    bool Empty() const { return m_Indices.empty(); }

    void PushClipRect(const ZSlate::UIRect& clip_rect, bool intersect_with_current);
    void PopClipRect();

    // Forces the next draw to open a fresh UiDrawCommand instead of merging into
    // the current one. Used to keep per-window command ranges disjoint so the
    // editor overlay can submit windows in z-order.
    void ForceNewCommand();

    void PushTransform(const UIAffine2D& transform);
    void PopTransform();

    void DrawQuad(const ZSlate::UIRect& rect, const ZSlate::UIColor& color, void* white_texture_id);
    void DrawRect(const ZSlate::UIRect& rect, const ZSlate::UIColor& color, float thickness, void* white_texture_id);
    // Solid convex polygon (>= 3 ordered points), fan-triangulated. Samples the
    // white texel (uv 0,0). Per-vertex clipping is NOT done here -- the recorded
    // command carries the active clip rect, so the editor overlay's GPU scissor
    // clips it (the runtime UIPass ignores clip, but never calls this).
    void DrawConvexPoly(const Vector2* points, int count, const ZSlate::UIColor& color, void* white_texture_id);
    void DrawTexturedQuad(const ZSlate::UIRect& rect,
                          void* texture_id,
                          const ZSlate::UIColor& color,
                          const Vector2& uv0,
                          const Vector2& uv1,
                          void* white_texture_id);

    const std::vector<UIVertex>& GetVertices() const { return m_Vertices; }
    const std::vector<uint16_t>& GetIndices() const { return m_Indices; }
    const std::vector<UiDrawCommand>& GetCommands() const { return m_Commands; }
    const ZSlate::UIRect& GetActiveClipRect() const { return m_ActiveClip; }

private:
    void BeginCommand(void* texture_id, void* white_texture_id);
    void TransformPoint(float x, float y, float& out_x, float& out_y) const;
    void AppendTexturedQuad(float x0,
                            float y0,
                            float x1,
                            float y1,
                            const ZSlate::UIColor& color,
                            float uv0x,
                            float uv0y,
                            float uv1x,
                            float uv1y,
                            void* texture_id,
                            void* white_texture_id);
    void AppendOutline(float x0,
                       float y0,
                       float x1,
                       float y1,
                       const ZSlate::UIColor& color,
                       float thickness,
                       void* white_texture_id);

    std::vector<UIVertex> m_Vertices;
    std::vector<uint16_t> m_Indices;
    std::vector<UiDrawCommand> m_Commands;
    std::vector<ZSlate::UIRect> m_ClipStack;
    std::vector<UIAffine2D> m_TransformStack;
    UIAffine2D m_ActiveTransform = UIAffine2D::Identity();
    ZSlate::UIRect m_ActiveClip {0.0f, 0.0f, 0.0f, 0.0f};
    bool m_HasClip {false};
    void* m_CurrentTexture {nullptr};
};
