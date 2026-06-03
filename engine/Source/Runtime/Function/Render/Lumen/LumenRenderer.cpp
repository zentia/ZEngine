#include "LumenRenderer.h"

#include "Runtime/Function/Render/Interface/RHI.h"
#include "Runtime/Function/Render/RenderResource.h"

LumenRenderer::LumenRenderer() {}

LumenRenderer::~LumenRenderer()
{
    clear();
}

bool LumenRenderer::Initialize(std::shared_ptr<RHI> rhi,
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
    m_RayTracing.setDistanceField(&m_DistanceField);
    m_GlobalIllumination.setRayTracing(&m_RayTracing);
    m_GlobalIllumination.setSurfaceCache(&m_SurfaceCache);
    m_Reflections.setRayTracing(&m_RayTracing);

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
    // 计算或更新世界空间边界
    if (!m_WorldBounds.IsValid())
    {
        // TODO: 从场景计算世界空间边界
        m_WorldBounds = AxisAlignedBox(Vector3(-1000.0f, -1000.0f, -1000.0f), Vector3(1000.0f, 1000.0f, 1000.0f));
    }

    // 更新距离场
    m_DistanceField.UpdateDistanceField(render_scene, camera_position);
}

void LumenRenderer::UpdateSurfaceCache(std::shared_ptr<RenderScene> render_scene, const Vector3& camera_position)
{
    m_SurfaceCache.UpdateSurfaceCache(render_scene, camera_position);
}