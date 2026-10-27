#pragma once

#include "NaniteTypes.h"
#include "NanitePagePool.h"
#include "Runtime/Core/Math/AxisAligned.h"
// BuildClustersFromMesh takes std::vector<Vector2> **by value**, so every caller
// needs the complete type, not just a forward declaration.
#include "Runtime/Core/Math/Vector2.h"

#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

class RHI;
class RenderResourceBase;

// 按 mesh.geometry_codec 解码第 index 个顶点位置。Raw 直接读 Vector3；Quantized 用
// quant_min/quant_scale 反量化。供渲染器与 smoke test 统一读取，屏蔽编码细节。
Vector3 NaniteDecodeVertex(const NaniteMeshResource& mesh, uint32_t index);

// 按 mesh.geometry_codec 解码第 index 个索引（Raw: uint32；Quantized: uint16）。
uint32_t NaniteDecodeIndex(const NaniteMeshResource& mesh, uint32_t index);

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

    // 计算集群的错误度量。只取决于集群本身的空间尺度：分割是空间的二分，
    // 所以越细的集群半径越小、误差越小，父节点天然 >= 子节点。
    float CalculateClusterError(const NaniteCluster& cluster) const;

private:
    // 递归分割网格为集群。
    //
    // 返回本次调用为**当前节点**创建的集群索引（kInvalidClusterIndex 表示这个节点
    // 没有任何三角形）。分割产生的内部节点在它的子节点都建好之后才创建（后序），
    // 这样它能把自己子树里那段连续的几何区间聚合成一个 LOD 节点。
    uint32_t RecursiveClusterSplit(const std::vector<Vector3>& positions,
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
    bool Initialize(RHI* rhi, const NaniteConfig& config);

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

    // 写入本帧的可见集群（裁剪结果）。以前这个字段从来没人写过，所以永远为空。
    void SetVisibleClusters(uint64_t mesh_id, std::vector<uint32_t> visible_clusters);

    // 获取整个网格资源（渲染器要按簇遍历它）
    const NaniteMeshResource* GetMeshResource(uint64_t mesh_id) const;

    uint32_t GetClusterCount(uint64_t mesh_id) const;

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

    RHI* m_Rhi;
    NaniteConfig m_Config;

    std::unordered_map<uint64_t, MeshResourceEntry> m_MeshResources;

    // GPU缓冲区
    void* m_ClusterDataBuffer = nullptr;
    void* m_VertexDataBuffer = nullptr;
    void* m_IndexDataBuffer = nullptr;

    // 流式加载队列
    std::vector<std::pair<uint64_t, uint32_t>> m_StreamingQueue;

    // 页池簿记层（step 2.5 后续）。streaming_pool_budget_bytes > 0 时启用：UpdateStreaming
    // 把每个叶子页的 (page_id, byte_size, distance) 喂进来，池子按"距离优先 + LRU"在字节
    // 预算内决定谁驻留，装不下的远页被驱逐。cluster_loaded_flags 随池子的驻留位同步——
    // 这样 IsClusterLoaded 反映的是"进没进预算池"，不只是"近不近"。页 id 用全局自增计数。
    nanite_pages::PagePool m_PagePool{1};
    uint32_t m_NextPageId{1};
    uint64_t m_StreamingFrame{0};  // 页池 LRU 时间戳（每帧 UpdateStreaming 自增）
    // mesh_id → 该 mesh 每个叶子 cluster 的 page_id（RegisterMeshResource 时分配）。
    std::unordered_map<uint64_t, std::vector<uint32_t>> m_ClusterPageIds;
};