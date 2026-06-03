#include "LumenReflections.h"

#include "Runtime/Function/Render/Interface/RHI.h"

#include <algorithm>
#include <cmath>

LumenReflections::LumenReflections() {}

LumenReflections::~LumenReflections()
{
    clear();
}

bool LumenReflections::Initialize(std::shared_ptr<RHI> rhi, const LumenConfig& config)
{
    m_Rhi = rhi;
    m_Config = config;
    return true;
}

void LumenReflections::clear()
{
    m_RayTracing = nullptr;
}

LumenReflectionData LumenReflections::ComputeReflection(const Vector3& world_position,
                                                        const Vector3& normal,
                                                        const Vector3& view_direction,
                                                        float roughness,
                                                        LumenRayTracing* ray_tracing) const
{
    LumenReflectionData result;
    result.reflection_color = Vector3(0.0f, 0.0f, 0.0f);
    result.reflection_distance = 0.0f;
    result.roughness_factor = roughness;

    // 计算反射方向
    Vector3 reflection_direction = view_direction - normal * 2.0f * view_direction.dotProduct(normal);
    result.reflection_direction = reflection_direction;

    // 根据粗糙度调整反射方向（模糊反射）
    if (roughness > 0.01f)
    {
        // TODO: 实现基于粗糙度的反射方向扰动
    }

    // 根据距离和粗糙度选择反射方法
    float max_reflection_distance = m_Config.max_trace_distance;

    // 屏幕空间反射（近距离，低粗糙度）
    LumenReflectionData ssr_result = ComputeScreenSpaceReflection(world_position, reflection_direction, roughness);

    // 距离场反射（中距离）
    LumenReflectionData dfr_result;
    if (ray_tracing)
    {
        dfr_result = ComputeDistanceFieldReflection(world_position, reflection_direction, roughness, ray_tracing);
    }

    // 硬件光线追踪反射（远距离，高质量）
    LumenReflectionData hwrt_result;
    if (ray_tracing && ray_tracing->isHardwareRayTracingSupported())
    {
        hwrt_result = ComputeHardwareRayTracingReflection(world_position, reflection_direction, roughness, ray_tracing);
    }

    // 混合反射结果
    result = BlendReflections(ssr_result, dfr_result, hwrt_result, roughness, max_reflection_distance);

    return result;
}

LumenReflectionData LumenReflections::ComputeScreenSpaceReflection(const Vector3& world_position,
                                                                   const Vector3& reflection_direction,
                                                                   float roughness) const
{
    LumenReflectionData result;
    result.reflection_type = RayTracingType::Software;  // SSR使用软件追踪

    // TODO: 实现屏幕空间反射
    // 这里需要访问深度缓冲区和GBuffer来进行屏幕空间追踪

    return result;
}

LumenReflectionData LumenReflections::ComputeDistanceFieldReflection(const Vector3& world_position,
                                                                     const Vector3& reflection_direction,
                                                                     float roughness,
                                                                     LumenRayTracing* ray_tracing) const
{
    LumenReflectionData result;
    result.reflection_type = RayTracingType::Software;

    if (!ray_tracing)
    {
        return result;
    }

    // 使用光线追踪计算反射
    float max_distance = m_Config.max_trace_distance;
    LumenRayTracingResult trace_result =
        ray_tracing->TraceRay(world_position, reflection_direction, max_distance, nullptr);

    if (trace_result.hit)
    {
        result.reflection_color = trace_result.albedo;
        result.reflection_distance = trace_result.hit_distance;

        // 根据粗糙度衰减反射
        float roughness_factor = 1.0f - roughness;
        result.reflection_color *= roughness_factor;
    }

    return result;
}

LumenReflectionData LumenReflections::ComputeHardwareRayTracingReflection(const Vector3& world_position,
                                                                          const Vector3& reflection_direction,
                                                                          float roughness,
                                                                          LumenRayTracing* ray_tracing) const
{
    LumenReflectionData result;
    result.reflection_type = RayTracingType::Hardware;

    if (!ray_tracing || !ray_tracing->isHardwareRayTracingSupported())
    {
        return result;
    }

    // 使用硬件光线追踪
    float max_distance = m_Config.max_trace_distance;
    LumenRayTracingResult trace_result =
        ray_tracing->TraceRayHardware(world_position, reflection_direction, max_distance);

    if (trace_result.hit)
    {
        result.reflection_color = trace_result.albedo;
        result.reflection_distance = trace_result.hit_distance;

        // 根据粗糙度衰减反射
        float roughness_factor = 1.0f - roughness;
        result.reflection_color *= roughness_factor;
    }

    return result;
}

LumenReflectionData LumenReflections::BlendReflections(const LumenReflectionData& ssr,
                                                       const LumenReflectionData& dfr,
                                                       const LumenReflectionData& hwrt,
                                                       float roughness,
                                                       float distance) const
{
    LumenReflectionData result;

    // 根据距离和粗糙度混合不同的反射方法
    float ssr_weight = 0.0f;
    float dfr_weight = 0.0f;
    float hwrt_weight = 0.0f;

    // 近距离优先使用SSR
    if (distance < 100.0f && roughness < 0.3f)
    {
        ssr_weight = 1.0f;
    }
    // 中距离使用距离场反射
    else if (distance < 1000.0f)
    {
        dfr_weight = 1.0f;
    }
    // 远距离使用硬件光线追踪
    else if (hwrt.reflection_distance > 0.0f)
    {
        hwrt_weight = 1.0f;
    }
    else
    {
        dfr_weight = 1.0f;
    }

    // 混合反射颜色
    result.reflection_color =
        ssr.reflection_color * ssr_weight + dfr.reflection_color * dfr_weight + hwrt.reflection_color * hwrt_weight;

    // 混合反射距离
    result.reflection_distance = ssr.reflection_distance * ssr_weight + dfr.reflection_distance * dfr_weight +
                                 hwrt.reflection_distance * hwrt_weight;

    // 选择反射类型
    if (hwrt_weight > 0.5f)
    {
        result.reflection_type = RayTracingType::Hardware;
    }
    else if (dfr_weight > 0.5f)
    {
        result.reflection_type = RayTracingType::Software;
    }
    else
    {
        result.reflection_type = RayTracingType::Software;
    }

    result.roughness_factor = roughness;

    return result;
}
