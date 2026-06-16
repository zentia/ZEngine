// DX-B1: RHI-only MainCamera RP1/RP2 attachments + framebuffers (backend agnostic).
// Vulkan MainCameraPass will eventually delegate here; DX12 uses this in DX-B1.

#pragma once

#include "Runtime/Function/Render/RenderPass.h"

#include <cstdint>
#include <vector>

class RHI;

struct MainCameraPassInitInfo : RenderPassInitInfo
{
    bool enable_fxaa {false};
};

class MainCameraFramebufferResources
{
public:
    bool Initialize(RHI* rhi, bool enable_fxaa);
    void Shutdown();

    // Swapchain resize: rebuild images + framebuffers (render passes are kept).
    void UpdateAfterFramebufferRecreate();

    // RP1 – Simplified: 3 independent render passes (no subpasses).
    // G-Buffer Pass: writes GBufferA, GBufferB, GBufferC, Depth.
    RHIRenderPass* getGBufferRenderPass() const { return m_GBufferRenderPass; }
    RHIFramebuffer* getGBufferFramebuffer() const { return m_GBufferFramebuffer; }

    // Deferred Lighting Pass: reads G-Buffer, writes BackupOdd (includes sky).
    RHIRenderPass* getDeferredLightingRenderPass() const { return m_DeferredLightingRenderPass; }
    RHIFramebuffer* getDeferredLightingFramebuffer() const { return m_DeferredLightingFramebuffer; }

    // Forward Lighting Pass: reads/writes BackupOdd (transparent objects).
    RHIRenderPass* getForwardLightingRenderPass() const { return m_ForwardLightingRenderPass; }
    RHIFramebuffer* getForwardLightingFramebuffer() const { return m_ForwardLightingFramebuffer; }

    // RP2 – UE-style: separate simple render passes (no subpasses).
    // HDR pass: for color_grading / fxaa (backup_odd/backup_even, R16G16B16A16_SFLOAT).
    // LDR pass: for combine_ui (swapchain, R8G8B8A8_UNORM).
    RHIRenderPass* getRp2HdrRenderPass() const { return m_Rp2HdrRenderPass; }
    RHIRenderPass* getRp2LdrRenderPass() const { return m_Rp2LdrRenderPass; }

    // RP2 framebuffers (one per post-process step).
    RHIFramebuffer* getRp2ColorGradingFramebuffer() const { return m_Rp2ColorGradingFramebuffer; }
    RHIFramebuffer* getRp2FxaaFramebuffer() const { return m_Rp2FxaaFramebuffer; }
    const std::vector<RHIFramebuffer*>& getRP2Framebuffers() const { return m_SwapchainFramebuffers; }

    const std::vector<RenderPass::FrameBufferAttachment>& getAttachments() const
    {
        return m_Framebuffer.attachments;
    }

    RHIImageView* getAttachmentView(uint32_t attachment_index) const;
    RHIImage* getAttachmentImage(uint32_t attachment_index) const;

private:
    void SetupAttachments();
    void SetupGBufferPass();
    void SetupDeferredLightingPass();
    void SetupForwardLightingPass();
    void SetupRenderPass2();  // RP2 remains unchanged (HDR/LDR passes).
    void SetupFramebuffers();
    void DestroyAttachments();
    void DestroyFramebuffers();

    RHI* m_Rhi {nullptr};
    bool m_EnableFxaa {false};
    bool m_Initialized {false};

    // Color/post attachments (indices match render_pass.h _main_camera_pass_*).
    // Stores all attachment images/views for RP1 and RP2.
    RenderPass::Framebuffer m_Framebuffer;

    // RP1 – Simplified: 3 independent render passes (no subpasses).
    // G-Buffer Pass: writes GBufferA, GBufferB, GBufferC, Depth.
    RHIRenderPass* m_GBufferRenderPass {nullptr};
    RHIFramebuffer* m_GBufferFramebuffer {nullptr};

    // Deferred Lighting Pass: reads G-Buffer, writes BackupOdd (includes sky).
    RHIRenderPass* m_DeferredLightingRenderPass {nullptr};
    RHIFramebuffer* m_DeferredLightingFramebuffer {nullptr};

    // Forward Lighting Pass: reads/writes BackupOdd (transparent objects).
    RHIRenderPass* m_ForwardLightingRenderPass {nullptr};
    RHIFramebuffer* m_ForwardLightingFramebuffer {nullptr};

    // RP2 – UE-style: separate simple render passes (no subpasses).
    RHIRenderPass* m_Rp2HdrRenderPass {nullptr};  // For color_grading/fxaa (HDR format R16G16B16A16_SFLOAT)
    RHIRenderPass* m_Rp2LdrRenderPass {nullptr};  // For combine_ui (LDR/swapchain format)
    RHIFramebuffer* m_Rp2ColorGradingFramebuffer {nullptr};  // Writes backup_odd (or post_odd)
    RHIFramebuffer* m_Rp2FxaaFramebuffer {nullptr};          // Writes backup_even
    std::vector<RHIFramebuffer*> m_SwapchainFramebuffers;     // One per swapchain image (combine_ui writes to swapchain)
};
