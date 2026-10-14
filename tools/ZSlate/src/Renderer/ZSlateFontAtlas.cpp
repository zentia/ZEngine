#include "ZSlate/Renderer/ZSlateFontAtlas.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace ZSlate
{

namespace
{
    constexpr int ATLAS_WIDTH  = 2048;
    constexpr int ATLAS_HEIGHT = 2048;
    constexpr int EM_PIXELS    = 48;      // rasterisation size
    constexpr int GLYPH_PAD    = 1;

    uint32_t ReadU32(const uint8_t* p)
    {
        return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16)
             | (static_cast<uint32_t>(p[2]) << 8)  |  static_cast<uint32_t>(p[3]);
    }

    uint16_t ReadU16(const uint8_t* p)
    {
        return static_cast<uint16_t>((static_cast<uint32_t>(p[0]) << 8)
                                    | static_cast<uint32_t>(p[1]));
    }

    bool ReadFileBytes(const char* path, std::vector<uint8_t>& out)
    {
        FILE* file = nullptr;
        if (fopen_s(&file, path, "rb") != 0 || file == nullptr) return false;
        fseek(file, 0, SEEK_END);
        const long size = ftell(file);
        fseek(file, 0, SEEK_SET);
        if (size <= 0) { fclose(file); return false; }
        out.resize(static_cast<size_t>(size));
        const size_t read = fread(out.data(), 1, out.size(), file);
        fclose(file);
        return read == out.size();
    }

    std::wstring Utf8ToWide(const std::string& utf8)
    {
        if (utf8.empty()) return {};
        const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
        if (needed <= 0) return {};
        std::wstring wide(static_cast<size_t>(needed), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, wide.data(), needed);
        if (!wide.empty() && wide.back() == L'\0') wide.pop_back();
        return wide;
    }

    // Pulls family name (nameID 1) out of a sfnt/ttcf file.
    std::string ExtractFamilyName(const std::vector<uint8_t>& data, size_t base)
    {
        if (data.size() < base + 12) return {};

        const uint32_t sfnt = ReadU32(data.data() + base);

        if (sfnt == 0x74746366)   // 'ttcf' — TrueType collection
        {
            if (data.size() < base + 16) return {};
            const uint32_t numFonts = ReadU32(data.data() + base + 8);
            if (numFonts == 0) return {};
            const uint32_t firstOffset = ReadU32(data.data() + base + 12);
            return ExtractFamilyName(data, firstOffset);
        }

        const uint16_t numTables = ReadU16(data.data() + base + 4);
        const size_t   tableDir  = base + 12;

        uint32_t nameOffset = 0;
        uint32_t nameLength = 0;
        for (uint16_t i = 0; i < numTables; ++i)
        {
            const size_t record = tableDir + static_cast<size_t>(i) * 16;
            if (record + 16 > data.size()) break;
            if (ReadU32(data.data() + record) == 0x6E616D65)   // 'name'
            {
                nameOffset = ReadU32(data.data() + record + 8);
                nameLength = ReadU32(data.data() + record + 12);
                break;
            }
        }

        if (nameOffset == 0 || nameOffset + nameLength > data.size()) return {};

        const uint8_t* nameTable = data.data() + nameOffset;
        const uint16_t count        = ReadU16(nameTable + 2);
        const uint16_t stringOffset = ReadU16(nameTable + 4);

        for (uint16_t i = 0; i < count; ++i)
        {
            const uint8_t* record = nameTable + 12 + static_cast<size_t>(i) * 12;
            const uint16_t platformId = ReadU16(record);
            const uint16_t encodingId = ReadU16(record + 2);
            const uint16_t nameId     = ReadU16(record + 6);
            const uint16_t length     = ReadU16(record + 8);
            const uint16_t offset     = ReadU16(record + 10);

            if (nameId != 1) continue;

            const size_t start = static_cast<size_t>(stringOffset) + offset;
            if (start + length > data.size()) continue;

            if (platformId == 3 && (encodingId == 1 || encodingId == 0))
            {
                std::wstring wide;
                for (uint16_t k = 0; k + 1 < length; k += 2)
                {
                    wide.push_back(static_cast<wchar_t>(ReadU16(nameTable + start + k)));
                }
                const int needed = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                                                       static_cast<int>(wide.size()),
                                                       nullptr, 0, nullptr, nullptr);
                std::string utf8(static_cast<size_t>(needed), '\0');
                WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                                    utf8.data(), needed, nullptr, nullptr);
                return utf8;
            }

            if (platformId == 1)
            {
                return std::string(reinterpret_cast<const char*>(nameTable + start), length);
            }
        }

        return {};
    }
}

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

struct ZSlateFontAtlas::Impl
{
    struct FFace
    {
        std::wstring Name;
        HFONT        Handle = nullptr;
    };

    std::vector<uint8_t> Pixels;
    std::vector<FFace>   Faces;
    std::unordered_map<uint32_t, FGlyph> Glyphs;

    HDC   Hdc       = nullptr;
    bool  Loaded    = false;
    bool  Dirty     = false;
    uint32_t Version = 1;

    int CursorX   = GLYPH_PAD;
    int CursorY   = GLYPH_PAD;
    int RowHeight = 0;

    float Ascent  = 0.95f;
    float Descent = 0.25f;

    Impl()
    {
        Pixels.assign(static_cast<size_t>(ATLAS_WIDTH) * ATLAS_HEIGHT * 4, 0u);
        Hdc = CreateCompatibleDC(nullptr);
        SetMapMode(Hdc, MM_TEXT);
    }

    ~Impl()
    {
        for (FFace& face : Faces)
        {
            if (face.Handle) DeleteObject(face.Handle);
        }
        if (Hdc) DeleteDC(Hdc);
    }

    bool AddFace(const char* path)
    {
        if (!path || !*path || !Hdc) return false;

        std::vector<uint8_t> data;
        if (!ReadFileBytes(path, data)) return false;

        const std::string family = ExtractFamilyName(data, 0);
        if (family.empty()) return false;

        // Make the face available to this process only.
        AddFontResourceExA(path, FR_PRIVATE, 0);

        const std::wstring wideFamily = Utf8ToWide(family);
        if (wideFamily.empty()) return false;

        HFONT font = CreateFontW(-EM_PIXELS, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                                 wideFamily.c_str());
        if (!font) return false;

        const bool isFirst = Faces.empty();
        Faces.push_back({ std::move(wideFamily), font });

        if (isFirst)
        {
            HGDIOBJ previous = SelectObject(Hdc, font);
            TEXTMETRICW metrics {};
            if (GetTextMetricsW(Hdc, &metrics) && metrics.tmHeight > 0)
            {
                Ascent  = static_cast<float>(metrics.tmAscent)  / static_cast<float>(EM_PIXELS);
                Descent = static_cast<float>(metrics.tmDescent) / static_cast<float>(EM_PIXELS);
            }
            SelectObject(Hdc, previous);
        }

        Loaded = true;
        return true;
    }

    bool Rasterize(uint32_t codepoint, FGlyph& outGlyph)
    {
        if (!Hdc) return false;

        // Identity transform: eM11/eM12/eM21/eM22 as 16.16 FIXED values.
        MAT2 transform = { { 0, 1 }, { 0, 0 }, { 0, 0 }, { 0, 1 } };

        for (const FFace& face : Faces)
        {
            HGDIOBJ previous = SelectObject(Hdc, face.Handle);

            GLYPHMETRICS metrics {};
            const DWORD required = GetGlyphOutlineW(Hdc, static_cast<UINT>(codepoint),
                                                    GGO_GRAY8_BITMAP, &metrics,
                                                    0, nullptr, &transform);
            if (required == GDI_ERROR || required == 0)
            {
                SelectObject(Hdc, previous);
                continue;
            }

            std::vector<BYTE> buffer(required);
            const DWORD result = GetGlyphOutlineW(Hdc, static_cast<UINT>(codepoint),
                                                  GGO_GRAY8_BITMAP, &metrics,
                                                  required, buffer.data(), &transform);
            SelectObject(Hdc, previous);

            if (result == GDI_ERROR) continue;

            const int bitmapW = static_cast<int>(metrics.gmBlackBoxX);
            const int bitmapH = static_cast<int>(metrics.gmBlackBoxY);

            outGlyph = FGlyph{};
            outGlyph.Valid    = true;
            outGlyph.Width    = static_cast<float>(bitmapW)    / static_cast<float>(EM_PIXELS);
            outGlyph.Height   = static_cast<float>(bitmapH)    / static_cast<float>(EM_PIXELS);
            outGlyph.BearingX = static_cast<float>(metrics.gmptGlyphOrigin.x) / static_cast<float>(EM_PIXELS);
            outGlyph.BearingY = static_cast<float>(metrics.gmptGlyphOrigin.y) / static_cast<float>(EM_PIXELS);
            outGlyph.Advance  = static_cast<float>(metrics.gmCellIncX)        / static_cast<float>(EM_PIXELS);

            if (bitmapW <= 0 || bitmapH <= 0) return true;   // whitespace glyph

            // ---- pack into the atlas -------------------------------------
            if (CursorX + bitmapW + GLYPH_PAD > ATLAS_WIDTH)
            {
                CursorX   = GLYPH_PAD;
                CursorY  += RowHeight + GLYPH_PAD;
                RowHeight = 0;
            }
            if (CursorY + bitmapH + GLYPH_PAD > ATLAS_HEIGHT)
            {
                // Atlas exhausted: keep the glyph but without a texture.
                return true;
            }

            const int stride = ((bitmapW + 3) / 4) * 4;   // GDI pads rows to DWORD
            for (int y = 0; y < bitmapH; ++y)
            {
                for (int x = 0; x < bitmapW; ++x)
                {
                    const int value = buffer[static_cast<size_t>(y) * stride + x];
                    int alpha = value * 255 / 64;         // GGO_GRAY8 => 64 levels
                    if (alpha > 255) alpha = 255;

                    const size_t index = (static_cast<size_t>(CursorY + y) * ATLAS_WIDTH
                                        + static_cast<size_t>(CursorX + x)) * 4;
                    Pixels[index + 0] = 255;
                    Pixels[index + 1] = 255;
                    Pixels[index + 2] = 255;
                    Pixels[index + 3] = static_cast<uint8_t>(alpha);
                }
            }

            outGlyph.U0 = static_cast<float>(CursorX)                / static_cast<float>(ATLAS_WIDTH);
            outGlyph.V0 = static_cast<float>(CursorY)                / static_cast<float>(ATLAS_HEIGHT);
            outGlyph.U1 = static_cast<float>(CursorX + bitmapW)      / static_cast<float>(ATLAS_WIDTH);
            outGlyph.V1 = static_cast<float>(CursorY + bitmapH)      / static_cast<float>(ATLAS_HEIGHT);

            CursorX  += bitmapW + GLYPH_PAD;
            RowHeight = std::max(RowHeight, bitmapH);

            return true;
        }

        return false;
    }
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

ZSlateFontAtlas::ZSlateFontAtlas()
    : m_Impl(new Impl())
{
}

ZSlateFontAtlas::~ZSlateFontAtlas()
{
    delete m_Impl;
    m_Impl = nullptr;
}

bool ZSlateFontAtlas::LoadFromFile(const char* path)
{
    return m_Impl->AddFace(path);
}

bool ZSlateFontAtlas::AddFallbackFont(const char* path)
{
    return m_Impl->AddFace(path);
}

bool ZSlateFontAtlas::IsLoaded() const
{
    return m_Impl->Loaded;
}

const ZSlateFontAtlas::FGlyph* ZSlateFontAtlas::GetGlyph(uint32_t codepoint)
{
    auto it = m_Impl->Glyphs.find(codepoint);
    if (it != m_Impl->Glyphs.end()) return &it->second;

    FGlyph glyph;
    if (m_Impl->Rasterize(codepoint, glyph))
    {
        glyph.Valid = true;
    }
    else
    {
        // Nothing could render it — keep the layout moving.
        glyph = FGlyph{};
        glyph.Valid   = false;
        glyph.Advance = 0.5f;
    }

    m_Impl->Glyphs[codepoint] = glyph;
    m_Impl->Dirty   = true;
    ++m_Impl->Version;
    return &m_Impl->Glyphs[codepoint];
}

float ZSlateFontAtlas::GetAscent() const   { return m_Impl->Ascent; }
float ZSlateFontAtlas::GetDescent() const  { return m_Impl->Descent; }
float ZSlateFontAtlas::GetLineHeight() const { return m_Impl->Ascent + m_Impl->Descent; }

float ZSlateFontAtlas::MeasureLine(const std::string& utf8, float fontSize)
{
    float width = 0.0f;
    size_t offset = 0;
    while (offset < utf8.size())
    {
        uint32_t codepoint = 0;
        if (Utf8::Decode(utf8, offset, codepoint) == 0) break;
        if (codepoint == '\n') break;
        const FGlyph* glyph = GetGlyph(codepoint);
        if (glyph) width += glyph->Advance * fontSize;
    }
    return width;
}

Vector2 ZSlateFontAtlas::Measure(const std::string& utf8, float fontSize)
{
    float widest  = 0.0f;
    float current = 0.0f;
    int   lines   = 1;

    size_t offset = 0;
    while (offset < utf8.size())
    {
        uint32_t codepoint = 0;
        if (Utf8::Decode(utf8, offset, codepoint) == 0) break;
        if (codepoint == '\n')
        {
            widest  = std::max(widest, current);
            current = 0.0f;
            ++lines;
            continue;
        }
        const FGlyph* glyph = GetGlyph(codepoint);
        if (glyph) current += glyph->Advance * fontSize;
    }

    widest = std::max(widest, current);
    return Vector2(widest, GetLineHeight() * fontSize * static_cast<float>(lines));
}

const uint8_t* ZSlateFontAtlas::GetPixels() const { return m_Impl->Pixels.data(); }
int  ZSlateFontAtlas::GetWidth()  const { return ATLAS_WIDTH; }
int  ZSlateFontAtlas::GetHeight() const { return ATLAS_HEIGHT; }
bool ZSlateFontAtlas::IsDirty()   const { return m_Impl->Dirty; }
void ZSlateFontAtlas::ClearDirty()      { m_Impl->Dirty = false; }
uint32_t ZSlateFontAtlas::GetVersion() const { return m_Impl->Version; }

} // namespace ZSlate
