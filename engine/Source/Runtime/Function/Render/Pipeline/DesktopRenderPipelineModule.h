#pragma once

#include "Runtime/Function/Render/Pipeline/RenderPipelineModule.h"

#include "Runtime/Function/Render/AppleRp2Chain.h"
#include "Runtime/Function/Render/AppleSsaoPass.h"
#include "Runtime/Function/Render/Passes/MainCameraFramebufferResources.h"

#include <memory>

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
    void SubmitDrawLists(RHI* rhi,
                         std::shared_ptr<RenderResourceBase> render_resource,
                         const RHIDrawList& draw_list) override;
    void UpdateAfterRecreate() override;

    uint32_t GetGuidOfPickedMesh(const Vector2& picked_uv) override;
    RHIRenderPass* GetUIRenderPass() const override;
    RHIImageView* GetUiLayerColorView() const override;
    RHIImageView* GetSceneColorView() const override;
    bool IsSceneColorTonemapped() const override;
    SsaoFrameStats GetAmbientOcclusionStats() const override;
    void FinishDx12ShadowPassDescriptorSetup() override;
    void ConsumeParticleSwapData(RenderSwapData& swap_data, RenderSwapContext& swap_context) override;

#if defined(__APPLE__)
    // Apple(Metal) 上主相机的 attachment / render pass 由模块自己持有，再用
    // SetFramebufferResources 借给 MainCameraRp1Pass。
    //
    // 不能像 DX12 那样塞进 DX12MainCameraPass —— 那个 TU 拉 d3d12.h，Apple 编不到；
    // 也不能像 Vulkan 那样塞进 MainCameraPass —— 那个 TU 拉 vulkan/vulkan.h。
    // MainCameraRp1Pass 本身是平台无关的（三个独立 pass、无 subpass，正好是 Metal
    // 的口味），所以 Apple 直接用它，只是 framebuffer 得有人替它养着。
    std::unique_ptr<MainCameraFramebufferResources> m_AppleMainCameraFramebuffers;

    // Apple 上的 RP2（离屏后处理链）。和上面那一套 attachment 配套：它只借用，
    // 不拥有。见 AppleRp2Chain.h 里"为什么不是复用 MainCameraRp2Pass"。
    std::unique_ptr<AppleRp2Chain> m_AppleRp2;

    // Apple 上的 SSAO（GBuffer 之后、延迟光照之前）。模块拥有，RP1 只借用
    // （SetAmbientOcclusionPass）。见 AppleSsaoPass.h。
    std::unique_ptr<AppleSsaoPass> m_AppleSsao;
#endif
};
