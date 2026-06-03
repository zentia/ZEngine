#include "NaniteRenderer.h"

#include "NaniteTypes.h"
#include "Runtime/Function/Render/Interface/RHI.h"
#include "Runtime/Function/Render/RenderCamera.h"
#include "Runtime/Function/Render/RenderResourceBase.h"
#include "Runtime/Function/Render/RenderScene.h"

// ==================== NaniteRenderer ====================

NaniteRenderer::NaniteRenderer() = default;

NaniteRenderer::~NaniteRenderer()
{
    clear();
}

bool NaniteRenderer::Initialize(std::shared_ptr<RHI> rhi,
                                std::shared_ptr<RenderResourceBase> render_resource,
                                const NaniteConfig& config)
{
    m_Rhi = rhi;
    m_RenderResource = render_resource;
    m_Config = config;

    if (!m_ClusterManager.Initialize(rhi, config))
    {
        return false;
    }

    if (!m_CullingSystem.Initialize(rhi, config))
    {
        return false;
    }

    // TODO: 初始化GPU资源（缓冲区、着色器等）

    return true;
}

void NaniteRenderer::clear()
{
    m_ClusterManager.clear();
    m_CullingSystem.clear();

    m_Instances.clear();
    m_InstanceIdToIndex.clear();
    m_RenderBatches.clear();
}

void NaniteRenderer::Render(std::shared_ptr<RHI> rhi,
                            std::shared_ptr<RenderScene> render_scene,
                            std::shared_ptr<RenderCamera> camera,
                            const NaniteConfig& config)
{
    if (!config.enabled)
    {
        return;
    }

    // 执行裁剪
    NaniteVisibilityResult visibility_result;
    if (config.use_gpu_culling)
    {
        visibility_result = PerformGPUCulling(camera);
    }
    else
    {
        visibility_result = m_CullingSystem.CullInstancesCPU(m_Instances, camera, config);
    }

    // 准备渲染数据
    PrepareRenderData(camera, m_RenderBatches);

    // 构建渲染批次
    BuildRenderBatches(visibility_result, m_RenderBatches);

    // 更新GPU缓冲区
    UpdateGPUBuffers();

    // TODO: 执行实际渲染
}

uint64_t NaniteRenderer::RegisterInstance(const NaniteInstance& instance)
{
    uint32_t index = static_cast<uint32_t>(m_Instances.size());
    m_Instances.push_back(instance);
    m_InstanceIdToIndex[instance.instance_id] = index;
    return instance.instance_id;
}

void NaniteRenderer::UnregisterInstance(uint64_t instance_id)
{
    auto it = m_InstanceIdToIndex.find(instance_id);
    if (it != m_InstanceIdToIndex.end())
    {
        uint32_t index = it->second;
        m_Instances.erase(m_Instances.begin() + index);
        m_InstanceIdToIndex.erase(it);

        // 更新索引映射
        for (auto& pair : m_InstanceIdToIndex)
        {
            if (pair.second > index)
            {
                pair.second--;
            }
        }
    }
}

void NaniteRenderer::UpdateInstance(uint64_t instance_id, const NaniteInstance& instance)
{
    auto it = m_InstanceIdToIndex.find(instance_id);
    if (it != m_InstanceIdToIndex.end())
    {
        m_Instances[it->second] = instance;
    }
}

void NaniteRenderer::PrepareRenderData(const std::shared_ptr<RenderCamera>& camera,
                                       std::vector<NaniteRenderBatch>& out_batches)
{
    out_batches.clear();

    // TODO: 准备渲染数据
    // 1. 选择LOD级别
    // 2. 组织渲染批次
}

void NaniteRenderer::BuildRenderBatches(const NaniteVisibilityResult& visibility_result,
                                        std::vector<NaniteRenderBatch>& out_batches)
{
    out_batches.clear();

    // 按实例和LOD级别组织批次
    for (uint32_t instance_idx : visibility_result.visible_instance_indices)
    {
        if (instance_idx >= m_Instances.size())
            continue;

        const auto& instance = m_Instances[instance_idx];

        // TODO: 获取可见集群
        // 构建批次
        NaniteRenderBatch batch;
        batch.batch_id = static_cast<uint32_t>(out_batches.size());
        batch.instance_id = instance_idx;
        batch.cluster_start = 0;
        batch.cluster_count = 0;
        batch.lod_level = instance.current_lod_level;

        out_batches.push_back(batch);
    }
}

void NaniteRenderer::UpdateGPUBuffers()
{
    // TODO: 更新GPU缓冲区
    // 1. 更新实例数据
    // 2. 更新集群数据
    // 3. 更新可见性数据
}

NaniteVisibilityResult NaniteRenderer::PerformGPUCulling(const std::shared_ptr<RenderCamera>& camera)
{
    return m_CullingSystem.CullInstancesGPU(m_Instances, camera, m_Config);
}

// ==================== NaniteRenderPass ====================

NaniteRenderPass::NaniteRenderPass() = default;

NaniteRenderPass::~NaniteRenderPass()
{
    clear();
}

void NaniteRenderPass::Initialize(const RenderPassInitInfo* init_info)
{
    // TODO: 初始化渲染通道
}

void NaniteRenderPass::Draw()
{
    if (m_NaniteRenderer)
    {
        // TODO: 执行Nanite渲染
    }
}

void NaniteRenderPass::clear()
{
    m_NaniteRenderer.reset();
}
