#pragma once

#include "Runtime/Core/Math/Bounds.h"
#include "Runtime/Core/Math/Matrix4.h"
#include "Runtime/Core/Math/Plane.h"

// =============================================================================
// Frustum —— 六个平面围出来的视锥（Unity 的 `GeometryUtility.CalculateFrustumPlanes`
// 返回的那个 `Plane[]`）
// =============================================================================
//
// 法线一律指向**锥内**，所以"在视锥里" == "对六个平面都在正面"。
// Unity 的 `TestPlanesAABB` 用的是同一个约定（`plane.GetDistanceToPoint(v) < 0`
// 就算在锥外）。
//
// ⚠️ 深度区间必须显式指定
// ------------------------
// 裁剪空间的不等式只有"深度那两条"有两种写法：
//
//   * OpenGL / `glClipControl(GL_ZERO_TO_ONE)` 之前：`-w <= z <= w`
//   * DirectX / Vulkan 默认（**本引擎**）：`0 <= z <= w`
//
// `Math::MakePerspectiveMatrix` 出来的是后者（`ret[2][2] = zfar/(znear-zfar)`、
// `ret[2][3] = zfar*znear/(znear-zfar)`：`z_eye = -znear` 映射到 0、`-zfar` 映射到 1）。
// 用错区间的话 **near 平面会整个错位**（`w + z >= 0` 变成 `z >= 0`），表现为
// "贴近相机的东西莫名被剔掉" —— 而 far 平面照样是对的，所以最容易误判成
// "近裁剪面设错了"。
struct Frustum
{
    /// Unity 的 `CalculateFrustumPlanes` 返回的数组长度就是 6。
    static constexpr int kPlaneCount = 6;

    enum Index
    {
        kLeft = 0,
        kRight = 1,
        kBottom = 2,
        kTop = 3,
        kNear = 4,
        kFar = 5,
    };

    /// 裁剪空间的深度区间。见文件头。
    enum class DepthRange
    {
        /// `-w <= z <= w`（OpenGL）。
        kNegOneToOne,
        /// `0 <= z <= w`（DirectX / Vulkan 默认，**本引擎**）。
        kZeroToOne,
    };

    Plane m_Planes[kPlaneCount];

    // ---- 构造 ------------------------------------------------------------

    /// 从 `projection * view` 那张矩阵里抽六个平面（Gribb-Hartmann）。
    ///
    /// `view_projection` 必须满足 `clip = view_projection * world`（列向量、行主
    /// 序，本引擎 `Matrix4x4::operator*(const Vector3&)` 的约定）。
    ///
    /// 矩阵奇异（比如全零）时六个平面都退化成"法线为零、distance 为零"的平面，
    /// 于是 `ContainsPoint` 恒为 true、`Intersects` 恒为 true —— 也就是"什么都
    /// 看得见"，而不是"什么都看不见"或 NaN。宁可多画不可闪掉。
    static Frustum FromViewProjection(const Matrix4x4& view_projection,
                                     DepthRange depth_range = DepthRange::kZeroToOne);

    // ---- 查询 ------------------------------------------------------------

    /// 点在视锥里吗。`tolerance` 给"贴着面"留余量（默认 0，边界算在锥内）。
    bool ContainsPoint(const Vector3& point, float tolerance = 0.f) const;

    /// 世界轴对齐盒与视锥相交吗。Unity 的 `GeometryUtility.TestPlanesAABB`。
    ///
    /// 判据是"正顶点"：对每个平面取盒子上**沿法线方向最远**的那个角，它若已经
    /// 在平面背面，整个盒子就在锥外。
    ///
    /// ⚠️ 这是**保守**的：盒子很大、斜穿过视锥的一个角时，六个平面的正顶点
    /// 可能都在正面而盒子其实不与之相交 —— 也就是会**多画**不会漏画。Unity
    /// 的 `TestPlanesAABB` 同样是这个精度（`GeometryUtility` 文档里写了它是
    /// "conservative"）。漏画的代价（物体凭空消失）远大于多画。
    bool Intersects(const Bounds& bounds) const;

    /// 六个平面里有任何一个退化（法线长度接近 0）吗。
    ///
    /// 给调用方一个"这张矩阵是坏的"的信号：退化意味着上面的查询会恒真。
    bool HasDegeneratePlane() const;
};
