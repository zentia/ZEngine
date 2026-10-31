#pragma once

#include "Runtime/Core/Math/Vector3.h"

// =============================================================================
// Bounds —— Unity 的 `UnityEngine.Bounds`
// =============================================================================
//
// 存的是 **center + extents**，其中 `extents` 是**半尺寸**（`size / 2`）。照
// Unity 的内部表示存，因为 `min` / `max` / `size` 都是它的函数，反过来存会在
// 改 `size` 时牵动中心点：Unity 的 `bounds.size = s` 保持中心不动，只有
// `SetMinMax` 才会挪中心。
//
// 与引擎自己的 `AxisAlignedBox` 的区别
// ------------------------------------
// `AxisAlignedBox` 缓存了 min/max 两个角（每次 `Update` 重算），**默认构造出来
// 是"反的"**（min = +FLT_MAX、max = -FLT_MAX，`IsValid() == false`），语义是
// "还没装进任何东西"。Unity 的 `default(Bounds)` 是中心原点、零尺寸的一个点盒。
// 这里是脚本面，要的是 Unity 的语义，所以两个都得是零。
//
// ⚠️ 默认构造是零盒而不是"无效盒"，代价是"空盒"和"原点上的一个点"没法区分。
// Unity 同样区分不了（`default(Bounds).Contains(Vector3.zero)` 是 true）。
struct Bounds
{
    Vector3 center{Vector3::ZERO};
    Vector3 extents{Vector3::ZERO};

    Bounds() = default;
    Bounds(const Vector3& in_center, const Vector3& in_extents) : center(in_center), extents(in_extents) {}

    // ---- 由 center/extents 派生的三个量 ----------------------------------

    /// 全尺寸（`extents * 2`）。Unity 的 `bounds.size`（可读可写）。
    Vector3 GetSize() const { return extents * 2.0f; }

    /// Unity 的 `bounds.min`（只读）。
    Vector3 GetMin() const { return center - extents; }

    /// Unity 的 `bounds.max`（只读）。
    Vector3 GetMax() const { return center + extents; }

    /// Unity 的 `bounds.size = s`：**中心不动**，半尺寸变成 s/2。
    void SetSize(const Vector3& size) { extents = size * 0.5f; }

    /// Unity 的 `bounds.SetMinMax`：中心与半尺寸都由两个角推出。
    void SetMinMax(const Vector3& min, const Vector3& max)
    {
        center = (min + max) * 0.5f;
        extents = (max - min) * 0.5f;
    }

    // ---- 查询 ------------------------------------------------------------

    /// Unity 的 `bounds.Contains`：边界**算包含**（`min <= p <= max`）。
    ///
    /// 边界算在内是 Unity 的行为（`default(Bounds).Contains(zero)` 为 true），
    /// 也是"点在盒面上"应该算命中的直觉。
    bool Contains(const Vector3& point) const
    {
        const Vector3 min = GetMin();
        const Vector3 max = GetMax();
        return point.x >= min.x && point.x <= max.x && point.y >= min.y && point.y <= max.y &&
               point.z >= min.z && point.z <= max.z;
    }

    /// Unity 的 `bounds.Intersects`：**贴面也算相交**（`min <= other.max`）。
    bool Intersects(const Bounds& other) const
    {
        const Vector3 min = GetMin();
        const Vector3 max = GetMax();
        const Vector3 other_min = other.GetMin();
        const Vector3 other_max = other.GetMax();
        return min.x <= other_max.x && max.x >= other_min.x && min.y <= other_max.y && max.y >= other_min.y &&
               min.z <= other_max.z && max.z >= other_min.z;
    }

    /// Unity 的 `bounds.ClosestPoint`：把点夹到盒上（在盒内就是它自己）。
    Vector3 ClosestPoint(const Vector3& point) const
    {
        const Vector3 min = GetMin();
        const Vector3 max = GetMax();
        Vector3 out = point;
        if (out.x < min.x)
            out.x = min.x;
        if (out.x > max.x)
            out.x = max.x;
        if (out.y < min.y)
            out.y = min.y;
        if (out.y > max.y)
            out.y = max.y;
        if (out.z < min.z)
            out.z = min.z;
        if (out.z > max.z)
            out.z = max.z;
        return out;
    }

    /// Unity 的 `bounds.SqrDistance`：到 `ClosestPoint` 的距离的**平方**。
    ///
    /// 给平方而不是距离是 Unity 的取舍：比较远近时省一次开方，而且盒内的点一律
    /// 得 0。
    float SqrDistance(const Vector3& point) const
    {
        const Vector3 delta = point - ClosestPoint(point);
        return delta.x * delta.x + delta.y * delta.y + delta.z * delta.z;
    }

    // ---- 就地修改（Unity 里这几个是 struct 的实例方法，改的是自己）--------

    /// Unity 的 `bounds.Encapsulate(point)`：把点包进来。
    void Encapsulate(const Vector3& point)
    {
        Vector3 min = GetMin();
        Vector3 max = GetMax();
        min.makeFloor(point);
        max.makeCeil(point);
        SetMinMax(min, max);
    }

    /// Unity 的 `bounds.Encapsulate(bounds)`：把另一个盒包进来。
    ///
    /// Unity 的实现是"包两个角"，不是"包 center"。两者对 AABB 等价，但**对
    /// 空盒不等价**：包 center 会漏掉另一个盒是零盒的情形。照 Unity 包角。
    void Encapsulate(const Bounds& other)
    {
        Encapsulate(other.GetMin());
        Encapsulate(other.GetMax());
    }

    /// Unity 的 `bounds.Expand(amount)`：⚠️ **先除以 2** 再加到 extents 上。
    ///
    /// `Expand(2)` 让 `size` 增加 2（不是 4），extents 只加 1。Unity 源码里就
    /// 是 `amount *= .5f; extents += new Vector3(amount, amount, amount);`。
    /// 漏掉这个 0.5 会得到"膨胀了两倍"的盒，而它仍然是自洽的（min/max/size 全
    /// 对），所以不会报任何错 —— 只会让触发体积莫名其妙地大一圈。
    void Expand(float amount)
    {
        amount *= 0.5f;
        extents = extents + Vector3(amount, amount, amount);
    }

    /// Unity 的 `bounds.Expand(Vector3)`：同样先除以 2，逐轴加到 extents。
    void Expand(const Vector3& amount)
    {
        extents = extents + amount * 0.5f;
    }

    // ---- 构造辅助 --------------------------------------------------------

    static Bounds FromCenterSize(const Vector3& center, const Vector3& size)
    {
        Bounds out;
        out.center = center;
        out.SetSize(size);
        return out;
    }

    static Bounds FromMinMax(const Vector3& min, const Vector3& max)
    {
        Bounds out;
        out.SetMinMax(min, max);
        return out;
    }

    // ---- 射线求交 --------------------------------------------------------

    /// Unity 的 `bounds.IntersectRay(ray, out distance)`。
    ///
    /// `in_direction` **不要求归一化**：返回的 `out_distance` 是射线参数 t
    /// （`hit = origin + direction * t`），所以非归一化方向下的 t 不是世界距离
    /// —— 这与 Unity 一致。
    ///
    /// 起点在盒内时返回 true 且 `out_distance = 0`（Unity 同）。
    /// 方向某分量为 0 时走"起点必须已经在该轴范围内"的分支（slab 法的标准写
    /// 法），不会因为除零产生 NaN 而假命中。
    ///
    /// 返回值：命中为 true，`out_distance >= 0`；不命中为 false。
    bool IntersectRay(const Vector3& in_origin, const Vector3& in_direction, float& out_distance) const;
};
