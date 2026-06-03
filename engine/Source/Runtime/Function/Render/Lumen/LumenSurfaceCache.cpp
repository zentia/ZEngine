#include "LumenSurfaceCache.h"

#include "Runtime/Function/Render/Interface/RHI.h"

#include <algorithm>
#include <cmath>

LumenSurfaceCacheManager::LumenSurfaceCacheManager() {}

LumenSurfaceCacheManager::~LumenSurfaceCacheManager()
{
    clear();
}

bool LumenSurfaceCacheManager::Initialize(std::shared_ptr<RHI> rhi, const LumenConfig& config)
{
    m_Rhi = rhi;
    m_Config = config;

    // 初始化表面缓存
    m_SurfaceCache.cache_id = 0;
    m_SurfaceCache.base_resolution = config.surface_cache_resolution;
    m_SurfaceCache.needs_update = true;
    m_SurfaceCache.active_page_count = 0;
    m_SurfaceCache.total_page_count = config.max_surface_cache_pages;

    // 预分配页面
    m_SurfaceCache.pages.resize(config.max_surface_cache_pages);
    for (uint32_t i = 0; i < config.max_surface_cache_pages; ++i)
    {
        m_SurfaceCache.pages[i].page_id = i;
        m_SurfaceCache.pages[i].resolution = config.surface_cache_page_size;
        m_SurfaceCache.pages[i].is_valid = false;
        m_FreePages.push_back(i);
    }

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
    m_PositionToPageMap.clear();
    m_FreePages.clear();
}

bool LumenSurfaceCacheManager::BuildFromScene(std::shared_ptr<RenderScene> render_scene, const Vector3& camera_position)
{
    if (!render_scene)
    {
        return false;
    }

    // TODO: 从场景几何构建表面缓存
    // 这里需要访问GBuffer或场景几何来构建表面信息

    m_SurfaceCache.needs_update = false;

    // 更新GPU资源
    UpdateGPUResources();

    return true;
}

void LumenSurfaceCacheManager::UpdateSurfaceCache(std::shared_ptr<RenderScene> render_scene,
                                                  const Vector3& camera_position)
{
    if (!m_SurfaceCache.needs_update)
    {
        return;
    }

    // 清理无效页面
    CleanupInvalidPages();

    // 重新构建表面缓存
    BuildFromScene(render_scene, camera_position);
}

bool LumenSurfaceCacheManager::GetSurfaceInfo(const Vector3& world_position,
                                              const Vector3& normal,
                                              LumenSurfaceCachePage& out_page) const
{
    // 查找对应的页面
    // 使用位置和法线的哈希值作为键
    uint64_t key = (static_cast<uint64_t>(world_position.x * 1000.0f) << 32) |
                   (static_cast<uint64_t>(world_position.y * 1000.0f) << 16) |
                   static_cast<uint64_t>(world_position.z * 1000.0f);

    auto it = m_PositionToPageMap.find(key);
    if (it != m_PositionToPageMap.end())
    {
        uint32_t page_id = it->second;
        if (page_id < m_SurfaceCache.pages.size() && m_SurfaceCache.pages[page_id].is_valid)
        {
            out_page = m_SurfaceCache.pages[page_id];
            return true;
        }
    }

    return false;
}

void LumenSurfaceCacheManager::CleanupInvalidPages()
{
    // 清理长时间未访问的页面
    const uint64_t max_age = 1000;  // 最大年龄（帧数）

    for (auto& page : m_SurfaceCache.pages)
    {
        if (page.is_valid)
        {
            // TODO: 实现基于时间的清理逻辑
            // 这里需要跟踪页面访问时间
        }
    }
}

uint32_t LumenSurfaceCacheManager::AllocatePage()
{
    if (m_FreePages.empty())
    {
        // 没有空闲页面，需要清理或扩展
        CleanupInvalidPages();
        if (m_FreePages.empty())
        {
            return UINT32_MAX;  // 分配失败
        }
    }

    uint32_t page_id = m_FreePages.back();
    m_FreePages.pop_back();
    m_SurfaceCache.active_page_count++;

    return page_id;
}

void LumenSurfaceCacheManager::FreePage(uint32_t page_id)
{
    if (page_id < m_SurfaceCache.pages.size())
    {
        m_SurfaceCache.pages[page_id].is_valid = false;
        m_FreePages.push_back(page_id);
        m_SurfaceCache.active_page_count--;
    }
}

uint32_t LumenSurfaceCacheManager::FindOrCreatePage(const Vector3& world_position, const Vector3& normal)
{
    // 查找现有页面
    uint64_t key = (static_cast<uint64_t>(world_position.x * 1000.0f) << 32) |
                   (static_cast<uint64_t>(world_position.y * 1000.0f) << 16) |
                   static_cast<uint64_t>(world_position.z * 1000.0f);

    auto it = m_PositionToPageMap.find(key);
    if (it != m_PositionToPageMap.end())
    {
        uint32_t page_id = it->second;
        if (page_id < m_SurfaceCache.pages.size() && m_SurfaceCache.pages[page_id].is_valid)
        {
            return page_id;
        }
    }

    // 创建新页面
    uint32_t page_id = AllocatePage();
    if (page_id != UINT32_MAX)
    {
        auto& page = m_SurfaceCache.pages[page_id];
        page.world_position = world_position;
        page.normal = normal;
        page.is_valid = true;
        page.resolution = m_Config.surface_cache_page_size;
        page.lod_level = 0;  // 将在后续计算

        // 初始化数据
        uint32_t texel_count = page.resolution * page.resolution;
        page.albedo_data.resize(texel_count);
        page.normal_data.resize(texel_count);
        page.roughness_data.resize(texel_count);
        page.metallic_data.resize(texel_count);
        page.emissive_data.resize(texel_count);
        page.indirect_lighting.resize(texel_count);
        page.visibility.resize(texel_count);

        m_PositionToPageMap[key] = page_id;
    }

    return page_id;
}

bool LumenSurfaceCacheManager::BuildPageFromGBuffer(uint32_t page_id,
                                                    const Vector3& world_position,
                                                    const Vector3& normal)
{
    if (page_id >= m_SurfaceCache.pages.size())
    {
        return false;
    }

    auto& page = m_SurfaceCache.pages[page_id];

    // TODO: 从GBuffer读取表面信息
    // 这里需要访问GBuffer纹理来获取表面材质信息

    return true;
}

void LumenSurfaceCacheManager::UpdatePageData(uint32_t page_id, const Vector3& world_position, const Vector3& normal)
{
    if (page_id >= m_SurfaceCache.pages.size())
    {
        return;
    }

    auto& page = m_SurfaceCache.pages[page_id];
    page.world_position = world_position;
    page.normal = normal;
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