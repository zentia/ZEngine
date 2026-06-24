#include "Runtime/UI/Render/UiRenderBatch.h"

#include <algorithm>
#include <cmath>

namespace
{
    UIRect IntersectRects(const UIRect& a, const UIRect& b)
    {
        const float x0 = std::max(a.x, b.x);
        const float y0 = std::max(a.y, b.y);
        const float x1 = std::min(a.x + a.width, b.x + b.width);
        const float y1 = std::min(a.y + a.height, b.y + b.height);
        if (x1 <= x0 || y1 <= y0)
        {
            return UIRect {0.0f, 0.0f, 0.0f, 0.0f};
        }
        return UIRect {x0, y0, x1 - x0, y1 - y0};
    }

    void ToVertexColor(const UIColor& color, float out_rgba[4])
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

    bool RectsEqual(const UIRect& a, const UIRect& b)
    {
        return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
    }
}  // namespace

void UiRenderBatch::clear()
{
    m_Vertices.clear();
    m_Indices.clear();
    m_Commands.clear();
    m_ClipStack.clear();
    m_TransformStack.clear();
    m_ActiveTransform = UiAffine2D::Identity();
    m_HasClip = false;
    m_ActiveClip = UIRect {0.0f, 0.0f, 0.0f, 0.0f};
    m_CurrentTexture = nullptr;
}

void UiRenderBatch::PushTransform(const UiAffine2D& transform)
{
    m_TransformStack.push_back(m_ActiveTransform);
    m_ActiveTransform = m_ActiveTransform * transform;
}

void UiRenderBatch::PopTransform()
{
    if (m_TransformStack.empty())
    {
        m_ActiveTransform = UiAffine2D::Identity();
        return;
    }

    m_ActiveTransform = m_TransformStack.back();
    m_TransformStack.pop_back();
}

void UiRenderBatch::transformPoint(float x, float y, float& out_x, float& out_y) const
{
    m_ActiveTransform.TransformPoint(x, y, out_x, out_y);
}

void UiRenderBatch::PushClipRect(const UIRect& clip_rect, bool intersect_with_current)
{
    UIRect next = clip_rect;
    if (m_HasClip && intersect_with_current)
    {
        next = IntersectRects(m_ActiveClip, clip_rect);
    }
    m_ClipStack.push_back(m_ActiveClip);
    m_ActiveClip = next;
    m_HasClip = true;
}

void UiRenderBatch::PopClipRect()
{
    if (m_ClipStack.empty())
    {
        m_HasClip = false;
        m_ActiveClip = UIRect {0.0f, 0.0f, 0.0f, 0.0f};
        return;
    }

    m_ActiveClip = m_ClipStack.back();
    m_ClipStack.pop_back();
    m_HasClip = !m_ClipStack.empty() || (m_ActiveClip.width > 0.0f && m_ActiveClip.height > 0.0f);
}

void UiRenderBatch::forceNewCommand()
{
    // Sentinel that cannot match any real (or null/white) texture id, so the next
    // beginCommand always opens a new command.
    m_CurrentTexture = reinterpret_cast<void*>(~static_cast<uintptr_t>(0));
}

void UiRenderBatch::beginCommand(void* texture_id, void* white_texture_id)
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

void UiRenderBatch::DrawQuad(const UIRect& rect, const UIColor& color, void* white_texture_id)
{
    DrawTexturedQuad(rect,
                     white_texture_id,
                     color,
                     Vector2(0.0f, 0.0f),
                     Vector2(1.0f, 1.0f),
                     white_texture_id);
}

void UiRenderBatch::DrawRect(const UIRect& rect, const UIColor& color, float thickness, void* white_texture_id)
{
    appendOutline(rect.x, rect.y, rect.x + rect.width, rect.y + rect.height, color, thickness, white_texture_id);
}

void UiRenderBatch::DrawConvexPoly(const Vector2* points, int count, const UIColor& color, void* white_texture_id)
{
    if (points == nullptr || count < 3)
    {
        return;
    }

    beginCommand(white_texture_id, white_texture_id);

    float rgba[4];
    ToVertexColor(color, rgba);

    const uint16_t base = static_cast<uint16_t>(m_Vertices.size());
    for (int i = 0; i < count; ++i)
    {
        UiVertex v {};
        transformPoint(points[i].x, points[i].y, v.pos[0], v.pos[1]);
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

void UiRenderBatch::DrawTexturedQuad(const UIRect& rect,
                                     void* texture_id,
                                     const UIColor& color,
                                     const Vector2& uv0,
                                     const Vector2& uv1,
                                     void* white_texture_id)
{
    appendTexturedQuad(rect.x,
                       rect.y,
                       rect.x + rect.width,
                       rect.y + rect.height,
                       color,
                       uv0.x,
                       uv0.y,
                       uv1.x,
                       uv1.y,
                       texture_id,
                       white_texture_id);
}

void UiRenderBatch::appendTexturedQuad(float x0,
                                       float y0,
                                       float x1,
                                       float y1,
                                       const UIColor& color,
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
        x1 = std::min(x1, m_ActiveClip.x + m_ActiveClip.width);
        y1 = std::min(y1, m_ActiveClip.y + m_ActiveClip.height);
        if (x1 <= x0 || y1 <= y0)
        {
            return;
        }
    }

    beginCommand(texture_id, white_texture_id);

    float corners_x[4] = {x0, x1, x1, x0};
    float corners_y[4] = {y0, y0, y1, y1};
    float uv_x[4] = {uv0x, uv1x, uv1x, uv0x};
    float uv_y[4] = {uv0y, uv0y, uv1y, uv1y};

    const uint16_t base = static_cast<uint16_t>(m_Vertices.size());
    UiVertex vertices[4] {};
    ToVertexColor(color, vertices[0].color);
    for (int i = 0; i < 4; ++i)
    {
        transformPoint(corners_x[i], corners_y[i], vertices[i].pos[0], vertices[i].pos[1]);
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

void UiRenderBatch::appendOutline(float x0,
                                  float y0,
                                  float x1,
                                  float y1,
                                  const UIColor& color,
                                  float thickness,
                                  void* white_texture_id)
{
    const float t = std::max(thickness, 1.0f);
    appendTexturedQuad(x0, y0, x1, y0 + t, color, 0.0f, 0.0f, 1.0f, 1.0f, white_texture_id, white_texture_id);
    appendTexturedQuad(x0, y1 - t, x1, y1, color, 0.0f, 0.0f, 1.0f, 1.0f, white_texture_id, white_texture_id);
    appendTexturedQuad(x0, y0, x0 + t, y1, color, 0.0f, 0.0f, 1.0f, 1.0f, white_texture_id, white_texture_id);
    appendTexturedQuad(x1 - t, y0, x1, y1, color, 0.0f, 0.0f, 1.0f, 1.0f, white_texture_id, white_texture_id);
}
