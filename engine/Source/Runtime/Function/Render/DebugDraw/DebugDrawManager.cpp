#include "DebugDrawManager.h"

#if defined(__APPLE__) || !defined(Z_HAS_VULKAN)

bool DebugDrawManager::Initialize()
{
    m_RenderEnabled = false;
    return true;
}

void DebugDrawManager::SetupPipelines() {}
void DebugDrawManager::Destory() {}
void DebugDrawManager::Shutdown() {}
void DebugDrawManager::Tick(float) {}
void DebugDrawManager::UpdateAfterRecreateSwapchain() {}
void DebugDrawManager::PreparePassData(std::shared_ptr<RenderResourceBase>) {}
void DebugDrawManager::Draw(uint32_t) {}
DebugDrawGroup* DebugDrawManager::TryGetOrCreateDebugDrawGroup(const std::string&)
{
    return nullptr;
}

#else
    #include "Runtime/Core/Math/MathHeaders.h"
    #include "Runtime/Function/Render/RenderSystem.h"
    #include "Runtime/Profiler/Profiler.h"

    #include <algorithm>
    #include <cmath>

namespace
{
    constexpr char k_scene_grid_group_name[] = "editor_scene_grid";
    constexpr float k_scene_grid_plane_height = -0.05f;
    constexpr int k_scene_grid_major_line_every = 10;
    constexpr int k_scene_grid_half_major_count = 10;
    constexpr int k_scene_grid_half_minor_count = k_scene_grid_half_major_count * k_scene_grid_major_line_every;
    constexpr float k_scene_grid_minor_alpha = 0.18f;
    constexpr float k_scene_grid_major_alpha = 0.32f;
    constexpr float k_scene_grid_axis_alpha = 0.85f;

    float getSceneGridMinorSpacing(float camera_height)
    {
        const float safe_height = std::max(camera_height, 1.0f);
        const float major_spacing = std::pow(10.0f, std::floor(std::log10(safe_height)));
        return std::max(1.0f, major_spacing / static_cast<float>(k_scene_grid_major_line_every));
    }

    Vector4 getSceneGridLineColor(int line_index, bool is_vertical_line)
    {
        if (is_vertical_line && line_index == 0)
        {
            return Vector4(0.35f, 0.80f, 0.45f, k_scene_grid_axis_alpha);
        }

        if (!is_vertical_line && line_index == 0)
        {
            return Vector4(0.85f, 0.35f, 0.35f, k_scene_grid_axis_alpha);
        }

        if (line_index % k_scene_grid_major_line_every == 0)
        {
            return Vector4(0.58f, 0.63f, 0.72f, k_scene_grid_major_alpha);
        }

        return Vector4(0.47f, 0.52f, 0.60f, k_scene_grid_minor_alpha);
    }
}  // namespace

bool DebugDrawManager::Initialize()
{
    m_Rhi = GET_SYSTEM(RHI);
    m_RenderEnabled = m_Rhi != nullptr && m_Rhi->getGraphicsAPI() == GraphicsAPI::Vulkan;
    if (!m_RenderEnabled)
    {
        return true;
    }

    SetupPipelines();
    return true;
}

void DebugDrawManager::SetupPipelines()
{
    if (!m_RenderEnabled)
    {
        return;
    }

    // setup pipelines
    for (uint8_t i = 0; i < DebugDrawPipelineType::_debug_draw_pipeline_type_count; i++)
    {
        m_DebugDrawPipeline[i] = new DebugDrawPipeline((DebugDrawPipelineType)i);
        m_DebugDrawPipeline[i]->Initialize();
    }
    m_BufferAllocator = new DebugDrawAllocator();
    m_Font = new DebugDrawFont();
    m_Font->Inialize();
    m_BufferAllocator->Initialize(m_Font);
}

void DebugDrawManager::Destory()
{
    for (uint8_t i = 0; i < DebugDrawPipelineType::_debug_draw_pipeline_type_count; i++)
    {
        if (m_DebugDrawPipeline[i] != nullptr)
        {
            m_DebugDrawPipeline[i]->Destory();
            delete m_DebugDrawPipeline[i];
            m_DebugDrawPipeline[i] = nullptr;
        }
    }

    if (m_BufferAllocator != nullptr)
    {
        m_BufferAllocator->Destory();
        delete m_BufferAllocator;
        m_BufferAllocator = nullptr;
    }

    // m_Font->Destroy();
    delete m_Font;
    m_Font = nullptr;
}
void DebugDrawManager::Shutdown()
{
    std::lock_guard<std::mutex> guard(m_Mutex);
    m_DebugDrawContext.clear();
}

void DebugDrawManager::Tick(float delta_time)
{
    if (!m_RenderEnabled || m_BufferAllocator == nullptr)
    {
        return;
    }

    Z_PROFILE_SCOPE("DebugDrawManager::tick");
    std::lock_guard<std::mutex> guard(m_Mutex);
    m_BufferAllocator->Tick();
    m_DebugDrawContext.Tick(delta_time);
    UpdateSceneGrid();
}

void DebugDrawManager::UpdateSceneGrid()
{
    DebugDrawGroup* scene_grid_group = m_DebugDrawContext.TryGetOrCreateDebugDrawGroup(k_scene_grid_group_name);
    if (scene_grid_group == nullptr)
    {
        return;
    }

    scene_grid_group->clear();

    RHIViewport* scene_viewport = m_Rhi->GetViewport(ViewportType::scene);
    if (scene_viewport == nullptr || scene_viewport->width <= 0.0f || scene_viewport->height <= 0.0f)
    {
        return;
    }

    std::shared_ptr<RenderCamera> scene_camera = GET_SYSTEM(RenderSystem)->GetCamera(ViewportType::scene);
    if (!scene_camera)
    {
        return;
    }

    const Vector3 camera_position = scene_camera->position();
    const float minor_spacing =
        getSceneGridMinorSpacing(std::abs(camera_position.z - k_scene_grid_plane_height));
    const float half_extent = minor_spacing * static_cast<float>(k_scene_grid_half_minor_count);

    const int min_x_index = static_cast<int>(std::floor((camera_position.x - half_extent) / minor_spacing));
    const int max_x_index = static_cast<int>(std::ceil((camera_position.x + half_extent) / minor_spacing));
    const int min_y_index = static_cast<int>(std::floor((camera_position.y - half_extent) / minor_spacing));
    const int max_y_index = static_cast<int>(std::ceil((camera_position.y + half_extent) / minor_spacing));

    const float min_x = min_x_index * minor_spacing;
    const float max_x = max_x_index * minor_spacing;
    const float min_y = min_y_index * minor_spacing;
    const float max_y = max_y_index * minor_spacing;

    for (int x_index = min_x_index; x_index <= max_x_index; ++x_index)
    {
        const float x = x_index * minor_spacing;
        const Vector4 color = getSceneGridLineColor(x_index, true);
        scene_grid_group->AddLine(Vector3(x, min_y, k_scene_grid_plane_height),
                                  Vector3(x, max_y, k_scene_grid_plane_height),
                                  color,
                                  color,
                                  k_debug_draw_one_frame,
                                  false);
    }

    for (int y_index = min_y_index; y_index <= max_y_index; ++y_index)
    {
        const float y = y_index * minor_spacing;
        const Vector4 color = getSceneGridLineColor(y_index, false);
        scene_grid_group->AddLine(Vector3(min_x, y, k_scene_grid_plane_height),
                                  Vector3(max_x, y, k_scene_grid_plane_height),
                                  color,
                                  color,
                                  k_debug_draw_one_frame,
                                  false);
    }
}

void DebugDrawManager::UpdateAfterRecreateSwapchain()
{
    if (!m_RenderEnabled)
    {
        return;
    }

    for (uint8_t i = 0; i < DebugDrawPipelineType::_debug_draw_pipeline_type_count; i++)
    {
        if (m_DebugDrawPipeline[i] != nullptr)
        {
            m_DebugDrawPipeline[i]->RecreateAfterSwapchain();
        }
    }
}

void DebugDrawManager::PreparePassData(std::shared_ptr<RenderResourceBase> render_resource)
{
    if (!m_RenderEnabled || render_resource == nullptr)
    {
        return;
    }

    const RenderResource* resource = static_cast<const RenderResource*>(render_resource.get());
    m_ProjViewMatrix =
        resource->m_MainCameraPerFrameByViewport[static_cast<size_t>(ViewportType::game)].proj_view_matrix;
    m_SceneProjViewMatrix =
        resource->m_MainCameraPerFrameByViewport[static_cast<size_t>(ViewportType::scene)].proj_view_matrix;
}

void DebugDrawManager::SwapDataToRender()
{
    std::lock_guard<std::mutex> guard(m_Mutex);

    m_DebugDrawGroupForRender.clear();
    m_SceneGridGroupForRender.clear();
    size_t debug_draw_group_count = m_DebugDrawContext.m_DebugDrawGroups.size();
    for (size_t debug_draw_group_index = 0; debug_draw_group_index < debug_draw_group_count; debug_draw_group_index++)
    {
        DebugDrawGroup* debug_draw_group = m_DebugDrawContext.m_DebugDrawGroups[debug_draw_group_index];
        if (debug_draw_group == nullptr)
            continue;

        if (debug_draw_group->GetName() == k_scene_grid_group_name)
        {
            m_SceneGridGroupForRender.MergeFrom(debug_draw_group);
            continue;
        }

        m_DebugDrawGroupForRender.MergeFrom(debug_draw_group);
    }
}

void DebugDrawManager::Draw(uint32_t current_swapchain_image_index)
{
    if (!m_RenderEnabled || m_BufferAllocator == nullptr)
    {
        return;
    }

    SwapDataToRender();

    float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    RHICommandBuffer* current_command_buffer = m_Rhi->GetCurrentCommandBuffer();
    m_Rhi->PushEvent(current_command_buffer, "DebugDrawManager", color);

    RHIViewport* scene_viewport = m_Rhi->GetViewport(ViewportType::scene);
    if (scene_viewport != nullptr && scene_viewport->width > 0.0f && scene_viewport->height > 0.0f)
    {
        RHIRect2D scene_scissor = m_Rhi->GetSwapchainInfo().scissor[static_cast<uint32_t>(ViewportType::scene)];
        m_Rhi->PushEvent(current_command_buffer, "SceneGrid", color);
        DrawDebugObject(
            m_SceneGridGroupForRender, m_SceneProjViewMatrix, current_swapchain_image_index, *scene_viewport, scene_scissor);
        m_Rhi->PopEvent(current_command_buffer);
    }

    DrawDebugObject(m_DebugDrawGroupForRender,
                    m_ProjViewMatrix,
                    current_swapchain_image_index,
                    *m_Rhi->GetSwapchainInfo().viewport,
                    *m_Rhi->GetSwapchainInfo().scissor);

    m_Rhi->PopEvent(current_command_buffer);
}

void DebugDrawManager::PrepareDrawBuffer(DebugDrawGroup& debug_draw_group, const Matrix4x4& proj_view_matrix)
{
    if (m_BufferAllocator == nullptr || m_Font == nullptr)
    {
        return;
    }

    m_BufferAllocator->clear();

    std::vector<DebugDrawVertex> vertexs;

    debug_draw_group.WritePointData(vertexs, false);
    m_PointStartOffset = m_BufferAllocator->CacheVertexs(vertexs);
    m_PointEndOffset = m_BufferAllocator->GetVertexCacheOffset();

    debug_draw_group.WriteLineData(vertexs, false);
    m_LineStartOffset = m_BufferAllocator->CacheVertexs(vertexs);
    m_LineEndOffset = m_BufferAllocator->GetVertexCacheOffset();

    debug_draw_group.WriteTriangleData(vertexs, false);
    m_TriangleStartOffset = m_BufferAllocator->CacheVertexs(vertexs);
    m_TriangleEndOffset = m_BufferAllocator->GetVertexCacheOffset();

    debug_draw_group.WritePointData(vertexs, true);
    m_NoDepthTestPointStartOffset = m_BufferAllocator->CacheVertexs(vertexs);
    m_NoDepthTestPointEndOffset = m_BufferAllocator->GetVertexCacheOffset();

    debug_draw_group.WriteLineData(vertexs, true);
    m_NoDepthTestLineStartOffset = m_BufferAllocator->CacheVertexs(vertexs);
    m_NoDepthTestLineEndOffset = m_BufferAllocator->GetVertexCacheOffset();

    debug_draw_group.WriteTriangleData(vertexs, true);
    m_NoDepthTestTriangleStartOffset = m_BufferAllocator->CacheVertexs(vertexs);
    m_NoDepthTestTriangleEndOffset = m_BufferAllocator->GetVertexCacheOffset();

    debug_draw_group.WriteTextData(vertexs, m_Font, proj_view_matrix);
    m_TextStartOffset = m_BufferAllocator->CacheVertexs(vertexs);
    m_TextEndOffset = m_BufferAllocator->GetVertexCacheOffset();

    m_BufferAllocator->CacheUniformObject(proj_view_matrix);

    std::vector<std::pair<Matrix4x4, Vector4>> dynamicObject = {
        std::make_pair(Matrix4x4::IDENTITY, Vector4(0, 0, 0, 0))};
    m_BufferAllocator->CacheUniformDynamicObject(
        dynamicObject);  // cache the first model matrix as Identity matrix, color as empty color. (default object)

    debug_draw_group.WriteUniformDynamicDataToCache(dynamicObject);
    m_BufferAllocator->CacheUniformDynamicObject(dynamicObject);  // cache the wire frame uniform dynamic object

    m_BufferAllocator->Allocator();
}

void DebugDrawManager::DrawPointLineTriangleBox(uint32_t current_swapchain_image_index)
{
    if (m_BufferAllocator == nullptr)
    {
        return;
    }

    // draw point, line ,triangle , triangle_without_depth_test
    RHIBuffer* vertex_buffers[] = {m_BufferAllocator->GetVertexBuffer()};
    if (vertex_buffers[0] == nullptr)
    {
        return;
    }
    RHIDeviceSize offsets[] = {0};
    m_Rhi->CmdBindVertexBuffersPFN(m_Rhi->GetCurrentCommandBuffer(), 0, 1, vertex_buffers, offsets);

    std::vector<DebugDrawPipeline*> vc_pipelines {
        m_DebugDrawPipeline[DebugDrawPipelineType::_debug_draw_pipeline_type_point],
        m_DebugDrawPipeline[DebugDrawPipelineType::_debug_draw_pipeline_type_line],
        m_DebugDrawPipeline[DebugDrawPipelineType::_debug_draw_pipeline_type_triangle],
        m_DebugDrawPipeline[DebugDrawPipelineType::_debug_draw_pipeline_type_point_no_depth_test],
        m_DebugDrawPipeline[DebugDrawPipelineType::_debug_draw_pipeline_type_line_no_depth_test],
        m_DebugDrawPipeline[DebugDrawPipelineType::_debug_draw_pipeline_type_triangle_no_depth_test],
        m_DebugDrawPipeline[DebugDrawPipelineType::_debug_draw_pipeline_type_triangle_no_depth_test]};
    std::vector<size_t> vc_start_offsets {m_PointStartOffset,
                                          m_LineStartOffset,
                                          m_TriangleStartOffset,
                                          m_NoDepthTestPointStartOffset,
                                          m_NoDepthTestLineStartOffset,
                                          m_NoDepthTestTriangleStartOffset,
                                          m_TextStartOffset};
    std::vector<size_t> vc_end_offsets {m_PointEndOffset,
                                        m_LineEndOffset,
                                        m_TriangleEndOffset,
                                        m_NoDepthTestPointEndOffset,
                                        m_NoDepthTestLineEndOffset,
                                        m_NoDepthTestTriangleEndOffset,
                                        m_TextEndOffset};
    RHIClearValue clear_values[2];
    clear_values[0].color = {0.0f, 0.0f, 0.0f, 0.0f};
    clear_values[1].depthStencil = {1.0f, 0};
    RHIRenderPassBeginInfo renderpass_begin_info {};
    renderpass_begin_info.sType = RHI_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderpass_begin_info.renderArea.offset = {0, 0};
    renderpass_begin_info.renderArea.extent = m_Rhi->GetSwapchainInfo().extent;
    renderpass_begin_info.clearValueCount = (sizeof(clear_values) / sizeof(clear_values[0]));
    renderpass_begin_info.pClearValues = clear_values;

    for (size_t i = 0; i < vc_pipelines.size(); i++)
    {
        if (vc_end_offsets[i] - vc_start_offsets[i] == 0)
        {
            continue;
        }
        renderpass_begin_info.renderPass = vc_pipelines[i]->GetFramebuffer().render_pass;
        renderpass_begin_info.framebuffer =
            vc_pipelines[i]->GetFramebuffer().framebuffers[current_swapchain_image_index];
        m_Rhi->CmdBeginRenderPassPFN(
            m_Rhi->GetCurrentCommandBuffer(), &renderpass_begin_info, RHI_SUBPASS_CONTENTS_INLINE);

        m_Rhi->CmdBindPipelinePFN(m_Rhi->GetCurrentCommandBuffer(),
                                  RHI_PIPELINE_BIND_POINT_GRAPHICS,
                                  vc_pipelines[i]->GetPipeline().pipeline);

        uint32_t dynamicOffset = 0;
        m_Rhi->CmdBindDescriptorSetsPFN(m_Rhi->GetCurrentCommandBuffer(),
                                        RHI_PIPELINE_BIND_POINT_GRAPHICS,
                                        vc_pipelines[i]->GetPipeline().layout,
                                        0,
                                        1,
                                        &m_BufferAllocator->GetDescriptorSet(),
                                        1,
                                        &dynamicOffset);
        m_Rhi->CmdDraw(
            m_Rhi->GetCurrentCommandBuffer(), vc_end_offsets[i] - vc_start_offsets[i], 1, vc_start_offsets[i], 0);

        m_Rhi->CmdEndRenderPassPFN(m_Rhi->GetCurrentCommandBuffer());
    }
}

void DebugDrawManager::DrawWireFrameObject(DebugDrawGroup& debug_draw_group, uint32_t current_swapchain_image_index)
{
    if (m_BufferAllocator == nullptr)
    {
        return;
    }

    // draw wire frame object : sphere, cylinder, capsule

    std::vector<DebugDrawPipeline*> vc_pipelines {
        m_DebugDrawPipeline[DebugDrawPipelineType::_debug_draw_pipeline_type_line],
        m_DebugDrawPipeline[DebugDrawPipelineType::_debug_draw_pipeline_type_line_no_depth_test]};
    std::vector<bool> no_depth_tests = {false, true};

    for (int32_t i = 0; i < 2; i++)
    {
        bool no_depth_test = no_depth_tests[i];

        RHIDeviceSize offsets[] = {0};
        RHIClearValue clear_values[2];
        clear_values[0].color = {0.0f, 0.0f, 0.0f, 0.0f};
        clear_values[1].depthStencil = {1.0f, 0};
        RHIRenderPassBeginInfo renderpass_begin_info {};
        renderpass_begin_info.sType = RHI_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderpass_begin_info.renderArea.offset = {0, 0};
        renderpass_begin_info.renderArea.extent = m_Rhi->GetSwapchainInfo().extent;
        renderpass_begin_info.clearValueCount = (sizeof(clear_values) / sizeof(clear_values[0]));
        renderpass_begin_info.pClearValues = clear_values;
        renderpass_begin_info.renderPass = vc_pipelines[i]->GetFramebuffer().render_pass;
        renderpass_begin_info.framebuffer =
            vc_pipelines[i]->GetFramebuffer().framebuffers[current_swapchain_image_index];
        m_Rhi->CmdBeginRenderPassPFN(
            m_Rhi->GetCurrentCommandBuffer(), &renderpass_begin_info, RHI_SUBPASS_CONTENTS_INLINE);
        m_Rhi->CmdBindPipelinePFN(m_Rhi->GetCurrentCommandBuffer(),
                                  RHI_PIPELINE_BIND_POINT_GRAPHICS,
                                  vc_pipelines[i]->GetPipeline().pipeline);

        size_t uniform_dynamic_size = m_BufferAllocator->GetSizeOfUniformBufferObject();
        uint32_t dynamicOffset = uniform_dynamic_size;

        size_t sphere_count = debug_draw_group.GetSphereCount(no_depth_test);
        size_t cylinder_count = debug_draw_group.GetCylinderCount(no_depth_test);
        size_t capsule_count = debug_draw_group.GetCapsuleCount(no_depth_test);

        if (sphere_count > 0)
        {
            RHIBuffer* sphere_vertex_buffers[] = {m_BufferAllocator->GetSphereVertexBuffer()};
            m_Rhi->CmdBindVertexBuffersPFN(m_Rhi->GetCurrentCommandBuffer(), 0, 1, sphere_vertex_buffers, offsets);
            for (size_t j = 0; j < sphere_count; j++)
            {
                m_Rhi->CmdBindDescriptorSetsPFN(m_Rhi->GetCurrentCommandBuffer(),
                                                RHI_PIPELINE_BIND_POINT_GRAPHICS,
                                                vc_pipelines[i]->GetPipeline().layout,
                                                0,
                                                1,
                                                &m_BufferAllocator->GetDescriptorSet(),
                                                1,
                                                &dynamicOffset);
                m_Rhi->CmdDraw(
                    m_Rhi->GetCurrentCommandBuffer(), m_BufferAllocator->GetSphereVertexBufferSize(), 1, 0, 0);
                dynamicOffset += uniform_dynamic_size;
            }
        }

        if (cylinder_count > 0)
        {
            RHIBuffer* cylinder_vertex_buffers[] = {m_BufferAllocator->GetCylinderVertexBuffer()};
            m_Rhi->CmdBindVertexBuffersPFN(m_Rhi->GetCurrentCommandBuffer(), 0, 1, cylinder_vertex_buffers, offsets);
            for (size_t j = 0; j < cylinder_count; j++)
            {
                m_Rhi->CmdBindDescriptorSetsPFN(m_Rhi->GetCurrentCommandBuffer(),
                                                RHI_PIPELINE_BIND_POINT_GRAPHICS,
                                                vc_pipelines[i]->GetPipeline().layout,
                                                0,
                                                1,
                                                &m_BufferAllocator->GetDescriptorSet(),
                                                1,
                                                &dynamicOffset);
                m_Rhi->CmdDraw(
                    m_Rhi->GetCurrentCommandBuffer(), m_BufferAllocator->GetCylinderVertexBufferSize(), 1, 0, 0);
                dynamicOffset += uniform_dynamic_size;
            }
        }

        if (capsule_count > 0)
        {
            RHIBuffer* capsule_vertex_buffers[] = {m_BufferAllocator->GetCapsuleVertexBuffer()};
            m_Rhi->CmdBindVertexBuffersPFN(m_Rhi->GetCurrentCommandBuffer(), 0, 1, capsule_vertex_buffers, offsets);
            for (size_t j = 0; j < capsule_count; j++)
            {
                // draw capsule up part
                m_Rhi->CmdBindDescriptorSetsPFN(m_Rhi->GetCurrentCommandBuffer(),
                                                RHI_PIPELINE_BIND_POINT_GRAPHICS,
                                                vc_pipelines[i]->GetPipeline().layout,
                                                0,
                                                1,
                                                &m_BufferAllocator->GetDescriptorSet(),
                                                1,
                                                &dynamicOffset);
                m_Rhi->CmdDraw(
                    m_Rhi->GetCurrentCommandBuffer(), m_BufferAllocator->GetCapsuleVertexBufferUpSize(), 1, 0, 0);
                dynamicOffset += uniform_dynamic_size;

                // draw capsule mid part
                m_Rhi->CmdBindDescriptorSetsPFN(m_Rhi->GetCurrentCommandBuffer(),
                                                RHI_PIPELINE_BIND_POINT_GRAPHICS,
                                                vc_pipelines[i]->GetPipeline().layout,
                                                0,
                                                1,
                                                &m_BufferAllocator->GetDescriptorSet(),
                                                1,
                                                &dynamicOffset);
                m_Rhi->CmdDraw(m_Rhi->GetCurrentCommandBuffer(),
                               m_BufferAllocator->GetCapsuleVertexBufferMidSize(),
                               1,
                               m_BufferAllocator->GetCapsuleVertexBufferUpSize(),
                               0);
                dynamicOffset += uniform_dynamic_size;

                // draw capsule down part
                m_Rhi->CmdBindDescriptorSetsPFN(m_Rhi->GetCurrentCommandBuffer(),
                                                RHI_PIPELINE_BIND_POINT_GRAPHICS,
                                                vc_pipelines[i]->GetPipeline().layout,
                                                0,
                                                1,
                                                &m_BufferAllocator->GetDescriptorSet(),
                                                1,
                                                &dynamicOffset);
                m_Rhi->CmdDraw(m_Rhi->GetCurrentCommandBuffer(),
                               m_BufferAllocator->GetCapsuleVertexBufferDownSize(),
                               1,
                               m_BufferAllocator->GetCapsuleVertexBufferUpSize() +
                                   m_BufferAllocator->GetCapsuleVertexBufferMidSize(),
                               0);
                dynamicOffset += uniform_dynamic_size;
            }
        }

        m_Rhi->CmdEndRenderPassPFN(m_Rhi->GetCurrentCommandBuffer());
    }
}

void DebugDrawManager::DrawDebugObject(DebugDrawGroup& debug_draw_group,
                                       const Matrix4x4& proj_view_matrix,
                                       uint32_t current_swapchain_image_index,
                                       const RHIViewport& viewport,
                                       const RHIRect2D& scissor)
{
    if (!m_RenderEnabled || m_BufferAllocator == nullptr)
    {
        return;
    }

    m_Rhi->CmdSetViewportPFN(m_Rhi->GetCurrentCommandBuffer(), 0, 1, &viewport);
    m_Rhi->CmdSetScissorPFN(m_Rhi->GetCurrentCommandBuffer(), 0, 1, &scissor);
    PrepareDrawBuffer(debug_draw_group, proj_view_matrix);
    DrawPointLineTriangleBox(current_swapchain_image_index);
    DrawWireFrameObject(debug_draw_group, current_swapchain_image_index);
}

DebugDrawGroup* DebugDrawManager::TryGetOrCreateDebugDrawGroup(const std::string& name)
{
    std::lock_guard<std::mutex> guard(m_Mutex);
    return m_DebugDrawContext.TryGetOrCreateDebugDrawGroup(name);
}

#endif
