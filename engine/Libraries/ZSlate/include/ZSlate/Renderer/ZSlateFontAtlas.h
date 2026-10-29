#pragma once
// ============================================================================
// ZSlateFontAtlas — TrueType glyph atlas (stb_truetype, no engine deps)
// ============================================================================
// Self-contained TTF rasterizer transplanted from ZEngine's ZFontAtlas.
// On-demand baking: glyphs are rasterised to a 2048x2048 RGBA atlas the first
// time a (codepoint, pixel_size) pair is requested.
// ============================================================================

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ZSlate
{

class ZSlateFontAtlas
{
public:
    struct Glyph
    {
        float u0 {0.0f}, v0 {0.0f};   // atlas UVs in [0,1]
        float u1 {0.0f}, v1 {0.0f};
        // Quad corners relative to pen (text top-left origin).
        float x0 {0.0f}, y0 {0.0f};
        float x1 {0.0f}, y1 {0.0f};
        float advance {0.0f};
        bool  valid {false};
    };

    ZSlateFontAtlas() = default;
    ~ZSlateFontAtlas();

    ZSlateFontAtlas(const ZSlateFontAtlas&) = delete;
    ZSlateFontAtlas& operator=(const ZSlateFontAtlas&) = delete;

    bool LoadFromFile(const std::string& ttf_path);
    bool IsLoaded() const { return m_Loaded; }

    // Fallback fonts tried when the primary font lacks a codepoint (e.g. CJK).
    bool AddFallbackFont(const std::string& ttf_path);

    // Cheap probe: does this font file actually contain the codepoint?
    // Loads the file into a scratch buffer and asks stb_truetype for the glyph
    // index; no atlas state is touched. Used by hosts that scan a fonts
    // directory for a CJK-capable face on stripped-down systems.
    static bool ProbeHasCodepoint(const std::string& ttf_path, unsigned int codepoint);

    // Bake-on-demand. pixel_size is rounded to an integer for cache keying.
    const Glyph& GetGlyph(unsigned int codepoint, float pixel_size);

    float GetLineHeight(float pixel_size) const;
    float GetAscent(float pixel_size) const;

    const uint8_t* GetPixels() const { return m_Pixels.data(); }
    uint32_t GetWidth()  const { return m_Width; }
    uint32_t GetHeight() const { return m_Height; }
    bool     IsDirty()   const { return m_Dirty; }
    void     ClearDirty()       { m_Dirty = false; }

private:
    float ScaleForSize(float pixel_size) const;
    bool  PackRect(uint32_t w, uint32_t h, uint32_t& out_x, uint32_t& out_y);

    bool m_Loaded {false};
    bool m_AtlasFullWarned {false};

    // Codepoints no font (primary or fallback) could cover. Warned once each,
    // bounded -- a mojibake stream can produce many distinct bogus codepoints.
    std::unordered_set<unsigned int> m_MissingCodepoints;

    std::vector<uint8_t> m_FontData;
    void* m_FontInfo {nullptr};    // stbtt_fontinfo* (opaque)

    int m_AscentUnits  {0};
    int m_DescentUnits {0};
    int m_LineGapUnits {0};

    std::vector<uint8_t> m_Pixels;
    uint32_t m_Width  {0};
    uint32_t m_Height {0};

    uint32_t m_PenX     {1};
    uint32_t m_PenY     {1};
    uint32_t m_RowHeight {0};

    bool m_Dirty {false};

    void* m_FallbackData {nullptr};  // std::vector<FallbackFont>* (opaque)

    std::unordered_map<uint64_t, Glyph> m_Glyphs;
    Glyph m_Invalid {};
};

}  // namespace ZSlate
