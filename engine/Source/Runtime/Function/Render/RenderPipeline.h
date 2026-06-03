#pragma once

#include "Runtime/Function/Render/Pipeline/RenderPipelineModule.h"
#include "Runtime/Function/Render/Pipeline/RenderPipelineSettings.h"
#include "Runtime/Function/Render/RenderPipelineBase.h"

#include <memory>

// Thin dispatcher over a swappable RenderPipelineModule (the active render path).
// Initialize creates the path's module and Setup()s it; every per-frame entry
// point (Build/Submit/DeferredRender/recreate/pick/UI accessors) delegates to
// m_ActiveModule. Milestone 1 always selects the Desktop module; Phase B adds
// RenderPath selection + runtime SetActiveModule.
class RenderPipeline : public RenderPipelineBase
{
public:
    virtual void Initialize(RenderPipelineInitInfo init_info) override final;

    void BuildDrawLists(std::shared_ptr<RenderResourceBase> render_resource, RHIDrawList& out_draw_list) override final;
    void SubmitDrawLists(RHI* rhi,
                         std::shared_ptr<RenderResourceBase> render_resource,
                         const RHIDrawList& draw_list) override final;

    virtual void DeferredRender(RHI* rhi,
                                std::shared_ptr<RenderResourceBase> render_resource) override final;

    void ConsumeParticleSwapData(RenderSwapData& swap_data, RenderSwapContext& swap_context) override;

    // DX12: shadow-pass descriptor tables bind after the upload ringbuffer exists.
    void FinishDx12ShadowPassDescriptorSetup();

    void passUpdateAfterRecreateSwapchain();

    virtual uint32_t GetGuidOfPickedMesh(const Vector2& picked_uv) override final;

    // 获取 UI subpass 使用的 render pass（供 Editor 创建 UI Pass 使用）
    virtual RHIRenderPass* GetUIRenderPass() const override final;

    virtual RHIImageView* GetUiLayerColorView() const override final;

    // Swap the active render path at runtime. MUST be called with the GPU idle and
    // no in-flight frame referencing the current module (the RenderSystem drives this
    // after FlushRenderingCommands, on the RHI thread). No-op if already on `path`.
    void SetActiveModule(RenderPipelineSettings::RenderPath path);

    // Name of the active path ("Desktop"/"Mobile"), for logging / editor display.
    const char* GetActiveModuleName() const { return m_ActiveModule ? m_ActiveModule->GetName() : "<none>"; }

private:
    std::unique_ptr<RenderPipelineModule> CreateModule(RenderPipelineSettings::RenderPath path);

    std::unique_ptr<RenderPipelineModule> m_ActiveModule;
    RenderPipelineInitInfo m_InitInfo;
};