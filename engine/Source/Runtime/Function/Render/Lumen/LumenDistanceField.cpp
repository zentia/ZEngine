#include "LumenDistanceField.h"

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Function/Render/Interface/RHI.h"
#include "Runtime/Function/Render/RenderHelper.h"
#include "Runtime/Function/Render/RenderResource.h"
#include "Runtime/Function/Render/RenderResourceBase.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

LumenDistanceField::LumenDistanceField() {}

LumenDistanceField::~LumenDistanceField()
{
    clear();
}

bool LumenDistanceField::Initialize(RHI* rhi, const LumenConfig& config)
{
    m_Rhi = rhi;
    m_Config = config;

    // 体素化还是 CPU 上的暴力实现，分辨率必须压住（见 kMaxCpuDistanceFieldResolution）
    const uint32_t requested_resolution = config.distance_field_resolution;
    const uint32_t resolution = std::max<uint32_t>(
        1, std::min<uint32_t>(requested_resolution, LumenConstants::kMaxCpuDistanceFieldResolution));
    if (resolution != requested_resolution)
    {
        LOG_WARNING(ZRender,
                    "lumenDistanceField: clamping distance field resolution from {} to {} (CPU voxelizer limit)",
                    requested_resolution,
                    resolution);
    }

    // 初始化距离场
    m_DistanceField.distance_field_id = 0;
    m_DistanceField.resolution_x = resolution;
    m_DistanceField.resolution_y = resolution;
    m_DistanceField.resolution_z = resolution;
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
    m_SceneGeometryCache.ranges.clear();
    m_SceneGeometryCache.transforms.clear();
    m_SceneGeometryCache.bounds.clear();
    m_SceneGeometryCache.geometry_hash = 0;
}

uint64_t LumenDistanceField::ComputeSceneGeometryHash(const std::vector<RenderEntity>& entities)
{
    // FNV-1a 64
    uint64_t hash = 1469598103934665603ull;
    const auto add_bytes = [&hash](const void* data, size_t size) {
        const unsigned char* bytes = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < size; ++i)
        {
            hash ^= static_cast<uint64_t>(bytes[i]);
            hash *= 1099511628211ull;
        }
    };

    const uint64_t entity_count = entities.size();
    add_bytes(&entity_count, sizeof(entity_count));

    for (const RenderEntity& entity : entities)
    {
        add_bytes(&entity.m_InstanceId, sizeof(entity.m_InstanceId));
        add_bytes(&entity.m_MeshAssetId, sizeof(entity.m_MeshAssetId));
        // 16 个 float 直接按字节喂进去：这是同一进程内的"变没变"判定，不需要跨机器
        // 稳定，位级比较正是我们想要的（NaN/±0 也不该被当成没变）
        add_bytes(entity.m_ModelMatrix.m_Mat, sizeof(entity.m_ModelMatrix.m_Mat));
    }

    return hash;
}

float LumenDistanceField::DistancePointToAxisAlignedBox(const Vector3& point, const AxisAlignedBox& box)
{
    // 无效盒子（默认构造：min=+FLT_MAX / max=-FLT_MAX）拿不到任何下界，返回 0 表示
    // "不做剪枝"，而不是让它算出 inf 把实例误判成很远。
    if (!box.IsValid())
    {
        return 0.0f;
    }

    const Vector3 min_corner = box.getMinCorner();
    const Vector3 max_corner = box.getMaxCorner();

    float squared_distance = 0.0f;
    for (int axis = 0; axis < 3; ++axis)
    {
        const float value = point[axis];
        const float lo = min_corner[axis];
        const float hi = max_corner[axis];
        if (value < lo)
        {
            const float delta = lo - value;
            squared_distance += delta * delta;
        }
        else if (value > hi)
        {
            const float delta = value - hi;
            squared_distance += delta * delta;
        }
    }

    return std::sqrt(squared_distance);
}

bool LumenDistanceField::CollectSceneGeometry(const std::vector<RenderEntity>& entities,
                                              RenderResourceBase* render_resource)
{
    if (render_resource == nullptr)
    {
        LOG_WARNING(ZRender, "lumenDistanceField: no render resource, cannot collect scene geometry");
        return false;
    }

    // 同一网格资产只往 positions/indices 里塞一次，后面的实例复用同一段区间
    std::unordered_map<size_t, SceneGeometryCache::MeshRange> range_by_mesh_asset;

    for (const RenderEntity& entity : entities)
    {
        const CpuMeshGeometry* geometry = render_resource->FindMeshGeometry(entity.m_MeshAssetId);
        if (geometry == nullptr || !geometry->IsValid())
        {
            continue;
        }

        const auto cached = range_by_mesh_asset.find(entity.m_MeshAssetId);
        if (cached != range_by_mesh_asset.end())
        {
            m_SceneGeometryCache.ranges.push_back(cached->second);
        }
        else
        {
            SceneGeometryCache::MeshRange range;
            range.vertex_offset = static_cast<uint32_t>(m_SceneGeometryCache.positions.size());
            range.vertex_count = static_cast<uint32_t>(geometry->positions.size());
            range.index_offset = static_cast<uint32_t>(m_SceneGeometryCache.indices.size());
            range.index_count = static_cast<uint32_t>(geometry->indices.size());

            m_SceneGeometryCache.positions.insert(m_SceneGeometryCache.positions.end(),
                                                  geometry->positions.begin(),
                                                  geometry->positions.end());
            m_SceneGeometryCache.indices.insert(m_SceneGeometryCache.indices.end(),
                                                geometry->indices.begin(),
                                                geometry->indices.end());

            m_SceneGeometryCache.ranges.push_back(range);
            range_by_mesh_asset.emplace(entity.m_MeshAssetId, range);
        }

        m_SceneGeometryCache.transforms.push_back(entity.m_ModelMatrix);

        // RenderEntity::m_BoundingBox 是局部空间的（可见性剔除那里也要用 m_ModelMatrix
        // 变换一遍才拿去和视锥比），这里同样变换成世界空间。网格资产加载失败的实例
        // 包围盒是无效的，退回用缓存里的几何包围盒。
        const AxisAlignedBox& local_bounds =
            entity.m_BoundingBox.IsValid() ? entity.m_BoundingBox : geometry->local_bounds;
        const BoundingBox local_box {local_bounds.getMinCorner(), local_bounds.getMaxCorner()};
        const BoundingBox world_box = BoundingBoxTransform(local_box, entity.m_ModelMatrix);

        AxisAlignedBox instance_bounds;
        instance_bounds.Merge(world_box.min_bound);
        instance_bounds.Merge(world_box.max_bound);
        m_SceneGeometryCache.bounds.push_back(instance_bounds);
    }

    return !m_SceneGeometryCache.ranges.empty();
}

bool LumenDistanceField::GenerateFromScene(std::shared_ptr<RenderScene> render_scene,
                                           RenderResourceBase* render_resource,
                                           const AxisAlignedBox& world_bounds)
{
    if (!render_scene)
    {
        return false;
    }

    const std::vector<RenderEntity>& entities = render_scene->m_RenderEntities;

    // 场景没变就不重建：体素化是 O(体素数 × 实例数)，每帧重跑毫无意义。
    // UpdateDistanceField 每帧都会被调用，这个短路是让它便宜的唯一办法。
    const uint64_t scene_hash = ComputeSceneGeometryHash(entities);
    if (m_DistanceField.is_loaded && !m_DistanceField.needs_update &&
        m_SceneGeometryCache.geometry_hash == scene_hash &&
        m_DistanceField.world_bounds.getMinCorner() == world_bounds.getMinCorner() &&
        m_DistanceField.world_bounds.getMaxCorner() == world_bounds.getMaxCorner())
    {
        return true;
    }

    // 设置世界空间边界
    m_DistanceField.world_bounds = world_bounds;

    // 收集场景几何数据
    m_SceneGeometryCache.positions.clear();
    m_SceneGeometryCache.indices.clear();
    m_SceneGeometryCache.ranges.clear();
    m_SceneGeometryCache.transforms.clear();
    m_SceneGeometryCache.bounds.clear();

    if (!CollectSceneGeometry(entities, render_resource))
    {
        LOG_WARNING(ZRender,
                    "lumenDistanceField: scene has no CPU-side mesh geometry ({} render entities); distance field left empty",
                    entities.size());
        m_DistanceField.distance_data.clear();
        m_DistanceField.hierarchy_levels.clear();
        m_DistanceField.is_loaded = false;
        return false;
    }

    m_SceneGeometryCache.geometry_hash = scene_hash;

    // 调用方没给世界边界（或给了个无效的）就从场景自身算一个：默认 ±1000 的盒子会让
    // 体素尺寸大到距离场毫无意义。
    if (!m_DistanceField.world_bounds.IsValid())
    {
        AxisAlignedBox grown_bounds;
        for (const AxisAlignedBox& instance_bounds : m_SceneGeometryCache.bounds)
        {
            if (!instance_bounds.IsValid())
            {
                continue;
            }
            grown_bounds.Merge(instance_bounds.getMinCorner());
            grown_bounds.Merge(instance_bounds.getMaxCorner());
        }

        // 留一格体素的余量，否则贴着边界的表面会落到边界体素的外沿
        if (grown_bounds.IsValid())
        {
            const Vector3 size = grown_bounds.getMaxCorner() - grown_bounds.getMinCorner();
            const float margin = std::max({size.x, size.y, size.z, 1.0f}) * 0.05f;
            grown_bounds.Merge(grown_bounds.getMinCorner() - Vector3(margin, margin, margin));
            grown_bounds.Merge(grown_bounds.getMaxCorner() + Vector3(margin, margin, margin));
        }

        m_DistanceField.world_bounds = grown_bounds;
    }

    if (!m_DistanceField.world_bounds.IsValid())
    {
        LOG_WARNING(ZRender, "lumenDistanceField: world bounds invalid; distance field left empty");
        m_DistanceField.distance_data.clear();
        m_DistanceField.is_loaded = false;
        return false;
    }

    // 生成距离场体素
    GenerateDistanceFieldVoxels(m_DistanceField.world_bounds,
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

void LumenDistanceField::UpdateDistanceField(std::shared_ptr<RenderScene> render_scene,
                                             RenderResourceBase* render_resource,
                                             const Vector3& camera_position)
{
    (void)camera_position;

    // 是否真的要重建由 GenerateFromScene 决定：它比较 geometry_hash 与世界边界，场景
    // 没变就直接返回。needs_update 只是"强制重建"开关（markForUpdate 会置上它）。
    GenerateFromScene(render_scene, render_resource, m_DistanceField.world_bounds);
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

    if (!bounds.IsValid())
    {
        LOG_WARNING(ZRender, "lumenDistanceField: voxelizer got invalid bounds; nothing to do");
        return;
    }

    Vector3 bounds_size = bounds.getMaxCorner() - bounds.getMinCorner();
    Vector3 voxel_size = Vector3(bounds_size.x / static_cast<float>(resolution_x),
                                 bounds_size.y / static_cast<float>(resolution_y),
                                 bounds_size.z / static_cast<float>(resolution_z));

    const size_t instance_count = m_SceneGeometryCache.GetInstanceCount();

    // 世界 -> 局部的矩阵每个实例只求一次逆。放在体素循环里的话，光矩阵求逆就能吃掉
    // 整个预算。
    std::vector<Matrix4x4> world_to_local(instance_count);
    for (size_t i = 0; i < instance_count; ++i)
    {
        world_to_local[i] = m_SceneGeometryCache.transforms[i].inverse();
    }

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

                // 遍历所有实例
                for (size_t instance_index = 0; instance_index < instance_count; ++instance_index)
                {
                    // 先拿世界空间 AABB 做一次下界剪枝：点到盒子的距离 <= 到盒内任意
                    // 三角形的距离，所以已经比当前最小值还远的实例可以整块跳过。
                    // 没有这一步的话体素化在大场景里是 O(体素数 × 全部三角面数)。
                    const float lower_bound =
                        DistancePointToAxisAlignedBox(voxel_center, m_SceneGeometryCache.bounds[instance_index]);
                    if (lower_bound >= min_distance)
                    {
                        continue;
                    }

                    const float distance = DistanceToMesh(voxel_center,
                                                          m_SceneGeometryCache.ranges[instance_index],
                                                          world_to_local[instance_index]);
                    min_distance = std::min(min_distance, distance);
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

    // 注意方向：这套重心坐标解（Ericson, Real-Time Collision Detection 5.1.5）里的
    // d / e 是 edge · (v0 - point)，不是 edge · (point - v0)。反过来写会让整个区域
    // 判定全部走错分支 —— 比如点正落在三角形内部时会退化成返回到 v0 的距离。
    Vector3 v0_minus_point = v0 - point;

    float a = edge0.dotProduct(edge0);
    float b = edge0.dotProduct(edge1);
    float c = edge1.dotProduct(edge1);
    float d = edge0.dotProduct(v0_minus_point);
    float e = edge1.dotProduct(v0_minus_point);

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

float LumenDistanceField::DistanceToMesh(const Vector3& world_point,
                                         const SceneGeometryCache::MeshRange& range,
                                         const Matrix4x4& world_to_local) const
{
    float min_distance = std::numeric_limits<float>::max();

    // 变换点到局部空间
    Vector3 local_point = world_to_local * world_point;

    const std::vector<Vector3>& positions = m_SceneGeometryCache.positions;
    const std::vector<uint32_t>& indices = m_SceneGeometryCache.indices;

    // 索引是相对本段几何的（资产内部的 0..n-1），要加上段起点才是拼接后的下标
    const uint32_t index_end = std::min<uint32_t>(range.index_offset + range.index_count,
                                                  static_cast<uint32_t>(indices.size()));
    const uint32_t vertex_end = std::min<uint32_t>(range.vertex_offset + range.vertex_count,
                                                   static_cast<uint32_t>(positions.size()));

    // 遍历所有三角形
    for (uint32_t i = range.index_offset; i + 2 < index_end; i += 3)
    {
        const uint32_t local_indices[3] = {indices[i], indices[i + 1], indices[i + 2]};

        Vector3 triangle[3];
        bool valid = true;
        for (size_t corner = 0; corner < 3; ++corner)
        {
            const uint32_t global_index = range.vertex_offset + local_indices[corner];
            if (global_index >= vertex_end)
            {
                valid = false;
                break;
            }
            triangle[corner] = positions[global_index];
        }
        if (!valid)
        {
            continue;
        }

        float distance = DistanceToTriangle(local_point, triangle[0], triangle[1], triangle[2]);
        min_distance = std::min(min_distance, distance);
    }

    return min_distance;
}

void LumenDistanceField::UpdateGPUResources()
{
    // TODO: 实现GPU资源更新
    // 将距离场数据上传到GPU纹理或缓冲区
}
