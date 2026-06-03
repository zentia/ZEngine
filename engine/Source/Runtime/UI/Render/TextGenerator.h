#pragma once

#include "Runtime/Core/Math/Vector2.h"
#include "Runtime/UI/Core/UITypes.h"

#include <string>
#include <vector>

class ZFontAtlas;

// CPU text layout engine -- the ZEngine analogue of Unity's UnityEngine.TextGenerator.
//
// It turns a UTF-8 string + a font atlas + layout settings into a list of
// positioned, textured glyph quads plus the overall measured extent. ALL glyph
// iteration lives here: UTF-8 decoding, line breaking, tab stops, character-level
// wrapping, and anchor alignment. Wrapping is character-level (rather than word-
// level) because it matches CJK and is simpler.
//
// The renderer stays free of font/glyph concerns: it resolves the atlas' GPU
// texture and submits the quads this produces, exactly like Unity's Text/Graphic
// copies TextGenerator.verts into its CanvasRenderer mesh. The atlas is consulted
// only for glyph metrics (advance, line height, baked quad rects/UVs); the
// generator never touches the GPU.
class TextGenerator
{
public:
    // One laid-out glyph, in the same pixel space as the input rect. `dest` is the
    // quad to draw; `uv0`/`uv1` are the atlas texture coordinates. Whitespace and
    // control characters (space, tab, newline) produce no entry.
    struct Glyph
    {
        UIRect dest;
        Vector2 uv0;
        Vector2 uv1;
    };

    struct Settings
    {
        // The box the text is laid into. Width drives wrapping and horizontal
        // anchoring; height drives vertical anchoring.
        UIRect rect;
        float font_size {16.0f};
        TextAnchor alignment {TextAnchor::MiddleCenter};
        TextWrapMode wrap {TextWrapMode::Wrap};
    };

    // Lay `text` out into `settings.rect`, populating GetGlyphs()/GetSize().
    // Bakes glyphs on demand into `atlas` (cache hits on subsequent passes).
    void Generate(ZFontAtlas& atlas, const std::string& text, const Settings& settings);

    // Measure only: returns the laid-out extent without producing draw geometry.
    // `wrap_width <= 0` (or wrap == NoWrap) disables wrapping. Static because it
    // needs no generator state -- callers that only size text avoid an instance.
    static Vector2 Measure(ZFontAtlas& atlas,
                           const std::string& text,
                           float font_size,
                           TextWrapMode wrap,
                           float wrap_width);

    const std::vector<Glyph>& GetGlyphs() const { return m_Glyphs; }
    const Vector2& GetSize() const { return m_Size; }

private:
    std::vector<Glyph> m_Glyphs;
    Vector2 m_Size {0.0f, 0.0f};
};
