#pragma once

#include "LumenTypes.h"
#include "Runtime/Core/Math/AxisAligned.h"
#include "Runtime/Function/Render/RenderScene.h"

#include <memory>
#include <unordered_map>
#include <vector>

class RHI;
class RenderResourceBase;
class RenderScene;

// 表面缓存管理器
class LumenSurfaceCacheManager
{
public:
    LumenSurfaceCacheManager();
    ~LumenSurfaceCacheManager();

    // 初始化
    bool Initialize(RHI* rhi, const LumenConfig& config);

    // 清理
    void clear();

    // 从场景构建表面缓存
    bool BuildFromScene(std::shared_ptr<RenderScene> render_scene, const Vector3& camera_position);

    // 更新表面缓存（增量更新）
    void UpdateSurfaceCache(std::shared_ptr<RenderScene> render_scene, const Vector3& camera_position);

    // 获取表面信息
    bool GetSurfaceInfo(const Vector3& world_position, const Vector3& normal, LumenSurfaceCachePage& out_page) const;

    // 获取表面缓存数据
    const LumenSurfaceCache& getSurfaceCache() const { return m_SurfaceCache; }
    LumenSurfaceCache& getSurfaceCache() { return m_SurfaceCache; }

    // 检查是否需要更新
    bool needsUpdate() const { return m_SurfaceCache.needs_update; }

    // 标记为需要更新
    void markForUpdate() { m_SurfaceCache.needs_update = true; }

    // 清理无效页面
    void CleanupInvalidPages();

private:
    // 分配新的缓存页面
    uint32_t AllocatePage();

    // 释放缓存页面
    void FreePage(uint32_t page_id);

    // 查找或创建页面
    uint32_t FindOrCreatePage(const Vector3& world_position, const Vector3& normal);

    // 从GBuffer构建页面
    bool BuildPageFromGBuffer(uint32_t page_id, const Vector3& world_position, const Vector3& normal);

    // 更新页面数据
    void UpdatePageData(uint32_t page_id, const Vector3& world_position, const Vector3& normal);

    // 计算页面LOD级别
    uint8_t CalculatePageLOD(const Vector3& world_position, const Vector3& camera_position) const;

    // 更新GPU资源
    void UpdateGPUResources();

    // 表面缓存数据
    LumenSurfaceCache m_SurfaceCache;

    // 配置
    LumenConfig m_Config;

    // RHI
    RHI* m_Rhi;

    // 页面映射（世界位置 -> 页面ID）
    std::unordered_map<uint64_t, uint32_t> m_PositionToPageMap;

    // 空闲页面列表
    std::vector<uint32_t> m_FreePages;

    // GPU资源
    RHIImage* m_SurfaceCacheTexture = nullptr;
    RHISampler* m_SurfaceCacheSampler = nullptr;
};