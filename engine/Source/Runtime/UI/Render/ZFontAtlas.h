#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// Native TrueType glyph atlas (stb_truetype). Replaces ImGui's ImFontAtlas/ImFont
// for ZSlate text so the 2D UI no longer depends on ImGui for font rasterization.
//
// On-demand baking: glyphs are rendered into a CPU RGBA bitmap the first time a
// (codepoint, pixel-size) pair is requested, not pre-ranged. This is CJK-capable
// without paying for the full CJK range up front -- only glyphs that are actually
// drawn ever get baked. The owner (UIGpuResources) uploads the bitmap to a GPU
// texture and re-uploads it whenever IsDirty() reports new glyphs since the last
// ClearDirty().
//
// Packing is a naive left-to-right shelf packer with 1px gaps; the atlas is a
// fixed 2048x2048 (no eviction in V1 -- enough for the editor's working set of
// Latin + a few hundred CJK glyphs at typical sizes). When the atlas fills, new
// glyphs return invalid (logged once) and render blank rather than corrupting.
class ZFontAtlas
{
public:
    struct Glyph
    {
        // Atlas UVs in [0,1].
        float u0 {0.0f};
        float v0 {0.0f};
        float u1 {0.0f};
        float v1 {0.0f};
        // Quad corners relative to the pen, where the pen is the text TOP-LEFT
        // (matches the ImGui glyph convention the old draw path used).
        float x0 {0.0f};
        float y0 {0.0f};
        float x1 {0.0f};
        float y1 {0.0f};
        float advance {0.0f};
        bool valid {false};
    };

    ZFontAtlas() = default;
    ~ZFontAtlas();

    ZFontAtlas(const ZFontAtlas&) = delete;
    ZFontAtlas& operator=(const ZFontAtlas&) = delete;

    bool LoadFromFile(const std::string& ttf_path);
    bool IsLoaded() const { return m_Loaded; }

    // Add a fallback font that will be tried when the primary font does not contain
    // a requested glyph. Multiple fallbacks can be added; they are tried in order.
    // Returns true if the fallback font was loaded successfully.
    // On Windows the caller should add CJK-capable fonts (e.g. msyh.ttc) so that
    // Chinese/Japanese/Korean text renders without missing glyphs.
    bool AddFallbackFont(const std::string& ttf_path);

    // Bake-on-demand. pixel_size is rounded to an integer for cache keying, so a
    // handful of DPI-scaled sizes do not explode the glyph cache.
    // If the primary font lacks the codepoint, fallback fonts are tried automatically.
    const Glyph& GetGlyph(unsigned int codepoint, float pixel_size);

    // Distance from one line's top to the next (ascent - descent + line gap).
    float GetLineHeight(float pixel_size) const;
    // Distance from text top to the baseline.
    float GetAscent(float pixel_size) const;

    const uint8_t* GetPixels() const { return m_Pixels.data(); }
    uint32_t GetWidth() const { return m_Width; }
    uint32_t GetHeight() const { return m_Height; }

    bool IsDirty() const { return m_Dirty; }
    void ClearDirty() { m_Dirty = false; }

private:
    float ScaleForSize(float pixel_size) const;
    bool PackRect(uint32_t w, uint32_t h, uint32_t& out_x, uint32_t& out_y);

    bool m_Loaded {false};
    bool m_AtlasFullWarned {false};

    std::vector<uint8_t> m_FontData;  // TTF bytes; must outlive the stbtt_fontinfo
    void* m_FontInfo {nullptr};       // stbtt_fontinfo* (opaque to keep stb out of header)

    int m_AscentUnits {0};
    int m_DescentUnits {0};
    int m_LineGapUnits {0};

    std::vector<uint8_t> m_Pixels;  // RGBA8 atlas (white RGB, glyph coverage in A)
    uint32_t m_Width {0};
    uint32_t m_Height {0};

    // Naive shelf packer cursor.
    uint32_t m_PenX {1};
    uint32_t m_PenY {1};
    uint32_t m_RowHeight {0};

    bool m_Dirty {false};

    // Opaque pointer to fallback font data (std::vector<FallbackFont>*).
    // All fallback management lives in ZFontAtlas.cpp; keeping it opaque here
    // avoids pulling stb_truetype.h into every translation unit that includes
    // this header (ZAEngine already does the same trick for m_FontInfo).
    void* m_FallbackData {nullptr};

    std::unordered_map<uint64_t, Glyph> m_Glyphs;
    Glyph m_Invalid {};
};
