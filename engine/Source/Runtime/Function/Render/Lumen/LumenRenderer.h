#pragma once

#include "LumenDistanceField.h"
#include "LumenGlobalIllumination.h"
#include "LumenRayTracing.h"
#include "LumenReflections.h"
#include "LumenSurfaceCache.h"
#include "LumenTypes.h"
#include "Runtime/Function/Render/RenderCamera.h"
#include "Runtime/Function/Render/RenderScene.h"

#include <memory>

class RHI;
class RenderResourceBase;

// Lumen渲染器主类
class LumenRenderer
{
public:
    LumenRenderer();
    ~LumenRenderer();

    // 初始化
    bool Initialize(RHI* rhi,
                    std::shared_ptr<RenderResourceBase> render_resource,
                    const LumenConfig& config);

    // 清理
    void clear();

    // 更新（每帧调用）
    void Update(std::shared_ptr<RenderScene> render_scene, std::shared_ptr<RenderCamera> camera, float delta_time);

    // 计算全局光照
    void ComputeGlobalIllumination(std::shared_ptr<RenderScene> render_scene,
                                   std::shared_ptr<RenderCamera> camera,
                                   float delta_time);

    // 计算反射
    void ComputeReflections(std::shared_ptr<RenderScene> render_scene, std::shared_ptr<RenderCamera> camera);

    // 获取配置
    const LumenConfig& getConfig() const { return m_Config; }
    LumenConfig& getConfig() { return m_Config; }

    // 获取统计信息
    const LumenStats& GetStats() const { return m_Stats; }

    // 获取子系统
    LumenDistanceField* getDistanceField() { return &m_DistanceField; }
    LumenSurfaceCacheManager* getSurfaceCache() { return &m_SurfaceCache; }
    LumenRayTracing* getRayTracing() { return &m_RayTracing; }
    LumenGlobalIllumination* getGlobalIllumination() { return &m_GlobalIllumination; }
    LumenReflections* getReflections() { return &m_Reflections; }

private:
    // 更新距离场
    void UpdateDistanceField(std::shared_ptr<RenderScene> render_scene, const Vector3& camera_position);

    // 更新表面缓存
    void UpdateSurfaceCache(std::shared_ptr<RenderScene> render_scene, const Vector3& camera_position);

    // 配置
    LumenConfig m_Config;

    // RHI和资源
    RHI* m_Rhi;
    std::shared_ptr<RenderResourceBase> m_RenderResource;

    // 子系统
    LumenDistanceField m_DistanceField;
    LumenSurfaceCacheManager m_SurfaceCache;
    LumenRayTracing m_RayTracing;
    LumenGlobalIllumination m_GlobalIllumination;
    LumenReflections m_Reflections;

    // 统计信息
    LumenStats m_Stats;

    // 世界空间边界（从场景计算）
    AxisAlignedBox m_WorldBounds;
};
