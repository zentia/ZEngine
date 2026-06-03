#pragma once

#include "NaniteTypes.h"
#include "Runtime/Core/Math/AxisAligned.h"

#include <memory>
#include <vector>

class RHI;
class RenderResourceBase;

// 集群构建器 - 负责将传统网格转换为Nanite集群
class NaniteClusterBuilder
{
public:
    NaniteClusterBuilder() = default;
    ~NaniteClusterBuilder() = default;

    // 从传统网格数据构建Nanite集群
    bool BuildClustersFromMesh(const std::vector<Vector3>& positions,
                               const std::vector<Vector3>& normals,
                               const std::vector<Vector2>& uvs,
                               const std::vector<uint32_t>& indices,
                               NaniteMeshResource& out_mesh_resource,
                               const NaniteConfig& config);

    // 构建层次结构
    bool BuildHierarchy(NaniteMeshResource& mesh_resource);

    // 计算集群的错误度量
    float CalculateClusterError(const NaniteCluster& cluster, const NaniteMeshResource& mesh_resource);

private:
    // 递归分割网格为集群
    void RecursiveClusterSplit(const std::vector<Vector3>& positions,
                               const std::vector<uint32_t>& indices,
                               const AxisAlignedBox& bounds,
                               uint32_t depth,
                               uint32_t max_depth,
                               std::vector<NaniteCluster>& out_clusters,
                               std::vector<Vector3>& out_positions,
                               std::vector<uint32_t>& out_indices);

    // 计算集群边界
    void CalculateClusterBounds(const std::vector<Vector3>& positions,
                                const std::vector<uint32_t>& indices,
                                uint32_t start_index,
                                uint32_t count,
                                NaniteCluster& cluster);

    // 优化集群（合并小集群）
    void OptimizeClusters(NaniteMeshResource& mesh_resource);

    // 验证集群有效性
    bool ValidateCluster(const NaniteCluster& cluster);
};

// 集群管理器 - 管理集群的加载、卸载和流式传输
class NaniteClusterManager
{
public:
    NaniteClusterManager();
    ~NaniteClusterManager();

    // 初始化
    bool Initialize(std::shared_ptr<RHI> rhi, const NaniteConfig& config);

    // 清理
    void clear();

    // 注册网格资源
    bool RegisterMeshResource(uint64_t mesh_id, const NaniteMeshResource& mesh_resource);

    // 注销网格资源
    void UnregisterMeshResource(uint64_t mesh_id);

    // 更新流式加载（根据相机位置）
    void UpdateStreaming(const Vector3& camera_position, float streaming_distance);

    // 获取可见集群
    const std::vector<uint32_t>& GetVisibleClusters(uint64_t mesh_id) const;

    // 检查集群是否已加载
    bool IsClusterLoaded(uint64_t mesh_id, uint32_t cluster_index) const;

    // 获取集群数据
    const NaniteCluster* GetCluster(uint64_t mesh_id, uint32_t cluster_index) const;

private:
    struct MeshResourceEntry
    {
        NaniteMeshResource mesh_resource;
        std::vector<bool> cluster_loaded_flags;
        std::vector<uint32_t> visible_clusters;
    };

    std::shared_ptr<RHI> m_Rhi;
    NaniteConfig m_Config;

    std::unordered_map<uint64_t, MeshResourceEntry> m_MeshResources;

    // GPU缓冲区
    void* m_ClusterDataBuffer = nullptr;
    void* m_VertexDataBuffer = nullptr;
    void* m_IndexDataBuffer = nullptr;

    // 流式加载队列
    std::vector<std::pair<uint64_t, uint32_t>> m_StreamingQueue;
};