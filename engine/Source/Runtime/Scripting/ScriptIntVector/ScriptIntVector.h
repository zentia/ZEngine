#pragma once

#include "Runtime/BaseClasses/Object.h"
#include "Runtime/Core/Math/Vector2.h"
#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Core/Math/VectorInt.h"

#include "ScriptIntVector.generated.h"

// =============================================================================
// `Vector2Int` / `Vector3Int` —— Unity 的 `UnityEngine.Vector2Int` / `Vector3Int`
// =============================================================================
//
// 与 `Bounds` / `AnimationCurve` 同一套分工（详见 ScriptBounds.h）：
//
//   * 值类型 `::Vector2Int` / `::Vector3Int`（`Runtime/Core/Math/VectorInt.h`）
//     作为**值**跨边界 —— JS 端就是 `{x, y}` / `{x, y, z}` 字面量，.d.ts 里是
//     `declare interface`，**字段可以直接读写**（`v.x = 3`）。
//   * 这两个 `Runtime::Vector2Int` / `Runtime::Vector3Int` 是**门面**：Unity 那批
//     静态成员（`zero` / `Distance` / `Min` / `Max` / `FloorToInt`）得挂在某个
//     绑定类上。
//
// 为什么是"静态函数"而不是"实例方法"
// ----------------------------------
// ZHeaderTool 绑不了构造函数，脚本拿不到"带实例方法的 Vector2Int 对象"。所以
// `Vector2Int.zero` 是静态 getter、`Vector2Int.Distance(a, b)` 的第一个参数是
// "自己" —— 与 `Vector3.Distance(a, b)` 同一个约定。
//
// ⚠️ 六个方向常量的命名是 **Unity 的 Y-up 命名**，值与 Unity 一致
// ------------------------------------------------------------------
// `up = (0, 1, 0)`、`forward = (0, 0, 1)`。⚠️ **这不是 ZEngine 的轴向约定** ——
// ZEngine 是 Z-up、forward = -Y（见 `doc/TRANSFORM_SCRIPT_API.md`）。这六个只是
// Unity 的字面常量（网格/瓦片坐标用），要"这个物体的前方"得用
// `transform.GetForward()`，别混用。
// =============================================================================

namespace Runtime
{
    /// Unity 的 `Vector2Int` 静态面。不该被实例化（同 `Vector3` / `Mathf`）。
    ZCLASS()
    class Vector2Int : public Object
    {
        ZGENERATED_BODY(Vector2Int)

    public:
        // ---- 构造 / 转换 --------------------------------------------------

        ZFUNCTION()
        static ::Vector2Int Create(int x, int y);

        /// ⚠️ **floor**（-0.5 → -1），不是截断（`static_cast<int>` 会得到 0）。
        /// Unity 的 `Vector2Int.FloorToInt` 同样是 floor —— 负数上这两个不一样，
        /// 而正数上完全一样，所以只用正数测不出来。
        ZFUNCTION()
        static ::Vector2Int FloorToInt(const Vector2& v);

        ZFUNCTION()
        static ::Vector2Int CeilToInt(const Vector2& v);

        /// 平局取偶（`std::rint`，与 `Mathf.RoundToInt` 同一规则），不是 `lround`。
        ZFUNCTION()
        static ::Vector2Int RoundToInt(const Vector2& v);

        ZFUNCTION()
        static Vector2 ToVector2(const ::Vector2Int& v);

        // ---- 常量 ----------------------------------------------------------

        ZFUNCTION()
        static ::Vector2Int GetZero();
        ZFUNCTION()
        static ::Vector2Int GetOne();
        ZFUNCTION()
        static ::Vector2Int GetUp();
        ZFUNCTION()
        static ::Vector2Int GetDown();
        ZFUNCTION()
        static ::Vector2Int GetLeft();
        ZFUNCTION()
        static ::Vector2Int GetRight();
        ZSCRIPTPROPERTY(Name = "zero", Getter = "GetZero")
        ZSCRIPTPROPERTY(Name = "one", Getter = "GetOne")
        ZSCRIPTPROPERTY(Name = "up", Getter = "GetUp")
        ZSCRIPTPROPERTY(Name = "down", Getter = "GetDown")
        ZSCRIPTPROPERTY(Name = "left", Getter = "GetLeft")
        ZSCRIPTPROPERTY(Name = "right", Getter = "GetRight")

        // ---- 算术 ----------------------------------------------------------

        ZFUNCTION()
        static ::Vector2Int Add(const ::Vector2Int& a, const ::Vector2Int& b);

        ZFUNCTION()
        static ::Vector2Int Sub(const ::Vector2Int& a, const ::Vector2Int& b);

        /// 分量各自相乘（Unity 的 `Vector2Int.Scale`）。
        ZFUNCTION()
        static ::Vector2Int Scale(const ::Vector2Int& a, const ::Vector2Int& b);

        /// 标量乘。
        ZFUNCTION()
        static ::Vector2Int MulScalar(const ::Vector2Int& v, int scalar);

        ZFUNCTION()
        static ::Vector2Int Min(const ::Vector2Int& a, const ::Vector2Int& b);

        ZFUNCTION()
        static ::Vector2Int Max(const ::Vector2Int& a, const ::Vector2Int& b);

        ZFUNCTION()
        static ::Vector2Int Clamp(const ::Vector2Int& v, const ::Vector2Int& min,
                                  const ::Vector2Int& max);

        // ---- 查询 ----------------------------------------------------------

        /// Unity 的 `Vector2Int.Distance`（**float** —— 与 `magnitude` 一致）。
        ZFUNCTION()
        static float Distance(const ::Vector2Int& a, const ::Vector2Int& b);

        /// Unity 的 `sqrMagnitude`（int，不开方）。
        ZFUNCTION()
        static int GetSqrMagnitude(const ::Vector2Int& v);

        /// Unity 的 `magnitude`（float）。
        ZFUNCTION()
        static float GetMagnitude(const ::Vector2Int& v);

        ZFUNCTION()
        static bool Equals(const ::Vector2Int& a, const ::Vector2Int& b);

        ZFUNCTION()
        static eastl::string ToString(const ::Vector2Int& v);
    };

    /// Unity 的 `Vector3Int` 静态面。
    ZCLASS()
    class Vector3Int : public Object
    {
        ZGENERATED_BODY(Vector3Int)

    public:
        ZFUNCTION()
        static ::Vector3Int Create(int x, int y, int z);

        ZFUNCTION()
        static ::Vector3Int FloorToInt(const Vector3& v);

        ZFUNCTION()
        static ::Vector3Int CeilToInt(const Vector3& v);

        ZFUNCTION()
        static ::Vector3Int RoundToInt(const Vector3& v);

        ZFUNCTION()
        static Vector3 ToVector3(const ::Vector3Int& v);

        ZFUNCTION()
        static ::Vector3Int GetZero();
        ZFUNCTION()
        static ::Vector3Int GetOne();
        ZFUNCTION()
        static ::Vector3Int GetUp();
        ZFUNCTION()
        static ::Vector3Int GetDown();
        ZFUNCTION()
        static ::Vector3Int GetLeft();
        ZFUNCTION()
        static ::Vector3Int GetRight();
        ZFUNCTION()
        static ::Vector3Int GetForward();
        ZFUNCTION()
        static ::Vector3Int GetBack();
        ZSCRIPTPROPERTY(Name = "zero", Getter = "GetZero")
        ZSCRIPTPROPERTY(Name = "one", Getter = "GetOne")
        ZSCRIPTPROPERTY(Name = "up", Getter = "GetUp")
        ZSCRIPTPROPERTY(Name = "down", Getter = "GetDown")
        ZSCRIPTPROPERTY(Name = "left", Getter = "GetLeft")
        ZSCRIPTPROPERTY(Name = "right", Getter = "GetRight")
        ZSCRIPTPROPERTY(Name = "forward", Getter = "GetForward")
        ZSCRIPTPROPERTY(Name = "back", Getter = "GetBack")

        ZFUNCTION()
        static ::Vector3Int Add(const ::Vector3Int& a, const ::Vector3Int& b);

        ZFUNCTION()
        static ::Vector3Int Sub(const ::Vector3Int& a, const ::Vector3Int& b);

        ZFUNCTION()
        static ::Vector3Int Scale(const ::Vector3Int& a, const ::Vector3Int& b);

        ZFUNCTION()
        static ::Vector3Int MulScalar(const ::Vector3Int& v, int scalar);

        ZFUNCTION()
        static ::Vector3Int Min(const ::Vector3Int& a, const ::Vector3Int& b);

        ZFUNCTION()
        static ::Vector3Int Max(const ::Vector3Int& a, const ::Vector3Int& b);

        ZFUNCTION()
        static ::Vector3Int Clamp(const ::Vector3Int& v, const ::Vector3Int& min,
                                  const ::Vector3Int& max);

        ZFUNCTION()
        static float Distance(const ::Vector3Int& a, const ::Vector3Int& b);

        ZFUNCTION()
        static int GetSqrMagnitude(const ::Vector3Int& v);

        ZFUNCTION()
        static float GetMagnitude(const ::Vector3Int& v);

        ZFUNCTION()
        static bool Equals(const ::Vector3Int& a, const ::Vector3Int& b);

        ZFUNCTION()
        static eastl::string ToString(const ::Vector3Int& v);
    };
}  // namespace Runtime
