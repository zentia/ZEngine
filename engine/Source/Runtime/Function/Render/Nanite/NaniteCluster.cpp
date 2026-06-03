#include "NaniteCluster.h"

#include "NaniteTypes.h"
#include "Runtime/Core/Math/Math.h"
#include "Runtime/Function/Render/Interface/RHI.h"

#include <algorithm>
#include <cmath>

// ==================== NaniteClusterBuilder ====================

bool NaniteClusterBuilder::BuildClustersFromMesh(const std::vector<Vector3>& positions,
                                                 const std::vector<Vector3>& normals,
                                                 const std::vector<Vector2>& uvs,
                                                 const std::vector<uint32_t>& indices,
                                                 NaniteMeshResource& out_mesh_resource,
                                                 const NaniteConfig& config)
{
    if (positions.empty() || indices.empty())
    {
        return false;
    }

    out_mesh_resource.clusters.clear();
    out_mesh_resource.vertex_data.clear();
    out_mesh_resource.index_data.clear();

    // 计算全局边界
    Vector3 min_pos = positions[0];
    Vector3 max_pos = positions[0];
    for (const auto& pos : positions)
    {
        min_pos.makeFloor(pos);
        max_pos.makeCeil(pos);
    }
    out_mesh_resource.global_bounds = AxisAlignedBox((min_pos + max_pos) * 0.5f, (max_pos - min_pos) * 0.5f);

    // 递归分割网格
    std::vector<Vector3> cluster_positions;
    std::vector<uint32_t> cluster_indices;
    RecursiveClusterSplit(positions,
                          indices,
                          out_mesh_resource.global_bounds,
                          0,
                          NaniteConstants::kMaxHierarchyDepth,
                          out_mesh_resource.clusters,
                          cluster_positions,
                          cluster_indices);

    // 构建层次结构
    if (!BuildHierarchy(out_mesh_resource))
    {
        return false;
    }

    // 优化集群
    OptimizeClusters(out_mesh_resource);

    // 计算统计信息
    out_mesh_resource.total_vertex_count = static_cast<uint32_t>(cluster_positions.size());
    out_mesh_resource.total_triangle_count = static_cast<uint32_t>(cluster_indices.size() / 3);
    out_mesh_resource.total_cluster_count = static_cast<uint32_t>(out_mesh_resource.clusters.size());

    // 压缩并存储几何数据
    // TODO: 实现几何压缩

    return true;
}

void NaniteClusterBuilder::RecursiveClusterSplit(const std::vector<Vector3>& positions,
                                                 const std::vector<uint32_t>& indices,
                                                 const AxisAlignedBox& bounds,
                                                 uint32_t depth,
                                                 uint32_t max_depth,
                                                 std::vector<NaniteCluster>& out_clusters,
                                                 std::vector<Vector3>& out_positions,
                                                 std::vector<uint32_t>& out_indices)
{
    if (depth >= max_depth)
    {
        return;
    }

    // 如果三角形数量足够少，创建单个集群
    uint32_t triangle_count = static_cast<uint32_t>(indices.size() / 3);
    if (triangle_count <= NaniteConstants::kMaxTrianglesPerCluster)
    {
        NaniteCluster cluster;
        cluster.vertex_count = 0;
        cluster.triangle_count = triangle_count;
        cluster.vertex_offset = static_cast<uint32_t>(out_positions.size());
        cluster.index_offset = static_cast<uint32_t>(out_indices.size());
        cluster.lod_level = static_cast<uint8_t>(depth);
        cluster.parent_cluster_index = UINT32_MAX;
        cluster.material_id = 0;
        cluster.is_visible = false;
        cluster.is_loaded = false;

        // 计算边界
        CalculateClusterBounds(positions, indices, 0, static_cast<uint32_t>(indices.size()), cluster);

        // 复制几何数据
        for (uint32_t i = 0; i < indices.size(); i += 3)
        {
            uint32_t idx0 = indices[i];
            uint32_t idx1 = indices[i + 1];
            uint32_t idx2 = indices[i + 2];

            // 检查顶点是否已存在（简单去重）
            // TODO: 实现更高效的顶点去重

            out_indices.push_back(static_cast<uint32_t>(out_positions.size()));
            out_positions.push_back(positions[idx0]);
            cluster.vertex_count++;

            out_indices.push_back(static_cast<uint32_t>(out_positions.size()));
            out_positions.push_back(positions[idx1]);
            cluster.vertex_count++;

            out_indices.push_back(static_cast<uint32_t>(out_positions.size()));
            out_positions.push_back(positions[idx2]);
            cluster.vertex_count++;
        }

        uint32_t cluster_index = static_cast<uint32_t>(out_clusters.size());
        out_clusters.push_back(cluster);
        return;
    }

    // 否则，分割边界框
    Vector3 center = bounds.getCenter();
    Vector3 half_extent = bounds.getHalfExtent();

    // 找到最长的轴
    float max_extent = Vector3::GetMaxElement(half_extent);
    int split_axis = 0;
    if (half_extent.y > half_extent.x && half_extent.y > half_extent.z)
        split_axis = 1;
    else if (half_extent.z > half_extent.x)
        split_axis = 2;

    // 分割
    float split_pos = center[split_axis];
    std::vector<uint32_t> left_indices, right_indices;

    for (uint32_t i = 0; i < indices.size(); i += 3)
    {
        Vector3 v0 = positions[indices[i]];
        Vector3 v1 = positions[indices[i + 1]];
        Vector3 v2 = positions[indices[i + 2]];

        // 简单的分割策略：根据三角形中心点
        Vector3 tri_center = (v0 + v1 + v2) / 3.0f;
        if (tri_center[split_axis] < split_pos)
        {
            left_indices.push_back(indices[i]);
            left_indices.push_back(indices[i + 1]);
            left_indices.push_back(indices[i + 2]);
        }
        else
        {
            right_indices.push_back(indices[i]);
            right_indices.push_back(indices[i + 1]);
            right_indices.push_back(indices[i + 2]);
        }
    }

    // 递归处理左右两部分
    if (!left_indices.empty())
    {
        Vector3 left_min = center;
        left_min[split_axis] = center[split_axis] - half_extent[split_axis];
        Vector3 left_max = center;
        left_max[split_axis] = split_pos;
        AxisAlignedBox left_bounds((left_min + left_max) * 0.5f, (left_max - left_min) * 0.5f);

        RecursiveClusterSplit(
            positions, left_indices, left_bounds, depth + 1, max_depth, out_clusters, out_positions, out_indices);
    }

    if (!right_indices.empty())
    {
        Vector3 right_min = center;
        right_min[split_axis] = split_pos;
        Vector3 right_max = center;
        right_max[split_axis] = center[split_axis] + half_extent[split_axis];
        AxisAlignedBox right_bounds((right_min + right_max) * 0.5f, (right_max - right_min) * 0.5f);

        RecursiveClusterSplit(
            positions, right_indices, right_bounds, depth + 1, max_depth, out_clusters, out_positions, out_indices);
    }
}

void NaniteClusterBuilder::CalculateClusterBounds(const std::vector<Vector3>& positions,
                                                  const std::vector<uint32_t>& indices,
                                                  uint32_t start_index,
                                                  uint32_t count,
                                                  NaniteCluster& cluster)
{
    if (count == 0)
        return;

    Vector3 min_pos = positions[indices[start_index]];
    Vector3 max_pos = positions[indices[start_index]];

    uint32_t end_index = start_index + count;
    for (uint32_t i = start_index; i < end_index && i < indices.size(); ++i)
    {
        const Vector3& pos = positions[indices[i]];
        min_pos.makeFloor(pos);
        max_pos.makeCeil(pos);
    }

    cluster.bounds = AxisAlignedBox((min_pos + max_pos) * 0.5f, (max_pos - min_pos) * 0.5f);
    cluster.center = cluster.bounds.getCenter();
    cluster.radius = (max_pos - min_pos).length() * 0.5f;
}

bool NaniteClusterBuilder::BuildHierarchy(NaniteMeshResource& mesh_resource)
{
    if (mesh_resource.clusters.empty())
        return false;

    // 简单的层次构建：根据LOD级别和空间位置
    // TODO: 实现更复杂的层次构建算法

    mesh_resource.root_cluster_index = 0;
    mesh_resource.hierarchy_depth = 0;

    for (size_t i = 0; i < mesh_resource.clusters.size(); ++i)
    {
        auto& cluster = mesh_resource.clusters[i];
        if (cluster.lod_level > mesh_resource.hierarchy_depth)
        {
            mesh_resource.hierarchy_depth = cluster.lod_level;
        }

        // 查找父集群（相同位置但LOD级别更低）
        cluster.parent_cluster_index = UINT32_MAX;
        for (size_t j = 0; j < mesh_resource.clusters.size(); ++j)
        {
            if (i != j && mesh_resource.clusters[j].lod_level < cluster.lod_level)
            {
                float distance = cluster.center.distance(mesh_resource.clusters[j].center);
                if (distance < cluster.radius * 2.0f)
                {
                    cluster.parent_cluster_index = static_cast<uint32_t>(j);
                    mesh_resource.clusters[j].child_cluster_indices.push_back(static_cast<uint32_t>(i));
                    break;
                }
            }
        }
    }

    return true;
}

float NaniteClusterBuilder::CalculateClusterError(const NaniteCluster& cluster, const NaniteMeshResource& mesh_resource)
{
    // 简单的错误度量：基于集群大小和LOD级别
    float base_error = cluster.radius * (1.0f + cluster.lod_level * 0.1f);
    return base_error;
}

void NaniteClusterBuilder::OptimizeClusters(NaniteMeshResource& mesh_resource)
{
    // TODO: 实现集群优化（合并小集群等）
}

bool NaniteClusterBuilder::ValidateCluster(const NaniteCluster& cluster)
{
    return cluster.vertex_count > 0 && cluster.triangle_count > 0 && cluster.radius > 0.0f;
}

// ==================== NaniteClusterManager ====================

NaniteClusterManager::NaniteClusterManager() = default;

NaniteClusterManager::~NaniteClusterManager()
{
    clear();
}

bool NaniteClusterManager::Initialize(RHI* rhi, const NaniteConfig& config)
{
    m_Rhi = rhi;
    m_Config = config;
    return true;
}

void NaniteClusterManager::clear()
{
    m_MeshResources.clear();
    m_StreamingQueue.clear();
}

bool NaniteClusterManager::RegisterMeshResource(uint64_t mesh_id, const NaniteMeshResource& mesh_resource)
{
    MeshResourceEntry entry;
    entry.mesh_resource = mesh_resource;
    entry.cluster_loaded_flags.resize(mesh_resource.clusters.size(), false);
    entry.visible_clusters.clear();

    m_MeshResources[mesh_id] = entry;
    return true;
}

void NaniteClusterManager::UnregisterMeshResource(uint64_t mesh_id)
{
    m_MeshResources.erase(mesh_id);
}

void NaniteClusterManager::UpdateStreaming(const Vector3& camera_position, float streaming_distance)
{
    // TODO: 实现流式加载逻辑
    m_StreamingQueue.clear();
}

const std::vector<uint32_t>& NaniteClusterManager::GetVisibleClusters(uint64_t mesh_id) const
{
    auto it = m_MeshResources.find(mesh_id);
    if (it != m_MeshResources.end())
    {
        return it->second.visible_clusters;
    }
    static std::vector<uint32_t> empty;
    return empty;
}

bool NaniteClusterManager::IsClusterLoaded(uint64_t mesh_id, uint32_t cluster_index) const
{
    auto it = m_MeshResources.find(mesh_id);
    if (it != m_MeshResources.end())
    {
        if (cluster_index < it->second.cluster_loaded_flags.size())
        {
            return it->second.cluster_loaded_flags[cluster_index];
        }
    }
    return false;
}

const NaniteCluster* NaniteClusterManager::GetCluster(uint64_t mesh_id, uint32_t cluster_index) const
{
    auto it = m_MeshResources.find(mesh_id);
    if (it != m_MeshResources.end())
    {
        if (cluster_index < it->second.mesh_resource.clusters.size())
        {
            return &it->second.mesh_resource.clusters[cluster_index];
        }
    }
    return nullptr;
}