#include "LumenRenderPass.h"

#include "LumenSettings.h"
#include "Runtime/Function/Render/Interface/RHI.h"
#include "Runtime/Function/Render/RenderResource.h"

LumenRenderPass::LumenRenderPass() {}

LumenRenderPass::~LumenRenderPass() {}

void LumenRenderPass::Initialize(const RenderPassInitInfo* init_info)
{
    (void)init_info;

    // 初始配置来自 CVar，而不是写死在这里 —— 以前这里硬编码了一组值，运行时
    // 没有任何办法开关 GI 或者调 probe 密度。
    m_LumenConfig = LumenSettings::BuildConfig();
    m_LumenConfig.use_hierarchical_distance_field = true;
    m_LumenConfig.use_software_ray_tracing = true;
    m_LumenConfig.use_hardware_ray_tracing = false;  // 默认使用软件追踪
    m_LumenConfig.ray_tracing_type = RayTracingType::Software;
    m_LumenConfig.quality = LumenQuality::High;

    // Lumen 今天**完全是 CPU 的**（距离场体素化、表面缓存、probe 网格），一行都不跑在
    // GPU 上。所以初始化只真的需要 render_resource（CPU 网格几何的唯一来源），
    // 以前那个 `m_Rhi && m_RenderResource` 的条件会在 headless / 还没有 RHI 的时候
    // 把整个子系统留成未初始化状态。
    m_RendererInitialized = false;
    if (m_RenderResource)
    {
        m_RendererInitialized = m_LumenRenderer.Initialize(m_Rhi, m_RenderResource, m_LumenConfig);
    }
}

void LumenRenderPass::PreparePassData(std::shared_ptr<RenderResourceBase> render_resource)
{
    m_RenderResource = render_resource;

    // Initialize 常常在 render_resource 还没建好时就被调用（管线 Setup 的顺序），
    // 所以真正的初始化在这里补 —— 没有 CPU 网格几何的话 Lumen 一个三角形都拿不到。
    if (m_RenderResource && !m_RendererInitialized)
    {
        m_RendererInitialized = m_LumenRenderer.Initialize(m_Rhi, m_RenderResource, m_LumenConfig);
    }
}

void LumenRenderPass::Render(std::shared_ptr<RenderScene> render_scene,
                             std::shared_ptr<RenderCamera> camera,
                             float delta_time)
{
    if (!render_scene || !camera)
    {
        return;
    }

    if (!m_RendererInitialized)
    {
        return;
    }

    ApplyConsoleVariables();

    // 更新Lumen系统
    m_LumenRenderer.Update(render_scene, camera, delta_time);

    // 计算全局光照
    m_LumenRenderer.ComputeGlobalIllumination(render_scene, camera, delta_time);

    // 计算反射
    m_LumenRenderer.ComputeReflections(render_scene, camera);

    // 把结果写出来，供后续 Pass / per-frame uniform 使用
    PublishIndirectLighting(*render_scene);
}

void LumenRenderPass::ApplyConsoleVariables()
{
    // 每帧都推一次，所以 r.Lumen.* 是热生效的。结构性字段
    // （distance_field_resolution / surface_cache_resolution）不在这里覆盖：它们变了
    // 必须重建资源，改它们要重开管线。
    LumenConfig desired = m_LumenConfig;
    LumenSettings::ApplyTo(desired);

    if (desired.enabled == m_LumenConfig.enabled &&
        desired.enable_global_illumination == m_LumenConfig.enable_global_illumination &&
        desired.enable_reflections == m_LumenConfig.enable_reflections &&
        desired.gi_probe_update_budget == m_LumenConfig.gi_probe_update_budget &&
        desired.gi_sample_count == m_LumenConfig.gi_sample_count &&
        desired.gi_probe_spacing == m_LumenConfig.gi_probe_spacing &&
        desired.gi_temporal_blend_rate == m_LumenConfig.gi_temporal_blend_rate)
    {
        return;
    }

    m_LumenConfig = desired;
    m_LumenRenderer.ApplyConfig(m_LumenConfig);
}

void LumenRenderPass::PublishIndirectLighting(RenderScene& render_scene)
{
    Vector3 irradiance(0.0f, 0.0f, 0.0f);
    uint32_t probe_count = 0;
    m_HasPublished = m_LumenRenderer.GetIndirectLighting(irradiance, probe_count);

    SceneIndirectLighting& published = render_scene.m_IndirectLighting;
    if (!m_HasPublished)
    {
        // 关掉了 / 没有距离场 / 一个 probe 都还没更新：权重归零，让延迟光照继续用
        // 原来的 baked irradiance cubemap。保留旧的 frame_index 没意义，但保留旧的
        // 颜色也没意义，所以整份清掉。
        published = SceneIndirectLighting {};
        return;
    }

    published.irradiance = irradiance * LumenSettings::GetIndirectIntensity();
    published.weight = 1.0f;
    published.frame_index = m_LumenRenderer.getGlobalIllumination()->getFrameIndex();
}
