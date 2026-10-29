#pragma once

// ZSlate: a standalone retained-mode (UE Slate-inspired) UI framework.
// This file defines the core math and type primitives ZSlate depends on,
// decoupled from any specific engine's math library.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdint>

namespace ZSlate
{
// ============================================================================
// Math Constants
// ============================================================================

static constexpr float Float_EPSILON = 1e-5f;

// ============================================================================
// Vector2 - 2D vector for positions and sizes
// ============================================================================

struct Vector2
{
    float x {0.0f};
    float y {0.0f};

    Vector2() = default;
    constexpr Vector2(float X, float Y) : x(X), y(Y) {}
    constexpr Vector2(float Value) : x(Value), y(Value) {}

    // Operators
    bool operator==(const Vector2& Other) const { return x == Other.x && y == Other.y; }
    bool operator!=(const Vector2& Other) const { return !(*this == Other); }
    bool operator<(const Vector2& Other) const { return x < Other.x && y < Other.y; }
    bool operator<=(const Vector2& Other) const { return x <= Other.x && y <= Other.y; }
    bool operator>(const Vector2& Other) const { return x > Other.x && y > Other.y; }
    bool operator>=(const Vector2& Other) const { return x >= Other.x && y >= Other.y; }

    Vector2 operator+(const Vector2& Other) const { return Vector2(x + Other.x, y + Other.y); }
    Vector2 operator-(const Vector2& Other) const { return Vector2(x - Other.x, y - Other.y); }
    Vector2 operator*(float S) const { return Vector2(x * S, y * S); }
    Vector2 operator/(float S) const { return Vector2(x / S, y / S); }
    Vector2 operator-() const { return Vector2(-x, -y); }

    Vector2& operator+=(const Vector2& Other) { x += Other.x; y += Other.y; return *this; }
    Vector2& operator-=(const Vector2& Other) { x -= Other.x; y -= Other.y; return *this; }
    Vector2& operator*=(float S) { x *= S; y *= S; return *this; }
    Vector2& operator/=(float S) { x /= S; y /= S; return *this; }

    // Math
    float Length() const { return std::sqrt(x * x + y * y); }
    float Size() const { return std::sqrt(x * x + y * y); }
    float SizeSquared() const { return x * x + y * y; }
    float Dot(const Vector2& Other) const { return x * Other.x + y * Other.y; }
    float Cross(const Vector2& Other) const { return x * Other.y - y * Other.x; }
    
    Vector2 GetNormalized() const
    {
        float L = Length();
        return L > Float_EPSILON ? Vector2(x / L, y / L) : Vector2(0.0f, 0.0f);
    }
    
    void Normalize()
    {
        float L = Length();
        if (L > Float_EPSILON)
        {
            x /= L;
            y /= L;
        }
    }

    // Utility
    bool IsZero() const { return x == 0.0f && y == 0.0f; }
    bool IsNan() const { return std::isnan(x) || std::isnan(y); }
    bool IsFinite() const { return std::isfinite(x) && std::isfinite(y); }

    // Component-wise min/max
    static Vector2 Min(const Vector2& A, const Vector2& B) { return Vector2((std::min)(A.x, B.x), (std::min)(A.y, B.y)); }
    static Vector2 Max(const Vector2& A, const Vector2& B) { return Vector2((std::max)(A.x, B.x), (std::max)(A.y, B.y)); }
    static Vector2 Clamp(const Vector2& V, const Vector2& MinVec, const Vector2& MaxVec)
    {
        return Vector2((std::min)((std::max)(V.x, MinVec.x), MaxVec.x),
                       (std::min)((std::max)(V.y, MinVec.y), MaxVec.y));
    }
};

// Free operators
inline Vector2 operator*(float S, const Vector2& V) { return V * S; }

// ============================================================================
// Vector4 - 4D vector, used for colors
// ============================================================================

struct Vector4
{
    float x {0.0f};
    float y {0.0f};
    float z {0.0f};
    float w {0.0f};

    Vector4() = default;
    constexpr Vector4(float X, float Y, float Z, float W) : x(X), y(Y), z(Z), w(W) {}
    constexpr Vector4(float Value) : x(Value), y(Value), z(Value), w(Value) {}

    // Operators
    bool operator==(const Vector4& Other) const { return x == Other.x && y == Other.y && z == Other.z && w == Other.w; }
    bool operator!=(const Vector4& Other) const { return !(*this == Other); }
    bool operator<(const Vector4& Other) const { return x < Other.x && y < Other.y && z < Other.z && w < Other.w; }
    bool operator<=(const Vector4& Other) const { return x <= Other.x && y <= Other.y && z <= Other.z && w <= Other.w; }
    bool operator>(const Vector4& Other) const { return x > Other.x && y > Other.y && z > Other.z && w > Other.w; }
    bool operator>=(const Vector4& Other) const { return x >= Other.x && y >= Other.y && z >= Other.z && w >= Other.w; }

    Vector4 operator+(const Vector4& Other) const { return Vector4(x + Other.x, y + Other.y, z + Other.z, w + Other.w); }
    Vector4 operator-(const Vector4& Other) const { return Vector4(x - Other.x, y - Other.y, z - Other.z, w - Other.w); }
    Vector4 operator*(float S) const { return Vector4(x * S, y * S, z * S, w * S); }
    Vector4 operator/(float S) const { return Vector4(x / S, y / S, z / S, w / S); }
    Vector4 operator-() const { return Vector4(-x, -y, -z, -w); }

    Vector4& operator+=(const Vector4& Other) { x += Other.x; y += Other.y; z += Other.z; w += Other.w; return *this; }
    Vector4& operator-=(const Vector4& Other) { x -= Other.x; y -= Other.y; z -= Other.z; w -= Other.w; return *this; }
    Vector4& operator*=(float S) { x *= S; y *= S; z *= S; w *= S; return *this; }
    Vector4& operator/=(float S) { x /= S; y /= S; z /= S; w /= S; return *this; }

    // Math
    float Length() const { return std::sqrt(x * x + y * y + z * z + w * w); }
    float Size() const { return std::sqrt(x * x + y * y + z * z + w * w); }
    float Dot(const Vector4& Other) const { return x * Other.x + y * Other.y + z * Other.z + w * Other.w; }

    // Utility
    bool IsZero() const { return x == 0.0f && y == 0.0f && z == 0.0f && w == 0.0f; }
    bool IsNan() const { return std::isnan(x) || std::isnan(y) || std::isnan(z) || std::isnan(w); }
    bool IsFinite() const { return std::isfinite(x) && std::isfinite(y) && std::isfinite(z) && std::isfinite(w); }
};

inline Vector4 operator*(float S, const Vector4& V) { return V * S; }

// Type alias for colors
using UIColor = Vector4;

// ============================================================================
// UIRect - 2D rectangle (used for clipping, geometry)
// ============================================================================

struct UIRect
{
    float x {0.0f};
    float y {0.0f};
    float w {0.0f};
    float h {0.0f};

    UIRect() = default;
    constexpr UIRect(float X, float Y, float W, float H) : x(X), y(Y), w(W), h(H) {}

    // Accessors
    float Left() const { return x; }
    float Top() const { return y; }
    float Right() const { return x + w; }
    float Bottom() const { return y + h; }
    Vector2 Position() const { return Vector2(x, y); }
    Vector2 Size() const { return Vector2(w, h); }
    float Width() const { return w; }
    float Height() const { return h; }
    float Area() const { return w * h; }

    // Modification
    void Set(float X, float Y, float W, float H) { x = X; y = Y; w = W; h = H; }
    void MoveTo(float X, float Y) { x = X; y = Y; }
    void MoveBy(float Dx, float Dy) { x += Dx; y += Dy; }
    void Resize(float W, float H) { w = W; h = H; }
    void Expand(float Dx, float Dy) { x -= Dx; y -= Dy; w += Dx * 2; h += Dy * 2; }
    void Expand(float Amount) { Expand(Amount, Amount); }

    // Queries
    bool IsEmpty() const { return w <= 0.0f || h <= 0.0f; }
    bool Contains(float Px, float Py) const { return Px >= x && Px <= x + w && Py >= y && Py <= y + h; }
    bool Intersects(const UIRect& Other) const
    {
        return !(Right() < Other.x || Other.Right() < x ||
                 Bottom() < Other.y || Other.Bottom() < y);
    }

    // Operations
    UIRect Intersection(const UIRect& Other) const
    {
        float X = (std::max)(x, Other.x);
        float Y = (std::max)(y, Other.y);
        float W = (std::min)(Right(), Other.Right()) - X;
        float H = (std::min)(Bottom(), Other.Bottom()) - Y;
        return UIRect(X, Y, (std::max)(0.0f, W), (std::max)(0.0f, H));
    }

    // Operators
    bool operator==(const UIRect& Other) const { return x == Other.x && y == Other.y && w == Other.w && h == Other.h; }
    bool operator!=(const UIRect& Other) const { return !(*this == Other); }

    // ---- Backward-compat aliases (migrating from legacy ::UIRect) ----
    Vector2 getMin() const { return Position(); }
    Vector2 getMax() const { return Vector2(Right(), Bottom()); }
    Vector2 getCenter() const { return Vector2(x + w * 0.5f, y + h * 0.5f); }
    Vector2 getSize() const { return Size(); }
    bool Contains(const Vector2& point) const { return Contains(point.x, point.y); }
    UIRect intersect(const UIRect& other) const { return Intersection(other); }
};

// ============================================================================
// Color Constants
// ============================================================================

namespace Colors
{
    inline constexpr UIColor White = UIColor(1.0f, 1.0f, 1.0f, 1.0f);
    inline constexpr UIColor Black = UIColor(0.0f, 0.0f, 0.0f, 1.0f);
    inline constexpr UIColor Red = UIColor(1.0f, 0.0f, 0.0f, 1.0f);
    inline constexpr UIColor Green = UIColor(0.0f, 1.0f, 0.0f, 1.0f);
    inline constexpr UIColor Blue = UIColor(0.0f, 0.0f, 1.0f, 1.0f);
    inline constexpr UIColor Yellow = UIColor(1.0f, 1.0f, 0.0f, 1.0f);
    inline constexpr UIColor Cyan = UIColor(0.0f, 1.0f, 1.0f, 1.0f);
    inline constexpr UIColor Magenta = UIColor(1.0f, 0.0f, 1.0f, 1.0f);
    inline constexpr UIColor Transparent = UIColor(0.0f, 0.0f, 0.0f, 0.0f);
}

}  // namespace ZSlate
