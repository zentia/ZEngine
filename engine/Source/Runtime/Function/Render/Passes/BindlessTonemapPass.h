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

    bool isReady() const;

private:
    void SetupRenderPass(RHIFormat target_format);
    void SetupFramebuffer(RHIImageView* target_ldr_view, uint32_t width, uint32_t height);
    void DestroyFramebuffer();

    bool InitBackendPipeline(RHIRenderPass* render_pass, const char* hlsl_search_root);
    void RecordBackendTonemap(RHICommandBuffer* cmd, uint32_t bindless_slot) const;

    uint32_t m_BindlessSlot {0xFFFFFFFFu};

#if defined(Z_HAS_VULKAN)
    VulkanBindlessTonemapPipeline m_VulkanPipeline;
#endif
#if defined(_WIN32)
    DX12BindlessTonemapPipeline m_Dx12Pipeline;
#endif

    uint32_t m_Width {0};
    uint32_t m_Height {0};
};
