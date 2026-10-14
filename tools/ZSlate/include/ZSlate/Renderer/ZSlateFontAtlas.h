#pragma once

// ============================================================================
// ZSlate/Renderer/ZSlateFontAtlas.h
//
// Dynamic (lazily rasterised) glyph atlas.
//
//  * LoadFromFile() accepts .ttf / .otf / .ttc and registers the face
//    privately with GDI; glyphs are rasterised on demand so CJK fonts do not
//    have to be pre-baked.
//  * AddFallbackFont() registers a secondary face used whenever the primary
//    face has no glyph for a code point (this is how CJK text works).
//  * The atlas owns an RGBA8 bitmap; whenever new glyphs land, IsDirty()
//    turns true and the D3D11 backend re-uploads the texture.
// ============================================================================

#include "../Core/ZSlateTypes.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ZSlate
{

class ZSlateFontAtlas
{
public:
    struct FGlyph
    {
        bool  Valid    = false;
        // UV rectangle inside the atlas (0..1).
        float U0 = 0.0f, V0 = 0.0f, U1 = 0.0f, V1 = 0.0f;
        // Metrics in "em" units: multiply by the font size in pixels.
        float Width    = 0.0f;
        float Height   = 0.0f;
        float BearingX = 0.0f;
        float BearingY = 0.0f;
        float Advance  = 0.0f;
    };

    ZSlateFontAtlas();
    ~ZSlateFontAtlas();

    ZSlateFontAtlas(const ZSlateFontAtlas&)            = delete;
    ZSlateFontAtlas& operator=(const ZSlateFontAtlas&) = delete;

    bool LoadFromFile(const char* path);
    bool AddFallbackFont(const char* path);
    bool IsLoaded() const;

    // Lazily rasterises `codepoint` if needed. Never returns nullptr; an
    // invalid glyph (blank box) is returned when nothing can render it.
    const FGlyph* GetGlyph(uint32_t codepoint);

    // Vertical metrics in em units.
    float GetAscent()     const;
    float GetDescent()    const;
    float GetLineHeight() const;

    float   MeasureLine(const std::string& utf8, float fontSize);
    Vector2 Measure(const std::string& utf8, float fontSize);

    // Atlas bitmap (RGB = white, A = coverage).
    const uint8_t* GetPixels() const;
    int  GetWidth()  const;
    int  GetHeight() const;
    bool IsDirty()   const;
    void ClearDirty();
    uint32_t GetVersion() const;

private:
    struct Impl;
    Impl* m_Impl = nullptr;
};

} // namespace ZSlate
