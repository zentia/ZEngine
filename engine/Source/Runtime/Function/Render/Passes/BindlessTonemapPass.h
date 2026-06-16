// =====================================================================
// BindlessTonemapPass (Vulkan PR-V4 + DX12 PR-DX2 / DX-B4)
// ---------------------------------------------------------------------
// Standalone render pass between MainCamera RP1 and RP2. Samples
// backup_odd (HDR) via bindless, writes backup_even (LDR).
// =====================================================================

#pragma once

#include "Runtime/Function/Render/RenderPass.h"

#include <cstdint>

#if defined(Z_HAS_VULKAN)
    #include "Runtime/Function/Render/Interface/Vulkan/Utility/BindlessTonemapPipeline.h"
#endif
#if defined(_WIN32)
    #include "Runtime/Function/Render/Interface/DX12/Utility/BindlessTonemapPipeline.h"
#endif

struct BindlessTonemapPassInitInfo : RenderPassInitInfo
{
    RHIImageView* source_hdr_view {nullptr};
    RHIImageView* target_ldr_view {nullptr};
    RHIFormat target_ldr_format {RHI_FORMAT_R16G16B16A16_SFLOAT};
    uint32_t width {0};
    uint32_t height {0};
    // Optional: directory containing bindless_blit_vs.hlsl + bindless_tonemap_ps.hlsl (DX12).
    const char* hlsl_search_root {nullptr};
};

class BindlessTonemapPass : public RenderPass
{
public:
    void Initialize(const RenderPassInitInfo* init_info) override final;
    void Draw() override final;

    void UpdateAfterFramebufferRecreate(RHIImageView* source_hdr_view,
                                        RHIImageView* target_ldr_view,
                                        uint32_t width,
                                        uint32_t height);

    RHIRenderPass* GetRenderPass() const { return m_Framebuffer.render_pass; }
    RHIFramebuffer* GetFramebuffer() const { return m_Framebuffer.framebuffer; }
    RHIImageView* GetSourceHdrView() const { return m_SourceHdrView; }

    bool isReady() const;

private:
    void SetupRenderPass(RHIFormat target_format);
    void SetupFramebuffer(RHIImageView* target_ldr_view, uint32_t width, uint32_t height);
    void DestroyFramebuffer();

    bool InitBackendPipeline(RHIRenderPass* render_pass, const char* hlsl_search_root);
    void RecordBackendTonemap(RHICommandBuffer* cmd, uint32_t bindless_slot) const;

#if defined(_WIN32)
    bool InitDx12DescriptorTonemap(RHIRenderPass* render_pass);
    void ShutdownDx12DescriptorTonemap();
    void UpdateDx12DescriptorBinding();
    void RecordDx12DescriptorTonemap(RHICommandBuffer* cmd) const;
#endif

    uint32_t m_BindlessSlot {0xFFFFFFFFu};
    RHIImageView* m_SourceHdrView {nullptr};

#if defined(Z_HAS_VULKAN)
    VulkanBindlessTonemapPipeline m_VulkanPipeline;
#endif
#if defined(_WIN32)
    DX12BindlessTonemapPipeline m_Dx12Pipeline;
    bool m_Dx12DescriptorTonemapReady {false};
    RHIDescriptorSetLayout* m_Dx12TonemapSetLayout {nullptr};
    RHIPipelineLayout* m_Dx12TonemapPipelineLayout {nullptr};
    RHIPipeline* m_Dx12TonemapPipeline {nullptr};
    RHIDescriptorSet* m_Dx12TonemapDescriptorSet {nullptr};
    RHISampler* m_Dx12TonemapSampler {nullptr};
#endif

    uint32_t m_Width {0};
    uint32_t m_Height {0};
};
