#include "ZSlate/Renderer/ZSlateFontAtlas.h"

#include "ZSlate/Core/SlateLog.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

// Zero-dependency, but no longer hard-wired to stderr: every line goes through
// ZSlate's process-wide sink, so a host that owns a real logging stack (the
// engine's bqlog / 扁鹊, say) receives these too. Nothing changes for a host
// that installs nothing -- the default sink is the same fprintf pair.
#define ZSFONT_WARN(fmt, ...) ZSlate::Log(ZSlate::ELogLevel::Warning, "[ZSlateFontAtlas] " fmt, ##__VA_ARGS__)
#define ZSFONT_INFO(fmt, ...) ZSlate::Log(ZSlate::ELogLevel::Info, "[ZSlateFontAtlas] " fmt, ##__VA_ARGS__)

// File-local stb_truetype.
#if defined(_MSC_VER)
    #pragma warning(push)
    #pragma warning(disable : 4244 4456 4457 4505 4701 4702)
#endif
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "zstb_truetype.h"
#if defined(_MSC_VER)
    #pragma warning(pop)
#endif

namespace ZSlate
{

namespace
{
    constexpr uint32_t kAtlasDim = 2048;

    uint64_t MakeKey(unsigned int codepoint, int rounded_size)
    {
        return (static_cast<uint64_t>(static_cast<uint32_t>(rounded_size)) << 32) |
               static_cast<uint64_t>(codepoint);
    }

    struct FallbackFont
    {
        std::vector<uint8_t> fontData;
        stbtt_fontinfo* fontInfo {nullptr};
        int ascentUnits {0};
        int descentUnits {0};
        int lineGapUnits {0};
    };
}

ZSlateFontAtlas::~ZSlateFontAtlas()
{
    if (m_FontInfo) { delete static_cast<stbtt_fontinfo*>(m_FontInfo); m_FontInfo = nullptr; }
    if (m_FallbackData)
    {
        auto* fbs = static_cast<std::vector<FallbackFont>*>(m_FallbackData);
        for (auto& fb : *fbs) delete fb.fontInfo;
        delete fbs;
        m_FallbackData = nullptr;
    }
}

bool ZSlateFontAtlas::LoadFromFile(const std::string& ttf_path)
{
    if (m_Loaded) return true;
    if (ttf_path.empty()) return false;

    std::FILE* fp = nullptr;
#if defined(_WIN32)
    fopen_s(&fp, ttf_path.c_str(), "rb");
#else
    fp = std::fopen(ttf_path.c_str(), "rb");
#endif
    if (!fp) { ZSFONT_WARN("cannot open '%s'", ttf_path.c_str()); return false; }

    std::fseek(fp, 0, SEEK_END);
    const long size = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    if (size <= 0) { std::fclose(fp); ZSFONT_WARN("empty font '%s'", ttf_path.c_str()); return false; }

    m_FontData.resize(static_cast<size_t>(size));
    const size_t read = std::fread(m_FontData.data(), 1, static_cast<size_t>(size), fp);
    std::fclose(fp);
    if (read != static_cast<size_t>(size))
    {
        m_FontData.clear();
        ZSFONT_WARN("short read on '%s'", ttf_path.c_str());
        return false;
    }

    auto* info = new stbtt_fontinfo();
    const int offset = stbtt_GetFontOffsetForIndex(m_FontData.data(), 0);
    if (offset < 0 || stbtt_InitFont(info, m_FontData.data(), offset) == 0)
    {
        delete info; m_FontData.clear();
        ZSFONT_WARN("stbtt_InitFont failed for '%s'", ttf_path.c_str());
        return false;
    }

    stbtt_GetFontVMetrics(info, &m_AscentUnits, &m_DescentUnits, &m_LineGapUnits);
    m_FontInfo = info;
    m_Width = m_Height = kAtlasDim;
    m_Pixels.assign(static_cast<size_t>(m_Width) * m_Height * 4u, 0u);
    m_PenX = 1; m_PenY = 1; m_RowHeight = 0;
    m_Dirty = true; m_Loaded = true;
    ZSFONT_INFO("loaded '%s' (%ld bytes)", ttf_path.c_str(), size);
    return true;
}

bool ZSlateFontAtlas::AddFallbackFont(const std::string& ttf_path)
{
    if (!m_Loaded) { ZSFONT_WARN("AddFallbackFont before LoadFromFile"); return false; }
    if (ttf_path.empty()) return false;

    std::FILE* fp = nullptr;
#if defined(_WIN32)
    fopen_s(&fp, ttf_path.c_str(), "rb");
#else
    fp = std::fopen(ttf_path.c_str(), "rb");
#endif
    if (!fp) { ZSFONT_WARN("cannot open fallback '%s'", ttf_path.c_str()); return false; }

    std::fseek(fp, 0, SEEK_END);
    const long size = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    if (size <= 0) { std::fclose(fp); ZSFONT_WARN("empty fallback '%s'", ttf_path.c_str()); return false; }

    std::vector<uint8_t> fontData(static_cast<size_t>(size));
    const size_t read = std::fread(fontData.data(), 1, static_cast<size_t>(size), fp);
    std::fclose(fp);
    if (read != static_cast<size_t>(size)) { ZSFONT_WARN("short read fallback '%s'", ttf_path.c_str()); return false; }

    auto* info = new stbtt_fontinfo();
    const int offset = stbtt_GetFontOffsetForIndex(fontData.data(), 0);
    if (offset < 0 || stbtt_InitFont(info, fontData.data(), offset) == 0)
    {
        delete info;
        ZSFONT_WARN("stbtt_InitFont failed for fallback '%s'", ttf_path.c_str());
        return false;
    }

    int asc = 0, desc = 0, lg = 0;
    stbtt_GetFontVMetrics(info, &asc, &desc, &lg);

    if (!m_FallbackData) m_FallbackData = new std::vector<FallbackFont>();
    auto* fbs = static_cast<std::vector<FallbackFont>*>(m_FallbackData);
    fbs->emplace_back();
    FallbackFont& fb = fbs->back();
    fb.fontData = std::move(fontData);
    fb.fontInfo = info;
    fb.ascentUnits = asc; fb.descentUnits = desc; fb.lineGapUnits = lg;

    ZSFONT_INFO("fallback '%s' (%ld bytes)", ttf_path.c_str(), size);
    return true;
}

float ZSlateFontAtlas::ScaleForSize(float pixel_size) const
{
    if (!m_FontInfo || pixel_size <= 0.0f) return 0.0f;
    return stbtt_ScaleForPixelHeight(static_cast<stbtt_fontinfo*>(m_FontInfo), pixel_size);
}

float ZSlateFontAtlas::GetAscent(float pixel_size) const
{
    return static_cast<float>(m_AscentUnits) * ScaleForSize(pixel_size);
}

float ZSlateFontAtlas::GetLineHeight(float pixel_size) const
{
    const float units = static_cast<float>(m_AscentUnits - m_DescentUnits + m_LineGapUnits);
    return units * ScaleForSize(pixel_size);
}

bool ZSlateFontAtlas::PackRect(uint32_t w, uint32_t h, uint32_t& out_x, uint32_t& out_y)
{
    if (w == 0 || h == 0 || w > m_Width || h > m_Height) return false;
    if (m_PenX + w > m_Width) { m_PenX = 1; m_PenY += m_RowHeight + 1; m_RowHeight = 0; }
    if (m_PenY + h > m_Height) return false;
    out_x = m_PenX; out_y = m_PenY;
    m_PenX += w + 1;
    m_RowHeight = std::max(m_RowHeight, h);
    return true;
}

const ZSlateFontAtlas::Glyph& ZSlateFontAtlas::GetGlyph(unsigned int codepoint, float pixel_size)
{
    if (!m_Loaded || pixel_size <= 0.0f) return m_Invalid;

    const int rounded = static_cast<int>(pixel_size + 0.5f);
    const uint64_t key = MakeKey(codepoint, rounded);
    auto found = m_Glyphs.find(key);
    if (found != m_Glyphs.end()) return found->second;

    stbtt_fontinfo* bake_info = nullptr;
    int bake_ascent = 0, bake_descent = 0, bake_line_gap = 0;

    auto* primary = static_cast<stbtt_fontinfo*>(m_FontInfo);
    if (primary && stbtt_FindGlyphIndex(primary, static_cast<int>(codepoint)) != 0)
    {
        bake_info = primary;
        bake_ascent = m_AscentUnits; bake_descent = m_DescentUnits; bake_line_gap = m_LineGapUnits;
    }
    if (!bake_info && m_FallbackData)
    {
        for (auto& fb : *static_cast<std::vector<FallbackFont>*>(m_FallbackData))
        {
            if (stbtt_FindGlyphIndex(fb.fontInfo, static_cast<int>(codepoint)) != 0)
            {
                bake_info = fb.fontInfo;
                bake_ascent = fb.ascentUnits; bake_descent = fb.descentUnits; bake_line_gap = fb.lineGapUnits;
                break;
            }
        }
    }
    if (!bake_info)
    {
        // Silent here used to make "the font loaded fine but text is still
        // boxes" undiagnosable from a user's log: nothing was ever recorded.
        if (m_MissingCodepoints.insert(codepoint).second)
        {
            if (m_MissingCodepoints.size() <= 16)
                ZSFONT_WARN("no font covers U+%04X (size %d)", codepoint, rounded);
            else if (m_MissingCodepoints.size() == 17)
                ZSFONT_WARN("further missing-codepoint warnings suppressed");
        }
        m_Glyphs.emplace(key, m_Invalid);
        return m_Invalid;
    }

    const float scale = stbtt_ScaleForPixelHeight(bake_info, static_cast<float>(rounded));
    int adv_units = 0, lb = 0;
    stbtt_GetCodepointHMetrics(bake_info, static_cast<int>(codepoint), &adv_units, &lb);

    Glyph g {};
    g.advance = static_cast<float>(adv_units) * scale;

    int ix0 = 0, iy0 = 0, ix1 = 0, iy1 = 0;
    stbtt_GetCodepointBitmapBox(bake_info, static_cast<int>(codepoint), scale, scale, &ix0, &iy0, &ix1, &iy1);
    const int gw = ix1 - ix0, gh = iy1 - iy0;

    if (gw <= 0 || gh <= 0) { g.valid = true; m_Glyphs.emplace(key, g); return m_Glyphs[key]; }

    uint32_t px = 0, py = 0;
    if (!PackRect(static_cast<uint32_t>(gw), static_cast<uint32_t>(gh), px, py))
    {
        if (!m_AtlasFullWarned) { ZSFONT_WARN("atlas full (%ux%u)", m_Width, m_Height); m_AtlasFullWarned = true; }
        m_Glyphs.emplace(key, m_Invalid); return m_Invalid;
    }

    std::vector<uint8_t> coverage(static_cast<size_t>(gw) * gh, 0u);
    stbtt_MakeCodepointBitmap(bake_info, coverage.data(), gw, gh, gw, scale, scale, static_cast<int>(codepoint));

    const float ascent = static_cast<float>(bake_ascent) * scale;
    for (int row = 0; row < gh; ++row)
    {
        const uint8_t* src = coverage.data() + static_cast<size_t>(row) * gw;
        uint8_t* dst = m_Pixels.data() + (static_cast<size_t>(py + row) * m_Width + px) * 4u;
        for (int col = 0; col < gw; ++col)
        {
            dst[0] = 255; dst[1] = 255; dst[2] = 255; dst[3] = src[col];
            dst += 4;
        }
    }

    const float invW = 1.0f / static_cast<float>(m_Width);
    const float invH = 1.0f / static_cast<float>(m_Height);
    g.u0 = static_cast<float>(px) * invW;         g.v0 = static_cast<float>(py) * invH;
    g.u1 = static_cast<float>(px + gw) * invW;     g.v1 = static_cast<float>(py + gh) * invH;
    g.x0 = static_cast<float>(ix0);                g.y0 = ascent + static_cast<float>(iy0);
    g.x1 = static_cast<float>(ix0 + gw);           g.y1 = ascent + static_cast<float>(iy0 + gh);
    g.valid = true;

    m_Dirty = true;
    m_Glyphs.emplace(key, g);
    return m_Glyphs[key];
}

bool ZSlateFontAtlas::ProbeHasCodepoint(const std::string& ttf_path, unsigned int codepoint)
{
    if (ttf_path.empty()) return false;

    std::FILE* fp = nullptr;
#if defined(_WIN32)
    fopen_s(&fp, ttf_path.c_str(), "rb");
#else
    fp = std::fopen(ttf_path.c_str(), "rb");
#endif
    if (!fp) return false;

    std::fseek(fp, 0, SEEK_END);
    const long size = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    if (size <= 0) { std::fclose(fp); return false; }

    std::vector<uint8_t> data(static_cast<size_t>(size));
    const size_t read = std::fread(data.data(), 1, static_cast<size_t>(size), fp);
    std::fclose(fp);
    if (read != static_cast<size_t>(size)) return false;

    stbtt_fontinfo info {};
    const int offset = stbtt_GetFontOffsetForIndex(data.data(), 0);
    if (offset < 0 || stbtt_InitFont(&info, data.data(), offset) == 0) return false;
    return stbtt_FindGlyphIndex(&info, static_cast<int>(codepoint)) != 0;
}

}  // namespace ZSlate
