#pragma once

#include "Runtime/Core/Math/Vector3.h"

// =============================================================================
// Plane —— Unity 的 `UnityEngine.Plane`
// =============================================================================
//
// 平面方程是 `dot(normal, p) + distance == 0`。
//
// ⚠️ `distance` 的符号：Unity 的定义是"沿 normal **反方向**到原点的距离"取负，
// 也就是上面那个方程里的常数项。原点在**正面**（normal 指的那一侧）时
// `distance > 0`。这与"点到平面的有符号距离 = dot(normal, p) + distance"是自洽
// 的，别写成 `dot(normal, p) - distance` —— 那会让整个视锥的内外反过来，而
// `TestPlanesAABB` 依旧返回 bool，看不出方向错了。
//
// 默认构造是 **XZ 平面**（normal = +Z, distance = 0），与 Unity 的
// `default(Plane)` 一致（Unity 的 Plane 默认法线是 (0,0,1)）。不是"无效平面"：
// Unity 的 Plane 没有无效态。
struct Plane
{
    Vector3 normal{Vector3::UNIT_Z};
    float distance{0.f};

    Plane() = default;
    Plane(const Vector3& in_normal, float in_distance) : normal(in_normal), distance(in_distance) {}

    /// 由法线与平面上一点构造。法线**不要求**归一化 —— 构造时归一化。
    Plane(const Vector3& in_normal, const Vector3& in_point)
    {
        SetNormalAndPosition(in_normal, in_point);
    }

    /// 由三个点构造。绕序按右手定则决定法线（a -> b -> c）。
    /// 三点共线时退化：法线取零向量、distance 取 0（不产生 NaN）。
    Plane(const Vector3& a, const Vector3& b, const Vector3& c)
    {
        Set3Points(a, b, c);
    }

    // ---- 查询 ------------------------------------------------------------

    /// 到点的**有符号**距离。正 = 在正面（normal 指的那一侧）。
    /// Unity 的 `plane.GetDistanceToPoint(p)`。
    ///
    /// ⚠️ 只有在 normal 是单位向量时这才是真正的距离。本文件里所有会改 normal
    /// 的入口（`SetNormalAndPosition` / `Set3Points` / `FromViewProjection`）
    /// 都归一化过了；直接写 `plane.normal = v` 的话要自己保证。
    float GetDistanceToPoint(const Vector3& point) const
    {
        return normal.dotProduct(point) + distance;
    }

    /// 点在正面吗。**边界算正面**（`distance >= 0`），与 Unity 的
    /// `plane.GetSide` 一致（Unity 的 GetSide 是 `>= 0`）。
    bool GetSide(const Vector3& point) const
    {
        return GetDistanceToPoint(point) >= 0.f;
    }

    /// 两点在同一侧吗。边界上的点算两侧都在（与 `GetSide` 的 >= 一致）。
    bool SameSide(const Vector3& in_pt0, const Vector3& in_pt1) const
    {
        const float d0 = GetDistanceToPoint(in_pt0);
        const float d1 = GetDistanceToPoint(in_pt1);
        return (d0 >= 0.f && d1 >= 0.f) || (d0 < 0.f && d1 < 0.f);
    }

    /// 平面上离 `point` 最近的点（Unity 的 `plane.ClosestPointOnPlane`）。
    Vector3 GetClosestPointOnPlane(const Vector3& point) const
    {
        return point - normal * GetDistanceToPoint(point);
    }

    /// 射线与平面求交。返回是否命中；`out_distance` 是射线参数 t
    /// （`hit = origin + direction * t`），与 `RaycastHit.distance` 一样要拿
    /// `direction` 的长度换算成世界距离。
    ///
    /// 方向**不要求**归一化（t 因此不是世界距离，与 Unity 的
    /// `plane.Raycast` 一致）。
    ///
    /// 射线朝着背面（`dot(normal, direction) >= 0`）时返回 false：Unity 的
    /// `Plane.Raycast` 只打正面。平行于平面（denom == 0）同样 false —— 不产生
    /// NaN，也不把"贴着平面飞"算成命中。
    bool Raycast(const Vector3& origin, const Vector3& direction, float& out_distance) const
    {
        const float denom = normal.dotProduct(direction);
        if (denom >= -1e-6f)
        {
            return false;
        }
        out_distance = -GetDistanceToPoint(origin) / denom;
        return out_distance >= 0.f;
    }

    // ---- 就地修改（Unity 里这几个是 struct 的实例方法，改的是自己）--------

    /// Unity 的 `plane.SetNormalAndPosition`：法线归一化，常数项由平面上一点定。
    ///
    /// 零法线时退化成"法线取零、distance 取 0" —— 不产生 NaN。
    void SetNormalAndPosition(const Vector3& in_normal, const Vector3& in_point)
    {
        const float len = in_normal.length();
        if (len < 1e-6f)
        {
            normal = Vector3::ZERO;
            distance = 0.f;
            return;
        }
        normal = in_normal / len;
        distance = -normal.dotProduct(in_point);
    }

    /// Unity 的 `plane.Set3Points`。
    void Set3Points(const Vector3& a, const Vector3& b, const Vector3& c)
    {
        SetNormalAndPosition((b - a).crossProduct(c - a), a);
    }

    /// Unity 的 `plane.Flip`：法线取反，常数项跟着取反（平面本身不变）。
    void Flip()
    {
        normal = -normal;
        distance = -distance;
    }

    /// Unity 的 `plane.Translate`：整个平面沿 `translation` 平移。
    ///
    /// ⚠️ `distance` 的变化是 `-dot(normal, translation)`，不是
    /// `+dot(...)`。写反符号的话平面会往**反**方向跑，而它仍然是一个自洽的
    /// 平面（法线没变、仍然是平面），所以不会报任何错 —— 只会让"推着走的
    /// 裁剪面"往错误的一侧动。
    void Translate(const Vector3& translation)
    {
        distance -= normal.dotProduct(translation);
    }

    /// 归一化 `normal` 并把 `distance` 按同一个尺度缩放（保持平面不变）。
    ///
    /// 从矩阵里抽出来的平面（见 `Frustum::FromViewProjection`）已经归一化了；
    /// 这个是给手写 `normal` / `distance` 的场景用的。零法线时什么都不做。
    void Normalize()
    {
        const float len = normal.length();
        if (len < 1e-6f)
        {
            return;
        }
        normal = normal / len;
        distance = distance / len;
    }
};
