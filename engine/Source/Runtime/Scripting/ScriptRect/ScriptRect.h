#pragma once

#include "Runtime/BaseClasses/Object.h"
#include "Runtime/Core/Math/Rect.h"
#include "Runtime/Core/Math/Vector2.h"
#include "Runtime/Core/Math/VectorInt.h"

#include "ScriptRect.generated.h"

// =============================================================================
// `Rect` / `RectInt` —— Unity 的 `UnityEngine.Rect` / `UnityEngine.RectInt`
// =============================================================================
//
// 与 `Bounds` / `AnimationCurve` 同一套分工（详见 ScriptBounds.h）：
//
//   * 值类型 `::Rect` / `::RectInt`（都是 `RectT<T>` 的 using，
//     `Runtime/Core/Math/Rect.h`）作为**值**跨边界 —— JS 端就是
//     `{x, y, width, height}` 字面量，.d.ts 里是 `declare interface`，字段可以
//     直接读写。
//   * 这两个 `Runtime::Rect` / `Runtime::RectInt` 是**门面**：Unity 那批静态成员
//     （`Rect.MinMaxRect`、`RectInt.ClampToBounds`）和"值类型上的实例方法"
//     （`rect.Contains(p)`）得挂在某个绑定类上。
//
// 为什么是"静态函数"而不是"实例方法"
// ----------------------------------
// 同 Bounds：ZHeaderTool 绑不了构造函数，脚本拿不到"带实例方法的 Rect 对象"。
// 所以 `Rect.Contains(r, p)` 的第一个参数是"自己"，与 `Vector3.Distance(a, b)`
// 同一个约定。
//
// ⚠️ 所有"改"都是**非破坏**的，返回新值
// --------------------------------------
// Unity 里 `rect.xMin = 3` 是原地改（struct 是值类型但字段可写）。这里的值跨
// 边界时是**快照**（见 `Converter` 的说明），原地改没有意义。所以
// `Rect.SetMinMax(r, ...)` 返回新矩形 —— **不接住返回值就等于没改**，而且不报
// 错。这与 `AnimationCurve.AddKey` 的坑同源。
//
// ⚠️ `Rect`（float）与 `RectInt`（int）的成员并**不相同**
// --------------------------------------------------------
// `ClampToBounds` 只有 `RectInt` 有；`PointToNormalized` / `NormalizedToPoint` /
// `OrderMinMax` 只有 `Rect` 有。这是 Unity 的真实形状，不是漏实现 —— 门面按
// Unity 逐个导出，没有"两边对齐"。
//
// 没导出的（`allPositions` / `allPositionsWithin`）
// ------------------------------------------------
// 这两个是 `IEnumerator<Vector2Int>`，Unity 用来 `foreach` 遍历矩形里的整数格。
// ZEngine 的脚本没有迭代器协议（协程 yield 的是 `YieldInstruction`），导出一个
// 假的迭代器比不导出更糟 —— 用 `Vector2Int` 双层 for 循环代替，语义清晰。
// =============================================================================

namespace Runtime
{
    /// Unity 的 `UnityEngine.Rect`（float）静态面。不该被实例化（同 `Bounds`）。
    ZCLASS()
    class Rect : public Object
    {
        ZGENERATED_BODY(Rect)

    public:
        // ---- 构造 --------------------------------------------------------

        /// Unity 的 `new Rect(x, y, width, height)`。
        ZFUNCTION()
        static ::Rect Create(float x, float y, float width, float height);

        /// Unity 的 `Rect.MinMaxRect(xmin, ymin, xmax, ymax)`：由两条**对角**
        /// 定矩形（顺序不限，内部做 Min/Max）。
        ZFUNCTION()
        static ::Rect MinMaxRect(float xmin, float ymin, float xmax, float ymax);

        // ---- 边（归一化后的两条边）------------------------------------------

        /// `rect.xMin`：两条竖边里较小的那条。⚠️ width 为负时**不是** `rect.x`。
        ZFUNCTION()
        static float GetXMin(const ::Rect& r);
        ZFUNCTION()
        static float GetXMax(const ::Rect& r);
        ZFUNCTION()
        static float GetYMin(const ::Rect& r);
        ZFUNCTION()
        static float GetYMax(const ::Rect& r);

        /// `rect.min` / `rect.max`：左下角 / 右上角（width 为负时自动交换）。
        ZFUNCTION()
        static Vector2 GetMin(const ::Rect& r);
        ZFUNCTION()
        static Vector2 GetMax(const ::Rect& r);

        // ---- 派生量 ------------------------------------------------------

        /// `rect.size`：**就是** `{width, height}`，Unity 里也是原样返回（可以为负）。
        /// 要"两条边围出来的正尺寸"用 `OrderMinMax` 或直接 `xMax - xMin`。
        ZFUNCTION()
        static Vector2 GetSize(const ::Rect& r);

        /// `rect.position`：**就是** `{x, y}`（不是 `min` —— width 为负时两者不同）。
        ZFUNCTION()
        static Vector2 GetPosition(const ::Rect& r);

        /// `rect.center`：**几何中心**（按 min/max 算，与 x/y 无关），float。
        ZFUNCTION()
        static Vector2 GetCenter(const ::Rect& r);

        // ---- 非破坏 setter ------------------------------------------------

        /// `rect.xMin = v` 的形式：移动**左**边，右边不动（width 跟着变）。
        ZFUNCTION(Name = "SetXMin")
        static ::Rect WithXMin(const ::Rect& r, float v);
        ZFUNCTION(Name = "SetXMax")
        static ::Rect WithXMax(const ::Rect& r, float v);
        ZFUNCTION(Name = "SetYMin")
        static ::Rect WithYMin(const ::Rect& r, float v);
        ZFUNCTION(Name = "SetYMax")
        static ::Rect WithYMax(const ::Rect& r, float v);

        /// `rect.Set(x, y, width, height)` 的非破坏形式。
        ZFUNCTION(Name = "Set")
        static ::Rect WithLayout(const ::Rect& r, float x, float y, float width, float height);

        /// `rect.min = ...` / `rect.max = ...`：挪对应的角，对角不动。
        ZFUNCTION(Name = "SetMin")
        static ::Rect WithMin(const ::Rect& r, const Vector2& min);
        ZFUNCTION(Name = "SetMax")
        static ::Rect WithMax(const ::Rect& r, const Vector2& max);

        /// `rect.size = ...`：改 width / height，**x / y 不动**（从左下往右上长）。
        ZFUNCTION(Name = "SetSize")
        static ::Rect WithSize(const ::Rect& r, const Vector2& size);

        /// `rect.position = ...`：平移（尺寸不变）。
        ZFUNCTION(Name = "SetPosition")
        static ::Rect WithPosition(const ::Rect& r, const Vector2& position);

        /// `rect.center = ...`：平移，使几何中心落在 `center`（尺寸不变）。
        ZFUNCTION(Name = "SetCenter")
        static ::Rect WithCenter(const ::Rect& r, const Vector2& center);

        // ---- 查询 --------------------------------------------------------

        /// Unity 的 `rect.Contains(point)`。⚠️ **半开区间** `[min, max)` —— 右
        /// 边和上边**不算**在内。这与 `Bounds.Contains`（闭区间）相反。
        ZFUNCTION(Name = "Contains")
        static bool ContainsPoint(const ::Rect& r, const Vector2& point);

        /// 分量形式（`rect.Contains(x, y)`）。Unity 没有这个重载 —— 导出是因为
        /// 绑定表按脚本名索引，同名重载后者会覆盖前者，所以分量版只能另起名字。
        ZFUNCTION(Name = "ContainsXY")
        static bool ContainsXY(const ::Rect& r, float x, float y);

        /// Unity 的 `rect.Overlaps(other)`。⚠️ 严格不等号：只共享一条边**不算**
        /// 相交（返回 false）。
        ZFUNCTION()
        static bool Overlaps(const ::Rect& a, const ::Rect& b);

        // ---- 工具 --------------------------------------------------------

        /// Unity 的 `Rect.OrderMinMax(r)`：把负的 width / height 翻正（min/max 不变，
        /// 即"两条边"不动、x/y 挪到 min）。
        ZFUNCTION()
        static ::Rect OrderMinMax(const ::Rect& r);

        /// Unity 的 `Rect.PointToNormalized(r, p)`：矩形内的点 → 归一化坐标
        /// （左下 `(0,0)`、右上 `(1,1)`）。矩形外**不夹取**，会超出 0..1。
        ZFUNCTION()
        static Vector2 PointToNormalized(const ::Rect& r, const Vector2& p);

        /// Unity 的 `Rect.NormalizedToPoint(r, uv)`：归一化坐标 → 矩形内的点。
        /// 这是上面那个的逆（`NormalizedToPoint(r, PointToNormalized(r, p)) == p`）。
        ZFUNCTION()
        static Vector2 NormalizedToPoint(const ::Rect& r, const Vector2& uv);

        ZFUNCTION()
        static eastl::string ToString(const ::Rect& r);
    };

    /// Unity 的 `UnityEngine.RectInt`（int）静态面。
    ///
    /// 与 `Rect` 的成员**刻意不齐**：`ClampToBounds` 只有它有，`PointToNormalized`
    /// 那三个只有 `Rect` 有 —— 照搬 Unity 的真实形状。
    ZCLASS()
    class RectInt : public Object
    {
        ZGENERATED_BODY(RectInt)

    public:
        ZFUNCTION()
        static ::RectInt Create(int x, int y, int width, int height);

        ZFUNCTION()
        static ::RectInt MinMaxRect(int xmin, int ymin, int xmax, int ymax);

        ZFUNCTION()
        static int GetXMin(const ::RectInt& r);
        ZFUNCTION()
        static int GetXMax(const ::RectInt& r);
        ZFUNCTION()
        static int GetYMin(const ::RectInt& r);
        ZFUNCTION()
        static int GetYMax(const ::RectInt& r);

        /// `RectInt.min` / `RectInt.max`：**Vector2Int**（与 `Rect` 的 Vector2 不同）。
        ZFUNCTION()
        static ::Vector2Int GetMin(const ::RectInt& r);
        ZFUNCTION()
        static ::Vector2Int GetMax(const ::RectInt& r);

        ZFUNCTION()
        static ::Vector2Int GetSize(const ::RectInt& r);

        ZFUNCTION()
        static ::Vector2Int GetPosition(const ::RectInt& r);

        /// `RectInt.center`：**Vector2**（float，与 `size` 的 Vector2Int 不同 ——
        /// Unity 里就是这样，整数矩形的中心可以落在半格上）。
        ZFUNCTION()
        static Vector2 GetCenter(const ::RectInt& r);

        /// `RectInt.SetMinMax(min, max)`：由两个角重设（顺序不限）。
        /// Unity 里这是**原地**改的 setter，这里返回新值。
        ZFUNCTION(Name = "SetMinMax")
        static ::RectInt WithMinMax(const ::RectInt& r, const ::Vector2Int& min,
                                    const ::Vector2Int& max);

        ZFUNCTION(Name = "SetMin")
        static ::RectInt WithMin(const ::RectInt& r, const ::Vector2Int& min);
        ZFUNCTION(Name = "SetMax")
        static ::RectInt WithMax(const ::RectInt& r, const ::Vector2Int& max);
        ZFUNCTION(Name = "SetSize")
        static ::RectInt WithSize(const ::RectInt& r, const ::Vector2Int& size);
        ZFUNCTION(Name = "SetPosition")
        static ::RectInt WithPosition(const ::RectInt& r, const ::Vector2Int& position);

        /// ⚠️ Unity 里 `Rect`（float）**没有**这个方法 —— 只有 `RectInt` 有。
        ///
        /// 把这个矩形挪进 `bounds`：尺寸尽量保持不变，超出 bounds 时才顶到
        /// bounds 的边上（所以结果一定完全落在 bounds 内）。
        ZFUNCTION()
        static ::RectInt ClampToBounds(const ::RectInt& r, const ::RectInt& bounds);

        /// 半开区间 `[min, max)`，同 `Rect.Contains`。
        ZFUNCTION(Name = "Contains")
        static bool ContainsPoint(const ::RectInt& r, const ::Vector2Int& point);

        ZFUNCTION(Name = "ContainsXY")
        static bool ContainsXY(const ::RectInt& r, int x, int y);

        /// 严格不等号：只共享一条边**不算**相交。
        ZFUNCTION()
        static bool Overlaps(const ::RectInt& a, const ::RectInt& b);

        ZFUNCTION()
        static eastl::string ToString(const ::RectInt& r);
    };
}  // namespace Runtime
