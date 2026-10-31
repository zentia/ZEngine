#include "Runtime/Core/Math/Bounds.h"

#include <algorithm>
#include <limits>

bool Bounds::IntersectRay(const Vector3& in_origin, const Vector3& in_direction, float& out_distance) const
{
    out_distance = 0.0f;

    const Vector3 min = GetMin();
    const Vector3 max = GetMax();

    // slab 法：逐轴求"进入"和"离开"的射线参数，取交集。
    float t_enter = -std::numeric_limits<float>::max();
    float t_exit = std::numeric_limits<float>::max();

    const float origin[3] = {in_origin.x, in_origin.y, in_origin.z};
    const float dir[3] = {in_direction.x, in_direction.y, in_direction.z};
    const float lo[3] = {min.x, min.y, min.z};
    const float hi[3] = {max.x, max.y, max.z};

    for (int axis = 0; axis < 3; ++axis)
    {
        if (dir[axis] > -1e-8f && dir[axis] < 1e-8f)
        {
            // 这一轴上是**平行**的：不除（那会出 ±inf 或 NaN，NaN 参与下面的
            // min/max 会把整次判断带成"命中"），改成判断起点是否已经在板内。
            if (origin[axis] < lo[axis] || origin[axis] > hi[axis])
            {
                return false;
            }
            continue;
        }

        const float inv = 1.0f / dir[axis];
        float t_near = (lo[axis] - origin[axis]) * inv;
        float t_far = (hi[axis] - origin[axis]) * inv;
        if (t_near > t_far)
        {
            std::swap(t_near, t_far);
        }
        t_enter = std::max(t_enter, t_near);
        t_exit = std::min(t_exit, t_far);
        if (t_enter > t_exit)
        {
            return false;
        }
    }

    if (t_exit < 0.0f)
    {
        // 整个盒都在射线起点背后。
        return false;
    }

    // 起点已经在盒内时 t_enter < 0：Unity 返回距离 0，不是负的进入距离。
    out_distance = (t_enter >= 0.0f) ? t_enter : 0.0f;
    return true;
}
