#include "LumenRayTracing.h"

#include "Runtime/Function/Render/Interface/RHI.h"

#include <algorithm>
#include <cmath>
#include <limits>

LumenRayTracing::LumenRayTracing() {}

LumenRayTracing::~LumenRayTracing()
{
    clear();
}

bool LumenRayTracing::Initialize(RHI* rhi, const LumenConfig& config)
{
    m_Rhi = rhi;
    m_Config = config;

    // 检查硬件光线追踪支持
    m_HardwareRayTracingSupported = InitializeHardwareRayTracing();

    return true;
}

void LumenRayTracing::clear()
{
    // TODO: 清理硬件光线追踪资源
    m_DistanceField = nullptr;
}

LumenRayTracingResult LumenRayTracing::TraceRaySoftware(const Vector3& ray_origin,
                                                        const Vector3& ray_direction,
                                                        float max_distance,
                                                        LumenDistanceField* distance_field) const
{
    LumenRayTracingResult result;
    result.hit = false;
    result.hit_distance = max_distance;
    result.tracing_type = RayTracingType::Software;

    if (!distance_field)
    {
        return result;
    }

    // 使用距离场进行追踪
    Vector3 hit_position;
    Vector3 hit_normal;
    float hit_distance;

    if (DistanceFieldMarch(ray_origin, ray_direction, max_distance, hit_position, hit_normal, hit_distance))
    {
        result.hit = true;
        result.hit_position = hit_position;
        result.hit_normal = hit_normal;
        result.hit_distance = hit_distance;

        // TODO: 从表面缓存或GBuffer获取材质信息
        result.albedo = Vector3(0.5f, 0.5f, 0.5f);
        result.roughness = 0.5f;
        result.metallic = 0.0f;
        result.emissive = Vector3(0.0f, 0.0f, 0.0f);
    }

    return result;
}

LumenRayTracingResult
LumenRayTracing::TraceRayHardware(const Vector3& ray_origin, const Vector3& ray_direction, float max_distance) const
{
    LumenRayTracingResult result;
    result.hit = false;
    result.hit_distance = max_distance;
    result.tracing_type = RayTracingType::Hardware;

    if (!m_HardwareRayTracingSupported)
    {
        return result;
    }

    // TODO: 实现硬件光线追踪
    // 这里需要调用Vulkan光线追踪扩展API

    return result;
}

LumenRayTracingResult LumenRayTracing::TraceRay(const Vector3& ray_origin,
                                                const Vector3& ray_direction,
                                                float max_distance,
                                                LumenDistanceField* distance_field) const
{
    // 根据配置选择追踪方式
    switch (m_Config.ray_tracing_type)
    {
        case RayTracingType::Hardware:
            return TraceRayHardware(ray_origin, ray_direction, max_distance);
        case RayTracingType::Software:
            return TraceRaySoftware(ray_origin, ray_direction, max_distance, distance_field);
        case RayTracingType::Hybrid:
            // 混合模式：近距离使用硬件，远距离使用软件
            if (max_distance < 1000.0f && m_HardwareRayTracingSupported)
            {
                return TraceRayHardware(ray_origin, ray_direction, max_distance);
            }
            else
            {
                return TraceRaySoftware(ray_origin, ray_direction, max_distance, distance_field);
            }
        default:
            return TraceRaySoftware(ray_origin, ray_direction, max_distance, distance_field);
    }
}

bool LumenRayTracing::DistanceFieldMarch(const Vector3& ray_origin,
                                         const Vector3& ray_direction,
                                         float max_distance,
                                         Vector3& out_hit_position,
                                         Vector3& out_hit_normal,
                                         float& out_hit_distance) const
{
    if (!m_DistanceField)
    {
        return false;
    }

    Vector3 ray_dir_normalized = ray_direction.normalisedCopy();
    float current_distance = 0.0f;
    int max_steps = 256;  // 最大步数
    float min_step_size = m_Config.min_trace_step;
    float max_step_size = m_Config.max_trace_step;

    for (int step = 0; step < max_steps && current_distance < max_distance; ++step)
    {
        Vector3 current_position = ray_origin + ray_dir_normalized * current_distance;

        // 查询距离场
        float distance = m_DistanceField->QueryDistance(current_position);

        // 如果距离为负或很小，说明已经进入表面
        if (distance < 0.01f)
        {
            out_hit_position = current_position;
            out_hit_distance = current_distance;

            // 计算法线（使用梯度）
            Vector3 normal;
            if (m_DistanceField->QueryNearestSurface(current_position, out_hit_position, normal))
            {
                out_hit_normal = normal;
            }
            else
            {
                // 使用数值梯度计算法线
                const float epsilon = 0.1f;
                float dx = m_DistanceField->QueryDistance(current_position + Vector3(epsilon, 0.0f, 0.0f)) - distance;
                float dy = m_DistanceField->QueryDistance(current_position + Vector3(0.0f, epsilon, 0.0f)) - distance;
                float dz = m_DistanceField->QueryDistance(current_position + Vector3(0.0f, 0.0f, epsilon)) - distance;
                out_hit_normal = Vector3(dx, dy, dz).normalisedCopy();
            }

            return true;
        }

        // 自适应步长：使用距离场值作为步长，但限制在最小和最大步长之间
        float step_size = std::max(min_step_size, std::min(max_step_size, distance * 0.5f));
        current_distance += step_size;
    }

    return false;
}

bool LumenRayTracing::InitializeHardwareRayTracing()
{
    // TODO: 检查Vulkan光线追踪扩展支持
    // 这里需要查询Vulkan物理设备的光线追踪特性
    return false;  // 暂时返回false，需要实现Vulkan光线追踪支持
}
