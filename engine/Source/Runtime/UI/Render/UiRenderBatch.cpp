#include "Runtime/UI/Render/UIRenderBatch.h"

#include <algorithm>
#include <cmath>

namespace
{
    ZSlate::UIRect IntersectRects(const ZSlate::UIRect& a, const ZSlate::UIRect& b)
    {
        const float x0 = std::max(a.x, b.x);
        const float y0 = std::max(a.y, b.y);
        const float x1 = std::min(a.x + a.w, b.x + b.w);
        const float y1 = std::min(a.y + a.h, b.y + b.h);
        if (x1 <= x0 || y1 <= y0)
        {
            return ZSlate::UIRect{0.0f, 0.0f, 0.0f, 0.0f};
        }
        return ZSlate::UIRect{x0, y0, x1 - x0, y1 - y0};
    }

    void ToVertexColor(const ZSlate::UIColor& color, float out_rgba[4])
    {
        out_rgba[0] = std::clamp(color.x, 0.0f, 1.0f);
        out_rgba[1] = std::clamp(color.y, 0.0f, 1.0f);
        out_rgba[2] = std::clamp(color.z, 0.0f, 1.0f);
        out_rgba[3] = std::clamp(color.w, 0.0f, 1.0f);
    }

    void* ResolveTextureId(void* texture_id, void* white_texture_id)
    {
        return texture_id != nullptr ? texture_id : white_texture_id;
    }

    bool RectsEqual(const ZSlate::UIRect& a, const ZSlate::UIRect& b)
    {
        return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
    }
}  // namespace

void UIRenderBatch::Clear()
{
    m_Vertices.clear();
    m_Indices.clear();
    m_Commands.clear();
    m_ClipStack.clear();
    m_TransformStack.clear();
    m_ActiveTransform = UIAffine2D::Identity();
    m_HasClip = false;
    m_ActiveClip = ZSlate::UIRect{0.0f, 0.0f, 0.0f, 0.0f};
    m_CurrentTexture = nullptr;
}

void UIRenderBatch::PushTransform(const UIAffine2D& transform)
{
    m_TransformStack.push_back(m_ActiveTransform);
    m_ActiveTransform = m_ActiveTransform * transform;
}

void UIRenderBatch::PopTransform()
{
    if (m_TransformStack.empty())
    {
        m_ActiveTransform = UIAffine2D::Identity();
        return;
    }

    m_ActiveTransform = m_TransformStack.back();
    m_TransformStack.pop_back();
}

void UIRenderBatch::TransformPoint(float x, float y, float& out_x, float& out_y) const
{
    m_ActiveTransform.TransformPoint(x, y, out_x, out_y);
}

void UIRenderBatch::PushClipRect(const ZSlate::UIRect& clip_rect, bool intersect_with_current)
{
    ZSlate::UIRect next = clip_rect;
    if (m_HasClip && intersect_with_current)
    {
        next = IntersectRects(m_ActiveClip, clip_rect);
    }
    m_ClipStack.push_back(m_ActiveClip);
    m_ActiveClip = next;
    m_HasClip = true;
}

void UIRenderBatch::PopClipRect()
{
    if (m_ClipStack.empty())
    {
        m_HasClip = false;
        m_ActiveClip = ZSlate::UIRect{0.0f, 0.0f, 0.0f, 0.0f};
        return;
    }

    m_ActiveClip = m_ClipStack.back();
    m_ClipStack.pop_back();
    m_HasClip = !m_ClipStack.empty() || (m_ActiveClip.w > 0.0f && m_ActiveClip.h > 0.0f);
}

void UIRenderBatch::ForceNewCommand()
{
    // Sentinel that cannot match any real (or null/white) texture id, so the next
    // BeginCommand always opens a new command.
    m_CurrentTexture = reinterpret_cast<void*>(~static_cast<uintptr_t>(0));
}

void UIRenderBatch::BeginCommand(void* texture_id, void* white_texture_id)
{
    void* resolved = ResolveTextureId(texture_id, white_texture_id);
    // Reuse the open command only when the texture AND the active clip both match;
    // a clip change must break the run so per-command GPU scissor stays correct.
    if (!m_Commands.empty() && m_CurrentTexture == resolved)
    {
        const UiDrawCommand& back = m_Commands.back();
        if (back.has_clip == m_HasClip && (!m_HasClip || RectsEqual(back.clip_rect, m_ActiveClip)))
        {
            return;
        }
    }

    UiDrawCommand command {};
    command.texture_id = resolved;
    command.index_offset = static_cast<uint32_t>(m_Indices.size());
    command.index_count = 0;
    command.has_clip = m_HasClip;
    command.clip_rect = m_ActiveClip;
    m_Commands.push_back(command);
    m_CurrentTexture = resolved;
}

void UIRenderBatch::DrawQuad(const ZSlate::UIRect& rect, const ZSlate::UIColor& color, void* white_texture_id)
{
    DrawTexturedQuad(rect,
                     white_texture_id,
                     color,
                     Vector2(0.0f, 0.0f),
                     Vector2(1.0f, 1.0f),
                     white_texture_id);
}

void UIRenderBatch::DrawRect(const ZSlate::UIRect& rect, const ZSlate::UIColor& color, float thickness, void* white_texture_id)
{
    AppendOutline(rect.x, rect.y, rect.x + rect.w, rect.y + rect.h, color, thickness, white_texture_id);
}

void UIRenderBatch::DrawConvexPoly(const Vector2* points, int count, const ZSlate::UIColor& color, void* white_texture_id)
{
    if (points == nullptr || count < 3)
    {
        return;
    }

    BeginCommand(white_texture_id, white_texture_id);

    float rgba[4];
    ToVertexColor(color, rgba);

    const uint16_t base = static_cast<uint16_t>(m_Vertices.size());
    for (int i = 0; i < count; ++i)
    {
        UIVertex v {};
        TransformPoint(points[i].x, points[i].y, v.pos[0], v.pos[1]);
        v.uv[0] = 0.0f;  // sample the white texel
        v.uv[1] = 0.0f;
        v.color[0] = rgba[0];
        v.color[1] = rgba[1];
        v.color[2] = rgba[2];
        v.color[3] = rgba[3];
        m_Vertices.push_back(v);
    }

    // Triangle fan: (0, i, i+1).
    for (int i = 1; i + 1 < count; ++i)
    {
        m_Indices.push_back(base);
        m_Indices.push_back(static_cast<uint16_t>(base + i));
        m_Indices.push_back(static_cast<uint16_t>(base + i + 1));
    }

    if (!m_Commands.empty())
    {
        m_Commands.back().index_count = static_cast<uint32_t>(m_Indices.size()) - m_Commands.back().index_offset;
    }
}

void UIRenderBatch::DrawTexturedQuad(const ZSlate::UIRect& rect,
                                     void* texture_id,
                                     const ZSlate::UIColor& color,
                                     const Vector2& uv0,
                                     const Vector2& uv1,
                                     void* white_texture_id)
{
    AppendTexturedQuad(rect.x,
                       rect.y,
                       rect.x + rect.w,
                       rect.y + rect.h,
                       color,
                       uv0.x,
                       uv0.y,
                       uv1.x,
                       uv1.y,
                       texture_id,
                       white_texture_id);
}

void UIRenderBatch::AppendTexturedQuad(float x0,
                                       float y0,
                                       float x1,
                                       float y1,
                                       const ZSlate::UIColor& color,
                                       float uv0x,
                                       float uv0y,
                                       float uv1x,
                                       float uv1y,
                                       void* texture_id,
                                       void* white_texture_id)
{
    if (m_HasClip)
    {
        x0 = std::max(x0, m_ActiveClip.x);
        y0 = std::max(y0, m_ActiveClip.y);
        x1 = std::min(x1, m_ActiveClip.x + m_ActiveClip.w);
        y1 = std::min(y1, m_ActiveClip.y + m_ActiveClip.h);
        if (x1 <= x0 || y1 <= y0)
        {
            return;
        }
    }

    BeginCommand(texture_id, white_texture_id);

    float corners_x[4] = {x0, x1, x1, x0};
    float corners_y[4] = {y0, y0, y1, y1};
    float uv_x[4] = {uv0x, uv1x, uv1x, uv0x};
    float uv_y[4] = {uv0y, uv0y, uv1y, uv1y};

    const uint16_t base = static_cast<uint16_t>(m_Vertices.size());
    UIVertex vertices[4] {};
    ToVertexColor(color, vertices[0].color);
    for (int i = 0; i < 4; ++i)
    {
        TransformPoint(corners_x[i], corners_y[i], vertices[i].pos[0], vertices[i].pos[1]);
        vertices[i].uv[0] = uv_x[i];
        vertices[i].uv[1] = uv_y[i];
    }

    for (int i = 0; i < 4; ++i)
    {
        vertices[i].color[0] = vertices[0].color[0];
        vertices[i].color[1] = vertices[0].color[1];
        vertices[i].color[2] = vertices[0].color[2];
        vertices[i].color[3] = vertices[0].color[3];
        m_Vertices.push_back(vertices[i]);
    }

    m_Indices.push_back(base + 0);
    m_Indices.push_back(base + 1);
    m_Indices.push_back(base + 2);
    m_Indices.push_back(base + 0);
    m_Indices.push_back(base + 2);
    m_Indices.push_back(base + 3);

    if (!m_Commands.empty())
    {
        m_Commands.back().index_count = static_cast<uint32_t>(m_Indices.size()) - m_Commands.back().index_offset;
    }
}

void UIRenderBatch::AppendOutline(float x0,
                                  float y0,
                                  float x1,
                                  float y1,
                                  const ZSlate::UIColor& color,
                                  float thickness,
                                  void* white_texture_id)
{
    const float t = std::max(thickness, 1.0f);
    AppendTexturedQuad(x0, y0, x1, y0 + t, color, 0.0f, 0.0f, 1.0f, 1.0f, white_texture_id, white_texture_id);
    AppendTexturedQuad(x0, y1 - t, x1, y1, color, 0.0f, 0.0f, 1.0f, 1.0f, white_texture_id, white_texture_id);
    AppendTexturedQuad(x0, y0, x0 + t, y1, color, 0.0f, 0.0f, 1.0f, 1.0f, white_texture_id, white_texture_id);
    AppendTexturedQuad(x1 - t, y0, x1, y1, color, 0.0f, 0.0f, 1.0f, 1.0f, white_texture_id, white_texture_id);
}
