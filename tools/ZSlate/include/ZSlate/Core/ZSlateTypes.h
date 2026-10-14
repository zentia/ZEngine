#pragma once

// ============================================================================
// ZSlate/Core/ZSlateTypes.h
//
// Fundamental value types shared by every ZSlate module.
// ============================================================================

#include <cstdint>
#include <cmath>
#include <string>
#include <algorithm>

namespace ZSlate
{

// ---------------------------------------------------------------------------
// Vector2
// ---------------------------------------------------------------------------
struct Vector2
{
    float x = 0.0f;
    float y = 0.0f;

    constexpr Vector2() = default;
    constexpr Vector2(float inX, float inY) : x(inX), y(inY) {}

    constexpr Vector2 operator+(const Vector2& o) const { return Vector2(x + o.x, y + o.y); }
    constexpr Vector2 operator-(const Vector2& o) const { return Vector2(x - o.x, y - o.y); }
    constexpr Vector2 operator*(float s)          const { return Vector2(x * s, y * s); }

    Vector2& operator+=(const Vector2& o) { x += o.x; y += o.y; return *this; }
    Vector2& operator-=(const Vector2& o) { x -= o.x; y -= o.y; return *this; }
};

inline float MaxComponent(const Vector2& v) { return std::max(v.x, v.y); }

// ---------------------------------------------------------------------------
// UIColor — linear-ish RGBA in 0..1
// ---------------------------------------------------------------------------
struct UIColor
{
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    float a = 1.0f;

    constexpr UIColor() = default;
    constexpr UIColor(float inR, float inG, float inB, float inA = 1.0f)
        : r(inR), g(inG), b(inB), a(inA) {}
};

// Commonly used palette entries.
namespace Colors
{
    inline constexpr UIColor White      { 1.00f, 1.00f, 1.00f, 1.00f };
    inline constexpr UIColor Black      { 0.00f, 0.00f, 0.00f, 1.00f };
    inline constexpr UIColor Transparent{ 0.00f, 0.00f, 0.00f, 0.00f };
    inline constexpr UIColor Gray       { 0.50f, 0.50f, 0.50f, 1.00f };
    inline constexpr UIColor DarkGray   { 0.20f, 0.20f, 0.22f, 1.00f };
    inline constexpr UIColor Red        { 0.90f, 0.25f, 0.25f, 1.00f };
    inline constexpr UIColor Green      { 0.30f, 0.80f, 0.35f, 1.00f };
    inline constexpr UIColor Blue       { 0.30f, 0.55f, 0.95f, 1.00f };
    inline constexpr UIColor Yellow     { 0.95f, 0.80f, 0.20f, 1.00f };
}

// ---------------------------------------------------------------------------
// UIRect
// ---------------------------------------------------------------------------
struct UIRect
{
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;

    constexpr UIRect() = default;
    constexpr UIRect(float inX, float inY, float inW, float inH)
        : x(inX), y(inY), w(inW), h(inH) {}

    constexpr float Right()   const { return x + w; }
    constexpr float Bottom()  const { return y + h; }

    bool Contains(const Vector2& p) const
    {
        return p.x >= x && p.x < Right() && p.y >= y && p.y < Bottom();
    }

    bool Intersects(const UIRect& o) const
    {
        return !(o.Right() <= x || o.x >= Right() || o.Bottom() <= y || o.y >= Bottom());
    }

    UIRect InsetBy(float amount) const
    {
        return UIRect(x + amount, y + amount,
                      std::max(0.0f, w - amount * 2.0f),
                      std::max(0.0f, h - amount * 2.0f));
    }
};

// ---------------------------------------------------------------------------
// Enums
// ---------------------------------------------------------------------------
enum class EOrientation
{
    Vertical,
    Horizontal
};

enum class TextAnchor
{
    TopLeft,
    MiddleLeft,
    MiddleCenter
};

enum class TextWrapMode
{
    NoWrap,
    Wrap
};

// ---------------------------------------------------------------------------
// UTF-8 helpers (header only, shared by text widgets and the renderer)
// ---------------------------------------------------------------------------
namespace Utf8
{
    // Decodes one code point. Returns the number of bytes consumed.
    inline int Decode(const std::string& s, size_t& i, uint32_t& outCp)
    {
        if (i >= s.size()) { outCp = 0; return 0; }
        const unsigned char c0 = static_cast<unsigned char>(s[i]);
        if (c0 < 0x80)       { outCp = c0;                                   i += 1; return 1; }
        if ((c0 & 0xE0) == 0xC0 && i + 1 < s.size())
        {
            outCp = ((c0 & 0x1Fu) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3Fu);
            i += 2; return 2;
        }
        if ((c0 & 0xF0) == 0xE0 && i + 2 < s.size())
        {
            outCp = ((c0 & 0x0Fu) << 12) | ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 6)
                  |  (static_cast<unsigned char>(s[i + 2]) & 0x3Fu);
            i += 3; return 3;
        }
        if ((c0 & 0xF8) == 0xF0 && i + 3 < s.size())
        {
            outCp = ((c0 & 0x07u) << 18) | ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 12)
                  | ((static_cast<unsigned char>(s[i + 2]) & 0x3Fu) << 6)
                  |  (static_cast<unsigned char>(s[i + 3]) & 0x3Fu);
            i += 4; return 4;
        }
        outCp = c0; i += 1; return 1;
    }

    inline void Append(std::string& s, uint32_t cp)
    {
        if (cp < 0x80)
        {
            s += static_cast<char>(cp);
        }
        else if (cp < 0x800)
        {
            s += static_cast<char>(0xC0 | (cp >> 6));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        }
        else if (cp < 0x10000)
        {
            s += static_cast<char>(0xE0 | (cp >> 12));
            s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        }
        else
        {
            s += static_cast<char>(0xF0 | (cp >> 18));
            s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    // Number of code points in a UTF-8 string.
    inline size_t Length(const std::string& s)
    {
        size_t n = 0;
        for (size_t i = 0; i < s.size();)
        {
            uint32_t cp = 0;
            if (Decode(s, i, cp) == 0) break;
            ++n;
        }
        return n;
    }

    // Returns the code point at code-point index `cpIndex`.
    inline uint32_t CodePointAt(const std::string& s, size_t cpIndex)
    {
        size_t idx = 0;
        for (size_t i = 0; i < s.size();)
        {
            uint32_t cp = 0;
            if (Decode(s, i, cp) == 0) break;
            if (idx == cpIndex) return cp;
            ++idx;
        }
        return 0;
    }

    // Byte offset of code-point index `cpIndex`.
    inline size_t ByteOffsetOf(const std::string& s, size_t cpIndex)
    {
        size_t idx = 0;
        size_t off = 0;
        while (off < s.size())
        {
            if (idx == cpIndex) return off;
            uint32_t cp = 0;
            if (Decode(s, off, cp) == 0) break;
            ++idx;
        }
        return s.size();
    }
}

} // namespace ZSlate
