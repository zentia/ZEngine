// RenderResource::UpdatePerFrameBuffer 的唯一实现（所有平台共用）。
//
// 这份函数以前**有两份**，而且已经分叉：
//   - `RenderResource.cpp`（Vulkan 侧）—— 整个文件体包在 `#if defined(Z_HAS_VULKAN)`
//     里，所以 Apple / Metal 上根本不存在这个函数（`nm` 看是未定义符号 U）。
//   - `RenderResourceDx12.cpp`（DX12 侧）—— 包在 `#if defined(_WIN32)` 里。
//
// 函数本身是**完全平台无关**的：纯数学 + 填 per-frame 结构体，零 `m_Rhi->` 调用。
// 它"属于某个后端"只是因为所在文件被宏包住了。放在这里之后三个平台共用一份。
//
// 合并时取并集，两边独有的东西都保留：
//   * 来自 DX12：render_scene/camera 的 null 守卫；用真实 viewport 的宽高比调
//     `GetProjectionMatrixForAspect`（`RenderScene.cpp` 里已是同一套写法）。
//   * 来自 Vulkan：`Z_PROFILE_SCOPE`；per-viewport 的粒子碰撞 / billboard buffer
//     与 `m_PickPassPerFrame`。这些只有 Vulkan 侧的 ParticlePass / PickPass 消费，
//     在别的后端上写了也没人读，但留着才不会让那份行为悄悄丢失。
//
// 注意：改这里会同时影响 Vulkan / DX12 / Metal 三个后端，别只在一边验证。

#include "Runtime/Function/Render/RenderResource.h"

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Function/Render/MegaLights/MegaLightsSettings.h"
#include "Runtime/Function/Render/RenderSystem.h"
#include "Runtime/Profiler/Profiler.h"

void RenderResource::UpdatePerFrameBuffer(std::shared_ptr<RenderScene> render_scene,
                                          std::shared_ptr<RenderCamera> camera)
{
    Z_PROFILE_SCOPE("RenderResource::updatePerFrameBuffer");

    if (render_scene == nullptr || camera == nullptr)
    {
        return;
    }

    Matrix4x4 view_matrix = camera->GetViewMatrix();
    ViewportType viewport_type =
        (camera->m_CurrentCameraType == RenderCameraType::Game) ? ViewportType::game : ViewportType::scene;

    float viewport_aspect = camera->getAspect();
    if (viewport_type == ViewportType::scene)
    {
        if (auto* render_system = GET_SYSTEM(RenderSystem))
        {
            RHIViewport render_viewport {};
            RHIRect2D render_scissor {};
            if (render_system->TryGetRenderSceneViewport(render_viewport, render_scissor) &&
                render_viewport.width > 0.0f && render_viewport.height > 0.0f)
            {
                viewport_aspect = render_viewport.width / render_viewport.height;
            }
        }
    }

    if (RHI* rhi = GET_SYSTEM(RHI))
    {
        if (RHIViewport* viewport = rhi->GetViewport(viewport_type))
        {
            if (viewport->width > 0.0f && viewport->height > 0.0f)
            {
                viewport_aspect = viewport->width / viewport->height;
            }
        }
    }

    // Advance the TAA sub-pixel jitter before the projection is built, so the matrix the
    // scene renders with and the one TAAPass reprojects with are the same one. A no-op
    // (jitter stays zero) while TAA is off.
    // One advance per frame: UpdatePerFrameBuffer runs once per camera (editor + game),
    // and a second advance would give the two viewports different jitter while they share
    // a single TAA history.
    if (viewport_type == ViewportType::game)
    {
        if (RHI* rhi = GET_SYSTEM(RHI))
        {
            const RHIExtent2D& extent = rhi->GetSwapchainInfo().extent;
            camera->AdvanceTaaJitter(extent.width, extent.height);
        }
    }

    Matrix4x4 proj_matrix = camera->GetProjectionMatrix();
    if (viewport_aspect > 0.0f)
    {
        proj_matrix = camera->GetProjectionMatrixForAspect(viewport_aspect);
    }
    Vector3 camera_position = camera->position();
    Matrix4x4 proj_view_matrix = proj_matrix * view_matrix;

    const size_t viewport_index = static_cast<size_t>(viewport_type);

    auto& mesh_perframe_storage_buffer_object = m_MainCameraPerFrameByViewport[viewport_index];
    auto& particle_collision_perframe_storage_buffer_object = m_ParticleCollisionPerFrameByViewport[viewport_index];
    auto& particlebillboard_perframe_storage_buffer_object = m_ParticleBillboardPerFrameByViewport[viewport_index];

    const Vector3 ambient_light = render_scene->m_AmbientLight.m_Irradiance;
    const uint32_t point_light_num = static_cast<uint32_t>(render_scene->m_PointLightList.m_Lights.size());

    particle_collision_perframe_storage_buffer_object.view_matrix = view_matrix;
    particle_collision_perframe_storage_buffer_object.proj_view_matrix = proj_view_matrix;
    particle_collision_perframe_storage_buffer_object.proj_inv_matrix = proj_matrix.inverse();

    mesh_perframe_storage_buffer_object.proj_view_matrix = proj_view_matrix;
    mesh_perframe_storage_buffer_object.camera_position = camera_position;
    // CSM：采样侧要靠朝向把片元位置换算成视图深度再去跟 split 距离比。per-frame
    // 里原来只有位置，径向距离 ≠ 视图深度，画面边缘会选错级。
    mesh_perframe_storage_buffer_object.camera_forward = camera->forward();
    bool viewport_rect_set = false;
    if (viewport_type == ViewportType::scene)
    {
        if (auto* render_system = GET_SYSTEM(RenderSystem))
        {
            RHIViewport render_viewport {};
            RHIRect2D render_scissor {};
            if (render_system->TryGetRenderSceneViewport(render_viewport, render_scissor) &&
                render_viewport.width > 0.0f && render_viewport.height > 0.0f)
            {
                mesh_perframe_storage_buffer_object.viewport_rect =
                    Vector4(render_viewport.x, render_viewport.y, render_viewport.width, render_viewport.height);
                viewport_rect_set = true;
            }
        }
    }
    if (!viewport_rect_set)
    {
        if (RHI* viewport_rhi = GET_SYSTEM(RHI))
        {
            if (RHIViewport* viewport = viewport_rhi->GetViewport(viewport_type))
            {
                if (viewport->width > 0.0f && viewport->height > 0.0f)
                {
                    mesh_perframe_storage_buffer_object.viewport_rect =
                        Vector4(viewport->x, viewport->y, viewport->width, viewport->height);
                }
            }
        }
    }
    if (LargeWorldCoordinates::IsEnabled())
    {
        const Vector3d tile = LargeWorldCoordinates::GetRenderTile();
        mesh_perframe_storage_buffer_object.render_tile = tile.ToVector3();
        mesh_perframe_storage_buffer_object.pre_view_translation =
            LargeWorldCoordinates::GetPreViewTranslation(camera->worldPosition());
    }
    else
    {
        mesh_perframe_storage_buffer_object.render_tile = Vector3::ZERO;
        mesh_perframe_storage_buffer_object.pre_view_translation = Vector3::ZERO;
    }
    mesh_perframe_storage_buffer_object.ambient_light = ambient_light;
    mesh_perframe_storage_buffer_object.point_light_num = point_light_num;
    if (auto* render_system = GET_SYSTEM(RenderSystem))
    {
        mesh_perframe_storage_buffer_object.show_skybox = render_system->IsSkyboxVisible(viewport_type) ? 1U : 0U;
    }
    else
    {
        mesh_perframe_storage_buffer_object.show_skybox = 1U;
    }

    // Lumen 的间接光：CPU probe 网格 -> shader 的唯一通路。权重 0 时 shader 侧行为与
    // 加字段之前完全一致，继续用 ambient_light。
    mesh_perframe_storage_buffer_object.lumen_indirect = render_scene->m_IndirectLighting.ToUniform();
    mesh_perframe_storage_buffer_object.lumen_reflection = render_scene->m_IndirectLighting.ToReflectionUniform();

    m_PointLightShadowPerFrame.point_light_num = point_light_num;
    for (uint32_t i = 0; i < point_light_num; ++i)
    {
        const Vector3 point_light_position = render_scene->m_PointLightList.m_Lights[i].m_Position;
        const Vector3 point_light_intensity =
            render_scene->m_PointLightList.m_Lights[i].m_Flux / (4.0f * Math_PI);
        const float radius = render_scene->m_PointLightList.m_Lights[i].calculateRadius();

        mesh_perframe_storage_buffer_object.scene_point_lights[i].position = point_light_position;
        mesh_perframe_storage_buffer_object.scene_point_lights[i].radius = radius;
        mesh_perframe_storage_buffer_object.scene_point_lights[i].intensity = point_light_intensity;

        // Spot payload: the shader keys the cone branch off `type`, so point
        // lights keep type=0 and their zeroed spot fields (bit-identical to the
        // pre-spotlight behaviour). Spot lights fill direction + cone cosines.
        const PointLight& src_light = render_scene->m_PointLightList.m_Lights[i];
        if (src_light.m_IsSpot)
        {
            mesh_perframe_storage_buffer_object.scene_point_lights[i].type = 1.0f;
            mesh_perframe_storage_buffer_object.scene_point_lights[i].spot_direction = src_light.m_Direction;
            mesh_perframe_storage_buffer_object.scene_point_lights[i].spot_inner_cos = src_light.m_InnerConeCos;
            mesh_perframe_storage_buffer_object.scene_point_lights[i].spot_outer_cos = src_light.m_OuterConeCos;
        }
        else
        {
            mesh_perframe_storage_buffer_object.scene_point_lights[i].type = 0.0f;
            mesh_perframe_storage_buffer_object.scene_point_lights[i].spot_direction = Vector3::ZERO;
            mesh_perframe_storage_buffer_object.scene_point_lights[i].spot_inner_cos = 0.0f;
            mesh_perframe_storage_buffer_object.scene_point_lights[i].spot_outer_cos = 0.0f;
        }

        m_PointLightShadowPerFrame.point_lights_position_and_radius[i] = Vector4(point_light_position, radius);
    }

    mesh_perframe_storage_buffer_object.scene_directional_light.direction =
        render_scene->m_DirectionalLight.m_Direction.normalisedCopy();
    mesh_perframe_storage_buffer_object.scene_directional_light.color = render_scene->m_DirectionalLight.m_Color;

    // pick pass view projection matrix
    m_PickPassPerFrame.proj_view_matrix = proj_view_matrix;

    particlebillboard_perframe_storage_buffer_object.proj_view_matrix = proj_view_matrix;
    particlebillboard_perframe_storage_buffer_object.right_direction = camera->right();
    particlebillboard_perframe_storage_buffer_object.forward_direction = camera->forward();
    particlebillboard_perframe_storage_buffer_object.up_direction = camera->up();

    m_ParticleCollisionPerFrame = particle_collision_perframe_storage_buffer_object;
    m_MainCameraPerFrame = mesh_perframe_storage_buffer_object;
    m_ParticleBillboardPerFrame = particlebillboard_perframe_storage_buffer_object;

    if (MegaLights::IsEnabled())
    {
        m_MegaLights.Update(*render_scene, camera, viewport_type);
        mesh_perframe_storage_buffer_object.point_light_num = 0;
        m_MainCameraPerFrame.point_light_num = 0;
    }
}
