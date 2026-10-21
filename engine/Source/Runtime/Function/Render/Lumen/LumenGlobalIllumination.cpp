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

bool LumenGlobalIllumination::Initialize(RHI* rhi, const LumenConfig& config)
{
    m_Rhi = rhi;
    m_Config = config;
    return true;
}

void LumenGlobalIllumination::clear()
{
    m_ProbeGrid.Reset();
    m_ProbeCursor = 0;
    m_FrameIndex = 0;
    m_RayTracing = nullptr;
    m_SurfaceCache = nullptr;
    m_DistanceField = nullptr;
}

float LumenGlobalIllumination::GetSurfaceOffset() const
{
    if (m_DistanceField == nullptr)
    {
        return m_Config.min_trace_step;
    }

    // 一个体素对角线：距离场里小于这个尺度的东西根本不存在
    return std::max(m_Config.min_trace_step, m_DistanceField->GetVoxelDiagonal());
}

Vector3 LumenGlobalIllumination::OffsetOriginFromSurface(const Vector3& world_position, const Vector3& normal) const
{
    return world_position + normal * GetSurfaceOffset();
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

    ++m_FrameIndex;

    // probe 网格锚在距离场的世界盒上。没有距离场就没有世界盒，也就没有 probe 可放 ——
    // 以前这里是个 TODO，直接返回 true，于是 GI 从来不产出任何东西。
    ResizeProbeGrid();
    if (!m_ProbeGrid.IsValid())
    {
        return false;
    }

    UpdateProbes(camera->position(), delta_time);

    return true;
}

void LumenGlobalIllumination::ResizeProbeGrid()
{
    if (m_DistanceField == nullptr || !m_DistanceField->isLoaded())
    {
        if (m_ProbeGrid.GetProbeCount() != 0)
        {
            m_ProbeGrid.Reset();
        }
        return;
    }

    const AxisAlignedBox& world_bounds = m_DistanceField->getWorldBounds();
    if (!world_bounds.IsValid())
    {
        m_ProbeGrid.Reset();
        return;
    }

    const Vector3 size = world_bounds.getMaxCorner() - world_bounds.getMinCorner();

    // 每轴分辨率先按期望间距算，再压到 kMaxGiProbeResolution。压缩后间距会被拉大 ——
    // 这是有意的：CPU 上一次 probe 更新就是 gi_sample_count 条光线，probe 总数必须有界。
    const auto resolution_for = [](float extent, float desired_spacing) {
        if (extent <= 0.0f || desired_spacing <= 0.0f)
        {
            return 1u;
        }
        const float raw = std::round(extent / desired_spacing);
        return std::clamp(static_cast<uint32_t>(std::max(raw, 1.0f)),
                          1u,
                          LumenConstants::kMaxGiProbeResolution);
    };

    const uint32_t resolution_x = resolution_for(size.x, m_Config.gi_probe_spacing);
    const uint32_t resolution_y = resolution_for(size.y, m_Config.gi_probe_spacing);
    const uint32_t resolution_z = resolution_for(size.z, m_Config.gi_probe_spacing);

    // 世界盒没变、分辨率也没变 -> 保留已有 probe（时间累积的历史比什么都值钱）
    if (m_ProbeGrid.IsValid() && m_ProbeGrid.resolution_x == resolution_x &&
        m_ProbeGrid.resolution_y == resolution_y && m_ProbeGrid.resolution_z == resolution_z &&
        m_ProbeGrid.world_bounds.getMinCorner() == world_bounds.getMinCorner() &&
        m_ProbeGrid.world_bounds.getMaxCorner() == world_bounds.getMaxCorner())
    {
        return;
    }

    m_ProbeGrid.Reset();
    m_ProbeGrid.world_bounds = world_bounds;
    m_ProbeGrid.resolution_x = resolution_x;
    m_ProbeGrid.resolution_y = resolution_y;
    m_ProbeGrid.resolution_z = resolution_z;
    m_ProbeGrid.probe_spacing = Vector3(size.x / static_cast<float>(resolution_x),
                                        size.y / static_cast<float>(resolution_y),
                                        size.z / static_cast<float>(resolution_z));

    const uint32_t probe_count = m_ProbeGrid.GetProbeCount();
    m_ProbeGrid.irradiance.assign(probe_count, Vector3(0.0f, 0.0f, 0.0f));
    m_ProbeGrid.visibility.assign(probe_count, 0.0f);
    m_ProbeGrid.last_updated_frame.assign(probe_count, 0);
    m_ProbeCursor = 0;
}

void LumenGlobalIllumination::UpdateProbes(const Vector3& camera_position, float delta_time)
{
    const uint32_t probe_count = m_ProbeGrid.GetProbeCount();
    if (probe_count == 0 || m_RayTracing == nullptr)
    {
        return;
    }

    const float alpha = ComputeTemporalAlpha(delta_time);
    const float max_distance_squared = m_Config.gi_probe_max_distance * m_Config.gi_probe_max_distance;

    // 离表面太远的 probe 没什么信息量（开阔空气里的辐照度本来就变化缓慢），跳过它们
    // 把预算留给贴着几何体的 probe。
    const float max_surface_distance =
        m_Config.gi_probe_max_surface_distance_factor *
        std::max({m_ProbeGrid.probe_spacing.x, m_ProbeGrid.probe_spacing.y, m_ProbeGrid.probe_spacing.z});

    const uint32_t budget = std::max(m_Config.gi_probe_update_budget, 1u);

    uint32_t updated = 0;
    uint32_t visited = 0;
    for (; visited < probe_count && updated < budget; ++visited)
    {
        const uint32_t index = (m_ProbeCursor + visited) % probe_count;
        const Vector3 probe_position = m_ProbeGrid.GetProbeWorldPosition(index);

        if ((probe_position - camera_position).squaredLength() > max_distance_squared)
        {
            continue;
        }
        if (m_DistanceField != nullptr && m_DistanceField->QueryDistance(probe_position) > max_surface_distance)
        {
            continue;
        }

        Vector3 sample_irradiance(0.0f, 0.0f, 0.0f);
        float sample_visibility = 0.0f;
        TraceProbeIrradiance(probe_position, sample_irradiance, sample_visibility);

        if (m_ProbeGrid.last_updated_frame[index] == 0 || !m_Config.enable_temporal_accumulation)
        {
            // 第一次更新（或者关掉了累积）直接采信，避免从 0 慢慢爬上来
            m_ProbeGrid.irradiance[index] = sample_irradiance;
            m_ProbeGrid.visibility[index] = sample_visibility;
        }
        else
        {
            m_ProbeGrid.irradiance[index] = BlendTemporally(m_ProbeGrid.irradiance[index], sample_irradiance, alpha);
            m_ProbeGrid.visibility[index] += (sample_visibility - m_ProbeGrid.visibility[index]) * alpha;
        }

        m_ProbeGrid.last_updated_frame[index] = m_FrameIndex;
        ++updated;
    }

    // 轮转游标：下一帧从这次停下的地方继续
    m_ProbeCursor = (m_ProbeCursor + visited) % probe_count;
}

void LumenGlobalIllumination::TraceProbeIrradiance(const Vector3& probe_position,
                                                   Vector3& out_irradiance,
                                                   float& out_visibility) const
{
    out_irradiance = Vector3(0.0f, 0.0f, 0.0f);
    out_visibility = 0.0f;

    if (m_RayTracing == nullptr)
    {
        return;
    }

    const uint32_t sample_count = std::max(m_Config.gi_sample_count, 1u);

    Vector3 accumulated(0.0f, 0.0f, 0.0f);
    uint32_t hit_count = 0;

    for (uint32_t i = 0; i < sample_count; ++i)
    {
        const Vector3 direction = SampleSphereDirection();

        // probe 在空中，不是表面点，所以不需要（也没有法线去做）自相交偏移
        const LumenRayTracingResult result =
            m_RayTracing->TraceRay(probe_position, direction, m_Config.max_trace_distance, m_DistanceField);
        if (!result.hit)
        {
            continue;
        }

        // 与 ComputeMultiBounceGI 同一套粗略的"出射辐射"约定：自发光 + 反照率的一个
        // 固定比例。没有直接光照可用，这个比例就是我们的天光替代项。
        //
        // 这里**不**做距离衰减：辐照度是入射辐射的方向平均，辐射在真空里不随距离衰减。
        accumulated += result.emissive + result.albedo * 0.1f;
        ++hit_count;
    }

    out_irradiance = accumulated / static_cast<float>(sample_count);
    out_visibility = static_cast<float>(hit_count) / static_cast<float>(sample_count);
}

float LumenGlobalIllumination::ComputeTemporalAlpha(float delta_time) const
{
    if (!m_Config.enable_temporal_accumulation)
    {
        return 1.0f;
    }

    if (m_Config.gi_temporal_blend_rate <= 0.0f || delta_time <= 0.0f)
    {
        // rate = 0 是"冻结"：一旦 probe 有了值就不再更新
        return 0.0f;
    }

    // 指数收敛，与帧率无关：1 - exp(-rate * dt)
    const float alpha = 1.0f - std::exp(-m_Config.gi_temporal_blend_rate * delta_time);
    return std::clamp(alpha, 0.0f, 1.0f);
}

Vector3 LumenGlobalIllumination::BlendTemporally(const Vector3& previous,
                                                 const Vector3& sample,
                                                 float delta_time) const
{
    const float alpha = ComputeTemporalAlpha(delta_time);
    return previous + (sample - previous) * alpha;
}

Vector3 LumenGlobalIllumination::SampleSphereDirection() const
{
    // 与 SampleIndirectDirection 共用一套 RNG
    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_real_distribution<float> dis(0.0f, 1.0f);

    // z 在 [-1, 1] 上均匀 -> 球面均匀（Archimedes）
    const float z = 2.0f * dis(gen) - 1.0f;
    const float theta = 2.0f * 3.14159265f * dis(gen);
    const float r = std::sqrt(std::max(0.0f, 1.0f - z * z));

    return Vector3(r * std::cos(theta), r * std::sin(theta), z);
}

Vector3 LumenGlobalIllumination::GetIndirectLighting(const Vector3& world_position, const Vector3& normal)
{
    // 1) probe 网格里有历史 -> 三线性插值直接取。这个值已经做过时间累积，噪声低。
    //
    // 判据是"附近有没有 probe 被更新过"，不是"插出来的值是不是非零"：被封闭几何体
    // 包住的 probe 其结果本来就该是 0，用长度判断会把它误当成"没数据"，于是每一次
    // 查询都退化成一次完整追踪。
    Vector3 cached_lighting(0.0f, 0.0f, 0.0f);
    if (m_Config.enable_spatial_reuse && SampleProbeGrid(world_position, cached_lighting))
    {
        return cached_lighting;
    }

    // 2) 没有历史（probe 还没轮转到 / 网格刚重建 / 关掉了空间复用）-> 直接追踪
    Vector3 view_direction = Vector3(0.0f, 0.0f, 1.0f);  // TODO: 从相机获取
    Vector3 lighting = ComputeMultiBounceGI(world_position, normal, view_direction, m_Config.max_reflection_bounces);

    // 3) 边查边填：把这次算出来的结果写回 probe 网格，下一次查询同一点就不用再追踪了。
    //    （这里没有帧时间，传 0 —— TemporalAccumulation 会保证只填空 probe。）
    TemporalAccumulation(world_position, lighting, 0.0f);

    return lighting;
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

    // 追踪光线。起点要抬出表面，距离场要显式传入 —— 以前这里传 nullptr，软件光追
    // 直接返回未命中，间接光照恒为 0。
    LumenRayTracingResult result = m_RayTracing->TraceRay(OffsetOriginFromSurface(world_position, normal),
                                                          indirect_dir,
                                                          m_Config.max_trace_distance,
                                                          m_DistanceField);

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

        // 追踪光线（同样要抬出表面 + 显式传距离场）
        LumenRayTracingResult result = m_RayTracing->TraceRay(OffsetOriginFromSurface(world_position, normal),
                                                              indirect_dir,
                                                              m_Config.max_trace_distance,
                                                              m_DistanceField);

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
    if (!m_ProbeGrid.IsValid())
    {
        return;
    }

    const Vector3& spacing = m_ProbeGrid.probe_spacing;
    if (spacing.x <= 0.0f || spacing.y <= 0.0f || spacing.z <= 0.0f)
    {
        return;
    }

    // 这个位置落在哪个 probe 的格子里（probe 在格心，所以不加 0.5 偏移，直接取整）
    const Vector3 min_corner = m_ProbeGrid.world_bounds.getMinCorner();
    const auto axis_index = [](float world, float min, float cell, uint32_t resolution) {
        const int32_t value = static_cast<int32_t>(std::floor((world - min) / cell));
        return static_cast<uint32_t>(std::clamp(value, 0, static_cast<int32_t>(resolution) - 1));
    };

    const uint32_t x = axis_index(world_position.x, min_corner.x, spacing.x, m_ProbeGrid.resolution_x);
    const uint32_t y = axis_index(world_position.y, min_corner.y, spacing.y, m_ProbeGrid.resolution_y);
    const uint32_t z = axis_index(world_position.z, min_corner.z, spacing.z, m_ProbeGrid.resolution_z);
    const uint32_t index = m_ProbeGrid.GetProbeIndex(x, y, z);

    if (m_ProbeGrid.last_updated_frame[index] == 0)
    {
        // 空 probe：直接采信。关掉时间累积时 alpha = 1，走的也是"直接采信"这条路。
        m_ProbeGrid.irradiance[index] = lighting;
        m_ProbeGrid.visibility[index] = 1.0f;
    }
    else
    {
        const float alpha = ComputeTemporalAlpha(delta_time);
        m_ProbeGrid.irradiance[index] = BlendTemporally(m_ProbeGrid.irradiance[index], lighting, delta_time);
        m_ProbeGrid.visibility[index] += (1.0f - m_ProbeGrid.visibility[index]) * alpha;
    }
    m_ProbeGrid.last_updated_frame[index] = m_FrameIndex;

    // 调用方拿走的是累积后的值 —— 这才是"减少噪声"的那一半
    lighting = m_ProbeGrid.irradiance[index];
}

Vector3 LumenGlobalIllumination::SpatialReuse(const Vector3& world_position, const Vector3& normal) const
{
    if (!m_Config.enable_spatial_reuse)
    {
        return Vector3(0.0f, 0.0f, 0.0f);
    }

    // probe 里存的是无方向的平均入射辐射，插值不看法线。normal 留在签名里是为了跟
    // 上层 API 保持一致 —— 将来换成 SH 或者六面辐照度时，这里就要按法线加权了。
    (void)normal;

    Vector3 irradiance(0.0f, 0.0f, 0.0f);
    if (!SampleProbeGrid(world_position, irradiance))
    {
        return Vector3(0.0f, 0.0f, 0.0f);
    }

    return irradiance;
}

bool LumenGlobalIllumination::SampleProbeGrid(const Vector3& world_position, Vector3& out_irradiance) const
{
    out_irradiance = Vector3(0.0f, 0.0f, 0.0f);

    if (!m_ProbeGrid.IsValid())
    {
        return false;
    }

    const Vector3& spacing = m_ProbeGrid.probe_spacing;
    if (spacing.x <= 0.0f || spacing.y <= 0.0f || spacing.z <= 0.0f)
    {
        return false;
    }

    // 世界坐标 -> 连续网格坐标（probe 在格心，所以要减 0.5）
    const Vector3 min_corner = m_ProbeGrid.world_bounds.getMinCorner();
    const float grid_x = (world_position.x - min_corner.x) / spacing.x - 0.5f;
    const float grid_y = (world_position.y - min_corner.y) / spacing.y - 0.5f;
    const float grid_z = (world_position.z - min_corner.z) / spacing.z - 0.5f;

    const auto lower_index = [](float coordinate, uint32_t resolution) {
        const int32_t value = static_cast<int32_t>(std::floor(coordinate));
        return static_cast<uint32_t>(std::clamp(value, 0, static_cast<int32_t>(resolution) - 1));
    };

    const uint32_t x0 = lower_index(grid_x, m_ProbeGrid.resolution_x);
    const uint32_t y0 = lower_index(grid_y, m_ProbeGrid.resolution_y);
    const uint32_t z0 = lower_index(grid_z, m_ProbeGrid.resolution_z);
    const uint32_t x1 = std::min(x0 + 1u, m_ProbeGrid.resolution_x - 1u);
    const uint32_t y1 = std::min(y0 + 1u, m_ProbeGrid.resolution_y - 1u);
    const uint32_t z1 = std::min(z0 + 1u, m_ProbeGrid.resolution_z - 1u);

    const float fx = std::clamp(grid_x - static_cast<float>(x0), 0.0f, 1.0f);
    const float fy = std::clamp(grid_y - static_cast<float>(y0), 0.0f, 1.0f);
    const float fz = std::clamp(grid_z - static_cast<float>(z0), 0.0f, 1.0f);

    Vector3 accumulated(0.0f, 0.0f, 0.0f);
    float total_weight = 0.0f;

    for (uint32_t corner = 0; corner < 8; ++corner)
    {
        const bool high_x = (corner & 1u) != 0;
        const bool high_y = (corner & 2u) != 0;
        const bool high_z = (corner & 4u) != 0;

        const uint32_t index = m_ProbeGrid.GetProbeIndex(high_x ? x1 : x0, high_y ? y1 : y0, high_z ? z1 : z0);

        // 从没更新过的 probe 没有数据，不能参与插值 —— 否则一个 0 会把整片区域拉黑
        if (m_ProbeGrid.last_updated_frame[index] == 0)
        {
            continue;
        }

        const float weight = (high_x ? fx : 1.0f - fx) * (high_y ? fy : 1.0f - fy) * (high_z ? fz : 1.0f - fz);
        if (weight <= 0.0f)
        {
            continue;
        }

        accumulated += m_ProbeGrid.irradiance[index] * weight;
        total_weight += weight;
    }

    if (total_weight <= 0.0f)
    {
        return false;
    }

    out_irradiance = accumulated / total_weight;
    return true;
}