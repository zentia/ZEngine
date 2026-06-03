#pragma once

#include "Runtime/Function/Render/Pipeline/RenderPipelineModule.h"

// Desktop render path: the existing deferred + forward assembly (DX12 RP1/RP2 +
// BindlessTonemap on Windows; MainCameraPass + ColorGrading/FXAA/UI/Combine/Pick/
// Particle/Lumen on Vulkan). This is a verbatim relocation of the former
// RenderPipeline bodies -- output is pixel-identical, just owned by a swappable
// module instead of hardcoded on the pipeline. Each pass keeps its own internal
// getGraphicsAPI()==DirectX12 / #if Z_HAS_VULKAN branch.
class DesktopRenderPipelineModule : public RenderPipelineModule
{
public:
    using RenderPipelineModule::RenderPipelineModule;

    const char* GetName() const override { return "Desktop"; }

    void Setup(const RenderPipelineInitInfo& init_info) override;
    void Shutdown() override;
    void BuildDrawLists(std::shared_ptr<RenderResourceBase> render_resource, RHIDrawList& out_draw_list) override;
    void SubmitDrawLists(std::shared_ptr<RHI> rhi,
                         std::shared_ptr<RenderResourceBase> render_resource,
                         const RHIDrawList& draw_list) override;
    void UpdateAfterRecreate() override;

    uint32_t GetGuidOfPickedMesh(const Vector2& picked_uv) override;
    RHIRenderPass* GetUIRenderPass() const override;
    RHIImageView* GetUiLayerColorView() const override;
    void FinishDx12ShadowPassDescriptorSetup() override;
    void ConsumeParticleSwapData(RenderSwapData& swap_data, RenderSwapContext& swap_context) override;
};
