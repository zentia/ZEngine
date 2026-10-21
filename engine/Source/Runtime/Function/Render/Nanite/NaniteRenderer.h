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
    NaniteClusterManager m_ClusterManager;
    NaniteCullingSystem m_CullingSystem;

    // 实例管理
    std::vector<NaniteInstance> m_Instances;
    std::unordered_map<uint64_t, uint32_t> m_InstanceIdToIndex;

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
class NaniteRenderPass : public RenderPassBase
{
public:
    NaniteRenderPass();
    ~NaniteRenderPass();

    virtual void Initialize(const RenderPassInitInfo* init_info) override;
    virtual void Draw();
    virtual void clear();

    // 设置Nanite渲染器
    void setNaniteRenderer(std::shared_ptr<NaniteRenderer> renderer) { m_NaniteRenderer = renderer; }

private:
    std::shared_ptr<NaniteRenderer> m_NaniteRenderer;
};