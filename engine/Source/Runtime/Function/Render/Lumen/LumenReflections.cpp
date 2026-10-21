#include "LumenReflections.h"

#include "Runtime/Function/Render/Interface/RHI.h"
#include "Runtime/Function/Render/RenderCamera.h"

#include <algorithm>
#include <cmath>
#include <random>

namespace
{
    // 粗糙度低于这个值时就打单条镜面光线：锥角已经小到采样不出差别，多打几条纯属浪费。
    constexpr float kMirrorRoughnessEpsilon = 0.02f;

    // 由粗糙度决定的圆锥半角上限。roughness=1 时是 90°（整个半球 = 漫反射），
    // roughness=0 时被上面的 epsilon 短路掉。
    constexpr float kMaxConeHalfAngle = 1.5707963f;  // pi / 2
}  // namespace

LumenReflections::LumenReflections() {}

LumenReflections::~LumenReflections()
{
    clear();
}

bool LumenReflections::Initialize(RHI* rhi, const LumenConfig& config)
{
    m_Rhi = rhi;
    m_Config = config;
    return true;
}

void LumenReflections::clear()
{
    m_RayTracing = nullptr;
    m_DistanceField = nullptr;
    m_ReflectionByInstance.clear();
    m_ReflectionFrame.clear();
    m_Cursor = 0;
    m_FrameIndex = 0;
}

Vector3 LumenReflections::OffsetOriginFromSurface(const Vector3& world_position, const Vector3& normal) const
{
    // 一个体素对角线是距离场能分辨的最小尺度；没有距离场时就退回配置的最小步长
    const float offset = m_DistanceField != nullptr ? std::max(m_Config.min_trace_step, m_DistanceField->GetVoxelDiagonal())
                                                    : m_Config.min_trace_step;
    return world_position + normal * offset;
}

LumenReflectionData LumenReflections::ComputeReflection(const Vector3& world_position,
                                                        const Vector3& normal,
                                                        const Vector3& view_direction,
                                                        float roughness,
                                                        LumenRayTracing* ray_tracing) const
{
    LumenReflectionData result {};
    result.roughness_factor = roughness;
    result.reflection_type = RayTracingType::Software;

    // 反射方向：view_direction 是"从着色点指向相机"，所以直接对法线做镜像
    const Vector3 mirror_direction = view_direction - normal * 2.0f * view_direction.dotProduct(normal);
    result.reflection_direction = mirror_direction;

    if (!ray_tracing)
    {
        return result;
    }

    // 粗糙度 -> 圆锥半角。粗糙表面不是"反射得更少"，而是把能量摊到一整个波瓣上，
    // 所以做法是在这个锥里多打几条光线取平均 —— 而不是把颜色乘以 (1 - roughness)。
    // 以前这里有个 "TODO: 基于粗糙度的方向扰动"，于是粗糙表面打的还是那条完美镜面
    // 光线，再被 (1 - roughness) 压暗一次。
    const float clamped_roughness = std::clamp(roughness, 0.0f, 1.0f);
    const bool needs_cone = clamped_roughness > kMirrorRoughnessEpsilon;
    const float half_angle = clamped_roughness * kMaxConeHalfAngle;
    const float cos_max_angle = std::cos(half_angle);
    const uint32_t sample_count = needs_cone ? std::max<uint32_t>(1u, m_Config.reflection_sample_count) : 1u;

    Vector3 accumulated(0.0f, 0.0f, 0.0f);
    float accumulated_distance = 0.0f;
    uint32_t hit_count = 0;

    for (uint32_t sample = 0; sample < sample_count; ++sample)
    {
        const Vector3 direction =
            (sample_count == 1) ? mirror_direction : SampleConeDirection(mirror_direction, cos_max_angle);

        const LumenReflectionData one =
            ComputeDistanceFieldReflection(world_position, normal, direction, clamped_roughness, ray_tracing);
        if (!one.valid)
        {
            continue;
        }
        accumulated += one.reflection_color;
        accumulated_distance += one.reflection_distance;
        ++hit_count;
    }

    LumenReflectionData dfr {};
    if (hit_count > 0)
    {
        dfr.valid = true;
        dfr.reflection_color = accumulated / static_cast<float>(hit_count);
        dfr.reflection_distance = accumulated_distance / static_cast<float>(hit_count);
        dfr.reflection_type = RayTracingType::Software;
        dfr.roughness_factor = clamped_roughness;
    }
    dfr.reflection_direction = mirror_direction;

    // 屏幕空间反射（需要深度 / GBuffer；CPU 路径里没有，函数恒返回 valid = false）
    const LumenReflectionData ssr =
        ComputeScreenSpaceReflection(world_position, mirror_direction, clamped_roughness);

    // 硬件光追（用户已决定不纳入，isHardwareRayTracingSupported() 恒 false）
    LumenReflectionData hwrt {};
    if (ray_tracing->isHardwareRayTracingSupported())
    {
        hwrt = ComputeHardwareRayTracingReflection(world_position, mirror_direction, clamped_roughness, ray_tracing);
    }

    LumenReflectionData blended = BlendReflections(ssr, dfr, hwrt, clamped_roughness);
    // BlendReflections 只决定"用哪一份颜色"，方向始终是算出来的那条镜像方向
    blended.reflection_direction = mirror_direction;
    return blended;
}

LumenReflectionData LumenReflections::ComputeScreenSpaceReflection(const Vector3& world_position,
                                                                   const Vector3& reflection_direction,
                                                                   float roughness) const
{
    LumenReflectionData result {};
    result.reflection_type = RayTracingType::Software;
    result.roughness_factor = roughness;
    result.reflection_direction = reflection_direction;

    // TODO: 实现屏幕空间反射 —— 需要深度缓冲和 GBuffer，CPU 路径两样都没有。
    //
    // 关键：**没有结果就要如实说没有**（valid 保持 false）。以前这个 TODO 返回一份
    // 全零的 LumenReflectionData，而混合阶段又是按"配置里的最大追踪距离"挑分支的，
    // 于是 max_trace_distance < 100 且 roughness < 0.3 时它会拿到 100% 权重，把距离
    // 场辛辛苦苦追到的命中直接换成黑色。
    return result;
}

LumenReflectionData LumenReflections::ComputeDistanceFieldReflection(const Vector3& world_position,
                                                                     const Vector3& normal,
                                                                     const Vector3& reflection_direction,
                                                                     float roughness,
                                                                     LumenRayTracing* ray_tracing) const
{
    LumenReflectionData result {};
    result.reflection_type = RayTracingType::Software;
    result.roughness_factor = roughness;
    result.reflection_direction = reflection_direction;

    if (!ray_tracing)
    {
        return result;
    }

    // 使用光线追踪计算反射。起点抬出表面 + 显式传距离场：以前这里传 nullptr，距离场
    // 反射永远拿不到命中，reflection_color 恒为 0。
    float max_distance = m_Config.max_trace_distance;
    const Vector3 ray_origin = OffsetOriginFromSurface(world_position, normal);
    LumenRayTracingResult trace_result =
        ray_tracing->TraceRay(ray_origin, reflection_direction, max_distance, m_DistanceField);

    if (trace_result.hit)
    {
        result.valid = true;
        result.reflection_color = trace_result.albedo;
        result.reflection_distance = trace_result.hit_distance;
    }

    // 这里**不**再乘 (1 - roughness)：粗糙度是 BRDF 波瓣的事（shader 里的 F /
    // brdfLUT 已经在做），在辐射这一侧再压一次就是重复计算，会把粗糙表面压成纯黑。
    // 粗糙度的物理效果由 ComputeReflection 的锥形采样来表达。
    return result;
}

LumenReflectionData LumenReflections::ComputeHardwareRayTracingReflection(const Vector3& world_position,
                                                                          const Vector3& reflection_direction,
                                                                          float roughness,
                                                                          LumenRayTracing* ray_tracing) const
{
    LumenReflectionData result {};
    result.reflection_type = RayTracingType::Hardware;
    result.roughness_factor = roughness;
    result.reflection_direction = reflection_direction;

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
        result.valid = true;
        result.reflection_color = trace_result.albedo;
        result.reflection_distance = trace_result.hit_distance;
    }

    return result;
}

LumenReflectionData LumenReflections::BlendReflections(const LumenReflectionData& ssr,
                                                       const LumenReflectionData& dfr,
                                                       const LumenReflectionData& hwrt,
                                                       float roughness) const
{
    // 按"哪一份真的命中了"来选，质量从高到低：硬件光追 > 屏幕空间 > 距离场。
    //
    // 以前这里是拿 max_trace_distance（一个**常量**）去分支的：< 100 用 SSR、
    // < 1000 用距离场、否则看硬件光追。那意味着"用哪种反射"完全由配置决定，而真正
    // 命中的那份结果可能被一个恒空的 TODO 分支顶掉。
    LumenReflectionData result {};
    result.roughness_factor = roughness;
    result.reflection_direction = dfr.reflection_direction;

    if (hwrt.valid)
    {
        result = hwrt;
    }
    else if (ssr.valid)
    {
        result = ssr;
    }
    else if (dfr.valid)
    {
        result = dfr;
    }
    else
    {
        result.valid = false;
        result.reflection_color = Vector3(0.0f, 0.0f, 0.0f);
        result.reflection_distance = 0.0f;
    }

    result.roughness_factor = roughness;
    return result;
}

Vector3 LumenReflections::SampleConeDirection(const Vector3& direction, float cos_max_angle) const
{
    const Vector3 axis = direction.normalisedCopy();

    // 绕 axis 建一组正交基：取与 axis 最不平行那个轴做叉积，避免退化
    Vector3 helper = (std::fabs(axis.z) < 0.9f) ? Vector3(0.0f, 0.0f, 1.0f) : Vector3(1.0f, 0.0f, 0.0f);
    Vector3 tangent = helper.crossProduct(axis).normalisedCopy();
    Vector3 bitangent = axis.crossProduct(tangent).normalisedCopy();

    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_real_distribution<float> dis(0.0f, 1.0f);

    // 按立体角均匀：cos(theta) 在 [cos_max, 1] 上均匀
    const float cos_theta = cos_max_angle + (1.0f - cos_max_angle) * dis(gen);
    const float sin_theta = std::sqrt(std::max(0.0f, 1.0f - cos_theta * cos_theta));
    const float phi = 2.0f * 3.14159265f * dis(gen);

    return (tangent * (sin_theta * std::cos(phi)) + bitangent * (sin_theta * std::sin(phi)) + axis * cos_theta)
        .normalisedCopy();
}

float LumenReflections::ComputeTemporalAlpha(float delta_time) const
{
    if (!m_Config.enable_temporal_accumulation)
    {
        return 1.0f;
    }

    if (m_Config.reflection_temporal_blend_rate <= 0.0f || delta_time <= 0.0f)
    {
        // rate = 0 是"冻结"：已经有值的实例不再更新
        return 0.0f;
    }

    // 指数收敛，与帧率无关（与 GI 同一套约定）
    const float alpha = 1.0f - std::exp(-m_Config.reflection_temporal_blend_rate * delta_time);
    return std::clamp(alpha, 0.0f, 1.0f);
}

void LumenReflections::UpdateReflections(std::shared_ptr<RenderScene> render_scene,
                                         std::shared_ptr<RenderCamera> camera,
                                         LumenRayTracing* ray_tracing,
                                         LumenSurfaceCacheManager* surface_cache,
                                         float delta_time)
{
    if (!render_scene || !camera || !ray_tracing || !surface_cache)
    {
        return;
    }

    ++m_FrameIndex;

    const std::vector<LumenSurfaceCachePage>& pages = surface_cache->getSurfaceCache().pages;
    const uint32_t page_count = static_cast<uint32_t>(pages.size());
    if (page_count == 0)
    {
        return;
    }

    const Vector3 camera_position = camera->position();
    const float alpha = ComputeTemporalAlpha(delta_time);
    const uint32_t budget = std::max<uint32_t>(1u, m_Config.reflection_update_budget);
    const float max_distance_squared = m_Config.reflection_max_distance * m_Config.reflection_max_distance;

    uint32_t updated = 0;
    uint32_t steps = 0;
    for (; steps < page_count && updated < budget; ++steps)
    {
        const LumenSurfaceCachePage& page = pages[(m_Cursor + steps) % page_count];
        if (!page.is_valid)
        {
            continue;
        }

        const Vector3 to_camera = camera_position - page.world_position;
        if (to_camera.squaredLength() > max_distance_squared)
        {
            continue;
        }
        if (to_camera.squaredLength() < 1e-8f)
        {
            continue;  // 相机就在这个点上，视线方向没有定义
        }

        // view_direction 的约定：从着色点指向相机
        const Vector3 view_direction = to_camera.normalisedCopy();
        const LumenReflectionData reflection =
            ComputeReflection(page.world_position, page.normal, view_direction, page.roughness, ray_tracing);
        if (!reflection.valid)
        {
            continue;
        }

        // 用 instance_id 做键：页面会因为重建 / 驱逐换槽位，而实例是稳定的。
        // 首次更新直接采信，之后按 alpha 混合（alpha = 0 时保持不动 = 冻结）。
        const uint64_t key = page.instance_id;
        const bool is_first = (m_ReflectionFrame[key] == 0);
        Vector3& stored = m_ReflectionByInstance[key];
        stored = is_first ? reflection.reflection_color
                          : stored + (reflection.reflection_color - stored) * alpha;
        m_ReflectionFrame[key] = m_FrameIndex;
        ++updated;
    }

    // 光标一定要前进：整轮都跳过（页面全无效 / 全超距）时原地不动会卡死在同一个位置
    m_Cursor = (m_Cursor + std::max<uint32_t>(1u, steps)) % page_count;
}

bool LumenReflections::GetAverageReflection(Vector3& out_color, uint32_t& out_sample_count) const
{
    out_color = Vector3(0.0f, 0.0f, 0.0f);
    out_sample_count = 0;

    if (m_ReflectionByInstance.empty())
    {
        return false;
    }

    Vector3 accumulated(0.0f, 0.0f, 0.0f);
    uint32_t counted = 0;
    for (const auto& entry : m_ReflectionByInstance)
    {
        // 与 GI 同一个教训：判据是"这个实例有没有被更新过"，不是"它的值是不是非零"。
        // 反射一个全黑的场景本来就该是黑的，那是数据。
        const auto frame_it = m_ReflectionFrame.find(entry.first);
        if (frame_it == m_ReflectionFrame.end() || frame_it->second == 0)
        {
            continue;
        }
        accumulated += entry.second;
        ++counted;
    }

    if (counted == 0)
    {
        return false;
    }

    out_color = accumulated / static_cast<float>(counted);
    out_sample_count = counted;
    return true;
}
