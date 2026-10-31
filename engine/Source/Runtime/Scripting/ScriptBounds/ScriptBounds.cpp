#include "Runtime/Scripting/ScriptBounds/ScriptBounds.h"

#include <cstdio>

// TS 侧没有 out 参数，`IntersectRay` 用它表示"没打中"。负数而不是 NaN：NaN
// 会一路传播进脚本的 `if (d > 0)` 比较里并且静默为 false，看起来和"打中了但
// 距离很近"没法区分。
namespace
{
    constexpr float kRayMissDistance = -1.0f;
}  // namespace

namespace Runtime
{
    ::Bounds Bounds::Create(const Vector3& center, const Vector3& size)
    {
        return ::Bounds::FromCenterSize(center, size);
    }

    ::Bounds Bounds::FromMinMax(const Vector3& min, const Vector3& max)
    {
        return ::Bounds::FromMinMax(min, max);
    }

    Vector3 Bounds::GetSize(const ::Bounds& bounds)
    {
        return bounds.GetSize();
    }

    Vector3 Bounds::GetMin(const ::Bounds& bounds)
    {
        return bounds.GetMin();
    }

    Vector3 Bounds::GetMax(const ::Bounds& bounds)
    {
        return bounds.GetMax();
    }

    bool Bounds::Contains(const ::Bounds& bounds, const Vector3& point)
    {
        return bounds.Contains(point);
    }

    bool Bounds::Intersects(const ::Bounds& a, const ::Bounds& b)
    {
        return a.Intersects(b);
    }

    Vector3 Bounds::ClosestPoint(const ::Bounds& bounds, const Vector3& point)
    {
        return bounds.ClosestPoint(point);
    }

    float Bounds::SqrDistance(const ::Bounds& bounds, const Vector3& point)
    {
        return bounds.SqrDistance(point);
    }

    float Bounds::IntersectRay(const ::Bounds& bounds, ::Ray* ray)
    {
        if (ray == nullptr)
        {
            return kRayMissDistance;
        }

        float distance = 0.0f;
        if (!bounds.IntersectRay(ray->GetOrigin(), ray->GetDirection(), distance))
        {
            return kRayMissDistance;
        }
        return distance;
    }

    ::Bounds Bounds::Encapsulate(const ::Bounds& bounds, const Vector3& point)
    {
        ::Bounds out = bounds;
        out.Encapsulate(point);
        return out;
    }

    ::Bounds Bounds::EncapsulateBounds(const ::Bounds& bounds, const ::Bounds& other)
    {
        ::Bounds out = bounds;
        out.Encapsulate(other);
        return out;
    }

    ::Bounds Bounds::Expand(const ::Bounds& bounds, float amount)
    {
        ::Bounds out = bounds;
        out.Expand(amount);
        return out;
    }

    ::Bounds Bounds::ExpandVector(const ::Bounds& bounds, const Vector3& amount)
    {
        ::Bounds out = bounds;
        out.Expand(amount);
        return out;
    }

    std::string Bounds::ToString(const ::Bounds& bounds)
    {
        char buffer[128];
        std::snprintf(buffer, sizeof(buffer), "Center: (%.1f, %.1f, %.1f), Extents: (%.1f, %.1f, %.1f)",
                      bounds.center.x, bounds.center.y, bounds.center.z, bounds.extents.x, bounds.extents.y,
                      bounds.extents.z);
        return std::string(buffer);
    }
}  // namespace Runtime
