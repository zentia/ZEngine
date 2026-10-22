#include "Runtime/Function/Render/RenderPipelineBase.h"

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Function/Render/RenderCamera.h"
#include "Runtime/Function/Render/RenderScene.h"
#include "Runtime/Function/Render/RenderSwapContext.h"
#include "Runtime/Function/Render/RenderingThread/RenderThreadChecks.h"
#include "Runtime/Profiler/Profiler.h"
#if defined(Z_HAS_LUMEN)
    #include "Runtime/Function/Render/Lumen/LumenRenderPass.h"
#endif
// Nanite 不像 Lumen 那样会被剥离：Runtime/CMakeLists.txt 从不把
// Function/Render/Nanite/*.cpp 过滤掉，所以这个通道在任何配置上都存在。
#include "Runtime/Function/Render/Nanite/NaniteRenderer.h"
#if defined(Z_HAS_VULKAN)
    #include "Runtime/Function/Render/DebugDraw/DebugDrawManager.h"
#endif

void RenderPipelineBase::PreparePassData(std::shared_ptr<RenderResourceBase> render_resource)
{
    CHECK_RENDER_THREAD();
    Z_PROFILE_SCOPE("RenderPipelineBase::preparePassData");
    if (m_MainCameraPass)
    {
        m_MainCameraPass->PreparePassData(render_resource);
    }
    if (m_LumenPass)
    {
        m_LumenPass->PreparePassData(render_resource);
    }
    if (m_NanitePass)
    {
        m_NanitePass->PreparePassData(render_resource);
    }
    if (m_PickPass)
    {
        m_PickPass->PreparePassData(render_resource);
    }
    if (m_DirectionalLightPass)
    {
        m_DirectionalLightPass->PreparePassData(render_resource);
    }
    if (m_PointLightShadowPass)
    {
        m_PointLightShadowPass->PreparePassData(render_resource);
    }
    if (m_ParticlePass)
    {
        m_ParticlePass->PreparePassData(render_resource);
    }
#if defined(Z_HAS_VULKAN)
    GET_SYSTEM(DebugDrawManager)->PreparePassData(render_resource);
#endif
}

void RenderPipelineBase::UpdateLumen(std::shared_ptr<RenderScene> render_scene,
                                     std::shared_ptr<RenderCamera> camera,
                                     float delta_time)
{
#if defined(Z_HAS_LUMEN)
    if (m_LumenPass == nullptr || render_scene == nullptr || camera == nullptr)
    {
        return;
    }

    // m_LumenPass 的静态类型是 RenderPassBase，因为槽位是给所有 Pass 共用的。
    // 用 dynamic_cast 而不是在基类里放一个 Lumen 指针：剥掉 Lumen 的配置里
    // LumenRenderPass 这个类根本不存在。
    auto* lumen_pass = dynamic_cast<LumenRenderPass*>(m_LumenPass.get());
    if (lumen_pass == nullptr)
    {
        return;
    }

    lumen_pass->Render(render_scene, camera, delta_time);
#else
    (void)render_scene;
    (void)camera;
    (void)delta_time;
#endif
}

void RenderPipelineBase::UpdateNanite(std::shared_ptr<RenderScene> render_scene,
                                      std::shared_ptr<RenderCamera> camera,
                                      float delta_time)
{
    if (m_NanitePass == nullptr || render_scene == nullptr || camera == nullptr)
    {
        return;
    }

    // 槽位的静态类型是 RenderPassBase（所有 Pass 共用），用 dynamic_cast 而不是在基类
    // 里放一个 Nanite 指针：基类不该知道 Nanite 的存在。
    auto* nanite_pass = dynamic_cast<NaniteRenderPass*>(m_NanitePass.get());
    if (nanite_pass == nullptr)
    {
        return;
    }

    nanite_pass->Render(render_scene, camera, delta_time);
}

NaniteRenderPass* RenderPipelineBase::GetNanitePass() const
{
    if (m_NanitePass == nullptr)
    {
        return nullptr;
    }
    // 和 UpdateNanite 同一个约定：槽位的静态类型是 RenderPassBase，是不是 Nanite
    // 只能靠 dynamic_cast 现场判定（基类不该知道 Nanite 的存在）。
    return dynamic_cast<NaniteRenderPass*>(m_NanitePass.get());
}

void RenderPipelineBase::ConsumeParticleSwapData(RenderSwapData& swap_data, RenderSwapContext& swap_context)
{
    if (swap_data.m_ParticleSubmitRequest.has_value())
    {
        swap_context.ResetPartilceBatchSwapData();
    }
    if (swap_data.m_EmitterTickRequest.has_value())
    {
        swap_context.ResetEmitterTickSwapData();
    }
    if (swap_data.m_EmitterTransformRequest.has_value())
    {
        swap_context.ResetEmitterTransformSwapData();
    }
}

void RenderPipelineBase::BuildDrawLists(std::shared_ptr<RenderResourceBase> render_resource, RHIDrawList& out_draw_list)
{
    (void)render_resource;
    out_draw_list.Clear();
}

void RenderPipelineBase::SubmitDrawLists(RHI* rhi,
                                         std::shared_ptr<RenderResourceBase> render_resource,
                                         const RHIDrawList& draw_list)
{
    (void)rhi;
    (void)render_resource;
    draw_list.ExecuteAll();
}

void RenderPipelineBase::notifySkippedFrameRender() const
{
    for (const auto& callback : m_SkippedFrameCallbacks)
    {
        if (callback)
        {
            callback();
        }
    }
}

void RenderPipelineBase::DeferredRender(RHI* rhi, std::shared_ptr<RenderResourceBase> render_resource)
{
    RHIDrawList draw_list;
    BuildDrawLists(render_resource, draw_list);
    SubmitDrawLists(rhi, render_resource, draw_list);
}

void RenderPipelineBase::SetAxisVisibleState(bool state)
{
    m_IsShowAxis = state;
}

bool RenderPipelineBase::IsAxisVisibleState() const
{
    return m_IsShowAxis;
}

void RenderPipelineBase::SetSkyboxVisibleState(ViewportType viewport_type, bool state)
{
    const size_t index = static_cast<size_t>(viewport_type);
    if (index < m_IsShowSkybox.size())
    {
        m_IsShowSkybox[index] = state;
    }
}

bool RenderPipelineBase::IsSkyboxVisibleState(ViewportType viewport_type) const
{
    const size_t index = static_cast<size_t>(viewport_type);
    return index < m_IsShowSkybox.size() ? m_IsShowSkybox[index] : false;
}

void RenderPipelineBase::SetSelectedAxis(size_t selected_axis)
{
    m_SelectedAxis = selected_axis;
}

size_t RenderPipelineBase::GetSelectedAxis() const
{
    return m_SelectedAxis;
}

void RenderPipelineBase::InitializeUIRenderBackend(WindowUI* window_ui)
{
    if (m_UiPass == nullptr || window_ui == nullptr)
    {
        return;
    }

    m_UiPass->InitializeUIRenderBackend(window_ui);
}
