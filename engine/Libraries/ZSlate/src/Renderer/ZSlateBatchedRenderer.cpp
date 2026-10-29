#include "ZSlate/Renderer/ZSlateBatchedRenderer.h"
#include "ZSlate/Renderer/ZSlateFontAtlas.h"
#include "ZSlate/Renderer/ZSlateTextGenerator.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ZSlate
{

// ---- Internal helpers -------------------------------------------------------

namespace
{
    bool RectsEqual(const UIRect& a, const UIRect& b)
    {
        return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
    }

    UIRect IntersectRects(const UIRect& a, const UIRect& b)
    {
        float x0 = std::max(a.x, b.x);
        float y0 = std::max(a.y, b.y);
        float x1 = std::min(a.Right(), b.Right());
        float y1 = std::min(a.Bottom(), b.Bottom());
        if (x1 <= x0 || y1 <= y0)
            return UIRect{0, 0, 0, 0};
        return UIRect{x0, y0, x1 - x0, y1 - y0};
    }

    void ToColorFloats(const UIColor& c, float out[4])
    {
        out[0] = std::clamp(c.x, 0.0f, 1.0f);
        out[1] = std::clamp(c.y, 0.0f, 1.0f);
        out[2] = std::clamp(c.z, 0.0f, 1.0f);
        out[3] = std::clamp(c.w, 0.0f, 1.0f);
    }
}  // namespace

// ---- Constructor ------------------------------------------------------------

ZSlateBatchedRenderer::ZSlateBatchedRenderer()
{
    m_Vertices.reserve(4096);
    m_Indices.reserve(6144);
    m_Commands.reserve(128);
    m_ClipStack.reserve(16);
}

void ZSlateBatchedRenderer::SetFontAtlas(ZSlateFontAtlas* atlas)
{
    m_FontAtlas = atlas;
    if (m_FontAtlas)
        m_TextGen = std::make_unique<ZSlateTextGenerator>();
    else
        m_TextGen.reset();
}

// ---- Frame lifecycle --------------------------------------------------------

void ZSlateBatchedRenderer::BeginFrame()
{
    m_Vertices.clear();
    m_Indices.clear();
    m_Commands.clear();
    m_ClipStack.clear();
    m_CurrentTexture = nullptr;
    m_HasClip = false;
    m_ActiveClip = UIRect{0, 0, 0, 0};
    m_Active = true;
}

void ZSlateBatchedRenderer::EndFrame()
{
    m_Active = false;
}

// ---- Clipping ---------------------------------------------------------------

void ZSlateBatchedRenderer::PushClipRect(const UIRect& rect)
{
    if (!m_Active) return;
    UIRect effective = m_HasClip ? IntersectRects(m_ActiveClip, rect) : rect;
    m_ClipStack.push_back(m_ActiveClip);
    m_ActiveClip = effective;
    m_HasClip = true;
    ForceNewCommand(m_CurrentTexture);
}

void ZSlateBatchedRenderer::PopClipRect()
{
    if (!m_Active) return;
    if (!m_ClipStack.empty())
    {
        m_ActiveClip = m_ClipStack.back();
        m_ClipStack.pop_back();
        m_HasClip = !m_ClipStack.empty() || (m_ActiveClip.w > 0 && m_ActiveClip.h > 0);
    }
    else
    {
        m_HasClip = false;
    }
    ForceNewCommand(m_CurrentTexture);
}

// ---- Command management -----------------------------------------------------

void ZSlateBatchedRenderer::BeginCommand(void* textureId)
{
    if (!m_Active) return;
    if (m_CurrentTexture != textureId)
    {
        m_CurrentTexture = textureId;
        ForceNewCommand(textureId);
    }
}

void ZSlateBatchedRenderer::ForceNewCommand(void* textureId)
{
    if (!m_Active) return;
    ZSBDrawCmd cmd;
    cmd.TextureId = textureId;
    cmd.IndexOffset = static_cast<uint32_t>(m_Indices.size());
    cmd.IndexCount = 0;
    cmd.ClipRect = m_ActiveClip;
    cmd.HasClip = m_HasClip;
    m_Commands.push_back(cmd);
}

// ---- Quad generation --------------------------------------------------------

void ZSlateBatchedRenderer::AppendTexturedQuad(
    const UIRect& rect, const UIColor& color, void* textureId,
    float u0, float v0, float u1, float v1)
{
    if (!m_Active) return;

    BeginCommand(textureId);

    // Clamp to active clip rect
    UIRect clipped = rect;
    if (m_HasClip)
    {
        UIRect inter = IntersectRects(rect, m_ActiveClip);
        if (inter.w <= 0 || inter.h <= 0) return;
        float scaleU = (u1 - u0) / rect.w;
        float scaleV = (v1 - v0) / rect.h;
        u0 += (inter.x - rect.x) * scaleU;
        v0 += (inter.y - rect.y) * scaleV;
        u1 -= (rect.Right() - inter.Right()) * scaleU;
        v1 -= (rect.Bottom() - inter.Bottom()) * scaleV;
        clipped = inter;
    }

    float x0 = clipped.x;
    float y0 = clipped.y;
    float x1 = clipped.Right();
    float y1 = clipped.Bottom();

    float c[4];
    ToColorFloats(color, c);

    uint16_t base = static_cast<uint16_t>(m_Vertices.size());

    ZSBVertex v;
    std::memcpy(v.Color, c, sizeof(c));

    // TL
    v.Pos[0] = x0; v.Pos[1] = y0; v.UV[0] = u0; v.UV[1] = v0;
    m_Vertices.push_back(v);
    // TR
    v.Pos[0] = x1; v.Pos[1] = y0; v.UV[0] = u1; v.UV[1] = v0;
    m_Vertices.push_back(v);
    // BR
    v.Pos[0] = x1; v.Pos[1] = y1; v.UV[0] = u1; v.UV[1] = v1;
    m_Vertices.push_back(v);
    // BL
    v.Pos[0] = x0; v.Pos[1] = y1; v.UV[0] = u0; v.UV[1] = v1;
    m_Vertices.push_back(v);

    m_Indices.push_back(base);
    m_Indices.push_back(base + 1);
    m_Indices.push_back(base + 2);
    m_Indices.push_back(base);
    m_Indices.push_back(base + 2);
    m_Indices.push_back(base + 3);

    if (!m_Commands.empty())
        m_Commands.back().IndexCount = static_cast<uint32_t>(m_Indices.size()) - m_Commands.back().IndexOffset;
}

// ---- ISlateRenderer implementation ------------------------------------------

void ZSlateBatchedRenderer::DrawQuad(const UIRect& rect, const UIColor& color)
{
    if (!m_Active) return;
    AppendTexturedQuad(rect, color, nullptr);  // nullptr = solid white 1x1 dummy
}

void ZSlateBatchedRenderer::DrawRect(const UIRect& rect, const UIColor& color, float thickness)
{
    if (!m_Active || thickness <= 0) return;

    // 4 thin quads forming a hollow border (top / bottom / left / right edges)
    float t = thickness;
    float x0 = rect.x, y0 = rect.y;
    float x1 = rect.Right(), y1 = rect.Bottom();

    // Top edge
    AppendTexturedQuad(UIRect(x0, y0, x1 - x0, t), color, nullptr);
    // Bottom edge
    AppendTexturedQuad(UIRect(x0, y1 - t, x1 - x0, t), color, nullptr);
    // Left edge (inset from top/bottom)
    AppendTexturedQuad(UIRect(x0, y0 + t, t, y1 - y0 - 2 * t), color, nullptr);
    // Right edge
    AppendTexturedQuad(UIRect(x1 - t, y0 + t, t, y1 - y0 - 2 * t), color, nullptr);
}

void ZSlateBatchedRenderer::DrawConvexPoly(const Vector2* points, int count, const UIColor& color)
{
    if (!m_Active || count < 3) return;
    // Simplified: fill bounding rect
    float minX = points[0].x, maxX = points[0].x;
    float minY = points[0].y, maxY = points[0].y;
    for (int i = 1; i < count; ++i)
    {
        minX = std::min(minX, points[i].x); maxX = std::max(maxX, points[i].x);
        minY = std::min(minY, points[i].y); maxY = std::max(maxY, points[i].y);
    }
    AppendTexturedQuad(UIRect{minX, minY, maxX - minX, maxY - minY}, color, nullptr);
}

void ZSlateBatchedRenderer::DrawRoundedRect(const UIRect& rect, float radius, const UIColor& color)
{
    // Simplified: draw as filled quad; full rounded-rect tessellation omitted for brevity
    (void)radius;
    DrawQuad(rect, color);
}

void ZSlateBatchedRenderer::DrawTexturedQuad(const UIRect& rect, void* textureHandle, const UIColor& tint)
{
    AppendTexturedQuad(rect, tint, textureHandle);
}

void ZSlateBatchedRenderer::DrawBox(const UIRect& rect, const FMargin& margin, void* textureHandle, const UIColor& tint)
{
    // 9-slice box — simplified: draw the whole rect as a single textured quad
    (void)margin;
    AppendTexturedQuad(rect, tint, textureHandle);
}

void ZSlateBatchedRenderer::DrawBorder(const UIRect& rect, const FMargin& margin, void* textureHandle, const UIColor& tint)
{
    // Simplified border: draw rect with outline thickness = margin.Left
    (void)textureHandle;
    DrawRect(rect, tint, margin.Left);
}

void ZSlateBatchedRenderer::DrawTextLabel(const UIRect& rect, const std::string& text, float fontSize,
                                         const UIColor& color, TextAnchor alignment,
                                         TextWrapMode wrap, void* fontHandle)
{
    (void)fontHandle;

    if (text.empty() || !m_Active) return;

    if (m_FontAtlas && m_TextGen)
    {
        // Real glyph rendering via font atlas — snap to pixel grid for sharp text
        ZSlateTextGenerator::Settings s;
        s.rect = UIRect(std::round(rect.x), std::round(rect.y),
                        std::ceil(rect.w), std::ceil(rect.h));
        s.font_size = std::round(fontSize);
        s.alignment = alignment;
        s.wrap = wrap;
        m_TextGen->Generate(*m_FontAtlas, text, s);

        for (const auto& g : m_TextGen->GetGlyphs())
        {
            UIRect snapped(std::round(g.dest.x), std::round(g.dest.y), g.dest.w, g.dest.h);
            AppendTexturedQuad(snapped, color, kFontAtlasTextureId,
                               g.uv0.x, g.uv0.y, g.uv1.x, g.uv1.y);
        }
        return;
    }

    // Fallback: per-character vertical bars (no font atlas)
    Vector2 measure = MeasureText(text, fontSize);
    float offsetX = rect.x;
    float offsetY = rect.y + (rect.h - measure.y) * 0.5f;

    switch (alignment)
    {
    case TextAnchor::TopRight: case TextAnchor::MiddleRight: case TextAnchor::BottomRight:
        offsetX = rect.Right() - measure.x; break;
    case TextAnchor::TopCenter: case TextAnchor::MiddleCenter: case TextAnchor::BottomCenter:
        offsetX = rect.x + (rect.w - measure.x) * 0.5f; break;
    default: break;
    }

    float charWidth  = fontSize * 0.55f;
    float charGap    = fontSize * 0.05f;
    float charStep   = charWidth + charGap;
    float barHeight  = fontSize * 1.0f;

    for (size_t i = 0; i < text.length(); ++i)
    {
        if (offsetX + charWidth > rect.Right()) break;
        UIRect chRect(offsetX + static_cast<float>(i) * charStep,
                       offsetY + (measure.y - barHeight) * 0.5f,
                       charWidth, barHeight);
        AppendTexturedQuad(chRect, color, nullptr);
    }
}

void ZSlateBatchedRenderer::DrawTextLabel(const std::string& text, const Vector2& pos, float fontSize, const UIColor& color)
{
    DrawTextLabel(UIRect{pos.x, pos.y, 300, fontSize + 4}, text, fontSize, color,
                  TextAnchor::MiddleLeft, TextWrapMode::NoWrap, nullptr);
}

Vector2 ZSlateBatchedRenderer::MeasureText(const std::string& text, float fontSize) const
{
    // Use font atlas if available
    if (m_FontAtlas && m_TextGen)
        return ZSlateTextGenerator::Measure(*m_FontAtlas, text, std::round(fontSize),
                                             TextWrapMode::NoWrap, 0.0f);

    if (m_TextMeasurer)
        return m_TextMeasurer(text, fontSize);

    // Default: rough ASCII estimate
    return Vector2(static_cast<float>(text.length()) * fontSize * 0.6f, fontSize * 1.2f);
}

}  // namespace ZSlate
