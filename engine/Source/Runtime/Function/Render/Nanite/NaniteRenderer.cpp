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

bool NaniteRenderer::Initialize(RHI* rhi,
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

void NaniteRenderer::Render(RHI* rhi,
                            std::shared_ptr<RenderScene> render_scene,
                            std::shared_ptr<RenderCamera> camera,
                            const NaniteConfig& config)
{
    if (!config.enabled)
    {
        return;
    }

    if (camera == nullptr)
    {
        return;
    }

    // 执行裁剪
    NaniteVisibilityResult visibility_result;
    if (config.use_gpu_culling)
    {
        visibility_result = PerformGPUCulling(camera, config);
    }
    else
    {
        visibility_result = m_CullingSystem.CullInstancesCPU(m_Instances, camera, config);
    }
    m_LastVisibilityResult = visibility_result;

    // 逐集群裁剪
    PrepareRenderData(camera, visibility_result, config);

    // 构建渲染批次。
    // 以前这两步都往**同一个** m_RenderBatches 里写，而 BuildRenderBatches 一进来就
    // clear() ⇒ PrepareRenderData 的产物被立刻抹掉。
    BuildRenderBatches(m_RenderBatches);

    // 更新GPU缓冲区
    UpdateGPUBuffers();

    // TODO: 执行实际渲染（需要 Nanite 的 shader 与间接绘制，两者都还没有）
}

bool NaniteRenderer::RegisterMeshResource(uint64_t mesh_id, const NaniteMeshResource& mesh_resource)
{
    return m_ClusterManager.RegisterMeshResource(mesh_id, mesh_resource);
}

uint64_t NaniteRenderer::RegisterInstance(const NaniteInstance& instance)
{
    NaniteInstance stored = instance;

    // 没填包围盒就从网格资源上补。裁剪拿不到边界时会保守地"不裁"，那等于关掉裁剪，
    // 所以这里补不上是个真问题 —— 但也不能因此拒绝注册（网格可能后注册）。
    if (!stored.local_bounds.IsValid())
    {
        if (const NaniteMeshResource* mesh = m_ClusterManager.GetMeshResource(stored.mesh_id))
        {
            stored.local_bounds = mesh->global_bounds;
        }
    }

    uint32_t index = static_cast<uint32_t>(m_Instances.size());
    m_Instances.push_back(stored);
    m_InstanceIdToIndex[stored.instance_id] = index;
    return stored.instance_id;
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
                                       const NaniteVisibilityResult& visibility_result,
                                       const NaniteConfig& config)
{
    m_VisibleClusters.clear();
    m_InstanceRanges.clear();

    if (camera == nullptr)
    {
        return;
    }

    // 视锥平面只需要算一次，不是每个集群一次。
    const Matrix4x4 view_projection = camera->GetProjectionMatrix() * camera->GetViewMatrix();
    const Vector3 camera_position = camera->position();

    for (uint32_t instance_idx : visibility_result.visible_instance_indices)
    {
        if (instance_idx >= m_Instances.size())
        {
            continue;
        }

        const NaniteInstance& instance = m_Instances[instance_idx];
        const NaniteMeshResource* mesh = m_ClusterManager.GetMeshResource(instance.mesh_id);
        if (mesh == nullptr)
        {
            continue;
        }

        const uint32_t range_start = static_cast<uint32_t>(m_VisibleClusters.size());
        std::vector<uint32_t> visible_for_instance;
        uint8_t finest_lod = 0;

        for (uint32_t cluster_index = 0; cluster_index < mesh->clusters.size(); ++cluster_index)
        {
            const NaniteCluster& cluster = mesh->clusters[cluster_index];

            // 内部节点不可渲染（它的几何区间只是子树的聚合）
            if (!cluster.is_leaf)
            {
                continue;
            }

            if (m_CullingSystem.FrustumCull(cluster.bounds, instance.transform, view_projection))
            {
                continue;
            }

            if (!m_CullingSystem.IsClusterVisible(cluster, camera_position, instance.transform, config))
            {
                continue;
            }

            visible_for_instance.push_back(cluster_index);
            finest_lod = std::max(finest_lod, cluster.lod_level);
        }

        if (visible_for_instance.empty())
        {
            // 这个实例整个被裁掉了，不占段、也不产批次
            continue;
        }

        m_VisibleClusters.insert(m_VisibleClusters.end(), visible_for_instance.begin(), visible_for_instance.end());

        InstanceClusterRange range;
        range.instance_index = instance_idx;
        range.start = range_start;
        range.count = static_cast<uint32_t>(visible_for_instance.size());
        range.lod_level = finest_lod;
        m_InstanceRanges.push_back(range);

        // 把结果写回集群管理器：它的 visible_clusters 字段以前从来没人写过。
        m_ClusterManager.SetVisibleClusters(instance.mesh_id, visible_for_instance);
    }
}

void NaniteRenderer::BuildRenderBatches(std::vector<NaniteRenderBatch>& out_batches)
{
    out_batches.clear();

    // 按实例组织批次：每个实例一段连续的可见集群。
    // **不产出空批次** —— 旧实现唯一的产出就是 cluster_count == 0 的批次。
    for (const InstanceClusterRange& range : m_InstanceRanges)
    {
        if (range.count == 0)
        {
            continue;
        }

        NaniteRenderBatch batch;
        batch.batch_id = static_cast<uint32_t>(out_batches.size());
        batch.instance_id = range.instance_index;
        batch.cluster_start = range.start;
        batch.cluster_count = range.count;
        batch.lod_level = range.lod_level;

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

NaniteVisibilityResult NaniteRenderer::PerformGPUCulling(const std::shared_ptr<RenderCamera>& camera,
                                                         const NaniteConfig& config)
{
    // GPU 裁剪还没有实现，CullInstancesGPU 现在就是 CPU 版本。用传进来的 config 而不是
    // 成员 m_Config —— 否则这一帧改的配置不生效。
    return m_CullingSystem.CullInstancesGPU(m_Instances, camera, config);
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
