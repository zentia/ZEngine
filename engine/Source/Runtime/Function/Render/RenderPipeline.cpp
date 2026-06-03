#include "Runtime/Function/Render/RenderPipeline.h"

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Function/Render/Pipeline/DesktopRenderPipelineModule.h"
#include "Runtime/Function/Render/Pipeline/MobileRenderPipelineModule.h"
#include "Runtime/Function/Render/RenderingThread/RHIDrawList.h"
#include "Runtime/Function/Render/RenderingThread/RenderThreadChecks.h"
#include "Runtime/Profiler/Profiler.h"

#include <string>

void RenderPipeline::Initialize(RenderPipelineInitInfo init_info)
{
    // 保存 render_resource 供 Editor 使用
    m_RenderResource = init_info.render_resource;
    // Stored so SetActiveModule can re-Setup a freshly-constructed module at runtime.
    m_InitInfo = init_info;

    // The effective render path (Desktop on PC, Mobile on phone, or the r.RenderPath
    // override) owns the pass assembly. Milestone 1 always resolves to Desktop until
    // Phase C wires the Mobile module.
    m_ActiveModule = CreateModule(RenderPipelineSettings::GetEffectivePath());
    m_ActiveModule->Setup(m_InitInfo);
}

std::unique_ptr<RenderPipelineModule> RenderPipeline::CreateModule(RenderPipelineSettings::RenderPath path)
{
    using RenderPipelineSettings::RenderPath;
    if (path == RenderPath::Mobile)
    {
        return std::make_unique<MobileRenderPipelineModule>(*this);
    }
    return std::make_unique<DesktopRenderPipelineModule>(*this);
}

void RenderPipeline::SetActiveModule(RenderPipelineSettings::RenderPath path)
{
    using RenderPipelineSettings::RenderPath;
    const char* target = (path == RenderPath::Mobile) ? "Mobile" : "Desktop";
    if (m_ActiveModule && std::string(m_ActiveModule->GetName()) == target)
    {
        return;  // already on this path -- skip the teardown/rebuild
    }

    if (m_ActiveModule)
    {
        m_ActiveModule->Shutdown();
        m_ActiveModule.reset();
    }
    m_ActiveModule = CreateModule(path);
    m_ActiveModule->Setup(m_InitInfo);
    LOG_INFO(ZRender, "RenderPipeline: active render path -> {}", m_ActiveModule->GetName());
}

void RenderPipeline::BuildDrawLists(std::shared_ptr<RenderResourceBase> render_resource, RHIDrawList& out_draw_list)
{
    CHECK_RENDER_THREAD();
    Z_PROFILE_SCOPE("RenderPipeline::BuildDrawLists");
    if (m_ActiveModule)
    {
        m_ActiveModule->BuildDrawLists(render_resource, out_draw_list);
    }
    else
    {
        out_draw_list.Clear();
    }
}

void RenderPipeline::SubmitDrawLists(std::shared_ptr<RHI> rhi,
                                     std::shared_ptr<RenderResourceBase> render_resource,
                                     const RHIDrawList& draw_list)
{
    CHECK_RHI_THREAD();
    Z_PROFILE_SCOPE("RenderPipeline::SubmitDrawLists");
    if (m_ActiveModule)
    {
        m_ActiveModule->SubmitDrawLists(rhi, render_resource, draw_list);
    }
    else
    {
        draw_list.ExecuteAll();
    }
}

void RenderPipeline::DeferredRender(std::shared_ptr<RHI> rhi, std::shared_ptr<RenderResourceBase> render_resource)
{
    RHIDrawList draw_list;
    BuildDrawLists(render_resource, draw_list);
    SubmitDrawLists(rhi, render_resource, draw_list);
}

void RenderPipeline::passUpdateAfterRecreateSwapchain()
{
    if (m_ActiveModule)
    {
        m_ActiveModule->UpdateAfterRecreate();
    }
}

uint32_t RenderPipeline::GetGuidOfPickedMesh(const Vector2& picked_uv)
{
    return m_ActiveModule ? m_ActiveModule->GetGuidOfPickedMesh(picked_uv) : 0;
}

RHIRenderPass* RenderPipeline::GetUIRenderPass() const
{
    return m_ActiveModule ? m_ActiveModule->GetUIRenderPass() : nullptr;
}

RHIImageView* RenderPipeline::GetUiLayerColorView() const
{
    return m_ActiveModule ? m_ActiveModule->GetUiLayerColorView() : nullptr;
}

void RenderPipeline::FinishDx12ShadowPassDescriptorSetup()
{
    if (m_ActiveModule)
    {
        m_ActiveModule->FinishDx12ShadowPassDescriptorSetup();
    }
}

void RenderPipeline::ConsumeParticleSwapData(RenderSwapData& swap_data, RenderSwapContext& swap_context)
{
    if (m_ActiveModule)
    {
        m_ActiveModule->ConsumeParticleSwapData(swap_data, swap_context);
    }
    else
    {
        RenderPipelineBase::ConsumeParticleSwapData(swap_data, swap_context);
    }
}
