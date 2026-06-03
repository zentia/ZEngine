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
    bool Initialize(std::shared_ptr<RHI> rhi,
                    std::shared_ptr<RenderResourceBase> render_resource,
                    const NaniteConfig& config);

    // 清理
    void clear();

    // 渲染一帧
    void Render(std::shared_ptr<RHI> rhi,
                std::shared_ptr<RenderScene> render_scene,
                std::shared_ptr<RenderCamera> camera,
                const NaniteConfig& config);

    // 注册实例
    uint64_t RegisterInstance(const NaniteInstance& instance);

    // 注销实例
    void UnregisterInstance(uint64_t instance_id);

    // 更新实例
    void UpdateInstance(uint64_t instance_id, const NaniteInstance& instance);

    // 获取配置
    const NaniteConfig& getConfig() const { return m_Config; }
    void SetConfig(const NaniteConfig& config) { m_Config = config; }

private:
    std::shared_ptr<RHI> m_Rhi;
    std::shared_ptr<RenderResourceBase> m_RenderResource;

    NaniteConfig m_Config;
    NaniteClusterManager m_ClusterManager;
    NaniteCullingSystem m_CullingSystem;

    // 实例管理
    std::vector<NaniteInstance> m_Instances;
    std::unordered_map<uint64_t, uint32_t> m_InstanceIdToIndex;

    // 渲染批次
    std::vector<NaniteRenderBatch> m_RenderBatches;

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

    // 准备渲染数据
    void PrepareRenderData(const std::shared_ptr<RenderCamera>& camera, std::vector<NaniteRenderBatch>& out_batches);

    // 构建渲染批次
    void BuildRenderBatches(const NaniteVisibilityResult& visibility_result,
                            std::vector<NaniteRenderBatch>& out_batches);

    // 更新GPU缓冲区
    void UpdateGPUBuffers();

    // 执行GPU裁剪
    NaniteVisibilityResult PerformGPUCulling(const std::shared_ptr<RenderCamera>& camera);
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