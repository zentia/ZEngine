#include "ScriptMesh.h"

#include <cmath>
#include <limits>

namespace
{
    /// 退化判据：叉积长度小于这个量就当"这个面没有法线"。
    ///
    /// 用**平方**长度比，省掉一次开方，也免得"很小的面"和"很大但很扁的面"被
    /// 同一个绝对阈值分开。
    constexpr float kDegenerateAreaSquared = 1.0e-12f;

    /// 归一化，长度为 0 时返回零向量（而不是 NaN）。
    Vector3 SafeNormalise(const Vector3& v)
    {
        const float squared = v.squaredLength();
        if (squared <= 0.0f)
        {
            return Vector3::ZERO;
        }
        return v * (1.0f / std::sqrt(squared));
    }
}  // namespace

Bounds Mesh::ComputeBounds() const
{
    if (m_Vertices.empty())
    {
        // 空网格退回零盒 —— 与 Unity 的 `default(Bounds)` 同形，也和
        // `BaseRenderer::GetLocalBounds` 的退回策略一致（那里的注释写了为什么
        // 是零盒而不是"单位立方体"）。
        return Bounds();
    }

    // 初值取"反的"：一个顶点都没装进去时 `min <= max` 不成立，能看出来。第一
    // 个顶点会同时把两个角拉到自己身上，所以不需要单独处理首元素。
    const float positive_infinity = std::numeric_limits<float>::max();
    Vector3 min_corner(positive_infinity, positive_infinity, positive_infinity);
    Vector3 max_corner(-positive_infinity, -positive_infinity, -positive_infinity);
    for (const Vector3& vertex : m_Vertices)
    {
        min_corner.makeFloor(vertex);
        max_corner.makeCeil(vertex);
    }

    Bounds result;
    result.SetMinMax(min_corner, max_corner);
    return result;
}

void Mesh::RecalculateNormals()
{
    m_Normals.assign(m_Vertices.size(), Vector3::ZERO);

    const std::size_t triangle_count = m_Triangles.size() / 3;
    for (std::size_t triangle = 0; triangle < triangle_count; ++triangle)
    {
        const int i0 = m_Triangles[triangle * 3 + 0];
        const int i1 = m_Triangles[triangle * 3 + 1];
        const int i2 = m_Triangles[triangle * 3 + 2];

        // 越界下标跳过而不是 clamp：clamp 会把"索引写错了"变成一个看起来合法
        // 的三角形，而这里要的是"错就是错"（顶点数不够时法线整体就是不对的）。
        if (i0 < 0 || i1 < 0 || i2 < 0 ||
            i0 >= static_cast<int>(m_Vertices.size()) || i1 >= static_cast<int>(m_Vertices.size()) ||
            i2 >= static_cast<int>(m_Vertices.size()))
        {
            continue;
        }

        const Vector3 edge0 = m_Vertices[i1] - m_Vertices[i0];
        const Vector3 edge1 = m_Vertices[i2] - m_Vertices[i0];
        const Vector3 face_normal = edge0.crossProduct(edge1);

        // 未归一化的叉积 = 2 * 面积 * 单位法线：面积大的面自然占更大权重。
        // 退化面（共线 / 重合）跳过 —— 它的法线没有方向，归一化会得到 NaN，而
        // NaN 一旦加进某个顶点就再也清不掉。
        if (face_normal.squaredLength() < kDegenerateAreaSquared)
        {
            continue;
        }

        m_Normals[i0] += face_normal;
        m_Normals[i1] += face_normal;
        m_Normals[i2] += face_normal;
    }

    for (Vector3& normal : m_Normals)
    {
        normal = SafeNormalise(normal);
    }
}

void Mesh::RecalculateTangents()
{
    // 切线由"UV 的 u 方向在模型空间指哪"定义，缺法线或缺 UV 就没有定义。
    // 直接返回、法线数组留原样：拿一个猜出来的切线比没有更糟（法线贴图方向错
    // 是那种一眼看得出、但永远查不到原因的问题）。
    if (m_Normals.size() != m_Vertices.size() || m_UVs.size() != m_Vertices.size())
    {
        return;
    }

    std::vector<Vector3> tangents(m_Vertices.size(), Vector3::ZERO);
    std::vector<Vector3> bitangents(m_Vertices.size(), Vector3::ZERO);

    const std::size_t triangle_count = m_Triangles.size() / 3;
    for (std::size_t triangle = 0; triangle < triangle_count; ++triangle)
    {
        const int i0 = m_Triangles[triangle * 3 + 0];
        const int i1 = m_Triangles[triangle * 3 + 1];
        const int i2 = m_Triangles[triangle * 3 + 2];
        if (i0 < 0 || i1 < 0 || i2 < 0 ||
            i0 >= static_cast<int>(m_Vertices.size()) || i1 >= static_cast<int>(m_Vertices.size()) ||
            i2 >= static_cast<int>(m_Vertices.size()))
        {
            continue;
        }

        const Vector3& p0 = m_Vertices[i0];
        const Vector3& p1 = m_Vertices[i1];
        const Vector3& p2 = m_Vertices[i2];
        const Vector2& uv0 = m_UVs[i0];
        const Vector2& uv1 = m_UVs[i1];
        const Vector2& uv2 = m_UVs[i2];

        const Vector3 edge0 = p1 - p0;
        const Vector3 edge1 = p2 - p0;
        const float du0 = uv1.x - uv0.x;
        const float dv0 = uv1.y - uv0.y;
        const float du1 = uv2.x - uv0.x;
        const float dv1 = uv2.y - uv0.y;

        // 行列式为 0 = UV 退化（三点 UV 共线），这个面对切线没有贡献。
        const float determinant = du0 * dv1 - du1 * dv0;
        if (std::fabs(determinant) < 1.0e-12f)
        {
            continue;
        }
        const float reciprocal = 1.0f / determinant;

        const Vector3 tangent = (edge0 * dv1 - edge1 * dv0) * reciprocal;
        const Vector3 bitangent = (edge1 * du0 - edge0 * du1) * reciprocal;

        tangents[i0] += tangent;
        tangents[i1] += tangent;
        tangents[i2] += tangent;
        bitangents[i0] += bitangent;
        bitangents[i1] += bitangent;
        bitangents[i2] += bitangent;
    }

    m_Tangents.resize(m_Vertices.size());
    for (std::size_t vertex = 0; vertex < m_Vertices.size(); ++vertex)
    {
        const Vector3& normal = m_Normals[vertex];

        // Gram-Schmidt：把切线里"沿着法线"的那一份减掉，剩下的才真的在切平面里。
        // 不做这一步的话，斜面 / 曲面上切线会戳进面里，法线贴图会歪。
        Vector3 tangent = tangents[vertex] - normal * normal.dotProduct(tangents[vertex]);
        tangent = SafeNormalise(tangent);

        // w = ±1：副切线该往哪边翻。判据是 `cross(N, T)` 与累计副切线的点积符号
        // —— 这是 Unity 的 tangent.w 约定，shader 侧靠它决定手性。
        const float handedness = normal.crossProduct(tangent).dotProduct(bitangents[vertex]);
        m_Tangents[vertex] = Vector4(tangent.x, tangent.y, tangent.z, handedness < 0.0f ? -1.0f : 1.0f);
    }
}
