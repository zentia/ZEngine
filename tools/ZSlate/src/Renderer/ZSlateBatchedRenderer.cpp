#include "ZSlate/Renderer/ZSlateBatchedRenderer.h"

#include <algorithm>
#include <cmath>

namespace ZSlate
{

// ---------------------------------------------------------------------------
// Frame lifecycle
// ---------------------------------------------------------------------------

void ZSlateBatchedRenderer::BeginFrame()
{
    m_Commands.clear();
    m_ClipStack.clear();
}

void ZSlateBatchedRenderer::EndFrame()
{
    m_ClipStack.clear();
}

void ZSlateBatchedRenderer::Clear()
{
    m_Commands.clear();
    m_ClipStack.clear();
}

UIRect ZSlateBatchedRenderer::CurrentClipRect() const
{
    UIRect clip(-1.0e6f, -1.0e6f, 2.0e6f, 2.0e6f);
    for (const UIRect& rect : m_ClipStack)
    {
        const float x0 = std::max(clip.x, rect.x);
        const float y0 = std::max(clip.y, rect.y);
        const float x1 = std::min(clip.Right(), rect.Right());
        const float y1 = std::min(clip.Bottom(), rect.Bottom());
        if (x1 <= x0 || y1 <= y0) return UIRect(0.0f, 0.0f, 0.0f, 0.0f);
        clip = UIRect(x0, y0, x1 - x0, y1 - y0);
    }
    return clip;
}

void ZSlateBatchedRenderer::PushClipRect(const UIRect& rect)
{
    m_ClipStack.push_back(rect);
}

void ZSlateBatchedRenderer::PopClipRect()
{
    if (!m_ClipStack.empty()) m_ClipStack.pop_back();
}

// ---------------------------------------------------------------------------
// Primitives
// ---------------------------------------------------------------------------

void ZSlateBatchedRenderer::DrawQuad(const UIRect& rect, const UIColor& color)
{
    if (color.a <= 0.0f || rect.w <= 0.0f || rect.h <= 0.0f) return;

    const UIRect clip = CurrentClipRect();
    if (clip.w <= 0.0f || clip.h <= 0.0f) return;

    const float x0 = std::max(rect.x, clip.x);
    const float y0 = std::max(rect.y, clip.y);
    const float x1 = std::min(rect.Right(), clip.Right());
    const float y1 = std::min(rect.Bottom(), clip.Bottom());
    if (x1 <= x0 || y1 <= y0) return;

    FDrawCommand command;
    command.Type  = EDrawCommandType::Quad;
    command.Rect  = UIRect(x0, y0, x1 - x0, y1 - y0);
    command.Color = color;
    m_Commands.push_back(command);
}

void ZSlateBatchedRenderer::DrawRect(const UIRect& rect, const UIColor& color, float thickness)
{
    const float t = std::max(0.5f, thickness);
    DrawQuad(UIRect(rect.x, rect.y, rect.w, t), color);
    DrawQuad(UIRect(rect.x, rect.Bottom() - t, rect.w, t), color);
    DrawQuad(UIRect(rect.x, rect.y + t, t, std::max(0.0f, rect.h - t * 2.0f)), color);
    DrawQuad(UIRect(rect.Right() - t, rect.y + t, t, std::max(0.0f, rect.h - t * 2.0f)), color);
}

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------

void ZSlateBatchedRenderer::DrawTextLabel(const UIRect& rect,
                                          const std::string& text,
                                          float fontSize,
                                          const UIColor& color,
                                          TextAnchor anchor,
                                          TextWrapMode wrap,
                                          void* fontInfo)
{
    (void)fontInfo;
    if (text.empty() || fontSize <= 0.0f || color.a <= 0.0f) return;

    DrawGlyphRun(rect, text, fontSize, color, anchor, wrap);
}

void ZSlateBatchedRenderer::DrawGlyphRun(const UIRect& rect,
                                         const std::string& text,
                                         float fontSize,
                                         const UIColor& color,
                                         TextAnchor anchor,
                                         TextWrapMode wrap)
{
    if (!m_FontAtlas || !m_FontAtlas->IsLoaded())
    {
        // No font: draw a faint placeholder bar so text is not invisible.
        DrawQuad(UIRect(rect.x, rect.y + rect.h * 0.35f,
                        std::min(rect.w, static_cast<float>(Utf8::Length(text)) * fontSize * 0.5f),
                        rect.h * 0.3f),
                 UIColor(color.r, color.g, color.b, color.a * 0.35f));
        return;
    }

    const float ascent  = m_FontAtlas->GetAscent()  * fontSize;
    const float descent = m_FontAtlas->GetDescent() * fontSize;
    const float lineHeight = ascent + descent;

    const UIRect clip = CurrentClipRect();

    float penX = rect.x;
    float baseline = rect.y + (rect.h - lineHeight) * 0.5f + ascent;

    if (anchor == TextAnchor::TopLeft)
    {
        baseline = rect.y + ascent;
    }
    else if (anchor == TextAnchor::MiddleCenter)
    {
        const float textWidth = m_FontAtlas->MeasureLine(text, fontSize);
        penX = rect.x + (rect.w - textWidth) * 0.5f;
    }

    size_t offset = 0;
    while (offset < text.size())
    {
        uint32_t codepoint = 0;
        if (Utf8::Decode(text, offset, codepoint) == 0) break;

        if (codepoint == '\n')
        {
            if (wrap == TextWrapMode::Wrap)
            {
                penX = rect.x;
                baseline += lineHeight;
                continue;
            }
            codepoint = ' ';
        }

        const ZSlateFontAtlas::FGlyph* glyph = m_FontAtlas->GetGlyph(codepoint);
        if (!glyph) continue;

        const float advance = glyph->Advance * fontSize;

        if (wrap == TextWrapMode::Wrap && penX + advance > rect.Right() && penX > rect.x)
        {
            penX = rect.x;
            baseline += lineHeight;
        }

        const float glyphX = penX + glyph->BearingX * fontSize;
        const float glyphY = baseline - glyph->BearingY * fontSize;
        const float glyphW = glyph->Width  * fontSize;
        const float glyphH = glyph->Height * fontSize;

        if (glyphW > 0.0f && glyphH > 0.0f)
        {
            // Cheap horizontal clipping: skip glyphs outside the visible box.
            if (glyphX < clip.Right() && glyphX + glyphW > clip.x &&
                glyphY < clip.Bottom() && glyphY + glyphH > clip.y)
            {
                FDrawCommand command;
                command.Type      = EDrawCommandType::Glyph;
                command.Rect      = UIRect(glyphX, glyphY, glyphW, glyphH);
                command.Color     = color;
                command.UVRect    = UIRect(glyph->U0, glyph->V0,
                                           glyph->U1 - glyph->U0, glyph->V1 - glyph->V0);
                command.Codepoint = codepoint;
                m_Commands.push_back(command);
            }
        }

        penX += advance;

        if (penX > rect.Right() + fontSize * 4.0f && wrap == TextWrapMode::NoWrap) break;
    }
}

// ---------------------------------------------------------------------------
// Measurement
// ---------------------------------------------------------------------------

float ZSlateBatchedRenderer::GetLineHeight(float fontSize) const
{
    if (m_FontAtlas) return m_FontAtlas->GetLineHeight() * fontSize;
    return fontSize * 1.35f;
}

Vector2 ZSlateBatchedRenderer::MeasureText(const std::string& text, float fontSize) const
{
    if (text.empty())
    {
        return Vector2(0.0f, GetLineHeight(fontSize));
    }

    if (!m_FontAtlas || !m_FontAtlas->IsLoaded())
    {
        return Vector2(static_cast<float>(Utf8::Length(text)) * fontSize * 0.55f,
                       GetLineHeight(fontSize));
    }

    float widest = 0.0f;
    float current = 0.0f;
    int   lines = 1;

    size_t offset = 0;
    while (offset < text.size())
    {
        uint32_t codepoint = 0;
        if (Utf8::Decode(text, offset, codepoint) == 0) break;

        if (codepoint == '\n')
        {
            widest = std::max(widest, current);
            current = 0.0f;
            ++lines;
            continue;
        }

        const ZSlateFontAtlas::FGlyph* glyph = m_FontAtlas->GetGlyph(codepoint);
        if (glyph) current += glyph->Advance * fontSize;
    }

    widest = std::max(widest, current);
    return Vector2(widest, GetLineHeight(fontSize) * static_cast<float>(lines));
}

} // namespace ZSlate
