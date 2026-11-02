#pragma once

#include <cmath>
#include <cstdint>

// =============================================================================
// Vector2Int / Vector3Int —— Unity 的 `UnityEngine.Vector2Int` / `Vector3Int`
// =============================================================================
//
// 与 `Vector2` / `Vector3` 一样是**值类型**：`ZScript::Converter` 把它们当普通
// JS 对象（`{x, y}` / `{x, y, z}`）搬运，.d.ts 里是 `declare interface`，脚本侧
// 的静态方法挂在 `Runtime::Vector2Int` / `Runtime::Vector3Int` 两个门面上（见
// ScriptIntVector.h）。
//
// 为什么要有整数版
// ----------------
// 网格坐标 / 瓦片 / 像素这类量用 float 存会引入"1.0000001 落在哪一格"的问题，
// 而 Unity 的 API（`Vector3Int.FloorToInt`、`Tilemap` 的坐标）本来就是整数。
// 用 `Vector3` 四舍五入代替的话，`FloorToInt` 与 `(int)` 截断的差异会在负数上
// 暴露：Unity 的 `FloorToInt` 是 **floor**（-0.5 → -1），`(int)` 是**截断**
// （-0.5 → 0）。
//
// ⚠️ 溢出行与 Unity 一致，没有额外防护
// ------------------------------------
// `GetSqrMagnitude()` 是 `x*x + y*y`，int 乘法溢出是 UB。Unity 同样溢出（它是
// 未检查的 C# int）。这里不做防护是因为"防护了就和 Unity 不一样"，而瓦片坐标
// 这类正常用法离溢出很远。
// ---------------------------------------------------------------------------

struct Vector2Int
{
    int x {0};
    int y {0};

    Vector2Int() = default;
    Vector2Int(int in_x, int in_y) : x(in_x), y(in_y) {}

    /// Unity 的 `Set(x, y)`。
    void Set(int new_x, int new_y)
    {
        x = new_x;
        y = new_y;
    }

    /// 分量各自相乘（Unity 的 `Scale`）。
    void Scale(const Vector2Int& scale)
    {
        x *= scale.x;
        y *= scale.y;
    }

    /// Unity 的 `sqrMagnitude`（int，不开方）。
    int GetSqrMagnitude() const { return x * x + y * y; }

    /// Unity 的 `magnitude`（**float** —— 开方不可能是整数，Unity 也是 float）。
    float GetMagnitude() const { return std::sqrt(static_cast<float>(x * x + y * y)); }

    Vector2Int operator+(const Vector2Int& other) const { return Vector2Int(x + other.x, y + other.y); }
    Vector2Int operator-(const Vector2Int& other) const { return Vector2Int(x - other.x, y - other.y); }
    Vector2Int operator*(int scalar) const { return Vector2Int(x * scalar, y * scalar); }
    /// 分量各自相乘，与 `Scale` 同（Unity 的 `Vector2Int * Vector2Int`）。
    Vector2Int operator*(const Vector2Int& other) const { return Vector2Int(x * other.x, y * other.y); }
    bool operator==(const Vector2Int& other) const { return x == other.x && y == other.y; }
    bool operator!=(const Vector2Int& other) const { return !(*this == other); }
};

struct Vector3Int
{
    int x {0};
    int y {0};
    int z {0};

    Vector3Int() = default;
    Vector3Int(int in_x, int in_y, int in_z) : x(in_x), y(in_y), z(in_z) {}

    void Set(int new_x, int new_y, int new_z)
    {
        x = new_x;
        y = new_y;
        z = new_z;
    }

    void Scale(const Vector3Int& scale)
    {
        x *= scale.x;
        y *= scale.y;
        z *= scale.z;
    }

    int GetSqrMagnitude() const { return x * x + y * y + z * z; }
    float GetMagnitude() const { return std::sqrt(static_cast<float>(x * x + y * y + z * z)); }

    Vector3Int operator+(const Vector3Int& other) const
    {
        return Vector3Int(x + other.x, y + other.y, z + other.z);
    }
    Vector3Int operator-(const Vector3Int& other) const
    {
        return Vector3Int(x - other.x, y - other.y, z - other.z);
    }
    Vector3Int operator*(int scalar) const { return Vector3Int(x * scalar, y * scalar, z * scalar); }
    Vector3Int operator*(const Vector3Int& other) const
    {
        return Vector3Int(x * other.x, y * other.y, z * other.z);
    }
    bool operator==(const Vector3Int& other) const
    {
        return x == other.x && y == other.y && z == other.z;
    }
    bool operator!=(const Vector3Int& other) const { return !(*this == other); }
};
