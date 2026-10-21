#include "LumenRayTracing.h"

#include "Runtime/Function/Render/Interface/RHI.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace
{
    // 把射线裁进 AABB。dir 必须是单位向量，返回的 enter/exit 是沿 dir 的距离。
    // 返回 false 表示射线根本碰不到这个盒子。
    //
    // 软件光追必须先做这一步：距离场只在 world_bounds 内有值，起点在盒外时不裁剪
    // 的话，QueryDistance 会一路返回 FLT_MAX，光线以最大步长空跑到步数耗尽，再远的
    // 目标也永远追不到。
    bool ClipRayToAxisAlignedBox(const Vector3& origin,
                                 const Vector3& direction,
                                 const AxisAlignedBox& box,
                                 float& out_enter,
                                 float& out_exit)
    {
        const Vector3 min_corner = box.getMinCorner();
        const Vector3 max_corner = box.getMaxCorner();

        float enter = 0.0f;
        float exit = std::numeric_limits<float>::max();

        for (int axis = 0; axis < 3; ++axis)
        {
            const float origin_axis = origin[axis];
            const float direction_axis = direction[axis];

            if (std::fabs(direction_axis) < 1e-8f)
            {
                // 平行于这对平面：只要起点落在板层之间就一直待在盒内，否则永远进不去
                if (origin_axis < min_corner[axis] || origin_axis > max_corner[axis])
                {
                    return false;
                }
                continue;
            }

            const float inverse_direction = 1.0f / direction_axis;
            float near_t = (min_corner[axis] - origin_axis) * inverse_direction;
            float far_t = (max_corner[axis] - origin_axis) * inverse_direction;
            if (near_t > far_t)
            {
                std::swap(near_t, far_t);
            }

            enter = std::max(enter, near_t);
            exit = std::min(exit, far_t);
            if (enter > exit)
            {
                return false;
            }
        }

        out_enter = enter;
        out_exit = exit;
        return true;
    }
}  // namespace

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
    LumenRayTracingResult result {};
    result.hit = false;
    result.hit_distance = max_distance;
    result.tracing_type = RayTracingType::Software;

    // 参数优先，nullptr 才退回成员。旧实现是"参数为空就直接返回未命中"，而三个调用
    // 点全都传 nullptr —— 于是软件光追永远返回 miss，GI 的结果恒为 0。
    LumenDistanceField* field = distance_field != nullptr ? distance_field : m_DistanceField;
    if (field == nullptr || !field->isLoaded())
    {
        return result;
    }

    // 使用距离场进行追踪
    Vector3 hit_position;
    Vector3 hit_normal;
    float hit_distance = max_distance;

    if (DistanceFieldMarch(ray_origin, ray_direction, max_distance, field, hit_position, hit_normal, hit_distance))
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
    LumenRayTracingResult result {};
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
                                         LumenDistanceField* distance_field,
                                         Vector3& out_hit_position,
                                         Vector3& out_hit_normal,
                                         float& out_hit_distance) const
{
    if (distance_field == nullptr || !distance_field->isLoaded())
    {
        return false;
    }

    const float direction_length = ray_direction.length();
    if (direction_length < 1e-6f || max_distance <= 0.0f)
    {
        return false;
    }
    const Vector3 ray_dir = ray_direction / direction_length;

    const AxisAlignedBox& world_bounds = distance_field->getWorldBounds();
    if (!world_bounds.IsValid())
    {
        return false;
    }

    // 只在距离场覆盖的那一段里走：盒外的 QueryDistance 是 FLT_MAX（"没数据"而非
    // "很远"），不裁剪的话起点在盒外时会一路空跑到步数耗尽。
    float enter_distance = 0.0f;
    float exit_distance = max_distance;
    if (!ClipRayToAxisAlignedBox(ray_origin, ray_dir, world_bounds, enter_distance, exit_distance))
    {
        return false;
    }
    exit_distance = std::min(exit_distance, max_distance);
    if (enter_distance >= exit_distance)
    {
        return false;
    }

    // 命中阈值必须随体素尺寸走，不能是常数。
    //
    // 距离场只在体素中心有值，查询出来的是"这个体素中心到表面的距离"。一个贴着表面
    // 的点，它所在体素中心离表面最多半个体素对角线，所以距离场的下界就是这个量级 ——
    // 原来的 0.01f 是一个永远够不到的门槛，光线只会在场景里以最小步长爬完 256 步
    // 然后返回"未命中"。
    const float voxel_diagonal = distance_field->GetVoxelDiagonal();
    const float hit_threshold = std::max(m_Config.min_trace_step, voxel_diagonal * 0.5f);

    const float min_step_size = m_Config.min_trace_step;
    const float max_step_size = m_Config.max_trace_step;
    const int max_steps = static_cast<int>(LumenConstants::kMaxSoftwareTraceSteps);
    const float infinite = std::numeric_limits<float>::max();

    float current_distance = enter_distance;
    float previous_distance = enter_distance;

    for (int step = 0; step < max_steps && current_distance < exit_distance; ++step)
    {
        const Vector3 current_position = ray_origin + ray_dir * current_distance;
        const float distance = distance_field->QueryDistance(current_position);

        if (distance >= infinite - 1.0f)
        {
            // 已经走出有数据的区域，后面不可能再命中
            break;
        }

        if (distance <= hit_threshold)
        {
            // 命中。距离是量化的，直接用 current_distance 会有最多一个体素的误差，
            // 二分几步把"跨过阈值"的那一点收敛出来。
            float low = previous_distance;
            float high = current_distance;
            for (uint32_t iteration = 0; iteration < LumenConstants::kTraceBinaryRefinementSteps; ++iteration)
            {
                const float middle = 0.5f * (low + high);
                const float middle_distance = distance_field->QueryDistance(ray_origin + ray_dir * middle);
                if (middle_distance <= hit_threshold)
                {
                    high = middle;
                }
                else
                {
                    low = middle;
                }
            }

            const float hit_distance = high;
            const Vector3 hit_position = ray_origin + ray_dir * hit_distance;

            out_hit_distance = hit_distance;
            out_hit_position = hit_position;

            Vector3 surface_position;
            Vector3 surface_normal;
            if (distance_field->QueryNearestSurface(hit_position, surface_position, surface_normal))
            {
                out_hit_normal = surface_normal;
            }
            else
            {
                // 拿不到梯度时退化成"面朝来向"，至少是个有限且方向合理的法线
                out_hit_normal = -ray_dir;
            }

            return true;
        }

        previous_distance = current_distance;

        // 球追踪：步长取当前距离（保证不会越过任何表面），再夹进配置的最小/最大步长。
        // 0.5 是保守系数 —— 距离场是无符号且按体素量化的，取满 1.0 会穿面。
        const float step_size = std::clamp(distance * 0.5f, min_step_size, max_step_size);
        current_distance = std::min(current_distance + step_size, exit_distance);
    }

    return false;
}

bool LumenRayTracing::InitializeHardwareRayTracing()
{
    // TODO: 检查Vulkan光线追踪扩展支持
    // 这里需要查询Vulkan物理设备的光线追踪特性
    return false;  // 暂时返回false，需要实现Vulkan光线追踪支持
}
