#include "LumenGlobalIllumination.h"

#include "Runtime/Function/Render/Interface/RHI.h"
#include "Runtime/Function/Render/RenderCamera.h"

#include <algorithm>
#include <cmath>
#include <random>

LumenGlobalIllumination::LumenGlobalIllumination() {}

LumenGlobalIllumination::~LumenGlobalIllumination()
{
    clear();
}

bool LumenGlobalIllumination::Initialize(std::shared_ptr<RHI> rhi, const LumenConfig& config)
{
    m_Rhi = rhi;
    m_Config = config;
    return true;
}

void LumenGlobalIllumination::clear()
{
    m_LightingCache.clear();
    m_RayTracing = nullptr;
    m_SurfaceCache = nullptr;
}

bool LumenGlobalIllumination::ComputeGlobalIllumination(std::shared_ptr<RenderScene> render_scene,
                                                        std::shared_ptr<RenderCamera> camera,
                                                        LumenRayTracing* ray_tracing,
                                                        LumenSurfaceCacheManager* surface_cache,
                                                        float delta_time)
{
    if (!render_scene || !camera || !ray_tracing)
    {
        return false;
    }

    m_RayTracing = ray_tracing;
    m_SurfaceCache = surface_cache;

    // TODO: 实现全局光照计算
    // 这里需要遍历场景中的每个像素或表面点，计算间接光照

    return true;
}

Vector3 LumenGlobalIllumination::GetIndirectLighting(const Vector3& world_position, const Vector3& normal) const
{
    // 首先尝试从缓存获取
    Vector3 cached_lighting = SpatialReuse(world_position, normal);
    if (cached_lighting.length() > 0.001f)
    {
        return cached_lighting;
    }

    // 计算新的间接光照
    Vector3 view_direction = Vector3(0.0f, 0.0f, 1.0f);  // TODO: 从相机获取
    return ComputeMultiBounceGI(world_position, normal, view_direction, m_Config.max_reflection_bounces);
}

Vector3 LumenGlobalIllumination::ComputeSingleBounceGI(const Vector3& world_position,
                                                       const Vector3& normal,
                                                       const Vector3& view_direction) const
{
    if (!m_RayTracing)
    {
        return Vector3(0.0f, 0.0f, 0.0f);
    }

    // 采样间接光照方向
    Vector3 indirect_dir = SampleIndirectDirection(normal, 0.5f);

    // 追踪光线
    LumenRayTracingResult result =
        m_RayTracing->TraceRay(world_position, indirect_dir, m_Config.max_trace_distance, nullptr);

    if (result.hit)
    {
        // 计算间接光照
        // 使用表面颜色和发射光作为间接光照源
        Vector3 indirect_lighting = result.emissive + result.albedo * 0.1f;  // 简化的间接光照计算

        // 考虑可见性
        float visibility = std::max(0.0f, normal.dotProduct(indirect_dir));
        return indirect_lighting * visibility;
    }

    return Vector3(0.0f, 0.0f, 0.0f);
}

Vector3 LumenGlobalIllumination::ComputeMultiBounceGI(const Vector3& world_position,
                                                      const Vector3& normal,
                                                      const Vector3& view_direction,
                                                      uint32_t max_bounces) const
{
    if (max_bounces == 0 || !m_RayTracing)
    {
        return Vector3(0.0f, 0.0f, 0.0f);
    }

    Vector3 total_lighting(0.0f, 0.0f, 0.0f);

    // 采样多个方向
    uint32_t sample_count = m_Config.gi_sample_count;
    for (uint32_t i = 0; i < sample_count; ++i)
    {
        // 采样间接方向
        Vector3 indirect_dir = SampleIndirectDirection(normal, 0.5f);

        // 追踪光线
        LumenRayTracingResult result =
            m_RayTracing->TraceRay(world_position, indirect_dir, m_Config.max_trace_distance, nullptr);

        if (result.hit)
        {
            // 第一反射的间接光照
            Vector3 first_bounce = result.emissive + result.albedo * 0.1f;
            float visibility = std::max(0.0f, normal.dotProduct(indirect_dir));
            total_lighting += first_bounce * visibility;

            // 递归计算多次反射
            if (max_bounces > 1)
            {
                Vector3 next_bounce =
                    ComputeMultiBounceGI(result.hit_position, result.hit_normal, -indirect_dir, max_bounces - 1);
                total_lighting += next_bounce * result.albedo * 0.5f;  // 衰减
            }
        }
    }

    return total_lighting / static_cast<float>(sample_count);
}

Vector3 LumenGlobalIllumination::SampleIndirectDirection(const Vector3& normal, float roughness) const
{
    // 使用余弦加权采样
    // TODO: 实现更高级的采样方法（如重要性采样）
    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_real_distribution<float> dis(0.0f, 1.0f);

    // 简单的半球采样
    float u1 = dis(gen);
    float u2 = dis(gen);

    float theta = 2.0f * 3.14159265f * u1;
    float phi = std::acos(std::sqrt(u2));

    Vector3 local_dir(std::sin(phi) * std::cos(theta), std::sin(phi) * std::sin(theta), std::cos(phi));

    // 转换到世界空间（假设normal是Z轴）
    Vector3 tangent = std::abs(normal.x) < 0.9f ? Vector3(1.0f, 0.0f, 0.0f) : Vector3(0.0f, 1.0f, 0.0f);
    Vector3 bitangent = normal.crossProduct(tangent).normalisedCopy();
    tangent = bitangent.crossProduct(normal).normalisedCopy();

    return tangent * local_dir.x + bitangent * local_dir.y + normal * local_dir.z;
}

void LumenGlobalIllumination::TemporalAccumulation(const Vector3& world_position, Vector3& lighting, float delta_time)
{
    if (!m_Config.enable_temporal_accumulation)
    {
        return;
    }

    // TODO: 实现时间累积
    // 使用上一帧的光照数据进行混合，减少噪声
}

Vector3 LumenGlobalIllumination::SpatialReuse(const Vector3& world_position, const Vector3& normal) const
{
    if (!m_Config.enable_spatial_reuse)
    {
        return Vector3(0.0f, 0.0f, 0.0f);
    }

    // 查找附近的光照缓存
    const float reuse_radius = 10.0f;
    Vector3 total_lighting(0.0f, 0.0f, 0.0f);
    float total_weight = 0.0f;

    for (const auto& entry : m_LightingCache)
    {
        float distance = (entry.position - world_position).length();
        if (distance < reuse_radius)
        {
            // 考虑法线相似度
            float normal_similarity = std::max(0.0f, entry.normal.dotProduct(normal));
            float weight = (1.0f - distance / reuse_radius) * normal_similarity;

            total_lighting += entry.indirect_lighting * weight;
            total_weight += weight;
        }
    }

    if (total_weight > 0.001f)
    {
        return total_lighting / total_weight;
    }

    return Vector3(0.0f, 0.0f, 0.0f);
}