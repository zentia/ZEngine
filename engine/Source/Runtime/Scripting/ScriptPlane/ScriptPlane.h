#pragma once

#include "Runtime/BaseClasses/Object.h"
#include "Runtime/Core/Math/Bounds.h"
#include "Runtime/Core/Math/Matrix4.h"
#include "Runtime/Core/Math/Plane.h"
#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Scripting/ScriptCamera/ScriptCamera.h"

#include "ScriptPlane.generated.h"

// =============================================================================
// `Plane` / `GeometryUtility` —— Unity 的 `UnityEngine.Plane` 与
// `UnityEngine.GeometryUtility`
// =============================================================================
//
// 与 `Bounds` / `Rect` 同一套分工（详见 ScriptBounds.h）：
//
//   * 值类型 `::Plane`（`Runtime/Core/Math/Plane.h`）：`{normal, distance}` 的
//     POD，作为**值**跨边界 —— .d.ts 里是 `declare interface Plane`，字段可以
//     直接读写。
//   * 这两个 `Runtime::Plane` / `Runtime::GeometryUtility` 是**门面**：Unity 那批
//     静态成员（`GeometryUtility.CalculateFrustumPlanes`、`TestPlanesAABB`）和
//     "值类型上的实例方法"（`plane.Raycast(...)`）得挂在某个绑定类上。
//
// 为什么是"静态函数"而不是"实例方法"
// ----------------------------------
// 同 Bounds：ZHeaderTool 绑不了构造函数，脚本拿不到"带实例方法的 Plane 对象"。
// 所以 `Plane.Raycast(plane, origin, dir)` 的第一个参数是"自己"，与
// `Vector3.Distance(a, b)` 同一个约定。
//
// ⚠️ 所有"改"都是**非破坏**的，返回新值
// --------------------------------------
// Unity 里 `plane.Flip()` 是原地改（Plane 是 struct 但字段可写）。这里的值跨
// 边界时是**快照**，原地改没有意义。所以 `Plane.Flip(p)` 返回翻转后的**新**平面
// —— **不接住返回值就等于没改**，而且不报错。与 `AnimationCurve.AddKey` /
// `Rect.SetXMin` 同源的坑。
//
// ⚠️ `Raycast` 返回 -1 表示不中，不是 bool
// ----------------------------------------
// Unity 是 `bool Plane.Raycast(Ray ray, out float enter)`。TS 没有 out 参数，
// 所以距离自己回来：命中给射线参数 t（>= 0），**不命中给 -1**。
//
// ⚠️ t 不是世界距离
// ------------------
// `direction` 未归一化时 t 是射线参数（`hit = origin + direction * t`），与
// Unity 一致。要世界距离得自己乘 `direction` 的长度。
// =============================================================================

namespace Runtime
{
    /// Unity 的 `UnityEngine.Plane` 静态面。不该被实例化（同 `Bounds` / `Rect`）。
    ZCLASS()
    class Plane : public Object
    {
        ZGENERATED_BODY(Plane)

    public:
        // ---- 构造（Unity 里是三个构造函数）--------------------------------

        /// Unity 的 `new Plane(normal, distance)`。
        ///
        /// ⚠️ **不**归一化 `normal`：Unity 的构造函数同样不归一化。要"真正的
        /// 距离"必须自己 `Normalize`。
        ZFUNCTION()
        static ::Plane Create(const Vector3& normal, float distance);

        /// Unity 的 `new Plane(normal, point)`：法线归一化的那个版本。
        ZFUNCTION()
        static ::Plane FromNormalAndPosition(const Vector3& normal, const Vector3& point);

        /// Unity 的 `new Plane(a, b, c)`：三点定面，绕序按右手定则给出法线。
        /// 三点共线时退化成"零法线、零 distance"的平面（不产生 NaN）。
        ZFUNCTION()
        static ::Plane FromPoints(const Vector3& a, const Vector3& b, const Vector3& c);

        // ---- 查询 --------------------------------------------------------

        /// 到点的**有符号**距离（`dot(normal, p) + distance`）。正 = 在正面。
        /// ⚠️ 只有 normal 是单位向量时这才是"距离"。
        ZFUNCTION()
        static float GetDistanceToPoint(const ::Plane& plane, const Vector3& point);

        /// 点在正面吗。**边界算正面**（与 Unity 的 `GetSide` 同为 `>= 0`）。
        ZFUNCTION()
        static bool GetSide(const ::Plane& plane, const Vector3& point);

        /// 两点在同一侧吗。边界上的点算两侧都在。
        ZFUNCTION()
        static bool SameSide(const ::Plane& plane, const Vector3& a, const Vector3& b);

        /// 平面上离 `point` 最近的一点。
        ZFUNCTION()
        static Vector3 GetClosestPointOnPlane(const ::Plane& plane, const Vector3& point);

        /// 射线与平面求交。命中返回射线参数 t（>= 0），**不命中返回 -1**。
        ///
        /// ⚠️ 只打**正面**：射线朝着背面（`dot(normal, direction) >= 0`）算不中。
        /// 平行于平面同样算不中（不产生 NaN，也不把"贴着平面飞"算成命中）。
        ZFUNCTION()
        static float Raycast(const ::Plane& plane, const Vector3& origin, const Vector3& direction);

        // ---- 非破坏的"改"（都返回新平面）----------------------------------

        ZFUNCTION(Name = "SetNormalAndPosition")
        static ::Plane WithNormalAndPosition(const ::Plane& plane, const Vector3& normal,
                                             const Vector3& point);

        ZFUNCTION(Name = "Set3Points")
        static ::Plane With3Points(const ::Plane& plane, const Vector3& a, const Vector3& b,
                                   const Vector3& c);

        /// Unity 的 `plane.flipped`（只读属性，Unity 2021.2+）：返回翻转后的新平面。
        /// 这里是静态方法形式（ZSCRIPTPROPERTY 的 getter 不能带参数）。
        ZFUNCTION(Name = "GetFlipped")
        static ::Plane Flipped(const ::Plane& plane);

        /// Unity 的 `plane.Translate(t)` 的非破坏形式。
        ///
        /// ⚠️ `distance` 的变化是 `-dot(normal, translation)`。写反符号的话平面
        /// 往**反**方向跑，而它仍然是一个自洽的平面（法线没变），不报任何错。
        ZFUNCTION(Name = "Translate")
        static ::Plane Translated(const ::Plane& plane, const Vector3& translation);

        /// 归一化 `normal` 并按同一个尺度缩 `distance`（平面本身不变）。
        /// 零法线时什么都不做。
        ZFUNCTION(Name = "Normalize")
        static ::Plane Normalized(const ::Plane& plane);

        ZFUNCTION()
        static eastl::string ToString(const ::Plane& plane);
    };

    /// Unity 的 `UnityEngine.GeometryUtility` 静态面。
    ///
    /// 目前只导出了视锥那两个：Unity 还有 `CalculateBounds(Vector3[], Matrix4x4)`
    /// （需要渲染器数组，ZEngine 的脚本拿不到 mesh 顶点）与
    /// `TryCreatePlaneFromPolygon`（内部 API）。
    ZCLASS()
    class GeometryUtility : public Object
    {
        ZGENERATED_BODY(GeometryUtility)

    public:
        /// Unity 的 `GeometryUtility.CalculateFrustumPlanes(camera)`。
        ///
        /// 返回 **6 个**平面（左 / 右 / 下 / 上 / 近 / 远，与 Unity 的索引顺序
        /// 一致），法线一律指向**锥内**。
        ///
        /// `camera` 为 null 或没绑到 RenderCamera 时返回**空数组**（不是 6 个
        /// 退化平面）—— 空的数组一眼能看出是"没拿到相机"，而 6 个退化平面会让
        /// 后面的 `TestPlanesAABB` 恒真，也就是"什么都看得见"，静默。
        ZFUNCTION()
        static std::vector<::Plane> CalculateFrustumPlanes(Camera* camera);

        /// 直接从 `projection * view` 那张矩阵取六个平面（不给相机的那条路）。
        ///
        /// Unity 没有这个重载 —— 导出是因为脚本能拿到的相机只有 `Camera.main`
        /// 那几台，而编辑器 / 离屏那几台没有 `Camera` 门面。深度区间是
        /// **0..1**（Vulkan / DX 的约定，本引擎的投影矩阵就是这个区间）。
        ZFUNCTION()
        static std::vector<::Plane> CalculateFrustumPlanesVP(const Matrix4x4& view_projection);

        /// Unity 的 `GeometryUtility.TestPlanesAABB(planes, bounds)`。
        ///
        /// 判据是"**正顶点**"：对每个平面取盒子上沿法线方向最远的那个角，它若
        /// 已在背面，整个盒子就在锥外。
        ///
        /// ⚠️ 这是**保守**的：盒子很大、斜穿过视锥的一角时可能返回 true 而其实
        /// 不相交 —— 也就是会**多画**不会漏画。Unity 的 `TestPlanesAABB` 同样
        /// 是这个精度。漏画的代价（物体凭空消失）远大于多画。
        ///
        /// `planes` 为空时返回 true（没有任何平面能把它判到外面去）。
        ZFUNCTION()
        static bool TestPlanesAABB(const std::vector<::Plane>& planes, const ::Bounds& bounds);
    };
}  // namespace Runtime
