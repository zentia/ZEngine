#pragma once

#include "LumenRayTracing.h"
#include "LumenTypes.h"
#include "Runtime/Core/Math/Vector3.h"

#include <memory>

class RHI;
class RenderResourceBase;
class RenderScene;
class RenderCamera;

// 反射系统
class LumenReflections
{
public:
    LumenReflections();
    ~LumenReflections();

    // 初始化
    bool Initialize(RHI* rhi, const LumenConfig& config);

    // 清理
    void clear();

    // 计算反射
    LumenReflectionData ComputeReflection(const Vector3& world_position,
                                          const Vector3& normal,
                                          const Vector3& view_direction,
                                          float roughness,
                                          LumenRayTracing* ray_tracing) const;

    // 设置光线追踪引用
    void setRayTracing(LumenRayTracing* ray_tracing) { m_RayTracing = ray_tracing; }

private:
    // 屏幕空间反射
    LumenReflectionData ComputeScreenSpaceReflection(const Vector3& world_position,
                                                     const Vector3& reflection_direction,
                                                     float roughness) const;

    // 距离场反射
    LumenReflectionData ComputeDistanceFieldReflection(const Vector3& world_position,
                                                       const Vector3& reflection_direction,
                                                       float roughness,
                                                       LumenRayTracing* ray_tracing) const;

    // 硬件光线追踪反射
    LumenReflectionData ComputeHardwareRayTracingReflection(const Vector3& world_position,
                                                            const Vector3& reflection_direction,
                                                            float roughness,
                                                            LumenRayTracing* ray_tracing) const;

    // 混合反射结果
    LumenReflectionData BlendReflections(const LumenReflectionData& ssr,
                                         const LumenReflectionData& dfr,
                                         const LumenReflectionData& hwrt,
                                         float roughness,
                                         float distance) const;

    // 配置
    LumenConfig m_Config;

    // RHI
    RHI* m_Rhi;

    // 子系统引用
    LumenRayTracing* m_RayTracing = nullptr;
};