#include "Runtime/Core/Math/Frustum.h"

#include <cmath>

namespace
{
    /// 把 `(a, b, c, d)` 归一化成 `Plane`，使 `dot(normal, p) + distance == 0`
    /// 里的 normal 是单位向量。
    ///
    /// 零法线（退化）时原样返回 `(0,0,0,0)` —— 于是 `GetDistanceToPoint` 恒为 0，
    /// `GetSide` 恒为 true。见 Frustum.h 文件头"宁可多画不可闪掉"。
    Plane MakePlane(float a, float b, float c, float d)
    {
        const float len = std::sqrt(a * a + b * b + c * c);
        if (!(len > 1e-8f))
        {
            return Plane(Vector3::ZERO, 0.f);
        }
        return Plane(Vector3(a / len, b / len, c / len), d / len);
    }
}  // namespace

//-------------------------------------------------------------------------------
// Gribb-Hartmann：裁剪空间的六个不等式直接读成平面。
//
//   -w <= x <= w   =>  w + x >= 0 (left)   w - x >= 0 (right)
//   -w <= y <= w   =>  w + y >= 0 (bottom) w - y >= 0 (top)
//   深度见下
//
// 因为 `clip = M * world`，第 i 行的点积 `row_i · world` 就是 clip 的第 i 个分量
// （x=row0, y=row1, z=row2, w=row3）。所以"w + x"就是 `(row3 + row0) · world`。
//
// ⚠️ 每个平面都要**归一化**：不归一化的话 `GetDistanceToPoint` 返回的不是距离，
// 而 `Intersects` 只比较符号 —— 虽然符号碰巧还对，但 `ContainsPoint` 的
// `tolerance`（那是按世界单位给的）就完全失去意义了。
Frustum Frustum::FromViewProjection(const Matrix4x4& view_projection, DepthRange depth_range)
{
    const float (&m)[4][4] = view_projection.m_Mat;

    // 用 `int` 而不是 `float`：下标要求整型（C2108）。
    const int x = 0, y = 1, z = 2, w = 3;

    Frustum out;
    out.m_Planes[kLeft] = MakePlane(m[w][0] + m[x][0], m[w][1] + m[x][1], m[w][2] + m[x][2],
                                    m[w][3] + m[x][3]);
    out.m_Planes[kRight] = MakePlane(m[w][0] - m[x][0], m[w][1] - m[x][1], m[w][2] - m[x][2],
                                     m[w][3] - m[x][3]);
    out.m_Planes[kBottom] = MakePlane(m[w][0] + m[y][0], m[w][1] + m[y][1], m[w][2] + m[y][2],
                                      m[w][3] + m[y][3]);
    out.m_Planes[kTop] = MakePlane(m[w][0] - m[y][0], m[w][1] - m[y][1], m[w][2] - m[y][2],
                                   m[w][3] - m[y][3]);

    // 只有这两条随深度区间变。
    if (depth_range == DepthRange::kZeroToOne)
    {
        // 0 <= z        =>  row2 · world >= 0
        // z <= w        =>  (row3 - row2) · world >= 0
        out.m_Planes[kNear] = MakePlane(m[z][0], m[z][1], m[z][2], m[z][3]);
        out.m_Planes[kFar] = MakePlane(m[w][0] - m[z][0], m[w][1] - m[z][1], m[w][2] - m[z][2],
                                       m[w][3] - m[z][3]);
    }
    else
    {
        // -w <= z       =>  (row3 + row2) · world >= 0
        //  z <= w       =>  (row3 - row2) · world >= 0
        out.m_Planes[kNear] = MakePlane(m[w][0] + m[z][0], m[w][1] + m[z][1], m[w][2] + m[z][2],
                                        m[w][3] + m[z][3]);
        out.m_Planes[kFar] = MakePlane(m[w][0] - m[z][0], m[w][1] - m[z][1], m[w][2] - m[z][2],
                                       m[w][3] - m[z][3]);
    }
    return out;
}

//-------------------------------------------------------------------------------
bool Frustum::ContainsPoint(const Vector3& point, float tolerance) const
{
    for (int i = 0; i < kPlaneCount; ++i)
    {
        if (m_Planes[i].GetDistanceToPoint(point) + tolerance < 0.f)
        {
            return false;
        }
    }
    return true;
}

//-------------------------------------------------------------------------------
bool Frustum::Intersects(const Bounds& bounds) const
{
    const Vector3 min_corner = bounds.GetMin();
    const Vector3 max_corner = bounds.GetMax();

    for (int i = 0; i < kPlaneCount; ++i)
    {
        const Vector3& n = m_Planes[i].normal;
        // 沿法线方向最远的那个角：normal 分量为正就取 max，为负就取 min。
        // （这是 AABB 对平面求"最正面点"的标准写法，八个角不需要都测。）
        const Vector3 positive_vertex(
            n.x >= 0.f ? max_corner.x : min_corner.x,
            n.y >= 0.f ? max_corner.y : min_corner.y,
            n.z >= 0.f ? max_corner.z : min_corner.z);

        // ⚠️ 只判这**一个**点：它若已在背面，盒子上所有点都在背面。
        // 反过来（它在正面）不代表盒子相交 —— 这就是"保守"的那一半，见头文件。
        if (m_Planes[i].GetDistanceToPoint(positive_vertex) < 0.f)
        {
            return false;
        }
    }
    return true;
}

//-------------------------------------------------------------------------------
bool Frustum::HasDegeneratePlane() const
{
    for (int i = 0; i < kPlaneCount; ++i)
    {
        if (m_Planes[i].normal.length() < 1e-6f)
        {
            return true;
        }
    }
    return false;
}
