#include "Runtime/Function/Render/RenderPipelineBase.h"

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Function/Render/RenderSwapContext.h"
#include "Runtime/Function/Render/RenderingThread/RenderThreadChecks.h"
#include "Runtime/Profiler/Profiler.h"
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

void RenderPipelineBase::SubmitDrawLists(std::shared_ptr<RHI> rhi,
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

void RenderPipelineBase::DeferredRender(std::shared_ptr<RHI> rhi, std::shared_ptr<RenderResourceBase> render_resource)
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
