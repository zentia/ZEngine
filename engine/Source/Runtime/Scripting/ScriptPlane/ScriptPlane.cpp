#include "Runtime/Scripting/ScriptPlane/ScriptPlane.h"

#include "Runtime/Core/Math/Frustum.h"

#include <cstdio>

namespace Runtime
{

    // =====================================================================
    // Plane
    // =====================================================================

    ::Plane Plane::Create(const Vector3& normal, float distance)
    {
        return ::Plane(normal, distance);
    }

    ::Plane Plane::FromNormalAndPosition(const Vector3& normal, const Vector3& point)
    {
        // ⚠️ 走 `::Plane(normal, point)` 那个构造函数：它内部是
        // SetNormalAndPosition，会归一化法线。写 `::Plane(normal, -normal.dot(point))`
        // 的话法线没归一化，后面的 GetDistanceToPoint 会整体缩放 —— 平面还是
        // 那个平面，但"距离"不是距离了，而没有任何一处会报错。
        return ::Plane(normal, point);
    }

    ::Plane Plane::FromPoints(const Vector3& a, const Vector3& b, const Vector3& c)
    {
        ::Plane out;
        out.Set3Points(a, b, c);
        return out;
    }

    float Plane::GetDistanceToPoint(const ::Plane& plane, const Vector3& point)
    {
        return plane.GetDistanceToPoint(point);
    }

    bool Plane::GetSide(const ::Plane& plane, const Vector3& point) { return plane.GetSide(point); }

    bool Plane::SameSide(const ::Plane& plane, const Vector3& a, const Vector3& b)
    {
        return plane.SameSide(a, b);
    }

    Vector3 Plane::GetClosestPointOnPlane(const ::Plane& plane, const Vector3& point)
    {
        return plane.GetClosestPointOnPlane(point);
    }

    float Plane::Raycast(const ::Plane& plane, const Vector3& origin, const Vector3& direction)
    {
        float distance = 0.f;
        // TS 没有 out 参数，所以"没打中"用 **-1** 表达（Unity 用 bool 返回值）。
        // 用 0 表达"没打中"会和"正好打在起点上"撞车。
        if (!plane.Raycast(origin, direction, distance))
        {
            return -1.f;
        }
        return distance;
    }

    ::Plane Plane::WithNormalAndPosition(const ::Plane& plane, const Vector3& normal,
                                         const Vector3& point)
    {
        ::Plane out = plane;
        out.SetNormalAndPosition(normal, point);
        return out;
    }

    ::Plane Plane::With3Points(const ::Plane& plane, const Vector3& a, const Vector3& b,
                               const Vector3& c)
    {
        ::Plane out = plane;
        out.Set3Points(a, b, c);
        return out;
    }

    ::Plane Plane::Flipped(const ::Plane& plane)
    {
        ::Plane out = plane;
        out.Flip();
        return out;
    }

    ::Plane Plane::Translated(const ::Plane& plane, const Vector3& translation)
    {
        ::Plane out = plane;
        out.Translate(translation);
        return out;
    }

    ::Plane Plane::Normalized(const ::Plane& plane)
    {
        ::Plane out = plane;
        out.Normalize();
        return out;
    }

    eastl::string Plane::ToString(const ::Plane& plane)
    {
        char buffer[192];
        std::snprintf(buffer, sizeof(buffer), "(normal:(%.2f, %.2f, %.2f), distance:%.2f)",
                      plane.normal.x, plane.normal.y, plane.normal.z, plane.distance);
        return eastl::string(buffer);
    }

    // =====================================================================
    // GeometryUtility
    // =====================================================================

    std::vector<::Plane> GeometryUtility::CalculateFrustumPlanes(Camera* camera)
    {
        // ⚠️ 拿不到相机就返回**空**数组，不是 6 个退化平面：后者会让后面的
        // TestPlanesAABB 恒真（"什么都看得见"），而那是静默的。
        if (camera == nullptr || camera->GetRenderCamera() == nullptr)
        {
            return std::vector<::Plane>();
        }
        const Matrix4x4 view = camera->GetWorldToCameraMatrix();
        const Matrix4x4 projection = camera->GetProjectionMatrix();
        // `clip = projection * view * world`：`operator*` 是 concatenate（行主序
        // 的 A·B），所以顺序是 projection.concatenate(view)，不是反过来。
        return CalculateFrustumPlanesVP(projection * view);
    }

    std::vector<::Plane> GeometryUtility::CalculateFrustumPlanesVP(const Matrix4x4& view_projection)
    {
        const Frustum frustum = Frustum::FromViewProjection(view_projection,
                                                           Frustum::DepthRange::kZeroToOne);
        std::vector<::Plane> out;
        out.reserve(static_cast<std::size_t>(Frustum::kPlaneCount));
        // 顺序与 Unity 的 `CalculateFrustumPlanes` 一致：左 / 右 / 下 / 上 /
        // 近 / 远。
        for (int i = 0; i < Frustum::kPlaneCount; ++i)
        {
            out.push_back(frustum.m_Planes[i]);
        }
        return out;
    }

    bool GeometryUtility::TestPlanesAABB(const std::vector<::Plane>& planes, const ::Bounds& bounds)
    {
        const Vector3 min_corner = bounds.GetMin();
        const Vector3 max_corner = bounds.GetMax();

        for (const ::Plane& plane : planes)
        {
            const Vector3& n = plane.normal;
            // 沿法线方向最远的那个角（AABB 对平面求"最正面点"的标准写法，
            // 八个角不需要都测）。
            const Vector3 positive_vertex(n.x >= 0.f ? max_corner.x : min_corner.x,
                                          n.y >= 0.f ? max_corner.y : min_corner.y,
                                          n.z >= 0.f ? max_corner.z : min_corner.z);
            // ⚠️ 只判这**一个**点：它若已在背面，盒子上所有点都在背面。反过来
            // （它在正面）不代表盒子相交 —— 这就是"保守"的那一半。
            if (plane.GetDistanceToPoint(positive_vertex) < 0.f)
            {
                return false;
            }
        }
        // 没有任何平面能把它判到外面去 —— 包括 `planes` 为空的情况。
        return true;
    }

}  // namespace Runtime
