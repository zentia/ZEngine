#include "LumenSurfaceCache.h"

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Function/Render/Interface/RHI.h"
#include "Runtime/Function/Render/RenderHelper.h"
#include "Runtime/Function/Render/RenderResourceBase.h"

#include <algorithm>
#include <cmath>
#include <limits>

LumenSurfaceCacheManager::LumenSurfaceCacheManager() {}

LumenSurfaceCacheManager::~LumenSurfaceCacheManager()
{
    clear();
}

bool LumenSurfaceCacheManager::Initialize(RHI* rhi, const LumenConfig& config)
{
    m_Rhi = rhi;
    m_Config = config;

    // 初始化表面缓存
    m_SurfaceCache.cache_id = 0;
    m_SurfaceCache.base_resolution = config.surface_cache_resolution;
    m_SurfaceCache.needs_update = true;
    m_SurfaceCache.active_page_count = 0;
    m_SurfaceCache.total_page_count = config.max_surface_cache_pages;

    // 预分配页面槽位。注意这里**不**分配任何 per-texel 数组 —— CPU 路径没有卡片
    // 光栅化，按 64×64 预分配 7 个数组 × 1024 页是几百 MB，而每个 texel 存的还是
    // 同一个常量材质。等光栅化真的存在了再按 resolution 分配。
    m_SurfaceCache.pages.clear();
    m_SurfaceCache.pages.resize(config.max_surface_cache_pages);
    for (uint32_t i = 0; i < config.max_surface_cache_pages; ++i)
    {
        m_SurfaceCache.pages[i] = LumenSurfaceCachePage {};
        m_SurfaceCache.pages[i].page_id = i;
        m_SurfaceCache.pages[i].resolution = 0;
        m_SurfaceCache.pages[i].is_valid = false;
    }

    ResetPages();

    return true;
}

void LumenSurfaceCacheManager::clear()
{
    // 清理GPU资源
    if (m_SurfaceCacheTexture)
    {
        // TODO: 释放纹理资源
        m_SurfaceCacheTexture = nullptr;
    }
    if (m_SurfaceCacheSampler)
    {
        // TODO: 释放采样器资源
        m_SurfaceCacheSampler = nullptr;
    }

    // 清理数据
    m_SurfaceCache.pages.clear();
    m_PageGrid.clear();
    m_InstanceToPage.clear();
    m_FreePages.clear();
    m_SceneHash = 0;
    m_SurfaceCache.active_page_count = 0;
}

void LumenSurfaceCacheManager::ResetPages()
{
    m_PageGrid.clear();
    m_InstanceToPage.clear();
    m_FreePages.clear();

    for (uint32_t i = 0; i < m_SurfaceCache.pages.size(); ++i)
    {
        m_SurfaceCache.pages[i].is_valid = false;
        m_SurfaceCache.pages[i].world_bounds = AxisAlignedBox();
        m_SurfaceCache.pages[i].last_access_time = 0;
        m_FreePages.push_back(i);
    }

    m_SurfaceCache.active_page_count = 0;
}

uint64_t LumenSurfaceCacheManager::ComputeSceneHash(const std::vector<RenderEntity>& entities)
{
    // FNV-1a 64。同进程内的"变没变"判定，位级比较正是想要的（NaN/±0 也不该被当成没变）。
    uint64_t hash = 1469598103934665603ull;
    const auto add_bytes = [&hash](const void* data, size_t size) {
        const unsigned char* bytes = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < size; ++i)
        {
            hash ^= static_cast<uint64_t>(bytes[i]);
            hash *= 1099511628211ull;
        }
    };

    const uint64_t entity_count = entities.size();
    add_bytes(&entity_count, sizeof(entity_count));

    for (const RenderEntity& entity : entities)
    {
        add_bytes(&entity.m_InstanceId, sizeof(entity.m_InstanceId));
        add_bytes(&entity.m_MeshAssetId, sizeof(entity.m_MeshAssetId));
        add_bytes(&entity.m_MaterialAssetId, sizeof(entity.m_MaterialAssetId));
        add_bytes(entity.m_ModelMatrix.m_Mat, sizeof(entity.m_ModelMatrix.m_Mat));
        // 材质 factor：改了颜色必须重建缓存，否则 GI 会带着旧的反照率继续弹光
        add_bytes(&entity.m_BaseColorFactor, sizeof(entity.m_BaseColorFactor));
        add_bytes(&entity.m_MetallicFactor, sizeof(entity.m_MetallicFactor));
        add_bytes(&entity.m_RoughnessFactor, sizeof(entity.m_RoughnessFactor));
        add_bytes(&entity.m_EmissiveFactor, sizeof(entity.m_EmissiveFactor));
    }

    return hash;
}

AxisAlignedBox LumenSurfaceCacheManager::ComputeInstanceWorldBounds(const RenderEntity& entity,
                                                                    RenderResourceBase* render_resource)
{
    // RenderEntity::m_BoundingBox 是局部空间的，要拿 m_ModelMatrix 变换一遍
    // （可见性剔除和距离场那边都是这么做的）。网格资产加载失败的实例包围盒是无效的，
    // 退回 CPU 几何缓存里的局部包围盒。
    AxisAlignedBox local_bounds = entity.m_BoundingBox;
    if (!local_bounds.IsValid() && render_resource != nullptr)
    {
        const CpuMeshGeometry* geometry = render_resource->FindMeshGeometry(entity.m_MeshAssetId);
        if (geometry != nullptr)
        {
            local_bounds = geometry->local_bounds;
        }
    }

    if (!local_bounds.IsValid())
    {
        return AxisAlignedBox();
    }

    const BoundingBox local_box {local_bounds.getMinCorner(), local_bounds.getMaxCorner()};
    const BoundingBox world_box = BoundingBoxTransform(local_box, entity.m_ModelMatrix);

    AxisAlignedBox instance_bounds;
    instance_bounds.Merge(world_box.min_bound);
    instance_bounds.Merge(world_box.max_bound);
    return instance_bounds;
}

bool LumenSurfaceCacheManager::BuildFromScene(std::shared_ptr<RenderScene> render_scene,
                                              RenderResourceBase* render_resource,
                                              const Vector3& camera_position)
{
    if (!render_scene)
    {
        return false;
    }

    const std::vector<RenderEntity>& entities = render_scene->m_RenderEntities;

    // 先把每个实例的世界 AABB 收齐：格子边长要按它们的最大值来定（见 m_PageGrid 注释）
    struct PageCandidate
    {
        const RenderEntity* entity {nullptr};
        AxisAlignedBox bounds;
    };

    std::vector<PageCandidate> candidates;
    candidates.reserve(entities.size());
    for (const RenderEntity& entity : entities)
    {
        const AxisAlignedBox bounds = ComputeInstanceWorldBounds(entity, render_resource);
        if (!bounds.IsValid())
        {
            continue;
        }
        candidates.push_back(PageCandidate {&entity, bounds});
    }

    ResetPages();

    if (candidates.empty())
    {
        m_SceneHash = ComputeSceneHash(entities);
        m_SurfaceCache.needs_update = false;
        m_SurfaceCache.world_bounds = AxisAlignedBox();
        return true;
    }

    float max_extent = 0.0f;
    AxisAlignedBox scene_bounds;
    for (const PageCandidate& candidate : candidates)
    {
        const Vector3 size = candidate.bounds.getMaxCorner() - candidate.bounds.getMinCorner();
        max_extent = std::max({max_extent, size.x, size.y, size.z});
        scene_bounds.Merge(candidate.bounds.getMinCorner());
        scene_bounds.Merge(candidate.bounds.getMaxCorner());
    }
    m_CellSize = std::max(max_extent, 1e-3f);

    // 页面覆盖区域要在实例 AABB 外留一点余量：距离场命中点最多落在表面外半个 voxel
    // 对角线处（见 LumenRayTracing），薄壳几何（比如一张地板 quad）的 AABB 厚度为 0，
    // 不留余量的话 Contains 永远为 false。余量按场景尺度取百分比 —— 距离场的 voxel
    // 尺寸本来就是"场景尺寸 / 分辨率"，两者是同阶的。
    const Vector3 scene_size = scene_bounds.getMaxCorner() - scene_bounds.getMinCorner();
    const float margin = std::max({scene_size.x, scene_size.y, scene_size.z, 1.0f}) * 0.02f;

    for (const PageCandidate& candidate : candidates)
    {
        const uint32_t page_id = AllocatePage();
        if (page_id == kInvalidPage)
        {
            LOG_WARNING(ZRender,
                        "lumenSurfaceCache: ran out of pages ({}); {} instances left without a cached surface",
                        m_SurfaceCache.pages.size(),
                        candidates.size() - m_SurfaceCache.active_page_count);
            break;
        }

        LumenSurfaceCachePage& page = m_SurfaceCache.pages[page_id];
        page.page_id = page_id;
        page.resolution = 0;  // CPU 路径无卡片光栅化，不分配 texel
        page.world_position = (candidate.bounds.getMinCorner() + candidate.bounds.getMaxCorner()) * 0.5f;
        page.normal = Vector3(0.0f, 1.0f, 0.0f);
        page.world_bounds = candidate.bounds;
        page.world_bounds.Merge(candidate.bounds.getMinCorner() - Vector3(margin, margin, margin));
        page.world_bounds.Merge(candidate.bounds.getMaxCorner() + Vector3(margin, margin, margin));

        page.albedo = Vector3(candidate.entity->m_BaseColorFactor.x,
                              candidate.entity->m_BaseColorFactor.y,
                              candidate.entity->m_BaseColorFactor.z);
        page.roughness = candidate.entity->m_RoughnessFactor;
        page.metallic = candidate.entity->m_MetallicFactor;
        page.emissive = candidate.entity->m_EmissiveFactor;
        page.instance_id = candidate.entity->m_InstanceId;

        page.lod_level = CalculatePageLOD(page.world_position, camera_position);
        page.last_access_time = m_FrameIndex;
        page.is_valid = true;

        m_InstanceToPage[candidate.entity->m_InstanceId] = page_id;
        RegisterPageInGrid(page_id);
    }

    m_SceneHash = ComputeSceneHash(entities);
    m_SurfaceCache.needs_update = false;
    m_SurfaceCache.world_bounds = scene_bounds;

    // 更新GPU资源
    UpdateGPUResources();

    return true;
}

void LumenSurfaceCacheManager::UpdateSurfaceCache(std::shared_ptr<RenderScene> render_scene,
                                                  RenderResourceBase* render_resource,
                                                  const Vector3& camera_position)
{
    // 帧号是所有 LRU 判定的时基，必须每帧推进
    ++m_FrameIndex;

    const uint64_t scene_hash = render_scene != nullptr ? ComputeSceneHash(render_scene->m_RenderEntities) : 0;

    // 场景变了 / 被 markForUpdate() 强制 / 缓存是空的 —— 三者任一都要重建。
    // 最后一条不能省：页面被驱逐光之后如果只看哈希，缓存会永远空着不再回填。
    if (scene_hash != m_SceneHash || m_SurfaceCache.needs_update || m_SurfaceCache.active_page_count == 0)
    {
        BuildFromScene(render_scene, render_resource, camera_position);
        return;
    }

    CleanupInvalidPages();
}

LumenSurfaceCacheManager::CellCoord LumenSurfaceCacheManager::CellOf(const Vector3& world_position) const
{
    CellCoord cell;
    cell.x = static_cast<int32_t>(std::floor(world_position.x / m_CellSize));
    cell.y = static_cast<int32_t>(std::floor(world_position.y / m_CellSize));
    cell.z = static_cast<int32_t>(std::floor(world_position.z / m_CellSize));
    return cell;
}

void LumenSurfaceCacheManager::RegisterPageInGrid(uint32_t page_id)
{
    if (page_id >= m_SurfaceCache.pages.size())
    {
        return;
    }

    const AxisAlignedBox& bounds = m_SurfaceCache.pages[page_id].world_bounds;
    if (!bounds.IsValid())
    {
        return;
    }

    // 页面登记到它覆盖的**每一个**格子，所以落在页面内的点所在的格子一定能查到它。
    // 格子边长 = 所有页面里最大的轴长，因此任一页面最多跨 2×2×2 = 8 个格子。
    const CellCoord low = CellOf(bounds.getMinCorner());
    const CellCoord high = CellOf(bounds.getMaxCorner());

    for (int32_t z = low.z; z <= high.z; ++z)
    {
        for (int32_t y = low.y; y <= high.y; ++y)
        {
            for (int32_t x = low.x; x <= high.x; ++x)
            {
                m_PageGrid[CellCoord {x, y, z}].push_back(page_id);
            }
        }
    }
}

void LumenSurfaceCacheManager::UnregisterPageFromGrid(uint32_t page_id)
{
    if (page_id >= m_SurfaceCache.pages.size())
    {
        return;
    }

    const AxisAlignedBox& bounds = m_SurfaceCache.pages[page_id].world_bounds;
    if (!bounds.IsValid())
    {
        return;
    }

    const CellCoord low = CellOf(bounds.getMinCorner());
    const CellCoord high = CellOf(bounds.getMaxCorner());

    for (int32_t z = low.z; z <= high.z; ++z)
    {
        for (int32_t y = low.y; y <= high.y; ++y)
        {
            for (int32_t x = low.x; x <= high.x; ++x)
            {
                const auto cell = m_PageGrid.find(CellCoord {x, y, z});
                if (cell == m_PageGrid.end())
                {
                    continue;
                }
                std::vector<uint32_t>& bucket = cell->second;
                bucket.erase(std::remove(bucket.begin(), bucket.end(), page_id), bucket.end());
                if (bucket.empty())
                {
                    m_PageGrid.erase(cell);
                }
            }
        }
    }
}

const std::vector<uint32_t>* LumenSurfaceCacheManager::FindGridCell(const Vector3& world_position) const
{
    const auto cell = m_PageGrid.find(CellOf(world_position));
    return cell != m_PageGrid.end() ? &cell->second : nullptr;
}

uint32_t LumenSurfaceCacheManager::FindPageAt(const Vector3& world_position) const
{
    const std::vector<uint32_t>* bucket = FindGridCell(world_position);
    if (bucket == nullptr)
    {
        return kInvalidPage;
    }

    // 先看有没有真正包含这个点的页面；重叠时取体积最小的那个 —— 它才是最贴身的表面
    uint32_t best = kInvalidPage;
    float best_volume = std::numeric_limits<float>::max();
    for (uint32_t page_id : *bucket)
    {
        if (page_id >= m_SurfaceCache.pages.size())
        {
            continue;
        }
        const LumenSurfaceCachePage& page = m_SurfaceCache.pages[page_id];
        if (!page.is_valid || !page.world_bounds.IsValid())
        {
            continue;
        }
        if (!page.world_bounds.Contains(world_position))
        {
            continue;
        }
        const float volume = page.GetBoundsVolume();
        if (volume < best_volume)
        {
            best_volume = volume;
            best = page_id;
        }
    }
    if (best != kInvalidPage)
    {
        return best;
    }

    // 没有精确命中（点在页面外侧一点点）：退回"中心最近"的页面。命中点确实可能落在
    // 页面外 —— 距离场把射线停在表面外侧约半个 voxel 处，而薄壳几何的 AABB 是扁的。
    float best_distance = std::numeric_limits<float>::max();
    for (uint32_t page_id : *bucket)
    {
        if (page_id >= m_SurfaceCache.pages.size())
        {
            continue;
        }
        const LumenSurfaceCachePage& page = m_SurfaceCache.pages[page_id];
        if (!page.is_valid)
        {
            continue;
        }
        const float distance = (page.world_position - world_position).length();
        if (distance < best_distance)
        {
            best_distance = distance;
            best = page_id;
        }
    }

    return best;
}

bool LumenSurfaceCacheManager::GetSurfaceInfo(const Vector3& world_position,
                                              const Vector3& normal,
                                              LumenSurfaceCachePage& out_page)
{
    // normal 目前只影响 LOD / 将来的 card 选择，位置就足以定位页面
    (void)normal;

    const uint32_t page_id = FindPageAt(world_position);
    if (page_id == kInvalidPage || page_id >= m_SurfaceCache.pages.size())
    {
        return false;
    }

    LumenSurfaceCachePage& page = m_SurfaceCache.pages[page_id];
    page.last_access_time = m_FrameIndex;
    out_page = page;
    return true;
}

void LumenSurfaceCacheManager::CleanupInvalidPages()
{
    for (uint32_t page_id = 0; page_id < m_SurfaceCache.pages.size(); ++page_id)
    {
        LumenSurfaceCachePage& page = m_SurfaceCache.pages[page_id];
        if (!page.is_valid)
        {
            continue;
        }

        // 时钟回绕（页面来自更大的帧号）时不要误判成"空转了很久"
        if (m_FrameIndex < page.last_access_time)
        {
            continue;
        }

        if (m_FrameIndex - page.last_access_time < LumenConstants::kMaxSurfaceCachePageIdleFrames)
        {
            continue;
        }

        EvictPage(page_id);
    }
}

void LumenSurfaceCacheManager::EvictPage(uint32_t page_id)
{
    UnregisterPageFromGrid(page_id);

    if (page_id < m_SurfaceCache.pages.size())
    {
        const uint64_t instance_id = m_SurfaceCache.pages[page_id].instance_id;
        const auto mapped = m_InstanceToPage.find(instance_id);
        if (mapped != m_InstanceToPage.end() && mapped->second == page_id)
        {
            m_InstanceToPage.erase(mapped);
        }
    }

    FreePage(page_id);
}

bool LumenSurfaceCacheManager::EvictOldestPage()
{
    uint32_t oldest = kInvalidPage;
    uint64_t oldest_time = m_FrameIndex;

    for (uint32_t page_id = 0; page_id < m_SurfaceCache.pages.size(); ++page_id)
    {
        const LumenSurfaceCachePage& page = m_SurfaceCache.pages[page_id];
        if (!page.is_valid)
        {
            continue;
        }
        if (page.last_access_time <= oldest_time)
        {
            oldest_time = page.last_access_time;
            oldest = page_id;
        }
    }

    if (oldest == kInvalidPage)
    {
        return false;
    }

    EvictPage(oldest);
    return true;
}

uint32_t LumenSurfaceCacheManager::AllocatePage()
{
    if (m_FreePages.empty())
    {
        CleanupInvalidPages();
    }

    // 还是没有（场景实例数超过页面上限）：强行按 LRU 驱逐，直到让出槽位
    while (m_FreePages.empty())
    {
        if (!EvictOldestPage())
        {
            return kInvalidPage;
        }
    }

    uint32_t page_id = m_FreePages.back();
    m_FreePages.pop_back();
    m_SurfaceCache.active_page_count++;

    return page_id;
}

void LumenSurfaceCacheManager::FreePage(uint32_t page_id)
{
    if (page_id >= m_SurfaceCache.pages.size())
    {
        return;
    }

    LumenSurfaceCachePage& page = m_SurfaceCache.pages[page_id];
    if (!page.is_valid)
    {
        // 重复释放会把 active_page_count 减成负数（它是无符号的，会绕成一个天文数字）
        return;
    }

    page.is_valid = false;
    m_FreePages.push_back(page_id);
    if (m_SurfaceCache.active_page_count > 0)
    {
        m_SurfaceCache.active_page_count--;
    }
}

uint8_t LumenSurfaceCacheManager::CalculatePageLOD(const Vector3& world_position, const Vector3& camera_position) const
{
    float distance = (world_position - camera_position).length();
    float lod_distance_threshold = 100.0f;  // 每100单位一个LOD级别

    uint8_t lod = static_cast<uint8_t>(distance / lod_distance_threshold);
    return std::min(lod, static_cast<uint8_t>(4));  // 最大4级LOD
}

void LumenSurfaceCacheManager::UpdateGPUResources()
{
    // TODO: 实现GPU资源更新
    // 将表面缓存数据上传到GPU纹理
}
