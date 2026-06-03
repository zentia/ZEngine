#include "LumenDistanceField.h"

#include "Runtime/Function/Render/Interface/RHI.h"
#include "Runtime/Function/Render/RenderResource.h"

#include <algorithm>
#include <cmath>
#include <limits>

LumenDistanceField::LumenDistanceField() {}

LumenDistanceField::~LumenDistanceField()
{
    clear();
}

bool LumenDistanceField::Initialize(std::shared_ptr<RHI> rhi, const LumenConfig& config)
{
    m_Rhi = rhi;
    m_Config = config;

    // 初始化距离场
    m_DistanceField.distance_field_id = 0;
    m_DistanceField.resolution_x = config.distance_field_resolution;
    m_DistanceField.resolution_y = config.distance_field_resolution;
    m_DistanceField.resolution_z = config.distance_field_resolution;
    m_DistanceField.type =
        config.use_hierarchical_distance_field ? DistanceFieldType::Hierarchical : DistanceFieldType::Global;
    m_DistanceField.needs_update = true;
    m_DistanceField.is_loaded = false;

    return true;
}

void LumenDistanceField::clear()
{
    // 清理GPU资源
    if (m_DistanceFieldTexture)
    {
        // TODO: 释放纹理资源
        m_DistanceFieldTexture = nullptr;
    }
    if (m_DistanceFieldBuffer)
    {
        // TODO: 释放缓冲区资源
        m_DistanceFieldBuffer = nullptr;
    }
    if (m_DistanceFieldSampler)
    {
        // TODO: 释放采样器资源
        m_DistanceFieldSampler = nullptr;
    }

    // 清理数据
    m_DistanceField.distance_data.clear();
    m_DistanceField.hierarchy_levels.clear();
    m_SceneGeometryCache.positions.clear();
    m_SceneGeometryCache.indices.clear();
    m_SceneGeometryCache.transforms.clear();
    m_SceneGeometryCache.bounds.clear();
}

bool LumenDistanceField::GenerateFromScene(std::shared_ptr<RenderScene> render_scene,
                                           const AxisAlignedBox& world_bounds)
{
    if (!render_scene)
    {
        return false;
    }

    // 设置世界空间边界
    m_DistanceField.world_bounds = world_bounds;

    // 收集场景几何数据
    m_SceneGeometryCache.positions.clear();
    m_SceneGeometryCache.indices.clear();
    m_SceneGeometryCache.transforms.clear();
    m_SceneGeometryCache.bounds.clear();

    // TODO: 从RenderScene获取网格数据
    // 这里需要访问RenderResource来获取实际的网格数据
    // 暂时使用占位实现

    // 生成距离场体素
    GenerateDistanceFieldVoxels(world_bounds,
                                m_DistanceField.resolution_x,
                                m_DistanceField.resolution_y,
                                m_DistanceField.resolution_z,
                                m_DistanceField.distance_data);

    // 如果需要，构建层次距离场
    if (m_Config.use_hierarchical_distance_field)
    {
        BuildHierarchicalDistanceField();
    }

    m_DistanceField.is_loaded = true;
    m_DistanceField.needs_update = false;

    // 更新GPU资源
    UpdateGPUResources();

    return true;
}

void LumenDistanceField::UpdateDistanceField(std::shared_ptr<RenderScene> render_scene, const Vector3& camera_position)
{
    // 检查是否需要更新
    if (!m_DistanceField.needs_update)
    {
        // 检查是否有动态几何变化
        // TODO: 实现增量更新逻辑
        return;
    }

    // 重新生成距离场
    if (m_DistanceField.world_bounds.IsValid())
    {
        GenerateFromScene(render_scene, m_DistanceField.world_bounds);
    }
}

float LumenDistanceField::QueryDistance(const Vector3& world_position) const
{
    if (!m_DistanceField.is_loaded || m_DistanceField.distance_data.empty())
    {
        return std::numeric_limits<float>::max();
    }

    // 检查是否在世界空间边界内
    if (!m_DistanceField.world_bounds.Contains(world_position))
    {
        return std::numeric_limits<float>::max();
    }

    // 计算体素坐标
    Vector3 local_pos = world_position - m_DistanceField.world_bounds.getMinCorner();
    Vector3 bounds_size = m_DistanceField.world_bounds.getMaxCorner() - m_DistanceField.world_bounds.getMinCorner();

    uint32_t x =
        static_cast<uint32_t>((local_pos.x / bounds_size.x) * static_cast<float>(m_DistanceField.resolution_x));
    uint32_t y =
        static_cast<uint32_t>((local_pos.y / bounds_size.y) * static_cast<float>(m_DistanceField.resolution_y));
    uint32_t z =
        static_cast<uint32_t>((local_pos.z / bounds_size.z) * static_cast<float>(m_DistanceField.resolution_z));

    // 限制在有效范围内
    x = std::min(x, m_DistanceField.resolution_x - 1);
    y = std::min(y, m_DistanceField.resolution_y - 1);
    z = std::min(z, m_DistanceField.resolution_z - 1);

    // 计算索引
    uint32_t index =
        z * m_DistanceField.resolution_x * m_DistanceField.resolution_y + y * m_DistanceField.resolution_x + x;

    if (index < m_DistanceField.distance_data.size())
    {
        return m_DistanceField.distance_data[index];
    }

    return std::numeric_limits<float>::max();
}

bool LumenDistanceField::QueryNearestSurface(const Vector3& world_position,
                                             Vector3& out_surface_position,
                                             Vector3& out_surface_normal) const
{
    float distance = QueryDistance(world_position);
    if (distance >= std::numeric_limits<float>::max() - 1.0f)
    {
        return false;
    }

    // 使用梯度计算法线
    const float epsilon = 0.1f;
    float dx = QueryDistance(world_position + Vector3(epsilon, 0.0f, 0.0f)) - distance;
    float dy = QueryDistance(world_position + Vector3(0.0f, epsilon, 0.0f)) - distance;
    float dz = QueryDistance(world_position + Vector3(0.0f, 0.0f, epsilon)) - distance;

    out_surface_normal = Vector3(dx, dy, dz).normalisedCopy();
    out_surface_position = world_position - out_surface_normal * distance;

    return true;
}

bool LumenDistanceField::BuildHierarchicalDistanceField()
{
    if (m_DistanceField.type != DistanceFieldType::Hierarchical)
    {
        return false;
    }

    // 构建层次级别（从粗到细）
    m_DistanceField.hierarchy_levels.clear();

    uint32_t current_resolution = m_DistanceField.resolution_x;
    uint32_t level_index = 0;

    while (current_resolution > 8)  // 最小分辨率
    {
        current_resolution /= 2;
        level_index++;

        LumenDistanceFieldData level;
        BuildHierarchyLevel(m_DistanceField, level_index, level);
        m_DistanceField.hierarchy_levels.push_back(level);
    }

    return true;
}

void LumenDistanceField::GenerateDistanceFieldVoxels(const AxisAlignedBox& bounds,
                                                     uint32_t resolution_x,
                                                     uint32_t resolution_y,
                                                     uint32_t resolution_z,
                                                     std::vector<float>& out_distance_data)
{
    out_distance_data.clear();
    out_distance_data.resize(resolution_x * resolution_y * resolution_z);

    Vector3 bounds_size = bounds.getMaxCorner() - bounds.getMinCorner();
    Vector3 voxel_size = Vector3(bounds_size.x / static_cast<float>(resolution_x),
                                 bounds_size.y / static_cast<float>(resolution_y),
                                 bounds_size.z / static_cast<float>(resolution_z));

    // 并行计算每个体素的距离值
    // TODO: 可以使用多线程加速
    for (uint32_t z = 0; z < resolution_z; ++z)
    {
        for (uint32_t y = 0; y < resolution_y; ++y)
        {
            for (uint32_t x = 0; x < resolution_x; ++x)
            {
                // 计算体素中心的世界空间位置
                Vector3 voxel_center = bounds.getMinCorner() + Vector3((static_cast<float>(x) + 0.5f) * voxel_size.x,
                                                                       (static_cast<float>(y) + 0.5f) * voxel_size.y,
                                                                       (static_cast<float>(z) + 0.5f) * voxel_size.z);

                // 计算到最近表面的距离
                float min_distance = std::numeric_limits<float>::max();

                // 遍历所有网格
                for (size_t mesh_idx = 0; mesh_idx < m_SceneGeometryCache.positions.size(); ++mesh_idx)
                {
                    // TODO: 实现实际的网格距离计算
                    // 这里需要访问实际的网格数据
                }

                // 存储距离值
                uint32_t index = z * resolution_x * resolution_y + y * resolution_x + x;
                out_distance_data[index] = min_distance;
            }
        }
    }
}

void LumenDistanceField::BuildHierarchyLevel(const LumenDistanceFieldData& parent_level,
                                             uint32_t level_index,
                                             LumenDistanceFieldData& out_level)
{
    // 构建下一级层次（分辨率减半）
    out_level.resolution_x = parent_level.resolution_x / 2;
    out_level.resolution_y = parent_level.resolution_y / 2;
    out_level.resolution_z = parent_level.resolution_z / 2;
    out_level.world_bounds = parent_level.world_bounds;
    out_level.distance_data.resize(out_level.resolution_x * out_level.resolution_y * out_level.resolution_z);

    // 下采样父级距离场
    for (uint32_t z = 0; z < out_level.resolution_z; ++z)
    {
        for (uint32_t y = 0; y < out_level.resolution_y; ++y)
        {
            for (uint32_t x = 0; x < out_level.resolution_x; ++x)
            {
                // 采样父级的8个邻居
                float sum_distance = 0.0f;
                int sample_count = 0;

                for (int dz = 0; dz < 2; ++dz)
                {
                    for (int dy = 0; dy < 2; ++dy)
                    {
                        for (int dx = 0; dx < 2; ++dx)
                        {
                            uint32_t px = x * 2 + dx;
                            uint32_t py = y * 2 + dy;
                            uint32_t pz = z * 2 + dz;

                            if (px < parent_level.resolution_x && py < parent_level.resolution_y &&
                                pz < parent_level.resolution_z)
                            {
                                uint32_t parent_index = pz * parent_level.resolution_x * parent_level.resolution_y +
                                                        py * parent_level.resolution_x + px;
                                if (parent_index < parent_level.distance_data.size())
                                {
                                    sum_distance += parent_level.distance_data[parent_index];
                                    sample_count++;
                                }
                            }
                        }
                    }
                }

                // 平均距离值
                uint32_t index = z * out_level.resolution_x * out_level.resolution_y + y * out_level.resolution_x + x;
                out_level.distance_data[index] =
                    sample_count > 0 ? sum_distance / static_cast<float>(sample_count) : 0.0f;
            }
        }
    }
}

float LumenDistanceField::DistanceToTriangle(const Vector3& point,
                                             const Vector3& v0,
                                             const Vector3& v1,
                                             const Vector3& v2) const
{
    // 计算点到三角形的最短距离
    Vector3 edge0 = v1 - v0;
    Vector3 edge1 = v2 - v0;
    Vector3 v0_to_point = point - v0;

    float a = edge0.dotProduct(edge0);
    float b = edge0.dotProduct(edge1);
    float c = edge1.dotProduct(edge1);
    float d = edge0.dotProduct(v0_to_point);
    float e = edge1.dotProduct(v0_to_point);

    float det = a * c - b * b;
    float s = b * e - c * d;
    float t = b * d - a * e;

    if (s + t < det)
    {
        if (s < 0.0f)
        {
            if (t < 0.0f)
            {
                // 区域4
                if (d < 0.0f)
                {
                    s = std::clamp(-d / a, 0.0f, 1.0f);
                    t = 0.0f;
                }
                else
                {
                    s = 0.0f;
                    t = std::clamp(-e / c, 0.0f, 1.0f);
                }
            }
            else
            {
                // 区域3
                s = 0.0f;
                t = std::clamp(-e / c, 0.0f, 1.0f);
            }
        }
        else if (t < 0.0f)
        {
            // 区域5
            s = std::clamp(-d / a, 0.0f, 1.0f);
            t = 0.0f;
        }
        else
        {
            // 区域0
            float inv_det = 1.0f / det;
            s *= inv_det;
            t *= inv_det;
        }
    }
    else
    {
        if (s < 0.0f)
        {
            // 区域2
            float tmp0 = b + d;
            float tmp1 = c + e;
            if (tmp1 > tmp0)
            {
                float numer = tmp1 - tmp0;
                float denom = a - 2.0f * b + c;
                s = std::clamp(numer / denom, 0.0f, 1.0f);
                t = 1.0f - s;
            }
            else
            {
                t = std::clamp(-e / c, 0.0f, 1.0f);
                s = 0.0f;
            }
        }
        else if (t < 0.0f)
        {
            // 区域6
            float tmp0 = b + d;
            float tmp1 = a + e;
            if (tmp1 > tmp0)
            {
                float numer = tmp1 - tmp0;
                float denom = a - 2.0f * b + c;
                t = std::clamp(numer / denom, 0.0f, 1.0f);
                s = 1.0f - t;
            }
            else
            {
                s = std::clamp(-d / a, 0.0f, 1.0f);
                t = 0.0f;
            }
        }
        else
        {
            // 区域1
            float numer = c + e - b - d;
            if (numer <= 0.0f)
            {
                s = 0.0f;
            }
            else
            {
                float denom = a - 2.0f * b + c;
                s = std::clamp(numer / denom, 0.0f, 1.0f);
            }
            t = 1.0f - s;
        }
    }

    Vector3 closest_point = v0 + s * edge0 + t * edge1;
    return (point - closest_point).length();
}

float LumenDistanceField::DistanceToMesh(const Vector3& point,
                                         const std::vector<Vector3>& positions,
                                         const std::vector<uint32_t>& indices,
                                         const Matrix4x4& transform) const
{
    float min_distance = std::numeric_limits<float>::max();

    // 变换点到局部空间
    Matrix4x4 inv_transform = transform.inverse();
    Vector3 local_point = inv_transform * point;

    // 遍历所有三角形
    for (size_t i = 0; i < indices.size(); i += 3)
    {
        if (i + 2 >= indices.size())
            break;

        uint32_t i0 = indices[i];
        uint32_t i1 = indices[i + 1];
        uint32_t i2 = indices[i + 2];

        if (i0 >= positions.size() || i1 >= positions.size() || i2 >= positions.size())
            continue;

        Vector3 v0 = positions[i0];
        Vector3 v1 = positions[i1];
        Vector3 v2 = positions[i2];

        float distance = DistanceToTriangle(local_point, v0, v1, v2);
        min_distance = std::min(min_distance, distance);
    }

    return min_distance;
}

void LumenDistanceField::UpdateGPUResources()
{
    // TODO: 实现GPU资源更新
    // 将距离场数据上传到GPU纹理或缓冲区
}
