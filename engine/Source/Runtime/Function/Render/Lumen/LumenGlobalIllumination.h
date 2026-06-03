#pragma once

#include "LumenRayTracing.h"
#include "LumenSurfaceCache.h"
#include "LumenTypes.h"
#include "Runtime/Core/Math/Vector3.h"

#include <memory>

class RHI;
class RenderResourceBase;
class RenderScene;
class RenderCamera;

// 全局光照计算系统
class LumenGlobalIllumination
{
public:
    LumenGlobalIllumination();
    ~LumenGlobalIllumination();

    // 初始化
    bool Initialize(RHI* rhi, const LumenConfig& config);

    // 清理
    void clear();

    // 计算全局光照
    bool ComputeGlobalIllumination(std::shared_ptr<RenderScene> render_scene,
                                   std::shared_ptr<RenderCamera> camera,
                                   LumenRayTracing* ray_tracing,
                                   LumenSurfaceCacheManager* surface_cache,
                                   float delta_time);

    // 获取间接光照
    Vector3 GetIndirectLighting(const Vector3& world_position, const Vector3& normal) const;

    // 设置光线追踪引用
    void setRayTracing(LumenRayTracing* ray_tracing) { m_RayTracing = ray_tracing; }

    // 设置表面缓存引用
    void setSurfaceCache(LumenSurfaceCacheManager* surface_cache) { m_SurfaceCache = surface_cache; }

private:
    // 计算单次间接光照
    Vector3
    ComputeSingleBounceGI(const Vector3& world_position, const Vector3& normal, const Vector3& view_direction) const;

    // 计算多次反射间接光照
    Vector3 ComputeMultiBounceGI(const Vector3& world_position,
                                 const Vector3& normal,
                                 const Vector3& view_direction,
                                 uint32_t max_bounces) const;

    // 采样间接光照方向
    Vector3 SampleIndirectDirection(const Vector3& normal, float roughness) const;

    // 时间累积（减少噪声）
    void TemporalAccumulation(const Vector3& world_position, Vector3& lighting, float delta_time);

    // 空间复用（提高性能）
    Vector3 SpatialReuse(const Vector3& world_position, const Vector3& normal) const;

    // 配置
    LumenConfig m_Config;

    // RHI
    RHI* m_Rhi;

    // 子系统引用
    LumenRayTracing* m_RayTracing = nullptr;
    LumenSurfaceCacheManager* m_SurfaceCache = nullptr;

    // 光照缓存（用于时间累积和空间复用）
    struct LightingCacheEntry
    {
        Vector3 position;
        Vector3 normal;
        Vector3 indirect_lighting;
        uint64_t timestamp;
    };
    std::vector<LightingCacheEntry> m_LightingCache;
};
