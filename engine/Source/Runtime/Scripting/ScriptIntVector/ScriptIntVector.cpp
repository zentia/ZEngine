#include "Runtime/Scripting/ScriptIntVector/ScriptIntVector.h"

#include <cstdio>

namespace Runtime
{

    // ---- Vector2Int ---------------------------------------------------------

    ::Vector2Int Vector2Int::Create(int x, int y) { return ::Vector2Int(x, y); }

    ::Vector2Int Vector2Int::FloorToInt(const Vector2& v)
    {
        return ::Vector2Int(static_cast<int>(std::floor(v.x)), static_cast<int>(std::floor(v.y)));
    }

    ::Vector2Int Vector2Int::CeilToInt(const Vector2& v)
    {
        return ::Vector2Int(static_cast<int>(std::ceil(v.x)), static_cast<int>(std::ceil(v.y)));
    }

    ::Vector2Int Vector2Int::RoundToInt(const Vector2& v)
    {
        return ::Vector2Int(static_cast<int>(std::rint(v.x)), static_cast<int>(std::rint(v.y)));
    }

    Vector2 Vector2Int::ToVector2(const ::Vector2Int& v)
    {
        return Vector2(static_cast<float>(v.x), static_cast<float>(v.y));
    }

    ::Vector2Int Vector2Int::GetZero() { return ::Vector2Int(0, 0); }
    ::Vector2Int Vector2Int::GetOne() { return ::Vector2Int(1, 1); }
    ::Vector2Int Vector2Int::GetUp() { return ::Vector2Int(0, 1); }
    ::Vector2Int Vector2Int::GetDown() { return ::Vector2Int(0, -1); }
    ::Vector2Int Vector2Int::GetLeft() { return ::Vector2Int(-1, 0); }
    ::Vector2Int Vector2Int::GetRight() { return ::Vector2Int(1, 0); }

    ::Vector2Int Vector2Int::Add(const ::Vector2Int& a, const ::Vector2Int& b) { return a + b; }
    ::Vector2Int Vector2Int::Sub(const ::Vector2Int& a, const ::Vector2Int& b) { return a - b; }
    ::Vector2Int Vector2Int::Scale(const ::Vector2Int& a, const ::Vector2Int& b) { return a * b; }
    ::Vector2Int Vector2Int::MulScalar(const ::Vector2Int& v, int scalar) { return v * scalar; }

    ::Vector2Int Vector2Int::Min(const ::Vector2Int& a, const ::Vector2Int& b)
    {
        return ::Vector2Int((a.x < b.x) ? a.x : b.x, (a.y < b.y) ? a.y : b.y);
    }

    ::Vector2Int Vector2Int::Max(const ::Vector2Int& a, const ::Vector2Int& b)
    {
        return ::Vector2Int((a.x > b.x) ? a.x : b.x, (a.y > b.y) ? a.y : b.y);
    }

    ::Vector2Int Vector2Int::Clamp(const ::Vector2Int& v, const ::Vector2Int& min,
                                   const ::Vector2Int& max)
    {
        return Min(Max(v, min), max);
    }

    float Vector2Int::Distance(const ::Vector2Int& a, const ::Vector2Int& b)
    {
        const int dx = b.x - a.x;
        const int dy = b.y - a.y;
        return std::sqrt(static_cast<float>(dx * dx + dy * dy));
    }

    int Vector2Int::GetSqrMagnitude(const ::Vector2Int& v) { return v.GetSqrMagnitude(); }
    float Vector2Int::GetMagnitude(const ::Vector2Int& v) { return v.GetMagnitude(); }
    bool Vector2Int::Equals(const ::Vector2Int& a, const ::Vector2Int& b) { return a == b; }

    eastl::string Vector2Int::ToString(const ::Vector2Int& v)
    {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "(%d, %d)", v.x, v.y);
        return eastl::string(buffer);
    }

    // ---- Vector3Int ---------------------------------------------------------

    ::Vector3Int Vector3Int::Create(int x, int y, int z) { return ::Vector3Int(x, y, z); }

    ::Vector3Int Vector3Int::FloorToInt(const Vector3& v)
    {
        return ::Vector3Int(static_cast<int>(std::floor(v.x)), static_cast<int>(std::floor(v.y)),
                            static_cast<int>(std::floor(v.z)));
    }

    ::Vector3Int Vector3Int::CeilToInt(const Vector3& v)
    {
        return ::Vector3Int(static_cast<int>(std::ceil(v.x)), static_cast<int>(std::ceil(v.y)),
                            static_cast<int>(std::ceil(v.z)));
    }

    ::Vector3Int Vector3Int::RoundToInt(const Vector3& v)
    {
        return ::Vector3Int(static_cast<int>(std::rint(v.x)), static_cast<int>(std::rint(v.y)),
                            static_cast<int>(std::rint(v.z)));
    }

    Vector3 Vector3Int::ToVector3(const ::Vector3Int& v)
    {
        return Vector3(static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z));
    }

    ::Vector3Int Vector3Int::GetZero() { return ::Vector3Int(0, 0, 0); }
    ::Vector3Int Vector3Int::GetOne() { return ::Vector3Int(1, 1, 1); }
    // ⚠️ 这六个是 **Unity 的 Y-up 命名**，值与 Unity 一致。ZEngine 自己的 forward
    // 是 -Y（Z-up），要"物体的前方"用 transform.GetForward()，别用 forward 这个常量。
    ::Vector3Int Vector3Int::GetUp() { return ::Vector3Int(0, 1, 0); }
    ::Vector3Int Vector3Int::GetDown() { return ::Vector3Int(0, -1, 0); }
    ::Vector3Int Vector3Int::GetLeft() { return ::Vector3Int(-1, 0, 0); }
    ::Vector3Int Vector3Int::GetRight() { return ::Vector3Int(1, 0, 0); }
    ::Vector3Int Vector3Int::GetForward() { return ::Vector3Int(0, 0, 1); }
    ::Vector3Int Vector3Int::GetBack() { return ::Vector3Int(0, 0, -1); }

    ::Vector3Int Vector3Int::Add(const ::Vector3Int& a, const ::Vector3Int& b) { return a + b; }
    ::Vector3Int Vector3Int::Sub(const ::Vector3Int& a, const ::Vector3Int& b) { return a - b; }
    ::Vector3Int Vector3Int::Scale(const ::Vector3Int& a, const ::Vector3Int& b) { return a * b; }
    ::Vector3Int Vector3Int::MulScalar(const ::Vector3Int& v, int scalar) { return v * scalar; }

    ::Vector3Int Vector3Int::Min(const ::Vector3Int& a, const ::Vector3Int& b)
    {
        return ::Vector3Int((a.x < b.x) ? a.x : b.x, (a.y < b.y) ? a.y : b.y,
                            (a.z < b.z) ? a.z : b.z);
    }

    ::Vector3Int Vector3Int::Max(const ::Vector3Int& a, const ::Vector3Int& b)
    {
        return ::Vector3Int((a.x > b.x) ? a.x : b.x, (a.y > b.y) ? a.y : b.y,
                            (a.z > b.z) ? a.z : b.z);
    }

    ::Vector3Int Vector3Int::Clamp(const ::Vector3Int& v, const ::Vector3Int& min,
                                   const ::Vector3Int& max)
    {
        return Min(Max(v, min), max);
    }

    float Vector3Int::Distance(const ::Vector3Int& a, const ::Vector3Int& b)
    {
        const int dx = b.x - a.x;
        const int dy = b.y - a.y;
        const int dz = b.z - a.z;
        return std::sqrt(static_cast<float>(dx * dx + dy * dy + dz * dz));
    }

    int Vector3Int::GetSqrMagnitude(const ::Vector3Int& v) { return v.GetSqrMagnitude(); }
    float Vector3Int::GetMagnitude(const ::Vector3Int& v) { return v.GetMagnitude(); }
    bool Vector3Int::Equals(const ::Vector3Int& a, const ::Vector3Int& b) { return a == b; }

    eastl::string Vector3Int::ToString(const ::Vector3Int& v)
    {
        char buffer[80];
        std::snprintf(buffer, sizeof(buffer), "(%d, %d, %d)", v.x, v.y, v.z);
        return eastl::string(buffer);
    }

}  // namespace Runtime
