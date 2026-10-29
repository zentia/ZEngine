#pragma once
// ============================================================================
// ZSlateTextGenerator — CPU text layout engine (no engine deps)
// ============================================================================
// Transplants ZEngine's TextGenerator.  UTF-8 → positioned glyph quads using a
// ZSlateFontAtlas.  Supports alignment, wrapping, tab stops.
// ============================================================================

#include "ZSlate/Core/ZSlateTypes.h"
#include "ZSlate/Core/SlateGeometry.h"

#include <string>
#include <vector>

namespace ZSlate
{

class ZSlateFontAtlas;

class ZSlateTextGenerator
{
public:
    struct Glyph
    {
        UIRect  dest;          // screen-space quad
        Vector2 uv0, uv1;      // atlas UVs
    };

    struct Settings
    {
        UIRect      rect;
        float       font_size {16.0f};
        TextAnchor  alignment {TextAnchor::MiddleCenter};
        TextWrapMode wrap    {TextWrapMode::Wrap};
    };

    // Lay out text into settings.rect, populating GetGlyphs()/GetSize().
    void Generate(ZSlateFontAtlas& atlas, const std::string& text, const Settings& settings);

    // Measure only: returns laid-out extent without producing draw geometry.
    static Vector2 Measure(ZSlateFontAtlas& atlas, const std::string& text,
                           float font_size, TextWrapMode wrap, float wrap_width);

    const std::vector<Glyph>& GetGlyphs() const { return m_Glyphs; }
    const Vector2&            GetSize()   const { return m_Size; }

private:
    std::vector<Glyph> m_Glyphs;
    Vector2 m_Size {0.0f, 0.0f};
};

}  // namespace ZSlate
