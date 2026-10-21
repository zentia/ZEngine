#include "NaniteRenderer.h"

#include "NaniteTypes.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Function/Render/Interface/RHI.h"
#include "Runtime/Function/Render/RenderCamera.h"
#include "Runtime/Function/Render/RenderEntity.h"
#include "Runtime/Function/Render/RenderResourceBase.h"
#include "Runtime/Function/Render/RenderScene.h"
#include "Runtime/Function/Render/Nanite/NaniteSettings.h"

#include <cstdint>

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

    // 集群随实例一起没了：下次 UpdateFromScene 必须重新认一遍场景，否则哈希还匹配、
    // 实例列表却是空的，Nanite 就永久静默了。
    m_SceneTopologyHash = 0;
    ClearFrameResults();
}

// ---- 场景 → 实例 / 集群 -------------------------------------------------------

// FNV-1a 64。用 offset basis 起头（而不是 0）是为了让"空场景"的哈希也不等于
// m_SceneTopologyHash 的初值 —— 否则首帧会被误判成"场景没变"而跳过重建。
uint64_t NaniteRenderer::ComputeSceneTopologyHash(const std::vector<RenderEntity>& entities,
                                                  RenderResourceBase& render_resource) const
{
    uint64_t hash = 14695981039346656037ull;

    auto mix = [&hash](uint64_t value) {
        hash ^= value;
        hash *= 1099511628211ull;
    };

    for (const RenderEntity& entity : entities)
    {
        const CpuMeshGeometry* geometry = render_resource.FindMeshGeometry(entity.m_MeshAssetId);
        if (geometry == nullptr || !geometry->IsValid())
        {
            // 没有 CPU 几何的实体进不了 Nanite。它**不**进哈希，否则"某个网格资产
            // 晚一帧才上传完成"会被当成拓扑变化，白白重建一次。
            continue;
        }

        mix(static_cast<uint64_t>(entity.m_InstanceId));
        mix(static_cast<uint64_t>(entity.m_MeshAssetId));
    }

    return hash;
}

bool NaniteRenderer::EnsureMeshResource(uint64_t mesh_id, const CpuMeshGeometry& geometry)
{
    if (m_ClusterManager.GetMeshResource(mesh_id) != nullptr)
    {
        return true;
    }

    NaniteMeshResource mesh_resource {};
    mesh_resource.mesh_id = mesh_id;

    // CPU 网格缓存只留了 position + index（12 + 4 字节/顶点，够距离场和裁剪用），
    // 法线和 UV 是没有的。集群构建器今天也只读 position + index，所以传空是对的；
    // 等 GPU 路径需要完整顶点属性时，这里得换成真的顶点缓冲。
    const std::vector<Vector3> empty_normals;
    const std::vector<Vector2> empty_uvs;

    if (!m_ClusterBuilder.BuildClustersFromMesh(geometry.positions,
                                                empty_normals,
                                                empty_uvs,
                                                geometry.indices,
                                                mesh_resource,
                                                m_Config))
    {
        LOG_WARNING(ZRender, "nanite: cluster build failed for mesh asset {}", mesh_id);
        return false;
    }

    return m_ClusterManager.RegisterMeshResource(mesh_id, mesh_resource);
}

bool NaniteRenderer::RebuildInstancesFromScene(const std::vector<RenderEntity>& entities,
                                               RenderResourceBase& render_resource)
{
    m_Instances.clear();
    m_InstanceIdToIndex.clear();
    ClearFrameResults();

    for (const RenderEntity& entity : entities)
    {
        const CpuMeshGeometry* geometry = render_resource.FindMeshGeometry(entity.m_MeshAssetId);
        if (geometry == nullptr || !geometry->IsValid())
        {
            continue;
        }

        const uint64_t mesh_id = static_cast<uint64_t>(entity.m_MeshAssetId);
        if (!EnsureMeshResource(mesh_id, *geometry))
        {
            continue;
        }

        const uint64_t instance_id = static_cast<uint64_t>(entity.m_InstanceId);
        if (m_InstanceIdToIndex.find(instance_id) != m_InstanceIdToIndex.end())
        {
            // 同一个实例 id 出现两次：m_InstanceIdToIndex 只能指向一个位置，第二个会
            // 变成"注册了但更新不到"。宁可跳过也不要这种半死状态。
            continue;
        }

        NaniteInstance instance {};
        instance.instance_id = instance_id;
        instance.mesh_id = mesh_id;
        instance.transform = entity.m_ModelMatrix;
        instance.prev_transform = entity.m_ModelMatrix;
        // RenderEntity::m_BoundingBox 是模型空间的（可见性剔除也要乘 m_ModelMatrix 才拿
        // 去和视锥比）。资产没带包围盒就退回缓存里的几何包围盒。
        instance.local_bounds = entity.m_BoundingBox.IsValid() ? entity.m_BoundingBox : geometry->local_bounds;

        RegisterInstance(instance);
    }

    return !m_Instances.empty();
}

void NaniteRenderer::UpdateInstanceTransforms(const std::vector<RenderEntity>& entities)
{
    for (const RenderEntity& entity : entities)
    {
        const auto it = m_InstanceIdToIndex.find(static_cast<uint64_t>(entity.m_InstanceId));
        if (it == m_InstanceIdToIndex.end())
        {
            continue;
        }

        NaniteInstance& instance = m_Instances[it->second];
        instance.prev_transform = instance.transform;
        instance.transform = entity.m_ModelMatrix;
    }
}

bool NaniteRenderer::UpdateFromScene(std::shared_ptr<RenderScene> render_scene, RenderResourceBase* render_resource)
{
    if (render_scene == nullptr || render_resource == nullptr)
    {
        return false;
    }

    const std::vector<RenderEntity>& entities = render_scene->m_RenderEntities;

    const uint64_t topology_hash = ComputeSceneTopologyHash(entities, *render_resource);
    if (topology_hash != m_SceneTopologyHash)
    {
        m_SceneTopologyHash = topology_hash;
        RebuildInstancesFromScene(entities, *render_resource);
    }
    else
    {
        UpdateInstanceTransforms(entities);
    }

    return !m_Instances.empty();
}

void NaniteRenderer::ClearFrameResults()
{
    m_RenderBatches.clear();
    m_VisibleClusters.clear();
    m_InstanceRanges.clear();
    m_LastVisibilityResult = NaniteVisibilityResult {};
}

void NaniteRenderer::Render(RHI* rhi,
                            std::shared_ptr<RenderScene> render_scene,
                            std::shared_ptr<RenderCamera> camera,
                            const NaniteConfig& config)
{
    if (!config.enabled)
    {
        // 关掉之后必须把上一帧的产物清掉：批次挂在渲染器上，下游（和未来要读它的
        // 编辑器/统计）会一直拿到最后一帧的陈旧结果。
        ClearFrameResults();
        return;
    }

    if (camera == nullptr)
    {
        ClearFrameResults();
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
    (void)init_info;

    // 初始配置来自 CVar，而不是写死在这里。
    m_NaniteConfig = NaniteSettings::BuildConfig();

    // Nanite 今天**完全是 CPU 的**（集群构建、逐簇裁剪、批次组装），一行都不跑在
    // GPU 上。所以初始化只真的需要 render_resource（CPU 网格几何的唯一来源）。
    m_RendererInitialized = false;
    if (m_RenderResource)
    {
        m_RendererInitialized = m_NaniteRenderer.Initialize(m_Rhi, m_RenderResource, m_NaniteConfig);
    }
}

void NaniteRenderPass::PreparePassData(std::shared_ptr<RenderResourceBase> render_resource)
{
    m_RenderResource = render_resource;

    // Initialize 常常在 render_resource 还没建好时就被调用（管线 Setup 的顺序），
    // 所以真正的初始化在这里补 —— 没有 CPU 网格几何的话 Nanite 一个三角形都拿不到。
    if (m_RenderResource && !m_RendererInitialized)
    {
        m_RendererInitialized = m_NaniteRenderer.Initialize(m_Rhi, m_RenderResource, m_NaniteConfig);
    }
}

void NaniteRenderPass::Render(std::shared_ptr<RenderScene> render_scene,
                              std::shared_ptr<RenderCamera> camera,
                              float delta_time)
{
    (void)delta_time;

    if (!render_scene || !camera)
    {
        return;
    }

    if (!m_RendererInitialized)
    {
        return;
    }

    ApplyConsoleVariables();

    if (!m_NaniteConfig.enabled)
    {
        // 让渲染器自己清掉上一帧的产物（批次挂在它身上，不清就是陈旧数据）。
        m_NaniteRenderer.Render(m_Rhi, render_scene, camera, m_NaniteConfig);
        m_HasVisibleClusters = false;
        return;
    }

    // 场景 → Nanite 实例 + 集群资源（几何来自 RenderResourceBase 的 CPU 网格缓存）
    m_NaniteRenderer.UpdateFromScene(render_scene, m_RenderResource.get());

    // 实例级裁剪 → 逐簇裁剪 → 批次
    m_NaniteRenderer.Render(m_Rhi, render_scene, camera, m_NaniteConfig);

    m_HasVisibleClusters = !m_NaniteRenderer.GetVisibleClusters().empty();
}

void NaniteRenderPass::ApplyConsoleVariables()
{
    // 每帧都推一次，所以 r.Nanite.* 是热生效的。
    NaniteConfig desired = m_NaniteConfig;
    NaniteSettings::ApplyTo(desired);

    if (desired.enabled == m_NaniteConfig.enabled && desired.enable_lod == m_NaniteConfig.enable_lod &&
        desired.enable_culling == m_NaniteConfig.enable_culling &&
        desired.max_screen_error == m_NaniteConfig.max_screen_error &&
        desired.lod_bias == m_NaniteConfig.lod_bias)
    {
        return;
    }

    m_NaniteConfig = desired;
    m_NaniteRenderer.SetConfig(m_NaniteConfig);
}

void NaniteRenderPass::clear()
{
    m_NaniteRenderer.clear();
    m_HasVisibleClusters = false;
    m_RendererInitialized = false;
}
