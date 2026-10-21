#include "LumenRenderer.h"

#include "Runtime/Function/Render/Interface/RHI.h"
#include "Runtime/Function/Render/RenderResource.h"

LumenRenderer::LumenRenderer() {}

LumenRenderer::~LumenRenderer()
{
    clear();
}

bool LumenRenderer::Initialize(RHI* rhi,
                               std::shared_ptr<RenderResourceBase> render_resource,
                               const LumenConfig& config)
{
    m_Rhi = rhi;
    m_RenderResource = render_resource;
    m_Config = config;

    // 初始化子系统
    if (!m_DistanceField.Initialize(rhi, config))
    {
        return false;
    }

    if (!m_SurfaceCache.Initialize(rhi, config))
    {
        return false;
    }

    if (!m_RayTracing.Initialize(rhi, config))
    {
        return false;
    }

    if (!m_GlobalIllumination.Initialize(rhi, config))
    {
        return false;
    }

    if (!m_Reflections.Initialize(rhi, config))
    {
        return false;
    }

    // 设置子系统之间的引用
    //
    // 距离场除了给 LumenRayTracing，也要直接给 GI / Reflections：它们要用它把着色点
    // 抬出表面（自相交偏移按体素尺寸算），并且追踪时必须显式把它传进 TraceRay。
    m_RayTracing.setDistanceField(&m_DistanceField);
    m_GlobalIllumination.setRayTracing(&m_RayTracing);
    m_GlobalIllumination.setSurfaceCache(&m_SurfaceCache);
    m_GlobalIllumination.setDistanceField(&m_DistanceField);
    m_Reflections.setRayTracing(&m_RayTracing);
    m_Reflections.setDistanceField(&m_DistanceField);

    // 初始化统计信息
    m_Stats = LumenStats {};

    return true;
}

void LumenRenderer::clear()
{
    m_Reflections.clear();
    m_GlobalIllumination.clear();
    m_RayTracing.clear();
    m_SurfaceCache.clear();
    m_DistanceField.clear();
}

void LumenRenderer::Update(std::shared_ptr<RenderScene> render_scene,
                           std::shared_ptr<RenderCamera> camera,
                           float delta_time)
{
    if (!m_Config.enabled || !render_scene || !camera)
    {
        return;
    }

    Vector3 camera_position = camera->position();

    // 更新距离场
    if (m_Config.enable_global_illumination || m_Config.enable_reflections)
    {
        UpdateDistanceField(render_scene, camera_position);
    }

    // 更新表面缓存
    if (m_Config.enable_global_illumination)
    {
        UpdateSurfaceCache(render_scene, camera_position);
    }
}

void LumenRenderer::ComputeGlobalIllumination(std::shared_ptr<RenderScene> render_scene,
                                              std::shared_ptr<RenderCamera> camera,
                                              float delta_time)
{
    if (!m_Config.enable_global_illumination)
    {
        return;
    }

    m_GlobalIllumination.ComputeGlobalIllumination(render_scene, camera, &m_RayTracing, &m_SurfaceCache, delta_time);
}

void LumenRenderer::ComputeReflections(std::shared_ptr<RenderScene> render_scene, std::shared_ptr<RenderCamera> camera)
{
    if (!m_Config.enable_reflections)
    {
        return;
    }

    // TODO: 实现反射计算
    // 这里需要在渲染Pass中调用
}

void LumenRenderer::UpdateDistanceField(std::shared_ptr<RenderScene> render_scene, const Vector3& camera_position)
{
    // 世界空间边界交给 LumenDistanceField 从场景几何推导（m_WorldBounds 还无效时）。
    // 以前这里写死一个 ±1000 的盒子，体素尺寸会大到距离场完全没有意义。
    //
    // render_resource 是 CPU 侧网格几何（CpuMeshGeometry 缓存）的唯一来源，没有它
    // 距离场连一个三角形都拿不到。
    m_DistanceField.UpdateDistanceField(render_scene, m_RenderResource.get(), camera_position);

    m_WorldBounds = m_DistanceField.getWorldBounds();
}

void LumenRenderer::UpdateSurfaceCache(std::shared_ptr<RenderScene> render_scene, const Vector3& camera_position)
{
    m_SurfaceCache.UpdateSurfaceCache(render_scene, camera_position);
}