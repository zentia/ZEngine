#pragma once

#include "LumenDistanceField.h"
#include "LumenTypes.h"
#include "Runtime/Core/Math/Vector3.h"

#include <memory>

class RHI;
class RenderResourceBase;
class RenderScene;

// 光线追踪系统
class LumenRayTracing
{
public:
    LumenRayTracing();
    ~LumenRayTracing();

    // 初始化
    bool Initialize(std::shared_ptr<RHI> rhi, const LumenConfig& config);

    // 清理
    void clear();

    // 追踪光线（软件追踪，基于距离场）
    LumenRayTracingResult TraceRaySoftware(const Vector3& ray_origin,
                                           const Vector3& ray_direction,
                                           float max_distance,
                                           LumenDistanceField* distance_field) const;

    // 追踪光线（硬件追踪）
    LumenRayTracingResult
    TraceRayHardware(const Vector3& ray_origin, const Vector3& ray_direction, float max_distance) const;

    // 追踪光线（自动选择硬件或软件）
    LumenRayTracingResult TraceRay(const Vector3& ray_origin,
                                   const Vector3& ray_direction,
                                   float max_distance,
                                   LumenDistanceField* distance_field) const;

    // 检查硬件光线追踪支持
    bool isHardwareRayTracingSupported() const { return m_HardwareRayTracingSupported; }

    // 设置距离场引用
    void setDistanceField(LumenDistanceField* distance_field) { m_DistanceField = distance_field; }

private:
    // 软件光线追踪（使用距离场）
    LumenRayTracingResult
    traceRayWithDistanceField(const Vector3& ray_origin, const Vector3& ray_direction, float max_distance) const;

    // 距离场步进追踪
    bool DistanceFieldMarch(const Vector3& ray_origin,
                            const Vector3& ray_direction,
                            float max_distance,
                            Vector3& out_hit_position,
                            Vector3& out_hit_normal,
                            float& out_hit_distance) const;

    // 初始化硬件光线追踪
    bool InitializeHardwareRayTracing();

    // 配置
    LumenConfig m_Config;

    // RHI
    std::shared_ptr<RHI> m_Rhi;

    // 距离场引用
    LumenDistanceField* m_DistanceField = nullptr;

    // 硬件光线追踪支持
    bool m_HardwareRayTracingSupported = false;

    // 硬件光线追踪资源
    // TODO: 添加硬件光线追踪相关资源（加速结构等）
};