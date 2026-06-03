// DX-B1: RHI-only MainCamera RP1/RP2 attachments + framebuffers (backend agnostic).
// Vulkan MainCameraPass will eventually delegate here; DX12 uses this in DX-B1.

#pragma once

#include "Runtime/Function/Render/RenderPass.h"

#include <cstdint>
#include <vector>

class RHI;

struct MainCameraPassInitInfo : RenderPassInitInfo
{
    bool enble_fxaa {false};
};

class MainCameraFramebufferResources
{
public:
    bool Initialize(RHI* rhi, bool enable_fxaa);
    void Shutdown();

    // Swapchain resize: rebuild images + framebuffers (render passes are kept).
    void UpdateAfterFramebufferRecreate();

    RHIRenderPass* getRP1RenderPass() const { return m_Rp1RenderPass; }
    RHIRenderPass* getRP2RenderPass() const { return m_Framebuffer.render_pass; }
    RHIFramebuffer* getRP1Framebuffer() const { return m_Rp1Framebuffer; }
    const std::vector<RHIFramebuffer*>& getRP2Framebuffers() const { return m_SwapchainFramebuffers; }

    const std::vector<RenderPass::FrameBufferAttachment>& getAttachments() const
    {
        return m_Framebuffer.attachments;
    }

    RHIImageView* getAttachmentView(uint32_t attachment_index) const;

private:
    void SetupAttachments();
    void SetupRenderPass1();
    void SetupRenderPass2();
    void SetupFramebuffers();
    void DestroyAttachments();
    void DestroyFramebuffers();

    RHI* m_Rhi {nullptr};
    bool m_EnableFxaa {false};
    bool m_Initialized {false};

    // Color/post attachments (indices match render_pass.h _main_camera_pass_*).
    RenderPass::Framebuffer m_Framebuffer;

    RHIRenderPass* m_Rp1RenderPass {nullptr};
    RHIFramebuffer* m_Rp1Framebuffer {nullptr};
    std::vector<RHIFramebuffer*> m_SwapchainFramebuffers;
};
