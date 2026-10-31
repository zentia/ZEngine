#pragma once

#include "Runtime/BaseClasses/Object.h"
#include "Runtime/Core/Math/Bounds.h"
#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Scripting/ScriptCamera/ScriptCamera.h"

#include "ScriptBounds.generated.h"

// =============================================================================
// `Bounds` —— Unity 的 `UnityEngine.Bounds` 在脚本侧的那一半。
//
// 与 `Color` 同一套分工（详见 ScriptColor.h 的开头）：
//
//   * 值类型 `::Bounds`（`Runtime/Core/Math/Bounds.h`）：center + extents 两个
//     Vector3 的 POD，进不了绑定表，作为**值**跨边界 —— `ZScript::Converter<Bounds>`
//     把它当 `{center, extents}` 对象搬运，.d.ts 里是 `declare interface Bounds`。
//   * 这个 `Runtime::Bounds` 是**门面**：Unity 那批 `bounds.Contains(p)` 之类
//     得挂在某个绑定类上。名字必须叫 `Bounds`（d.ts 的类名就是 C++ 类名），
//     于是只能放进命名空间避开同名冲突。
//
// 为什么是"静态函数"而不是"实例方法"
// ----------------------------------
// Unity 里 `Bounds` 是 struct，`b.Contains(p)` 是改自己的实例方法。ZEngine 这
// 一侧是值类型（JS 端就是个 `{center, extents}` 字面量），没有地方挂实例方法。
//
// 更根本的一条：**ZHeaderTool 绑不了构造函数**（parser.py: "a constructor
// cannot be bound (no return type)"），所以脚本写不出 `new Bounds(c, s)`，也就
// 拿不到"一个带方法的 Bounds 实例"。改成 `Bounds.Create(c, s)` 返回一个值 +
// `Bounds.Contains(b, p)` 这样的静态形式，和引擎里 `Vector3.Distance(a, b)` /
// `Color.Lerp(a, b, t)` 是同一个约定 —— 凡 Unity 那边是静态的（Vector3.Distance
// / Color.Lerp）形状就完全一致，凡 Unity 那边是 struct 实例方法的，这里退化为
// "第一个参数是自己"。
//
// 同理 `Encapsulate(Bounds)` / `Expand(Vector3)` 这两个重载也没法同名导出：绑定
// 表按脚本名索引（pesapi `set_method_info`），两个 `Encapsulate` 后者会覆盖前
// 者，第一个永远调不到。所以叫 `EncapsulateBounds` / `ExpandVector`。
// =============================================================================

namespace Runtime
{
    /// Unity 的 `Bounds` 静态面。
    ///
    /// 它不该被实例化 —— 它只是脚本侧的命名空间（同 `Color` / `Vector3` /
    /// `Mathf`）。之所以还得是个 `Object` 派生类，是因为 ZCLASS 要求。
    ZCLASS()
    class Bounds : public Object
    {
        ZGENERATED_BODY(Bounds)

    public:
        // ---- 构造 --------------------------------------------------------

        /// Unity 的 `new Bounds(center, size)`。⚠️ 第二个参数是 **size（全尺
        /// 寸）**，不是 extents —— 这是 Unity 的构造函数签名，也是最容易传错的
        /// 一个参数（传 extents 会得到只有一半大的盒，且不报错）。
        ZFUNCTION()
        static ::Bounds Create(const Vector3& center, const Vector3& size);

        /// Unity 的 `Bounds.SetMinMax` 的构造形式：两个角 → 一个盒。
        ZFUNCTION()
        static ::Bounds FromMinMax(const Vector3& min, const Vector3& max);

        // ---- 派生量 ------------------------------------------------------

        /// `bounds.size`：extents * 2。
        ZFUNCTION()
        static Vector3 GetSize(const ::Bounds& bounds);

        /// `bounds.min`。
        ZFUNCTION()
        static Vector3 GetMin(const ::Bounds& bounds);

        /// `bounds.max`。
        ZFUNCTION()
        static Vector3 GetMax(const ::Bounds& bounds);

        // ---- 查询 --------------------------------------------------------

        /// `bounds.Contains(point)`。边界算包含（点正好在面上是 true）。
        ZFUNCTION()
        static bool Contains(const ::Bounds& bounds, const Vector3& point);

        /// `bounds.Intersects(other)`。贴面也算相交。
        ZFUNCTION()
        static bool Intersects(const ::Bounds& a, const ::Bounds& b);

        /// `bounds.ClosestPoint(point)`：盒上（或盒内）离 point 最近的一点。
        ZFUNCTION()
        static Vector3 ClosestPoint(const ::Bounds& bounds, const Vector3& point);

        /// `bounds.SqrDistance(point)`：**平方**距离，盒内为 0。
        ZFUNCTION()
        static float SqrDistance(const ::Bounds& bounds, const Vector3& point);

        /// Unity 的 `bounds.IntersectRay(ray, out distance)`。
        ///
        /// TS 没有 out 参数，所以：命中返回距离（>= 0），**不命中返回 -1**。
        /// 距离是射线参数 t（`hit = origin + direction * t`），`direction` 未归一
        /// 化时它不是世界距离 —— 与 Unity 一致。起点在盒内时返回 0。
        ///
        /// `ray` 为 null 时返回 -1。
        ZFUNCTION()
        static float IntersectRay(const ::Bounds& bounds, ::Ray* ray);

        // ---- 变换（返回新值，不改传入的那个）------------------------------

        /// `bounds.Encapsulate(point)` 的非破坏形式：返回把 point 包进来的新盒。
        ZFUNCTION()
        static ::Bounds Encapsulate(const ::Bounds& bounds, const Vector3& point);

        /// `bounds.Encapsulate(other)` 的非破坏形式。
        ZFUNCTION()
        static ::Bounds EncapsulateBounds(const ::Bounds& bounds, const ::Bounds& other);

        /// `bounds.Expand(amount)`：⚠️ 尺寸**只增加 amount**（不是 2*amount），
        /// 因为 Unity 的 Expand 先把 amount 除以 2 再加到 extents 上。
        ZFUNCTION()
        static ::Bounds Expand(const ::Bounds& bounds, float amount);

        /// `bounds.Expand(Vector3)` 的非破坏形式，逐轴。
        ZFUNCTION()
        static ::Bounds ExpandVector(const ::Bounds& bounds, const Vector3& amount);

        /// Unity 的 `bounds.ToString()`：`"Center: (0.0, 0.0, 0.0), Extents:
        /// (1.0, 1.0, 1.0)"`。与 Unity 的打印格式一致（一位小数）。
        ZFUNCTION()
        static std::string ToString(const ::Bounds& bounds);
    };
}  // namespace Runtime
