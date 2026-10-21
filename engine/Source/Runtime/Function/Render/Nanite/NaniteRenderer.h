#pragma once

#include "NaniteCluster.h"
#include "NaniteCulling.h"
#include "NaniteTypes.h"
#include "Runtime/Function/Render/RenderPassBase.h"

#include <memory>
#include <vector>

class RHI;
class RenderResourceBase;
class RenderScene;
class RenderCamera;
class RenderEntity;
struct CpuMeshGeometry;

// Nanite渲染器 - 负责Nanite几何的渲染
class NaniteRenderer
{
public:
    NaniteRenderer();
    ~NaniteRenderer();

    // 初始化
    bool Initialize(RHI* rhi,
                    std::shared_ptr<RenderResourceBase> render_resource,
                    const NaniteConfig& config);

    // 清理
    void clear();

    // 从渲染场景刷新实例与网格资源。
    //
    // 几何的唯一来源是 RenderResourceBase 的 CPU 网格缓存（CpuMeshGeometry），和 Lumen
    // 距离场同源 —— RenderMeshData 在上传完就被释放了，之后再也读不到顶点。
    //
    // 以前 NaniteRenderer **没有任何办法**拿到场景：实例只能靠外部手工 RegisterInstance，
    // 而全代码库一个调用者都没有，所以 Nanite 从来没在真实帧循环里跑过。
    //
    // 场景拓扑（实例 id → 网格资产 id 的集合）没变时只更新变换矩阵，不重建集群 ——
    // 集群构建是递归二分整个网格，每帧重跑毫无意义。
    // 返回是否有至少一个可用实例。
    bool UpdateFromScene(std::shared_ptr<RenderScene> render_scene, RenderResourceBase* render_resource);

    // 渲染一帧
    void Render(RHI* rhi,
                std::shared_ptr<RenderScene> render_scene,
                std::shared_ptr<RenderCamera> camera,
                const NaniteConfig& config);

    // 注册网格资源（集群构建器产出之后交给它）
    bool RegisterMeshResource(uint64_t mesh_id, const NaniteMeshResource& mesh_resource);

    // 注册实例。若调用方没填 local_bounds，这里从已注册的网格资源上取 ——
    // 否则视锥裁剪拿不到边界，只能保守地什么都不裁。
    uint64_t RegisterInstance(const NaniteInstance& instance);

    // 注销实例
    void UnregisterInstance(uint64_t instance_id);

    // 更新实例
    void UpdateInstance(uint64_t instance_id, const NaniteInstance& instance);

    // 获取配置
    const NaniteConfig& getConfig() const { return m_Config; }
    void SetConfig(const NaniteConfig& config) { m_Config = config; }

    // 上一帧的结果，供渲染通道（和测试）读取。
    const std::vector<NaniteRenderBatch>& GetRenderBatches() const { return m_RenderBatches; }
    const NaniteVisibilityResult& GetLastVisibilityResult() const { return m_LastVisibilityResult; }
    // 每个批次用 cluster_start/cluster_count 索引的就是这个数组（按实例分组、连续）。
    const std::vector<uint32_t>& GetVisibleClusters() const { return m_VisibleClusters; }
    const std::vector<NaniteInstance>& GetInstances() const { return m_Instances; }

    // 某个网格资产已经建好的集群数（0 = 还没建 / 建失败）。测试与统计用。
    uint32_t GetClusterCount(uint64_t mesh_id) const { return m_ClusterManager.GetClusterCount(mesh_id); }

private:
    // 一个可见实例在 m_VisibleClusters 里占的一段
    struct InstanceClusterRange
    {
        uint32_t instance_index {0};
        uint32_t start {0};
        uint32_t count {0};
        uint8_t lod_level {0};
    };

    RHI* m_Rhi;
    std::shared_ptr<RenderResourceBase> m_RenderResource;

    NaniteConfig m_Config;
    NaniteClusterBuilder m_ClusterBuilder;
    NaniteClusterManager m_ClusterManager;
    NaniteCullingSystem m_CullingSystem;

    // 实例管理
    std::vector<NaniteInstance> m_Instances;
    std::unordered_map<uint64_t, uint32_t> m_InstanceIdToIndex;

    // 场景跟踪：上一次 UpdateFromScene 看到的"实例 id → 网格资产 id"集合的哈希
    uint64_t m_SceneTopologyHash {0};

    // 渲染批次
    std::vector<NaniteRenderBatch> m_RenderBatches;

    // 本帧可见的集群下标（按实例分组，段内连续），批次里的 cluster_start/count 索引它
    std::vector<uint32_t> m_VisibleClusters;
    std::vector<InstanceClusterRange> m_InstanceRanges;
    NaniteVisibilityResult m_LastVisibilityResult;

    // GPU资源
    void* m_InstanceBuffer = nullptr;
    void* m_ClusterBuffer = nullptr;
    void* m_VertexBuffer = nullptr;
    void* m_IndexBuffer = nullptr;
    void* m_VisibilityBuffer = nullptr;

    // 着色器资源
    void* m_NaniteVertexShader = nullptr;
    void* m_NanitePixelShader = nullptr;
    void* m_NaniteComputeShader = nullptr;

    // 渲染管线
    void* m_NanitePipeline = nullptr;
    void* m_NanitePipelineLayout = nullptr;

    // 场景里"有哪些实例、各自用哪个网格"的指纹。变换矩阵**不**进哈希 ——
    // 它每帧都可能变，而变了不需要重建集群。
    uint64_t ComputeSceneTopologyHash(const std::vector<RenderEntity>& entities,
                                      RenderResourceBase& render_resource) const;

    // 拓扑变了：整份实例列表重建（集群按需构建一次，之后按网格资产 id 复用）。
    bool RebuildInstancesFromScene(const std::vector<RenderEntity>& entities, RenderResourceBase& render_resource);

    // 拓扑没变：只把变换矩阵推进一帧（prev_transform 留给运动模糊）。
    void UpdateInstanceTransforms(const std::vector<RenderEntity>& entities);

    // 保证 mesh_id 对应的 NaniteMeshResource 已注册。已注册就直接返回 ——
    // 集群构建是递归二分，同一个资产做第二次是纯浪费。
    bool EnsureMeshResource(uint64_t mesh_id, const CpuMeshGeometry& geometry);

    // 关掉 Nanite（或相机缺失）时把上一帧的产物清掉，否则下游会一直读到陈旧的批次。
    void ClearFrameResults();

    // 逐集群裁剪：把可见的叶子簇收集进 m_VisibleClusters，并按实例记下各自的段。
    // 这是 Nanite 真正的收益所在 —— 实例级裁剪之前是恒真的 TODO，集群级裁剪压根不存在。
    void PrepareRenderData(const std::shared_ptr<RenderCamera>& camera,
                           const NaniteVisibilityResult& visibility_result,
                           const NaniteConfig& config);

    // 把 m_InstanceRanges 变成批次。**不产出空批次**：cluster_count == 0 的批次以前
    // 是常态（旧实现的唯一产出），下游拿到它只会画出一堆空绘制。
    void BuildRenderBatches(std::vector<NaniteRenderBatch>& out_batches);

    // 更新GPU缓冲区
    void UpdateGPUBuffers();

    // 执行GPU裁剪
    NaniteVisibilityResult PerformGPUCulling(const std::shared_ptr<RenderCamera>& camera, const NaniteConfig& config);
};

// Nanite渲染通道
//
// 这是 Nanite 唯一的生产者：每帧在渲染线程上被 RenderPipelineBase::UpdateNanite 调用。
// 产出的渲染批次挂在 pass 上，等 GPU 路径（shader + 间接绘制）落地后由它消费。
class NaniteRenderPass : public RenderPassBase
{
public:
    NaniteRenderPass();
    ~NaniteRenderPass();

    virtual void Initialize(const RenderPassInitInfo* init_info) override;
    virtual void PreparePassData(std::shared_ptr<RenderResourceBase> render_resource) override;
    virtual void clear();

    // 执行一帧的 Nanite 计算（场景 → 实例/集群 → 裁剪 → 批次）
    void Render(std::shared_ptr<RenderScene> render_scene, std::shared_ptr<RenderCamera> camera, float delta_time);

    // 获取Nanite渲染器
    NaniteRenderer* getNaniteRenderer() { return &m_NaniteRenderer; }

    // 最近一次 Render 产出的批次（按实例分组，非空批次）
    const std::vector<NaniteRenderBatch>& GetRenderBatches() const { return m_NaniteRenderer.GetRenderBatches(); }

    // 最近一次 Render 有没有产出任何可见集群。给调试/编辑器用。
    bool hasVisibleClusters() const { return m_HasVisibleClusters; }

private:
    // 把 r.Nanite.* 的当前值推给渲染器（每帧都做，所以 CVar 是热生效的）
    void ApplyConsoleVariables();

    // Nanite渲染器
    NaniteRenderer m_NaniteRenderer;

    // Nanite配置
    NaniteConfig m_NaniteConfig;

    // 最近一次 Render 是否产出了可见集群
    bool m_HasVisibleClusters {false};

    // 子系统是否真的初始化过（需要 render_resource，而它在 Initialize 时经常还没有）
    bool m_RendererInitialized {false};
};
