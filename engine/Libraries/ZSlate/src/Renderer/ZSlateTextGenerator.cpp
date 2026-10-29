#include "ZSlate/Renderer/ZSlateTextGenerator.h"
#include "ZSlate/Renderer/ZSlateFontAtlas.h"

#include <algorithm>
#include <cmath>

namespace ZSlate
{

namespace
{
    int Utf8Decode(unsigned int* out_char, const char* in, const char* end)
    {
        constexpr unsigned int kReplacement = 0xFFFDu;
        const unsigned char* s = reinterpret_cast<const unsigned char*>(in);
        const unsigned char* e = reinterpret_cast<const unsigned char*>(end);
        if (s >= e) { *out_char = 0; return 0; }
        const unsigned int c0 = s[0];
        if (c0 < 0x80u) { *out_char = c0; return 1; }
        if ((c0 & 0xE0u) == 0xC0u) {
            if (s + 1 >= e || (s[1] & 0xC0u) != 0x80u) { *out_char = kReplacement; return 1; }
            *out_char = ((c0 & 0x1Fu) << 6) | (s[1] & 0x3Fu); return 2;
        }
        if ((c0 & 0xF0u) == 0xE0u) {
            if (s + 2 >= e || (s[1] & 0xC0u) != 0x80u || (s[2] & 0xC0u) != 0x80u) { *out_char = kReplacement; return 1; }
            *out_char = ((c0 & 0x0Fu) << 12) | ((s[1] & 0x3Fu) << 6) | (s[2] & 0x3Fu); return 3;
        }
        if ((c0 & 0xF8u) == 0xF0u) {
            if (s + 3 >= e || (s[1] & 0xC0u) != 0x80u || (s[2] & 0xC0u) != 0x80u || (s[3] & 0xC0u) != 0x80u)
            { *out_char = kReplacement; return 1; }
            *out_char = ((c0 & 0x07u) << 18) | ((s[1] & 0x3Fu) << 12) | ((s[2] & 0x3Fu) << 6) | (s[3] & 0x3Fu); return 4;
        }
        *out_char = kReplacement; return 1;
    }

    constexpr int kTabColumns = 4;

    float AdvanceTabStop(ZSlateFontAtlas& atlas, float size, float rel_x)
    {
        const float space_adv = atlas.GetGlyph(' ', size).advance;
        const float tab_px = space_adv * static_cast<float>(kTabColumns);
        if (tab_px <= 0.0f) return rel_x;
        return std::ceil((rel_x + 0.001f) / tab_px) * tab_px;
    }

    struct PlacedGlyph
    {
        float rel_x {0.0f}, rel_y {0.0f};
        const ZSlateFontAtlas::Glyph* glyph {nullptr};
    };

    Vector2 LayoutText(ZSlateFontAtlas& atlas, const std::string& text, float size,
                       TextWrapMode wrap, float wrap_width, std::vector<PlacedGlyph>* out)
    {
        const float line_h = atlas.GetLineHeight(size);
        const bool do_wrap = (wrap == TextWrapMode::Wrap) && wrap_width > 0.0f;

        float max_w = 0.0f, pen_x = 0.0f, pen_y = 0.0f;
        int lines = 1;

        const char* s = text.c_str();
        const char* end = s + text.size();
        while (s < end)
        {
            unsigned int c = 0;
            s += Utf8Decode(&c, s, end);
            if (c == 0) break;
            if (c == '\n') { max_w = std::max(max_w, pen_x); pen_x = 0.0f; pen_y += line_h; ++lines; continue; }
            if (c == '\r') continue;
            if (c == '\t') { pen_x = AdvanceTabStop(atlas, size, pen_x); continue; }
            const ZSlateFontAtlas::Glyph& g = atlas.GetGlyph(c, size);
            if (do_wrap && pen_x > 0.0f && (pen_x + g.advance) > wrap_width)
            { max_w = std::max(max_w, pen_x); pen_x = 0.0f; pen_y += line_h; ++lines; }
            if (out) out->push_back({pen_x, pen_y, &g});
            pen_x += g.advance;
        }
        max_w = std::max(max_w, pen_x);
        return Vector2(max_w, static_cast<float>(lines) * line_h);
    }

    Vector2 AnchorToFactor(TextAnchor anchor)
    {
        switch (anchor)
        {
            case TextAnchor::TopLeft:   return Vector2(0.0f, 0.0f);
            case TextAnchor::TopCenter: return Vector2(0.5f, 0.0f);
            case TextAnchor::TopRight:  return Vector2(1.0f, 0.0f);
            case TextAnchor::MiddleLeft:  return Vector2(0.0f, 0.5f);
            case TextAnchor::MiddleCenter:return Vector2(0.5f, 0.5f);
            case TextAnchor::MiddleRight: return Vector2(1.0f, 0.5f);
            case TextAnchor::BottomLeft:   return Vector2(0.0f, 1.0f);
            case TextAnchor::BottomCenter: return Vector2(0.5f, 1.0f);
            case TextAnchor::BottomRight:  return Vector2(1.0f, 1.0f);
        }
        return Vector2(0.5f, 0.5f);
    }
}

void ZSlateTextGenerator::Generate(ZSlateFontAtlas& atlas, const std::string& text, const Settings& settings)
{
    m_Glyphs.clear();
    m_Size = Vector2(0.0f, 0.0f);
    if (text.empty()) return;

    const float size = settings.font_size > 0.0f ? settings.font_size : 16.0f;
    const float wrap_w = settings.wrap == TextWrapMode::Wrap ? settings.rect.w : 0.0f;

    std::vector<PlacedGlyph> placed;
    placed.reserve(text.size());
    m_Size = LayoutText(atlas, text, size, settings.wrap, wrap_w, &placed);

    const Vector2 align = AnchorToFactor(settings.alignment);
    const float origin_x = settings.rect.x + (settings.rect.w - m_Size.x) * align.x;
    const float origin_y = settings.rect.y + (settings.rect.h - m_Size.y) * align.y;

    m_Glyphs.reserve(placed.size());
    for (const auto& p : placed)
    {
        const ZSlateFontAtlas::Glyph& g = *p.glyph;
        if (!g.valid || g.x1 <= g.x0 || g.y1 <= g.y0) continue;
        Glyph quad;
        quad.dest = UIRect(origin_x + p.rel_x + g.x0, origin_y + p.rel_y + g.y0,
                           g.x1 - g.x0, g.y1 - g.y0);
        quad.uv0 = Vector2(g.u0, g.v0);
        quad.uv1 = Vector2(g.u1, g.v1);
        m_Glyphs.push_back(quad);
    }
}

Vector2 ZSlateTextGenerator::Measure(ZSlateFontAtlas& atlas, const std::string& text,
                                     float font_size, TextWrapMode wrap, float wrap_width)
{
    if (text.empty()) return Vector2(0.0f, 0.0f);
    const float size = font_size > 0.0f ? font_size : 16.0f;
    const float eff_wrap = (wrap == TextWrapMode::Wrap) ? wrap_width : 0.0f;
    return LayoutText(atlas, text, size, wrap, eff_wrap, nullptr);
}

}  // namespace ZSlate
