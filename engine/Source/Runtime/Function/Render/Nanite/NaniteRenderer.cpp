#include "NaniteRenderer.h"

#include "NaniteTypes.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Function/Render/Interface/RHI.h"
#include "Runtime/Function/Render/RenderCamera.h"
#include "Runtime/Function/Render/RenderEntity.h"
#include "Runtime/Function/Render/RenderResourceBase.h"
#include "Runtime/Function/Render/RenderScene.h"
#include "Runtime/Function/Render/Nanite/NaniteSettings.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

// ===========================================================================
// 生成的 SPIR-V（engine/shader/glsl → engine/shader/generated/cpp）。
//
// 这些头文件是构建时由 glslangValidator 生成的，没有它的环境根本不会产生。所以
// 每个都单独 __has_include 保护：缺了就退化成"没有 shader"，Nanite 的 GPU 路径
// 安静地不启用 —— 而不是让整个 ZRuntime 编译不过。
// ===========================================================================
#if defined(__has_include)
#if __has_include(<nanite_cluster_cull_comp.h>)
#include <nanite_cluster_cull_comp.h>
#define ZENGINE_NANITE_HAS_CULL_SPIRV 1
#endif
#if __has_include(<nanite_cluster_vert.h>)
#include <nanite_cluster_vert.h>
#define ZENGINE_NANITE_HAS_VERT_SPIRV 1
#endif
#if __has_include(<nanite_cluster_frag.h>)
#include <nanite_cluster_frag.h>
#define ZENGINE_NANITE_HAS_FRAG_SPIRV 1
#endif
#endif

#ifndef ZENGINE_NANITE_HAS_CULL_SPIRV
namespace Z { namespace Shader {
static const std::vector<unsigned char> NANITE_CLUSTER_CULL_COMP {};
}}  // namespace Z::Shader
#endif
#ifndef ZENGINE_NANITE_HAS_VERT_SPIRV
namespace Z { namespace Shader {
static const std::vector<unsigned char> NANITE_CLUSTER_VERT {};
}}  // namespace Z::Shader
#endif
#ifndef ZENGINE_NANITE_HAS_FRAG_SPIRV
namespace Z { namespace Shader {
static const std::vector<unsigned char> NANITE_CLUSTER_FRAG {};
}}  // namespace Z::Shader
#endif

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

    // GPU 缓冲跟着实例一起没了。留着它们只会让下一帧的剔除对着一份空场景发 dispatch。
    if (m_Rhi != nullptr)
    {
        DestroyGPUResources();
    }
    m_SceneGPUData.clear();

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

    // ---- GPU 路径 ----
    //
    // CPU 到这里就交棒了：上传打包好的场景 → 发一次剔除 kernel → 发一次间接绘制。
    // 命令缓冲里现在躺着这一帧全部（且只躺着这一帧）的绘制命令，而 CPU 不知道
    // 有多少条是活的 —— 这正是 GPU-driven 的意义。
    //
    // 失败（没有 RHI、shader 没编译出来、缓冲建不起来）时静默退回 CPU 结果：
    // m_RenderBatches 还在，编辑器和统计不会因此变成空的。
    if (config.use_gpu_culling && PrepareGPUData(camera, config))
    {
        DispatchClusterCulling(camera, config);

        // 画不画、画到哪儿，取决于有没有人注入过光栅目标：
        //   * 注入过（SetRasterRenderPass）⇒ 这个 pass 属于调用方，绘制必须发生在
        //     它开着的时候。这里在任何 pass 之前执行，画不了 —— 由调用方在 pass 里
        //     调 DrawClustersIndirectInPass。DrawClustersIndirect 自己也认这个约定。
        //   * 没注入 ⇒ Nanite 用自己的离屏 target，自己开/关 pass。
        DrawClustersIndirect();
    }
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

// ===========================================================================
// GPU 路径。
//
// 以前这里是 "TODO: 更新GPU缓冲区" 加上一堆 void* 成员 —— Nanite 一行都不跑在
// GPU 上。现在 CPU 只做两件事：把场景打包上传，然后发一次 dispatch。剩下的
// （哪些集群活着、画多少）全在 GPU 上决定。
// ===========================================================================

namespace
{
    // 全部缓冲建在 host-visible 上：实例变换每帧都要更新，而这套 RHI 没有 staging
    // 抽象，host-visible 是唯一能简单地反复写的选项。
    constexpr RHIMemoryPropertyFlags kNaniteHostMemory =
        RHI_MEMORY_PROPERTY_HOST_VISIBLE_BIT | RHI_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    // 离屏光栅目标的兜底尺寸：没有 swapchain（headless）也没有显式设置时用。
    // 它只影响"Nanite 自己那张图多大"，不影响正确性。
    constexpr uint32_t kNaniteDefaultRasterTargetSize = 512;

    // swapchain extent 的合法性上界。headless（MetalRHI 只建了 device 和 queue）时
    // m_SwapchainExtent **根本没有初始化过**，读出来是栈/堆上的垃圾 —— 曾经拿它当
    // 纹理宽度，Metal 直接断言 "width (4249268848) greater than the maximum
    // allowed size of 16384"。16384 是 Metal 的纹理尺寸上限（DX12/Vulkan 的常见
    // 上限也在这个量级），所以它是个足够保守的筛子。
    constexpr uint32_t kNaniteMaxRasterTargetSize = 16384;

    // 只有落在 (0, 上限] 里的 swapchain extent 才算数。
    bool IsSaneTargetSize(uint32_t value)
    {
        return value != 0 && value <= kNaniteMaxRasterTargetSize;
    }

    // 容量按 2 的幂增长。集群数随场景变化，每帧精确重建会把分配抖动变成卡顿。
    uint32_t NaniteGrowCapacity(uint32_t needed)
    {
        uint32_t capacity = 64;
        while (capacity < needed)
        {
            capacity = (capacity > (1u << 30)) ? needed : capacity * 2;
        }
        return capacity;
    }
}  // namespace

void NaniteRenderer::DestroyGPUResources()
{
    auto release = [this](RHIBuffer*& buffer, RHIDeviceMemory*& memory) {
        if (memory != nullptr)
        {
            m_Rhi->FreeMemory(memory);
        }
        if (buffer != nullptr)
        {
            m_Rhi->DestroyBuffer(buffer);
        }
    };

    release(m_GPU.clusters, m_GPU.clusters_memory);
    release(m_GPU.instances, m_GPU.instances_memory);
    release(m_GPU.positions, m_GPU.positions_memory);
    release(m_GPU.indices, m_GPU.indices_memory);
    release(m_GPU.visible_list, m_GPU.visible_list_memory);
    release(m_GPU.draw_commands, m_GPU.draw_commands_memory);
    release(m_GPU.cull_counter, m_GPU.cull_counter_memory);
    release(m_GPU.cull_constants, m_GPU.cull_constants_memory);

    m_GPU.cluster_capacity = 0;
    m_GPU.instance_capacity = 0;
    m_GPU.position_capacity = 0;
    m_GPU.index_capacity = 0;
    m_GPU.compute_ready = false;

    // 离屏光栅目标跟着重建（改尺寸、clear 都会走到这里）。
    DestroyRasterTarget();
    m_GPU.raster_render_pass = nullptr;
    m_GPU.raster_framebuffer = nullptr;
    m_GPU.raster_ready = false;

    // 已知缺口：RHI 抽象层没有 DestroyPipeline / DestroyPipelineLayout /
    // DestroyDescriptorSetLayout 这几个入口，所以 cull_pipeline、raster_pipeline、
    // pipeline_layout、descriptor_set_layout 只能随进程一起结束。它们每个实例只建
    // 一次（懒建），不是每帧增长 —— 但确实是泄漏，等 RHI 补上销毁接口再收。
    m_GPU.cull_pipeline = nullptr;
    m_GPU.raster_pipeline = nullptr;
    m_GPU.pipeline_layout = nullptr;
    m_GPU.descriptor_set_layout = nullptr;
    m_GPU.descriptor_set = nullptr;
}

bool NaniteRenderer::UploadBytes(RHIBuffer* buffer, RHIDeviceMemory* memory, const void* data, size_t bytes)
{
    if (m_Rhi == nullptr || buffer == nullptr || memory == nullptr || bytes == 0)
    {
        return false;
    }

    void* mapped = nullptr;
    if (!m_Rhi->MapMemory(memory, 0, static_cast<RHIDeviceSize>(bytes), 0, &mapped) || mapped == nullptr)
    {
        return false;
    }
    std::memcpy(mapped, data, bytes);
    m_Rhi->UnmapMemory(memory);
    return true;
}

bool NaniteRenderer::EnsureGPUResources(uint32_t cluster_count,
                                        uint32_t instance_count,
                                        uint32_t position_count,
                                        uint32_t index_count)
{
    if (m_Rhi == nullptr || cluster_count == 0 || instance_count == 0)
    {
        return false;
    }

    auto ensure = [this](RHIBuffer*& buffer,
                         RHIDeviceMemory*& memory,
                         uint32_t& capacity,
                         uint32_t needed,
                         uint32_t element_size,
                         RHIBufferUsageFlags usage) {
        if (buffer != nullptr && memory != nullptr && capacity >= needed)
        {
            return true;
        }
        if (memory != nullptr)
        {
            m_Rhi->FreeMemory(memory);
        }
        if (buffer != nullptr)
        {
            m_Rhi->DestroyBuffer(buffer);
        }
        capacity = NaniteGrowCapacity(needed);
        m_Rhi->CreateBuffer(static_cast<RHIDeviceSize>(capacity) * static_cast<RHIDeviceSize>(element_size),
                            usage,
                            kNaniteHostMemory,
                            buffer,
                            memory);
        return buffer != nullptr && memory != nullptr;
    };

    const bool ok_clusters =
        ensure(m_GPU.clusters, m_GPU.clusters_memory, m_GPU.cluster_capacity, cluster_count,
               static_cast<uint32_t>(sizeof(NaniteGPUCluster)),
               RHI_BUFFER_USAGE_STORAGE_BUFFER_BIT | RHI_BUFFER_USAGE_TRANSFER_DST_BIT);
    const bool ok_instances =
        ensure(m_GPU.instances, m_GPU.instances_memory, m_GPU.instance_capacity, instance_count,
               static_cast<uint32_t>(sizeof(NaniteGPUInstance)),
               RHI_BUFFER_USAGE_STORAGE_BUFFER_BIT | RHI_BUFFER_USAGE_TRANSFER_DST_BIT);
    const bool ok_positions =
        ensure(m_GPU.positions, m_GPU.positions_memory, m_GPU.position_capacity, position_count,
               static_cast<uint32_t>(sizeof(Vector4)),
               RHI_BUFFER_USAGE_STORAGE_BUFFER_BIT | RHI_BUFFER_USAGE_TRANSFER_DST_BIT);
    const bool ok_indices =
        ensure(m_GPU.indices, m_GPU.indices_memory, m_GPU.index_capacity, index_count,
               static_cast<uint32_t>(sizeof(uint32_t)),
               RHI_BUFFER_USAGE_INDEX_BUFFER_BIT | RHI_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                   RHI_BUFFER_USAGE_TRANSFER_DST_BIT);

    // 剔除的产出：可见列表、间接命令、计数器。容量跟着集群数走。
    const bool ok_visible =
        ensure(m_GPU.visible_list, m_GPU.visible_list_memory, m_GPU.cluster_capacity, cluster_count,
               static_cast<uint32_t>(sizeof(uint32_t)),
               RHI_BUFFER_USAGE_STORAGE_BUFFER_BIT | RHI_BUFFER_USAGE_TRANSFER_DST_BIT);
    const bool ok_commands =
        ensure(m_GPU.draw_commands, m_GPU.draw_commands_memory, m_GPU.cluster_capacity, cluster_count,
               static_cast<uint32_t>(sizeof(RHIDrawIndexedIndirectCommand)),
               RHI_BUFFER_USAGE_STORAGE_BUFFER_BIT | RHI_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                   RHI_BUFFER_USAGE_TRANSFER_DST_BIT);
    const bool ok_counter =
        ensure(m_GPU.cull_counter, m_GPU.cull_counter_memory, m_GPU.cluster_capacity, cluster_count,
               static_cast<uint32_t>(sizeof(NaniteCullCounter)),
               RHI_BUFFER_USAGE_STORAGE_BUFFER_BIT | RHI_BUFFER_USAGE_TRANSFER_DST_BIT);
    const bool ok_constants =
        ensure(m_GPU.cull_constants, m_GPU.cull_constants_memory, m_GPU.cluster_capacity, cluster_count,
               static_cast<uint32_t>(sizeof(NaniteCullConstants)),
               RHI_BUFFER_USAGE_UNIFORM_BUFFER_BIT | RHI_BUFFER_USAGE_TRANSFER_DST_BIT);

    // cluster_capacity 被上面四个共用，取一次就够 —— 但 ensure 会在容量变小时
    // 不动它，所以这里只要确认它不小于需求即可。
    m_GPU.cluster_capacity = std::max(m_GPU.cluster_capacity, cluster_count);

    return ok_clusters && ok_instances && ok_positions && ok_indices && ok_visible && ok_commands &&
           ok_counter && ok_constants;
}

void NaniteRenderer::UpdateDescriptorWrites()
{
    if (m_Rhi == nullptr || m_GPU.descriptor_set == nullptr)
    {
        return;
    }

    auto make_write = [](RHIDescriptorSet* set, uint32_t binding) {
        RHIWriteDescriptorSet write {};
        write.sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = set;
        write.dstBinding = binding;
        write.dstArrayElement = 0;
        write.descriptorCount = 1;
        return write;
    };

    RHIDescriptorBufferInfo cluster_info {};
    cluster_info.offset = 0;
    cluster_info.range = static_cast<RHIDeviceSize>(m_GPU.cluster_capacity) * sizeof(NaniteGPUCluster);
    cluster_info.buffer = m_GPU.clusters;

    RHIDescriptorBufferInfo instance_info {};
    instance_info.offset = 0;
    instance_info.range = static_cast<RHIDeviceSize>(m_GPU.instance_capacity) * sizeof(NaniteGPUInstance);
    instance_info.buffer = m_GPU.instances;

    RHIDescriptorBufferInfo visible_info {};
    visible_info.offset = 0;
    visible_info.range = static_cast<RHIDeviceSize>(m_GPU.cluster_capacity) * sizeof(uint32_t);
    visible_info.buffer = m_GPU.visible_list;

    RHIDescriptorBufferInfo command_info {};
    command_info.offset = 0;
    command_info.range =
        static_cast<RHIDeviceSize>(m_GPU.cluster_capacity) * sizeof(RHIDrawIndexedIndirectCommand);
    command_info.buffer = m_GPU.draw_commands;

    RHIDescriptorBufferInfo counter_info {};
    counter_info.offset = 0;
    counter_info.range = sizeof(NaniteCullCounter);
    counter_info.buffer = m_GPU.cull_counter;

    RHIDescriptorBufferInfo constants_info {};
    constants_info.offset = 0;
    constants_info.range = sizeof(NaniteCullConstants);
    constants_info.buffer = m_GPU.cull_constants;

    RHIDescriptorBufferInfo positions_info {};
    positions_info.offset = 0;
    positions_info.range = static_cast<RHIDeviceSize>(m_GPU.position_capacity) * sizeof(Vector4);
    positions_info.buffer = m_GPU.positions;

    RHIWriteDescriptorSet writes[] = {
        make_write(m_GPU.descriptor_set, NaniteGPUBinding::kCullConstants),
        make_write(m_GPU.descriptor_set, NaniteGPUBinding::kClusters),
        make_write(m_GPU.descriptor_set, NaniteGPUBinding::kInstances),
        make_write(m_GPU.descriptor_set, NaniteGPUBinding::kVisibleList),
        make_write(m_GPU.descriptor_set, NaniteGPUBinding::kDrawCommands),
        make_write(m_GPU.descriptor_set, NaniteGPUBinding::kDrawCounter),
        make_write(m_GPU.descriptor_set, NaniteGPUBinding::kPositions),
    };

    writes[0].descriptorType = RHI_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[0].pBufferInfo = &constants_info;
    for (size_t i = 1; i < sizeof(writes) / sizeof(writes[0]); ++i)
    {
        writes[i].descriptorType = RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    }
    writes[1].pBufferInfo = &cluster_info;
    writes[2].pBufferInfo = &instance_info;
    writes[3].pBufferInfo = &visible_info;
    writes[4].pBufferInfo = &command_info;
    writes[5].pBufferInfo = &counter_info;
    writes[6].pBufferInfo = &positions_info;

    m_Rhi->UpdateDescriptorSets(static_cast<uint32_t>(sizeof(writes) / sizeof(writes[0])), writes, 0, nullptr);
}

bool NaniteRenderer::EnsureComputePipeline()
{
    if (m_GPU.compute_ready)
    {
        return true;
    }
    if (m_GPU.gpu_pipelines_unsupported)
    {
        return false;
    }
    if (m_Rhi == nullptr || Z::Shader::NANITE_CLUSTER_CULL_COMP.empty())
    {
        return false;
    }

    // The nanite cull shaders only ship as SPIR-V (see the ZENGINE_NANITE_*_SPIRV
    // guards at the top of this file). CreateShaderModule() happily wraps the blob
    // on any backend, but CreateComputePipelines() then fails with E_FAIL, so on a
    // non-Vulkan RHI this used to be retried -- and to log "cluster-cull compute
    // pipeline creation failed" -- once per frame, forever. Those per-frame
    // failures are also what pushed frames past the EditorUI 500 ms RHI wait,
    // which is what let the game thread run ahead in the first place.
    if (m_Rhi->getGraphicsAPI() != GraphicsAPI::Vulkan)
    {
        m_GPU.gpu_pipelines_unsupported = true;
        LOG_WARNING(ZRender,
                    "nanite: GPU cluster culling needs SPIR-V shaders; unavailable on this RHI "
                    "(graphics api {}), falling back to the CPU path",
                    static_cast<int>(m_Rhi->getGraphicsAPI()));
        return false;
    }

    // descriptor set layout：一个 set 同时给 compute 和 graphics。
    if (m_GPU.descriptor_set_layout == nullptr)
    {
        RHIDescriptorSetLayoutBinding bindings[NaniteGPUBinding::kBindingCount] = {};
        bindings[NaniteGPUBinding::kCullConstants] = {NaniteGPUBinding::kCullConstants,
                                                      RHI_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                                      1,
                                                      RHI_SHADER_STAGE_COMPUTE_BIT | RHI_SHADER_STAGE_VERTEX_BIT,
                                                      nullptr};
        for (uint32_t slot = 1; slot < NaniteGPUBinding::kBindingCount; ++slot)
        {
            bindings[slot] = {slot,
                              RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                              1,
                              RHI_SHADER_STAGE_COMPUTE_BIT | RHI_SHADER_STAGE_VERTEX_BIT,
                              nullptr};
        }

        RHIDescriptorSetLayoutCreateInfo layout_info {};
        layout_info.sType = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layout_info.bindingCount = NaniteGPUBinding::kBindingCount;
        layout_info.pBindings = bindings;
        if (!m_Rhi->CreateDescriptorSetLayout(&layout_info, m_GPU.descriptor_set_layout))
        {
            return false;
        }
    }

    if (m_GPU.pipeline_layout == nullptr)
    {
        RHIPipelineLayoutCreateInfo layout_info {};
        layout_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layout_info.setLayoutCount = 1;
        layout_info.pSetLayouts = &m_GPU.descriptor_set_layout;
        if (!m_Rhi->CreatePipelineLayout(&layout_info, m_GPU.pipeline_layout))
        {
            return false;
        }
    }

    if (m_GPU.descriptor_set == nullptr)
    {
        RHIDescriptorSetAllocateInfo alloc_info {};
        alloc_info.sType = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc_info.descriptorPool = m_Rhi->GetDescriptorPoor();
        alloc_info.descriptorSetCount = 1;
        alloc_info.pSetLayouts = &m_GPU.descriptor_set_layout;
        if (RHI_SUCCESS != m_Rhi->AllocateDescriptorSets(&alloc_info, m_GPU.descriptor_set))
        {
            return false;
        }
    }

    if (m_GPU.cull_pipeline == nullptr)
    {
        RHIShader* shader = m_Rhi->CreateShaderModule(Z::Shader::NANITE_CLUSTER_CULL_COMP);
        if (shader == nullptr)
        {
            LOG_WARNING(ZRender, "nanite: cluster-cull compute shader unavailable -- GPU culling stays off");
            return false;
        }

        RHIPipelineShaderStageCreateInfo stage {};
        stage.sType = RHI_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage = RHI_SHADER_STAGE_COMPUTE_BIT;
        stage.module = shader;
        stage.pName = "main";

        RHIComputePipelineCreateInfo pipeline_info {};
        pipeline_info.sType = RHI_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipeline_info.pStages = &stage;
        pipeline_info.layout = m_GPU.pipeline_layout;

        const bool ok = m_Rhi->CreateComputePipelines(RHI_NULL_HANDLE, 1, &pipeline_info, m_GPU.cull_pipeline);
        m_Rhi->DestroyShaderModule(shader);
        if (!ok || m_GPU.cull_pipeline == nullptr)
        {
            // Latch it: pipeline creation is deterministic, so retrying every frame
            // only spams the log and stalls the frame.
            m_GPU.gpu_pipelines_unsupported = true;
            LOG_WARNING(ZRender,
                        "nanite: cluster-cull compute pipeline creation failed; GPU culling "
                        "stays off for this session");
            return false;
        }
    }

    m_GPU.compute_ready = true;
    return true;
}

bool NaniteRenderer::EnsureRasterPipeline()
{
    if (m_GPU.raster_ready)
    {
        return true;
    }
    if (m_Rhi == nullptr || m_GPU.raster_render_pass == nullptr)
    {
        return false;
    }
    if (Z::Shader::NANITE_CLUSTER_VERT.empty() || Z::Shader::NANITE_CLUSTER_FRAG.empty())
    {
        return false;
    }
    if (!EnsureComputePipeline())
    {
        // 光栅管线共用同一份 layout / descriptor set。
        return false;
    }

    RHIShader* vert = m_Rhi->CreateShaderModule(Z::Shader::NANITE_CLUSTER_VERT);
    RHIShader* frag = m_Rhi->CreateShaderModule(Z::Shader::NANITE_CLUSTER_FRAG);
    if (vert == nullptr || frag == nullptr)
    {
        if (vert != nullptr)
        {
            m_Rhi->DestroyShaderModule(vert);
        }
        if (frag != nullptr)
        {
            m_Rhi->DestroyShaderModule(frag);
        }
        return false;
    }

    RHIPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = RHI_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = RHI_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].sType = RHI_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = RHI_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";

    // **没有顶点输入状态**：顶点位置是从 storage buffer 里按 gl_VertexIndex 取的，
    // 顶点属性一个都不用。只绑索引缓冲。
    RHIPipelineVertexInputStateCreateInfo vertex_input {};
    vertex_input.sType = RHI_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount = 0;
    vertex_input.pVertexBindingDescriptions = nullptr;
    vertex_input.vertexAttributeDescriptionCount = 0;
    vertex_input.pVertexAttributeDescriptions = nullptr;

    RHIPipelineInputAssemblyStateCreateInfo input_assembly {};
    input_assembly.sType = RHI_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = RHI_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    input_assembly.primitiveRestartEnable = RHI_FALSE;

    RHIPipelineViewportStateCreateInfo viewport_state {};
    viewport_state.sType = RHI_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.pViewports = m_Rhi->GetSwapchainInfo().viewport;
    viewport_state.scissorCount = 1;
    viewport_state.pScissors = m_Rhi->GetSwapchainInfo().scissor;

    RHIPipelineRasterizationStateCreateInfo rasterization {};
    rasterization.sType = RHI_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.polygonMode = RHI_POLYGON_MODE_FILL;
    rasterization.cullMode = RHI_CULL_MODE_BACK_BIT;
    rasterization.frontFace = RHI_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.lineWidth = 1.0f;

    RHIPipelineMultisampleStateCreateInfo multisample {};
    multisample.sType = RHI_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = RHI_SAMPLE_COUNT_1_BIT;

    // 三个 blend 附件 —— 一片元写三个 GBuffer target，一个不多一个不少。
    // 写 1 个的话 Vulkan 直接拒绝建管线（附件数与 subpass 不符），Metal 会在开
    // pass 时因为附件 1/2 没配而炸；写 4 个则配不上 GBuffer 的 render pass。
    RHIPipelineColorBlendAttachmentState
        blend[kNaniteRasterColorAttachmentCount] {};
    for (uint32_t i = 0; i < kNaniteRasterColorAttachmentCount; ++i)
    {
        blend[i].blendEnable = RHI_FALSE;
        blend[i].colorWriteMask = RHI_COLOR_COMPONENT_R_BIT | RHI_COLOR_COMPONENT_G_BIT |
                                  RHI_COLOR_COMPONENT_B_BIT | RHI_COLOR_COMPONENT_A_BIT;
    }

    RHIPipelineColorBlendStateCreateInfo blend_state {};
    blend_state.sType = RHI_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend_state.attachmentCount = kNaniteRasterColorAttachmentCount;
    blend_state.pAttachments = blend;

    RHIPipelineDepthStencilStateCreateInfo depth_stencil {};
    depth_stencil.sType = RHI_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil.depthTestEnable = RHI_TRUE;
    depth_stencil.depthWriteEnable = RHI_TRUE;
    depth_stencil.depthCompareOp = RHI_COMPARE_OP_LESS;

    RHIDynamicState dynamic_states[] = {RHI_DYNAMIC_STATE_VIEWPORT, RHI_DYNAMIC_STATE_SCISSOR};
    RHIPipelineDynamicStateCreateInfo dynamic_state {};
    dynamic_state.sType = RHI_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic_state.dynamicStateCount = 2;
    dynamic_state.pDynamicStates = dynamic_states;

    RHIGraphicsPipelineCreateInfo pipeline_info {};
    pipeline_info.sType = RHI_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_info.stageCount = 2;
    pipeline_info.pStages = stages;
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &input_assembly;
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &rasterization;
    pipeline_info.pMultisampleState = &multisample;
    pipeline_info.pColorBlendState = &blend_state;
    pipeline_info.pDepthStencilState = &depth_stencil;
    pipeline_info.pDynamicState = &dynamic_state;
    pipeline_info.layout = m_GPU.pipeline_layout;
    pipeline_info.renderPass = m_GPU.raster_render_pass;
    pipeline_info.subpass = 0;

    const bool ok =
        m_Rhi->CreateGraphicsPipelines(RHI_NULL_HANDLE, 1, &pipeline_info, m_GPU.raster_pipeline);
    m_Rhi->DestroyShaderModule(vert);
    m_Rhi->DestroyShaderModule(frag);

    if (!ok || m_GPU.raster_pipeline == nullptr)
    {
        LOG_WARNING(ZRender, "nanite: cluster raster pipeline creation failed");
        return false;
    }

    m_GPU.raster_ready = true;
    return true;
}

NaniteCullConstants NaniteRenderer::BuildCullConstants(const std::shared_ptr<RenderCamera>& camera,
                                                       const NaniteConfig& config)
{
    NaniteCullConstants constants {};

    if (camera != nullptr)
    {
        const Matrix4x4 view_projection = camera->GetProjectionMatrix() * camera->GetViewMatrix();
        NaniteGPUPackMatrix(view_projection, &constants.view_projection.m_Mat[0][0]);

        std::vector<Vector4> planes;
        m_CullingSystem.ExtractFrustumPlanes(view_projection, planes);
        for (size_t i = 0; i < planes.size() && i < 6; ++i)
        {
            constants.frustum_planes[i] = planes[i];
        }

        const Vector3 position = camera->position();
        constants.camera_position = Vector4(position.x, position.y, position.z, 0.0f);
    }

    constants.lod_params = Vector4(config.max_screen_error, config.lod_bias, 0.0f, 0.0f);
    constants.counts.x = static_cast<uint32_t>(m_SceneGPUData.clusters.size());
    constants.counts.y = static_cast<uint32_t>(m_SceneGPUData.instances.size());
    constants.counts.z = config.enable_lod ? 1u : 0u;
    constants.counts.w = config.enable_culling ? 1u : 0u;

    return constants;
}

bool NaniteRenderer::PrepareGPUData(const std::shared_ptr<RenderCamera>& camera, const NaniteConfig& config)
{
    if (m_Rhi == nullptr)
    {
        return false;
    }

    NaniteBuildSceneGPUData(m_Instances, m_ClusterManager, m_SceneGPUData);
    if (m_SceneGPUData.empty())
    {
        return false;
    }

    const uint32_t cluster_count = static_cast<uint32_t>(m_SceneGPUData.clusters.size());
    const uint32_t instance_count = static_cast<uint32_t>(m_SceneGPUData.instances.size());
    const uint32_t position_count = static_cast<uint32_t>(m_SceneGPUData.positions.size());
    const uint32_t index_count = static_cast<uint32_t>(m_SceneGPUData.indices.size());

    const uint32_t old_cluster_capacity = m_GPU.cluster_capacity;
    if (!EnsureGPUResources(cluster_count, instance_count, position_count, index_count))
    {
        return false;
    }

    // 几何和集群只在容量变化（也就是缓冲被重建）时才需要重传；但实例变换每帧都可能
    // 变，所以它每帧都传。这样"相机不动、物体动"这种最常见的情况不会被跳过。
    const bool buffers_recreated = m_GPU.cluster_capacity != old_cluster_capacity;

    if (buffers_recreated || !m_GPU.compute_ready)
    {
        UploadBytes(m_GPU.clusters, m_GPU.clusters_memory, m_SceneGPUData.clusters.data(),
                    m_SceneGPUData.clusters.size() * sizeof(NaniteGPUCluster));
        UploadBytes(m_GPU.positions, m_GPU.positions_memory, m_SceneGPUData.positions.data(),
                    m_SceneGPUData.positions.size() * sizeof(Vector4));
        UploadBytes(m_GPU.indices, m_GPU.indices_memory, m_SceneGPUData.indices.data(),
                    m_SceneGPUData.indices.size() * sizeof(uint32_t));
    }

    UploadBytes(m_GPU.instances, m_GPU.instances_memory, m_SceneGPUData.instances.data(),
                m_SceneGPUData.instances.size() * sizeof(NaniteGPUInstance));

    if (!EnsureComputePipeline())
    {
        return false;
    }

    // 描述符写入必须放在缓冲都建好之后 —— 缓冲重建过就得重写，否则 kernel 还指着
    // 被销毁的旧句柄。
    UpdateDescriptorWrites();

    const NaniteCullConstants constants = BuildCullConstants(camera, config);
    UploadBytes(m_GPU.cull_constants, m_GPU.cull_constants_memory, &constants, sizeof(constants));

    return true;
}

void NaniteRenderer::DispatchClusterCulling(const std::shared_ptr<RenderCamera>& camera, const NaniteConfig& config)
{
    if (m_Rhi == nullptr || !m_GPU.compute_ready || m_GPU.cull_pipeline == nullptr)
    {
        return;
    }

    const uint32_t cluster_count = static_cast<uint32_t>(m_SceneGPUData.clusters.size());
    if (cluster_count == 0)
    {
        return;
    }

    RHICommandBuffer* command_buffer = m_Rhi->GetCurrentCommandBuffer();

    // 没有帧命令缓冲（编辑器离屏更新、无窗口的 headless、或者这一帧还没
    // PrepareBeforePass）时退到"单次命令缓冲"：自己开、自己提交并等它跑完。
    // 以前这里是直接 return —— 于是除了完整帧循环之外，Nanite 的 GPU 剔除一行都
    // 跑不起来，也没有任何办法在没有窗口的机器上验证它。
    const bool owns_command_buffer = (command_buffer == nullptr);
    if (owns_command_buffer)
    {
        command_buffer = m_Rhi->BeginSingleTimeCommands();
    }
    if (command_buffer == nullptr)
    {
        return;
    }

    // **计数器必须清零**：kernel 用 atomicAdd，不清零就是往上一帧的结果上累加。
    NaniteCullCounter zero_counter {};
    UploadBytes(m_GPU.cull_counter, m_GPU.cull_counter_memory, &zero_counter, sizeof(zero_counter));

    // 命令缓冲清的是**整个容量段**，不是 [0, cluster_count)。
    //
    // kernel 会给每个 cluster_index < counts.x 的槽位写一条命令（被剔除的写的是
    // indexCount == 0 的空命令），所以 [0, cluster_count) 这段其实它自己就刷干净了。
    // 真正没人写的是**尾部**：group 数向上取整多出来的那些线程直接 return，对应槽
    // 位留的是上一帧的值。只清 cluster_count 个等于把唯一需要保护的那段漏掉了 ——
    // 变异测试（把这段上传删掉）之所以抓不到，正是因为清的和 kernel 清的是同一段。
    //
    // 尾部今天确实不会被 CmdDrawIndexedIndirect 读到（drawCount = cluster_count），
    // 但它是"容量涨回去"那一刻的定时炸弹：集群数一变多，那些槽位立刻进入绘制范围。
    std::vector<RHIDrawIndexedIndirectCommand> zero_commands(m_GPU.cluster_capacity);
    for (RHIDrawIndexedIndirectCommand& command : zero_commands)
    {
        command = RHIDrawIndexedIndirectCommand {};
        command.vertexOffset = 0;
    }
    UploadBytes(m_GPU.draw_commands,
                m_GPU.draw_commands_memory,
                zero_commands.data(),
                zero_commands.size() * sizeof(RHIDrawIndexedIndirectCommand));

    const NaniteCullConstants constants = BuildCullConstants(camera, config);
    UploadBytes(m_GPU.cull_constants, m_GPU.cull_constants_memory, &constants, sizeof(constants));

    m_Rhi->CmdBindPipelinePFN(command_buffer, RHI_PIPELINE_BIND_POINT_COMPUTE, m_GPU.cull_pipeline);
    m_Rhi->CmdBindDescriptorSetsPFN(command_buffer,
                                    RHI_PIPELINE_BIND_POINT_COMPUTE,
                                    m_GPU.pipeline_layout,
                                    0,
                                    1,
                                    &m_GPU.descriptor_set,
                                    0,
                                    nullptr);

    // local_size_x = 64，group 数向上取整。
    const uint32_t group_count = (cluster_count + 63u) / 64u;
    m_Rhi->CmdDispatch(command_buffer, group_count, 1, 1);

    // 自己开的缓冲就得自己收尾：EndSingleTimeCommands 的契约是"提交并等它跑完"
    // （Vulkan 是 vkQueueWaitIdle，DX12 是 Signal + WaitForFences，Metal 之前漏了
    // 这一步，见 MetalRHI 的改动）。等完这一步，draw_commands 的内容才对 CPU 可见
    // —— 冒烟测试就是靠这一点把结果读回来的。
    if (owns_command_buffer)
    {
        m_Rhi->EndSingleTimeCommands(command_buffer);
    }

    m_GPU.dispatch_cluster_count = cluster_count;
}

void NaniteRenderer::DrawClustersIndirect()
{
    if (m_Rhi == nullptr || m_GPU.dispatch_cluster_count == 0)
    {
        return;
    }

    // 已经注入了目标 ⇒ 那个 pass 属于调用方，绘制要发生在它开着的时候。这里再画
    // 一遍就是往没人读的离屏图上再画一份，白白多一次绘制。
    //
    // 以前这里只认 raster_render_pass，而它**从来没有调用者** ⇒ 光栅管线永远建不
    // 起来 ⇒ 剔除完的命令一条都没被消费。Nanite 于是"跑在 GPU 上"却什么都不画。
    if (m_GPU.external_render_pass != nullptr)
    {
        return;
    }

    // ---- 1. 光栅目标：Nanite 自己的离屏 target ----------------------------------
    if (!EnsureRasterTarget())
    {
        return;
    }

    RHIRenderPass*  pass = m_GPU.target_render_pass;
    RHIFramebuffer* framebuffer = m_GPU.target_framebuffer;
    if (pass == nullptr || framebuffer == nullptr)
    {
        return;
    }

    // 目标换了 ⇒ 管线得照着新 pass 的附件格式重建。
    if (m_GPU.raster_render_pass != pass)
    {
        m_GPU.raster_render_pass = pass;
        m_GPU.raster_framebuffer = framebuffer;
        m_GPU.raster_ready = false;
    }

    if (!m_GPU.raster_ready && !EnsureRasterPipeline())
    {
        return;
    }

    // ---- 2. 命令缓冲 ------------------------------------------------------------
    //
    // 没有帧命令缓冲（headless、编辑器离屏更新、还没 PrepareBeforePass）就自己开
    // 一条一次性的：自己开、自己提交并等它跑完。返回时离屏 target 的内容已经落地。
    const bool owns_command_buffer = (m_Rhi->GetCurrentCommandBuffer() == nullptr);
    RHICommandBuffer* command_buffer =
        owns_command_buffer ? m_Rhi->BeginSingleTimeCommands() : m_Rhi->GetCurrentCommandBuffer();
    if (command_buffer == nullptr)
    {
        return;
    }

    // ---- 3. 开 pass --------------------------------------------------------------
    //
    // 走到这里 target 一定是 Nanite 自己的，所以 pass 的开与关都归它。
    const uint32_t width = m_GPU.target_width;
    const uint32_t height = m_GPU.target_height;

    // 三张 GBuffer 全部清成 0（alpha 也是 0），深度清成 1.0 —— 和主相机
    // DrawGBufferPass 的清屏值逐项一致。
    //
    // 全零是有语义的，不是随便挑的：GBufferB 全零 ⇒ shadingModelId = 0（unlit），
    // 正是"这块像素没有几何"该有的值，延迟光照不会去给它打光。
    //
    // 别改成"深灰当背景"那种非零值：GBufferC 是 R8G8B8A8_SRGB，清屏值会被做一次
    // linear→sRGB 编码，0.05 落到字节上是 62 —— 一个看起来"很暗"的清屏色会被读回
    // 来当成"有东西画上去了"。0 的编码还是 0，这才是唯一安全的"空"标记。
    constexpr uint32_t kClearCount = kNaniteRasterColorAttachmentCount + 1;
    RHIClearValue     clear_values[kClearCount] = {};
    for (uint32_t i = 0; i < kNaniteRasterColorAttachmentCount; ++i)
    {
        clear_values[i].color.float32[0] = 0.0f;
        clear_values[i].color.float32[1] = 0.0f;
        clear_values[i].color.float32[2] = 0.0f;
        clear_values[i].color.float32[3] = 0.0f;
    }
    clear_values[kNaniteRasterColorAttachmentCount].depthStencil.depth = 1.0f;
    clear_values[kNaniteRasterColorAttachmentCount].depthStencil.stencil = 0;

    RHIRenderPassBeginInfo begin_info {};
    begin_info.sType = RHI_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    begin_info.pNext = nullptr;
    begin_info.renderPass = pass;
    begin_info.framebuffer = framebuffer;
    begin_info.renderArea.offset.x = 0;
    begin_info.renderArea.offset.y = 0;
    begin_info.renderArea.extent.width = width;
    begin_info.renderArea.extent.height = height;
    begin_info.clearValueCount = kClearCount;
    begin_info.pClearValues = clear_values;

    m_Rhi->CmdBeginRenderPassPFN(command_buffer, &begin_info, RHI_SUBPASS_CONTENTS_INLINE);

    // viewport / scissor 在管线里是**动态状态**，Vulkan / DX12 不设就是未定义
    // 行为（Vulkan 上会真的画不出来）。所以这里必须设。
    //
    // 已知盲区：Metal 的默认 viewport 恰好等于附件尺寸，所以"删掉这两行"这个
    // 变异在本机跑不出来（59 项全绿）。这两行是被另外三个后端需要的，不是被
    // 这个测试钉住的 —— 别以为它绿就等于验证了。
    RHIViewport viewport {};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(width);
    viewport.height = static_cast<float>(height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    m_Rhi->CmdSetViewportPFN(command_buffer, 0, 1, &viewport);

    RHIRect2D scissor {};
    scissor.offset.x = 0;
    scissor.offset.y = 0;
    scissor.extent.width = width;
    scissor.extent.height = height;
    m_Rhi->CmdSetScissorPFN(command_buffer, 0, 1, &scissor);

    // ---- 4. 唯一的一次绘制 ------------------------------------------------------
    RecordClusterDraw(command_buffer);

    m_Rhi->CmdEndRenderPassPFN(command_buffer);

    if (owns_command_buffer)
    {
        m_Rhi->EndSingleTimeCommands(command_buffer);
    }
}

void NaniteRenderer::DrawClustersIndirectInPass(RHICommandBuffer* command_buffer)
{
    if (m_Rhi == nullptr || command_buffer == nullptr)
    {
        // 没有命令缓冲 = 调用方并没有真的站在一个开着的 pass 里。宁可什么都不画，
        // 也不要往一个不存在的 encoder 里塞绘制命令。
        return;
    }
    if (m_GPU.external_render_pass == nullptr)
    {
        // 没有注入目标 ⇒ 没有人声明"我在这个 pass 里"。离屏那半边由
        // DrawClustersIndirect() 负责，这里不替它做决定。
        return;
    }
    if (m_GPU.dispatch_cluster_count == 0)
    {
        return;
    }

    // 注入的 pass 换了（或者第一次用）⇒ 管线照着它的附件格式重建。
    //
    // raster_framebuffer 在这里恒为 nullptr：那个 framebuffer 是调用方的，Nanite
    // 既不 begin 也不 end 它。
    if (m_GPU.raster_render_pass != m_GPU.external_render_pass)
    {
        m_GPU.raster_render_pass = m_GPU.external_render_pass;
        m_GPU.raster_framebuffer = nullptr;
        m_GPU.raster_ready = false;
    }

    if (!m_GPU.raster_ready && !EnsureRasterPipeline())
    {
        return;
    }

    // 只追加绘制。viewport / scissor / clear 都是调用方的：它在开 pass 之前已经
    // 设好了自己的视口，Nanite 抢过去改会把排在后面的绘制一起改坏。
    RecordClusterDraw(command_buffer);
}

void NaniteRenderer::RecordClusterDraw(RHICommandBuffer* command_buffer)
{
    if (m_Rhi == nullptr || command_buffer == nullptr)
    {
        return;
    }

    m_Rhi->CmdBindPipelinePFN(command_buffer, RHI_PIPELINE_BIND_POINT_GRAPHICS, m_GPU.raster_pipeline);
    m_Rhi->CmdBindDescriptorSetsPFN(command_buffer,
                                    RHI_PIPELINE_BIND_POINT_GRAPHICS,
                                    m_GPU.pipeline_layout,
                                    0,
                                    1,
                                    &m_GPU.descriptor_set,
                                    0,
                                    nullptr);
    m_Rhi->CmdBindIndexBufferPFN(command_buffer, m_GPU.indices, 0, RHI_INDEX_TYPE_UINT32);

    // drawCount 是**上界**（集群总数）。被剔除的集群贡献 indexCount == 0 的空命令，
    // GPU 处理它们几乎不花钱，CPU 因此不必把计数读回来。
    m_Rhi->CmdDrawIndexedIndirect(command_buffer,
                                  m_GPU.draw_commands,
                                  0,
                                  m_GPU.dispatch_cluster_count,
                                  static_cast<uint32_t>(sizeof(RHIDrawIndexedIndirectCommand)));
}

void NaniteRenderer::SetRasterRenderPass(RHIRenderPass* render_pass, RHIFramebuffer* framebuffer)
{
    // 只认"两个都给了"。给了 render pass 却不给 framebuffer 的情况没法开 pass，
    // 与其猜不如退回离屏 target —— 那条路至少是能跑通的。
    const bool has_external = render_pass != nullptr && framebuffer != nullptr;

    if (m_GPU.external_render_pass == (has_external ? render_pass : nullptr) &&
        m_GPU.external_framebuffer == (has_external ? framebuffer : nullptr))
    {
        return;
    }

    m_GPU.external_render_pass = has_external ? render_pass : nullptr;
    m_GPU.external_framebuffer = has_external ? framebuffer : nullptr;

    // 目标换人了 ⇒ 光栅管线得照着新 pass 的附件格式重建。
    m_GPU.raster_render_pass = nullptr;
    m_GPU.raster_framebuffer = nullptr;
    m_GPU.raster_ready = false;
}

void NaniteRenderer::SetRasterTargetSize(uint32_t width, uint32_t height)
{
    if (m_RasterTargetWidth == width && m_RasterTargetHeight == height)
    {
        return;
    }
    m_RasterTargetWidth = width;
    m_RasterTargetHeight = height;

    // 尺寸变了，离屏 target 及其上的管线都得重建。外部注入的目标不受影响。
    m_GPU.raster_render_pass = nullptr;
    m_GPU.raster_framebuffer = nullptr;
    m_GPU.raster_ready = false;
    DestroyRasterTarget();
}

uint32_t NaniteRenderer::ResolveRasterTargetWidth() const
{
    if (m_RasterTargetWidth != 0)
    {
        return m_RasterTargetWidth;
    }
    if (m_Rhi != nullptr)
    {
        const RHISwapChainDesc desc = m_Rhi->GetSwapchainInfo();
        if (IsSaneTargetSize(desc.extent.width))
        {
            return desc.extent.width;
        }
    }
    return kNaniteDefaultRasterTargetSize;
}

uint32_t NaniteRenderer::ResolveRasterTargetHeight() const
{
    if (m_RasterTargetHeight != 0)
    {
        return m_RasterTargetHeight;
    }
    if (m_Rhi != nullptr)
    {
        const RHISwapChainDesc desc = m_Rhi->GetSwapchainInfo();
        if (IsSaneTargetSize(desc.extent.height))
        {
            return desc.extent.height;
        }
    }
    return kNaniteDefaultRasterTargetSize;
}

bool NaniteRenderer::EnsureRasterTarget()
{
    if (m_Rhi == nullptr)
    {
        return false;
    }

    const uint32_t width = ResolveRasterTargetWidth();
    const uint32_t height = ResolveRasterTargetHeight();
    if (width == 0 || height == 0)
    {
        return false;
    }

    if (m_GPU.target_render_pass != nullptr && m_GPU.target_framebuffer != nullptr &&
        m_GPU.target_width == width && m_GPU.target_height == height)
    {
        return true;
    }

    DestroyRasterTarget();

    // ---- GBuffer 形状的三张颜色图 + 一张深度 -------------------------------------
    //
    // 格式抄 MainCameraFramebufferResources::SetupAttachments：A/B 是 R8G8B8A8_UNORM，
    // C 是 R8G8B8A8_SRGB（sRGB 给暗部多一点精度）。离屏 target 照抄真实 GBuffer 是
    // 故意的 —— 这样本机跑的就是主相机那条通路，而不是一个"形状差不多"的替身。
    //
    // 三张都带 TRANSFER_SRC：冒烟测试要把它们读回 CPU 才能断言写进去的值对不对。
    constexpr RHIFormat kGbufferFormats[kNaniteRasterColorAttachmentCount] = {
        RHI_FORMAT_R8G8B8A8_UNORM,  // GBufferA：编码后的世界法线
        RHI_FORMAT_R8G8B8A8_UNORM,  // GBufferB：metallic / specular / roughness / shading model
        RHI_FORMAT_R8G8B8A8_SRGB,   // GBufferC：baseColor
    };
    constexpr RHIFormat kDepthFormat = RHI_FORMAT_D32_SFLOAT;

    for (uint32_t i = 0; i < kNaniteRasterColorAttachmentCount; ++i)
    {
        m_Rhi->CreateImage(width,
                           height,
                           kGbufferFormats[i],
                           RHI_IMAGE_TILING_OPTIMAL,
                           RHI_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | RHI_IMAGE_USAGE_TRANSFER_SRC_BIT,
                           RHI_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                           m_GPU.target_gbuffer[i],
                           m_GPU.target_gbuffer_memory[i],
                           0,
                           1,
                           1);
        m_Rhi->CreateImageView(m_GPU.target_gbuffer[i],
                               kGbufferFormats[i],
                               RHI_IMAGE_ASPECT_COLOR_BIT,
                               RHI_IMAGE_VIEW_TYPE_2D,
                               1,
                               1,
                               m_GPU.target_gbuffer_view[i]);
    }

    m_Rhi->CreateImage(width,
                       height,
                       kDepthFormat,
                       RHI_IMAGE_TILING_OPTIMAL,
                       RHI_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                       RHI_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                       m_GPU.target_depth,
                       m_GPU.target_depth_memory,
                       0,
                       1,
                       1);
    m_Rhi->CreateImageView(m_GPU.target_depth,
                           kDepthFormat,
                           RHI_IMAGE_ASPECT_DEPTH_BIT,
                           RHI_IMAGE_VIEW_TYPE_2D,
                           1,
                           1,
                           m_GPU.target_depth_view);

    bool gbuffer_views_ready = m_GPU.target_depth_view != nullptr;
    for (uint32_t i = 0; i < kNaniteRasterColorAttachmentCount; ++i)
    {
        if (m_GPU.target_gbuffer_view[i] == nullptr)
        {
            gbuffer_views_ready = false;
        }
    }
    if (!gbuffer_views_ready)
    {
        LOG_WARNING(ZRender, "nanite: offscreen raster target allocation failed");
        DestroyRasterTarget();
        return false;
    }

    // ---- render pass：3 color + 1 depth，都 clear --------------------------------
    constexpr uint32_t kDepthAttachmentIndex = kNaniteRasterColorAttachmentCount;
    constexpr uint32_t kAttachmentCount = kNaniteRasterColorAttachmentCount + 1;

    RHIAttachmentDescription attachments[kAttachmentCount] = {};
    for (uint32_t i = 0; i < kNaniteRasterColorAttachmentCount; ++i)
    {
        attachments[i].flags = 0;
        attachments[i].format = kGbufferFormats[i];
        attachments[i].samples = RHI_SAMPLE_COUNT_1_BIT;
        attachments[i].loadOp = RHI_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[i].storeOp = RHI_ATTACHMENT_STORE_OP_STORE;
        attachments[i].stencilLoadOp = RHI_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[i].stencilStoreOp = RHI_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[i].initialLayout = RHI_IMAGE_LAYOUT_UNDEFINED;
        attachments[i].finalLayout = RHI_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }

    attachments[kDepthAttachmentIndex].flags = 0;
    attachments[kDepthAttachmentIndex].format = kDepthFormat;
    attachments[kDepthAttachmentIndex].samples = RHI_SAMPLE_COUNT_1_BIT;
    attachments[kDepthAttachmentIndex].loadOp = RHI_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[kDepthAttachmentIndex].storeOp = RHI_ATTACHMENT_STORE_OP_STORE;
    attachments[kDepthAttachmentIndex].stencilLoadOp = RHI_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[kDepthAttachmentIndex].stencilStoreOp = RHI_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[kDepthAttachmentIndex].initialLayout = RHI_IMAGE_LAYOUT_UNDEFINED;
    attachments[kDepthAttachmentIndex].finalLayout = RHI_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    RHIAttachmentReference color_references[kNaniteRasterColorAttachmentCount] = {};
    for (uint32_t i = 0; i < kNaniteRasterColorAttachmentCount; ++i)
    {
        color_references[i].attachment = i;
        color_references[i].layout = RHI_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }

    RHIAttachmentReference depth_reference {};
    depth_reference.attachment = kDepthAttachmentIndex;
    depth_reference.layout = RHI_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    RHISubpassDescription subpass {};
    subpass.flags = 0;
    subpass.pipelineBindPoint = RHI_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.inputAttachmentCount = 0;
    subpass.pInputAttachments = nullptr;
    subpass.colorAttachmentCount = kNaniteRasterColorAttachmentCount;
    subpass.pColorAttachments = color_references;
    subpass.pResolveAttachments = nullptr;
    subpass.pDepthStencilAttachment = &depth_reference;
    subpass.preserveAttachmentCount = 0;
    subpass.pPreserveAttachments = nullptr;

    RHIRenderPassCreateInfo render_pass_info {};
    render_pass_info.sType = RHI_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    render_pass_info.pNext = nullptr;
    render_pass_info.flags = 0;
    render_pass_info.attachmentCount = kAttachmentCount;
    render_pass_info.pAttachments = attachments;
    render_pass_info.subpassCount = 1;
    render_pass_info.pSubpasses = &subpass;
    render_pass_info.dependencyCount = 0;
    render_pass_info.pDependencies = nullptr;

    if (!m_Rhi->CreateRenderPass(&render_pass_info, m_GPU.target_render_pass))
    {
        LOG_WARNING(ZRender, "nanite: offscreen render pass creation failed");
        DestroyRasterTarget();
        return false;
    }

    // ---- framebuffer ------------------------------------------------------------
    RHIImageView* views[kAttachmentCount] = {};
    for (uint32_t i = 0; i < kNaniteRasterColorAttachmentCount; ++i)
    {
        views[i] = m_GPU.target_gbuffer_view[i];
    }
    views[kDepthAttachmentIndex] = m_GPU.target_depth_view;

    RHIFramebufferCreateInfo framebuffer_info {};
    framebuffer_info.sType = RHI_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebuffer_info.pNext = nullptr;
    framebuffer_info.flags = 0;
    framebuffer_info.renderPass = m_GPU.target_render_pass;
    framebuffer_info.attachmentCount = kAttachmentCount;
    framebuffer_info.pAttachments = views;
    framebuffer_info.width = width;
    framebuffer_info.height = height;
    framebuffer_info.layers = 1;

    if (!m_Rhi->CreateFramebuffer(&framebuffer_info, m_GPU.target_framebuffer))
    {
        LOG_WARNING(ZRender, "nanite: offscreen framebuffer creation failed");
        DestroyRasterTarget();
        return false;
    }

    m_GPU.target_width = width;
    m_GPU.target_height = height;
    return true;
}

void NaniteRenderer::DestroyRasterTarget()
{
    if (m_Rhi == nullptr)
    {
        return;
    }

    if (m_GPU.target_framebuffer != nullptr)
    {
        m_Rhi->DestroyFramebuffer(m_GPU.target_framebuffer);
        m_GPU.target_framebuffer = nullptr;
    }
    if (m_GPU.target_render_pass != nullptr)
    {
        m_Rhi->DestroyRenderPass(m_GPU.target_render_pass);
        m_GPU.target_render_pass = nullptr;
    }

    // 逆序销毁：view 先于 image，image 先于 memory。顺序写反在 Metal 上不会报错，
    // 但在 Vulkan 上是对已销毁句柄的 use-after-free。
    for (uint32_t i = 0; i < kNaniteRasterColorAttachmentCount; ++i)
    {
        if (m_GPU.target_gbuffer_view[i] != nullptr)
        {
            m_Rhi->DestroyImageView(m_GPU.target_gbuffer_view[i]);
            m_GPU.target_gbuffer_view[i] = nullptr;
        }
    }
    if (m_GPU.target_depth_view != nullptr)
    {
        m_Rhi->DestroyImageView(m_GPU.target_depth_view);
        m_GPU.target_depth_view = nullptr;
    }
    for (uint32_t i = 0; i < kNaniteRasterColorAttachmentCount; ++i)
    {
        if (m_GPU.target_gbuffer[i] != nullptr)
        {
            m_Rhi->DestroyImage(m_GPU.target_gbuffer[i]);
            m_GPU.target_gbuffer[i] = nullptr;
        }
    }
    if (m_GPU.target_depth != nullptr)
    {
        m_Rhi->DestroyImage(m_GPU.target_depth);
        m_GPU.target_depth = nullptr;
    }
    for (uint32_t i = 0; i < kNaniteRasterColorAttachmentCount; ++i)
    {
        if (m_GPU.target_gbuffer_memory[i] != nullptr)
        {
            m_Rhi->FreeMemory(m_GPU.target_gbuffer_memory[i]);
            m_GPU.target_gbuffer_memory[i] = nullptr;
        }
    }
    if (m_GPU.target_depth_memory != nullptr)
    {
        m_Rhi->FreeMemory(m_GPU.target_depth_memory);
        m_GPU.target_depth_memory = nullptr;
    }

    m_GPU.target_width = 0;
    m_GPU.target_height = 0;
}

void NaniteRenderer::UpdateGPUBuffers()
{
    // 真正的上传在 PrepareGPUData 里（它在 dispatch 之前，且知道相机与配置）。
    // 这里保留为空实现是为了不破坏既有的调用顺序 —— Render() 里 PrepareGPUData
    // 已经覆盖了它原本的职责。
}

NaniteVisibilityResult NaniteRenderer::PerformGPUCulling(const std::shared_ptr<RenderCamera>& camera,
                                                         const NaniteConfig& config)
{
    // GPU 剔除的结果是**留在 GPU 上的**（间接绘制命令 + 计数），CPU 这一帧不读回 ——
    // 读回要等一帧，整条 GPU-driven 链路就退化成同步的了。所以这里返回的是实例级的
    // CPU 结果：CPU 侧仍然需要它来组装 m_RenderBatches（编辑器/统计/调试在用），
    // 而逐集群的裁决完全交给 kernel。
    //
    // 用传进来的 config 而不是成员 m_Config —— 否则这一帧改的配置不生效。
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
        desired.use_gpu_culling == m_NaniteConfig.use_gpu_culling &&
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
