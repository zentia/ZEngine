#include "Runtime/UI/Render/ZFontAtlas.h"

#include "core/Log/LogSystem.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

// Compile a file-local (static) copy of stb_truetype. STBTT_STATIC makes every
// stbtt_* symbol internal-linkage, so this stays self-contained. The header is
// vendored locally (zstb_truetype.h, a copy of stb_truetype 1.26) so ZRuntime
// no longer depends on ImGui's include directory now that the engine is
// ImGui-free.
#if defined(_MSC_VER)
    #pragma warning(push)
    // stb_truetype is C; with STBTT_STATIC its unused helpers trip C4505 and the
    // rasterizer trips the usual narrowing/shadow set. None are actionable here.
    #pragma warning(disable : 4244 4456 4457 4505 4701 4702)
#endif
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "zstb_truetype.h"
#if defined(_MSC_VER)
    #pragma warning(pop)
#endif

namespace
{
    // Fixed atlas dimensions. 2048x2048 RGBA = 16 MB CPU + 16 MB GPU. At ~18px
    // glyphs that holds well over 5000 distinct glyphs -- comfortably more than
    // the editor's Latin + working-set CJK at any instant.
    constexpr uint32_t kAtlasDim = 2048;

    uint64_t MakeKey(unsigned int codepoint, int rounded_size)
    {
        return (static_cast<uint64_t>(static_cast<uint32_t>(rounded_size)) << 32) |
               static_cast<uint64_t>(codepoint);
    }

    // Per-font metrics needed to bake a glyph from that specific font face.
    struct FallbackFont
    {
        std::vector<uint8_t> fontData;  // TTF bytes; must outlive fontInfo
        stbtt_fontinfo* fontInfo {nullptr};
        int ascentUnits {0};
        int descentUnits {0};
        int lineGapUnits {0};
    };
}  // namespace

ZFontAtlas::~ZFontAtlas()
{
    if (m_FontInfo != nullptr)
    {
        delete static_cast<stbtt_fontinfo*>(m_FontInfo);
        m_FontInfo = nullptr;
    }

    // Free fallback fonts (opaque vector<FallbackFont> allocated in AddFallbackFont).
    if (m_FallbackData != nullptr)
    {
        auto* fallbacks = static_cast<std::vector<FallbackFont>*>(m_FallbackData);
        for (FallbackFont& fb : *fallbacks)
        {
            delete fb.fontInfo;
        }
        delete fallbacks;
        m_FallbackData = nullptr;
    }
}

bool ZFontAtlas::LoadFromFile(const std::string& ttf_path)
{
    if (m_Loaded)
    {
        return true;
    }
    if (ttf_path.empty())
    {
        return false;
    }

    std::FILE* fp = nullptr;
#if defined(_WIN32)
    fopen_s(&fp, ttf_path.c_str(), "rb");
#else
    fp = std::fopen(ttf_path.c_str(), "rb");
#endif
    if (fp == nullptr)
    {
        LOG_WARNING(ZRender, "ZFontAtlas: cannot open font '{}'", ttf_path.c_str());
        return false;
    }

    std::fseek(fp, 0, SEEK_END);
    const long size = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    if (size <= 0)
    {
        std::fclose(fp);
        LOG_WARNING(ZRender, "ZFontAtlas: empty font '{}'", ttf_path.c_str());
        return false;
    }

    m_FontData.resize(static_cast<size_t>(size));
    const size_t read = std::fread(m_FontData.data(), 1, static_cast<size_t>(size), fp);
    std::fclose(fp);
    if (read != static_cast<size_t>(size))
    {
        m_FontData.clear();
        LOG_WARNING(ZRender, "ZFontAtlas: short read on font '{}'", ttf_path.c_str());
        return false;
    }

    auto* info = new stbtt_fontinfo();
    const int offset = stbtt_GetFontOffsetForIndex(m_FontData.data(), 0);
    if (offset < 0 || stbtt_InitFont(info, m_FontData.data(), offset) == 0)
    {
        delete info;
        m_FontData.clear();
        LOG_WARNING(ZRender, "ZFontAtlas: stbtt_InitFont failed for '{}'", ttf_path.c_str());
        return false;
    }

    stbtt_GetFontVMetrics(info, &m_AscentUnits, &m_DescentUnits, &m_LineGapUnits);

    m_FontInfo = info;
    m_Width = kAtlasDim;
    m_Height = kAtlasDim;
    m_Pixels.assign(static_cast<size_t>(m_Width) * m_Height * 4u, 0u);
    m_PenX = 1;
    m_PenY = 1;
    m_RowHeight = 0;
    m_Dirty = true;
    m_Loaded = true;

    LOG_INFO(ZRender, "ZFontAtlas: loaded native font '{}' ({} bytes)", ttf_path.c_str(), size);
    return true;
}

bool ZFontAtlas::AddFallbackFont(const std::string& ttf_path)
{
    if (!m_Loaded)
    {
        LOG_WARNING(ZRender, "ZFontAtlas: AddFallbackFont called before LoadFromFile");
        return false;
    }
    if (ttf_path.empty())
    {
        return false;
    }

    std::FILE* fp = nullptr;
#if defined(_WIN32)
    fopen_s(&fp, ttf_path.c_str(), "rb");
#else
    fp = std::fopen(ttf_path.c_str(), "rb");
#endif
    if (fp == nullptr)
    {
        LOG_WARNING(ZRender, "ZFontAtlas: cannot open fallback font '{}'", ttf_path.c_str());
        return false;
    }

    std::fseek(fp, 0, SEEK_END);
    const long size = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    if (size <= 0)
    {
        std::fclose(fp);
        LOG_WARNING(ZRender, "ZFontAtlas: empty fallback font '{}'", ttf_path.c_str());
        return false;
    }

    std::vector<uint8_t> fontData(static_cast<size_t>(size));
    const size_t read = std::fread(fontData.data(), 1, static_cast<size_t>(size), fp);
    std::fclose(fp);
    if (read != static_cast<size_t>(size))
    {
        LOG_WARNING(ZRender, "ZFontAtlas: short read on fallback font '{}'", ttf_path.c_str());
        return false;
    }

    auto* info = new stbtt_fontinfo();
    const int offset = stbtt_GetFontOffsetForIndex(fontData.data(), 0);
    if (offset < 0 || stbtt_InitFont(info, fontData.data(), offset) == 0)
    {
        delete info;
        LOG_WARNING(ZRender, "ZFontAtlas: stbtt_InitFont failed for fallback '{}'", ttf_path.c_str());
        return false;
    }

    int ascent = 0, descent = 0, lineGap = 0;
    stbtt_GetFontVMetrics(info, &ascent, &descent, &lineGap);

    if (m_FallbackData == nullptr)
    {
        m_FallbackData = new std::vector<FallbackFont>();
    }
    auto* fallbacks = static_cast<std::vector<FallbackFont>*>(m_FallbackData);
    fallbacks->emplace_back();
    FallbackFont& fb = fallbacks->back();
    fb.fontData = std::move(fontData);
    fb.fontInfo = info;
    fb.ascentUnits = ascent;
    fb.descentUnits = descent;
    fb.lineGapUnits = lineGap;

    LOG_INFO(ZRender, "ZFontAtlas: loaded fallback font '{}' ({} bytes)", ttf_path.c_str(), size);
    return true;
}

float ZFontAtlas::ScaleForSize(float pixel_size) const
{
    if (m_FontInfo == nullptr || pixel_size <= 0.0f)
    {
        return 0.0f;
    }
    return stbtt_ScaleForPixelHeight(static_cast<stbtt_fontinfo*>(m_FontInfo), pixel_size);
}

float ZFontAtlas::GetAscent(float pixel_size) const
{
    return static_cast<float>(m_AscentUnits) * ScaleForSize(pixel_size);
}

float ZFontAtlas::GetLineHeight(float pixel_size) const
{
    // descent is negative in font units; (ascent - descent + line_gap) * scale.
    const float units = static_cast<float>(m_AscentUnits - m_DescentUnits + m_LineGapUnits);
    return units * ScaleForSize(pixel_size);
}

bool ZFontAtlas::PackRect(uint32_t w, uint32_t h, uint32_t& out_x, uint32_t& out_y)
{
    if (w == 0 || h == 0 || w > m_Width || h > m_Height)
    {
        return false;
    }
    if (m_PenX + w > m_Width)
    {
        // Wrap to the next shelf.
        m_PenX = 1;
        m_PenY += m_RowHeight + 1;
        m_RowHeight = 0;
    }
    if (m_PenY + h > m_Height)
    {
        return false;  // atlas full
    }
    out_x = m_PenX;
    out_y = m_PenY;
    m_PenX += w + 1;
    m_RowHeight = std::max(m_RowHeight, h);
    return true;
}

const ZFontAtlas::Glyph& ZFontAtlas::GetGlyph(unsigned int codepoint, float pixel_size)
{
    if (!m_Loaded || pixel_size <= 0.0f)
    {
        return m_Invalid;
    }

    const int rounded = static_cast<int>(pixel_size + 0.5f);
    const uint64_t key = MakeKey(codepoint, rounded);
    const auto found = m_Glyphs.find(key);
    if (found != m_Glyphs.end())
    {
        return found->second;
    }

    // Find which font (primary or fallback) contains this codepoint.
    // stbtt_FindGlyphIndex returns 0 when the glyph is not in the font's cmap
    // (glyph index 0 is .notdef, which we treat as "not present" for fallback
    // purposes -- codepoint 0 never appears in real text).
    stbtt_fontinfo* bake_info = nullptr;
    int bake_ascent = 0;
    int bake_descent = 0;
    int bake_line_gap = 0;

    // Try primary font first.
    auto* primary_info = static_cast<stbtt_fontinfo*>(m_FontInfo);
    if (primary_info != nullptr && stbtt_FindGlyphIndex(primary_info, static_cast<int>(codepoint)) != 0)
    {
        bake_info = primary_info;
        bake_ascent = m_AscentUnits;
        bake_descent = m_DescentUnits;
        bake_line_gap = m_LineGapUnits;
    }

    // Try fallback fonts (e.g. CJK font when primary is Latin-only).
    if (bake_info == nullptr && m_FallbackData != nullptr)
    {
        auto* fallbacks = static_cast<std::vector<FallbackFont>*>(m_FallbackData);
        for (FallbackFont& fb : *fallbacks)
        {
            if (stbtt_FindGlyphIndex(fb.fontInfo, static_cast<int>(codepoint)) != 0)
            {
                bake_info = fb.fontInfo;
                bake_ascent = fb.ascentUnits;
                bake_descent = fb.descentUnits;
                bake_line_gap = fb.lineGapUnits;
                break;
            }
        }
    }

    if (bake_info == nullptr)
    {
        // Codepoint not found in any font; return invalid (renders as blank).
        const auto inserted = m_Glyphs.emplace(key, m_Invalid);
        return inserted.first->second;
    }

    // Bake the glyph from the font that actually contains it.
    const float scale = stbtt_ScaleForPixelHeight(bake_info, static_cast<float>(rounded));
    const float ascent = static_cast<float>(bake_ascent) * scale;

    int advance_units = 0;
    int left_bearing = 0;
    stbtt_GetCodepointHMetrics(bake_info, static_cast<int>(codepoint), &advance_units, &left_bearing);

    Glyph g {};
    g.advance = static_cast<float>(advance_units) * scale;

    int ix0 = 0, iy0 = 0, ix1 = 0, iy1 = 0;
    stbtt_GetCodepointBitmapBox(bake_info, static_cast<int>(codepoint), scale, scale, &ix0, &iy0, &ix1, &iy1);

    const int gw = ix1 - ix0;
    const int gh = iy1 - iy0;

    if (gw <= 0 || gh <= 0)
    {
        // Whitespace / non-rendering glyph: valid advance, empty quad.
        g.valid = true;
        const auto inserted = m_Glyphs.emplace(key, g);
        return inserted.first->second;
    }

    uint32_t px = 0;
    uint32_t py = 0;
    if (!PackRect(static_cast<uint32_t>(gw), static_cast<uint32_t>(gh), px, py))
    {
        if (!m_AtlasFullWarned)
        {
            LOG_WARNING(ZRender, "ZFontAtlas: glyph atlas full ({}x{}); further glyphs render blank",
                        m_Width, m_Height);
            m_AtlasFullWarned = true;
        }
        const auto inserted = m_Glyphs.emplace(key, m_Invalid);
        return inserted.first->second;
    }

    // Rasterize coverage into a scratch buffer, then expand into the RGBA atlas as
    // (255,255,255, coverage) so color modulation in the UI shader tints text.
    std::vector<uint8_t> coverage(static_cast<size_t>(gw) * gh, 0u);
    stbtt_MakeCodepointBitmap(bake_info, coverage.data(), gw, gh, gw, scale, scale, static_cast<int>(codepoint));

    for (int row = 0; row < gh; ++row)
    {
        const uint8_t* src = coverage.data() + static_cast<size_t>(row) * gw;
        uint8_t* dst = m_Pixels.data() + (static_cast<size_t>(py + row) * m_Width + px) * 4u;
        for (int col = 0; col < gw; ++col)
        {
            dst[0] = 255;
            dst[1] = 255;
            dst[2] = 255;
            dst[3] = src[col];
            dst += 4;
        }
    }

    const float inv_w = 1.0f / static_cast<float>(m_Width);
    const float inv_h = 1.0f / static_cast<float>(m_Height);
    g.u0 = static_cast<float>(px) * inv_w;
    g.v0 = static_cast<float>(py) * inv_h;
    g.u1 = static_cast<float>(px + gw) * inv_w;
    g.v1 = static_cast<float>(py + gh) * inv_h;

    g.x0 = static_cast<float>(ix0);
    g.y0 = ascent + static_cast<float>(iy0);
    g.x1 = static_cast<float>(ix0 + gw);
    g.y1 = ascent + static_cast<float>(iy0 + gh);
    g.valid = true;

    m_Dirty = true;
    const auto inserted = m_Glyphs.emplace(key, g);
    return inserted.first->second;
}
