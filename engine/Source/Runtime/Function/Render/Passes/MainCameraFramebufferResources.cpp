// DX-B1: MainCamera framebuffer + render-pass scaffolding (RHI-only).

#include "Runtime/Function/Render/Passes/MainCameraFramebufferResources.h"

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Function/Render/Interface/RHI.h"
#include "Runtime/Function/Render/RenderPass.h"

#include <stdexcept>

bool MainCameraFramebufferResources::Initialize(RHI* rhi, bool enable_fxaa)
{
    Shutdown();

    m_Rhi = rhi;
    m_EnableFxaa = enable_fxaa;
    if (m_Rhi == nullptr)
    {
        return false;
    }

    SetupAttachments();
    SetupGBufferPass();
    SetupDeferredLightingPass();
    SetupForwardLightingPass();
    SetupRenderPass2();
    SetupFramebuffers();

    m_Initialized = true;
    LOG_INFO(ZRender,
             "MainCameraFramebufferResources: simplified RP1 (3 independent passes) + RP2 ready ({}x{}, fxaa={})",
             m_Rhi->GetSwapchainInfo().extent.width,
             m_Rhi->GetSwapchainInfo().extent.height,
             m_EnableFxaa ? "on" : "off");
    return true;
}

void MainCameraFramebufferResources::Shutdown()
{
    DestroyFramebuffers();
    DestroyAttachments();

    // Destroy RP1 render passes (simplified: 3 independent passes).
    if (m_GBufferRenderPass != nullptr && m_Rhi != nullptr)
    {
        m_Rhi->DestroyRenderPass(m_GBufferRenderPass);
        m_GBufferRenderPass = nullptr;
    }
    if (m_DeferredLightingRenderPass != nullptr && m_Rhi != nullptr)
    {
        m_Rhi->DestroyRenderPass(m_DeferredLightingRenderPass);
        m_DeferredLightingRenderPass = nullptr;
    }
    if (m_ForwardLightingRenderPass != nullptr && m_Rhi != nullptr)
    {
        m_Rhi->DestroyRenderPass(m_ForwardLightingRenderPass);
        m_ForwardLightingRenderPass = nullptr;
    }

    // Destroy RP2 render passes.
    if (m_Rp2HdrRenderPass != nullptr && m_Rhi != nullptr)
    {
        m_Rhi->DestroyRenderPass(m_Rp2HdrRenderPass);
        m_Rp2HdrRenderPass = nullptr;
    }
    if (m_Rp2LdrRenderPass != nullptr && m_Rhi != nullptr)
    {
        m_Rhi->DestroyRenderPass(m_Rp2LdrRenderPass);
        m_Rp2LdrRenderPass = nullptr;
    }

    m_Rhi = nullptr;
    m_Initialized = false;
}

void MainCameraFramebufferResources::UpdateAfterFramebufferRecreate()
{
    if (!m_Initialized || m_Rhi == nullptr)
    {
        return;
    }

    DestroyFramebuffers();
    DestroyAttachments();
    SetupAttachments();
    SetupFramebuffers();

    LOG_INFO(ZRender,
             "MainCameraFramebufferResources: recreated FBs ({}x{})",
             m_Rhi->GetSwapchainInfo().extent.width,
             m_Rhi->GetSwapchainInfo().extent.height);
}

RHIImageView* MainCameraFramebufferResources::getAttachmentView(uint32_t attachment_index) const
{
    if (attachment_index >= m_Framebuffer.attachments.size())
    {
        return nullptr;
    }
    return m_Framebuffer.attachments[attachment_index].view;
}

RHIImage* MainCameraFramebufferResources::getAttachmentImage(uint32_t attachment_index) const
{
    if (attachment_index >= m_Framebuffer.attachments.size())
    {
        return nullptr;
    }
    return m_Framebuffer.attachments[attachment_index].image;
}

void MainCameraFramebufferResources::DestroyAttachments()
{
    if (m_Rhi == nullptr)
    {
        m_Framebuffer.attachments.clear();
        return;
    }

    for (auto& attachment : m_Framebuffer.attachments)
    {
        if (attachment.image != nullptr)
        {
            m_Rhi->DestroyImage(attachment.image);
            attachment.image = nullptr;
        }
        if (attachment.view != nullptr)
        {
            m_Rhi->DestroyImageView(attachment.view);
            attachment.view = nullptr;
        }
        if (attachment.mem != nullptr)
        {
            m_Rhi->FreeMemory(attachment.mem);
            attachment.mem = nullptr;
        }
    }
}

void MainCameraFramebufferResources::DestroyFramebuffers()
{
    if (m_Rhi == nullptr)
    {
        m_GBufferFramebuffer = nullptr;
        m_DeferredLightingFramebuffer = nullptr;
        m_ForwardLightingFramebuffer = nullptr;
        m_Rp2ColorGradingFramebuffer = nullptr;
        m_Rp2FxaaFramebuffer = nullptr;
        m_SwapchainFramebuffers.clear();
        return;
    }

    // Destroy RP1 framebuffers (simplified: 3 independent framebuffers).
    if (m_GBufferFramebuffer != nullptr)
    {
        m_Rhi->DestroyFramebuffer(m_GBufferFramebuffer);
        m_GBufferFramebuffer = nullptr;
    }

    if (m_DeferredLightingFramebuffer != nullptr)
    {
        m_Rhi->DestroyFramebuffer(m_DeferredLightingFramebuffer);
        m_DeferredLightingFramebuffer = nullptr;
    }

    if (m_ForwardLightingFramebuffer != nullptr)
    {
        m_Rhi->DestroyFramebuffer(m_ForwardLightingFramebuffer);
        m_ForwardLightingFramebuffer = nullptr;
    }

    // Destroy RP2 framebuffers.
    if (m_Rp2ColorGradingFramebuffer != nullptr)
    {
        m_Rhi->DestroyFramebuffer(m_Rp2ColorGradingFramebuffer);
        m_Rp2ColorGradingFramebuffer = nullptr;
    }

    if (m_Rp2FxaaFramebuffer != nullptr)
    {
        m_Rhi->DestroyFramebuffer(m_Rp2FxaaFramebuffer);
        m_Rp2FxaaFramebuffer = nullptr;
    }

    for (RHIFramebuffer* framebuffer : m_SwapchainFramebuffers)
    {
        if (framebuffer != nullptr)
        {
            m_Rhi->DestroyFramebuffer(framebuffer);
        }
    }
    m_SwapchainFramebuffers.clear();
}

void MainCameraFramebufferResources::SetupAttachments()
{
    m_Framebuffer.attachments.resize(_main_camera_pass_custom_attachment_count +
                                     _main_camera_pass_post_process_attachment_count);

    m_Framebuffer.attachments[_main_camera_pass_gbuffer_a].format = RHI_FORMAT_R8G8B8A8_UNORM;
    m_Framebuffer.attachments[_main_camera_pass_gbuffer_b].format = RHI_FORMAT_R8G8B8A8_UNORM;
    m_Framebuffer.attachments[_main_camera_pass_gbuffer_c].format = RHI_FORMAT_R8G8B8A8_SRGB;
    m_Framebuffer.attachments[_main_camera_pass_backup_buffer_odd].format = RHI_FORMAT_R16G16B16A16_SFLOAT;
    m_Framebuffer.attachments[_main_camera_pass_backup_buffer_even].format = RHI_FORMAT_R16G16B16A16_SFLOAT;

    for (int buffer_index = 0; buffer_index < _main_camera_pass_custom_attachment_count; ++buffer_index)
    {
        if (buffer_index == _main_camera_pass_gbuffer_a)
        {
            m_Rhi->CreateImage(m_Rhi->GetSwapchainInfo().extent.width,
                               m_Rhi->GetSwapchainInfo().extent.height,
                               m_Framebuffer.attachments[_main_camera_pass_gbuffer_a].format,
                               RHI_IMAGE_TILING_OPTIMAL,
                               RHI_IMAGE_USAGE_INPUT_ATTACHMENT_BIT | RHI_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                   RHI_IMAGE_USAGE_TRANSFER_SRC_BIT,
                               RHI_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                               m_Framebuffer.attachments[_main_camera_pass_gbuffer_a].image,
                               m_Framebuffer.attachments[_main_camera_pass_gbuffer_a].mem,
                               0,
                               1,
                               1);
        }
        else
        {
            // backup_odd / backup_even: written in RP1 deferred, sampled by bindless tonemap
            // and RP2 color grading / combine. Must not use TRANSIENT (content must survive
            // RP1 -> tonemap -> RP2) and must include SAMPLED (DX12 bindless + post passes).
            RHIImageUsageFlags usage =
                RHI_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | RHI_IMAGE_USAGE_INPUT_ATTACHMENT_BIT |
                RHI_IMAGE_USAGE_SAMPLED_BIT | RHI_IMAGE_USAGE_TRANSFER_SRC_BIT;
            if (buffer_index != _main_camera_pass_backup_buffer_odd &&
                buffer_index != _main_camera_pass_backup_buffer_even)
            {
                usage |= RHI_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
            }

            m_Rhi->CreateImage(m_Rhi->GetSwapchainInfo().extent.width,
                               m_Rhi->GetSwapchainInfo().extent.height,
                               m_Framebuffer.attachments[buffer_index].format,
                               RHI_IMAGE_TILING_OPTIMAL,
                               usage,
                               RHI_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                               m_Framebuffer.attachments[buffer_index].image,
                               m_Framebuffer.attachments[buffer_index].mem,
                               0,
                               1,
                               1);
        }

        m_Rhi->CreateImageView(m_Framebuffer.attachments[buffer_index].image,
                               m_Framebuffer.attachments[buffer_index].format,
                               RHI_IMAGE_ASPECT_COLOR_BIT,
                               RHI_IMAGE_VIEW_TYPE_2D,
                               1,
                               1,
                               m_Framebuffer.attachments[buffer_index].view);
    }

    m_Framebuffer.attachments[_main_camera_pass_post_process_buffer_odd].format = RHI_FORMAT_R16G16B16A16_SFLOAT;
    m_Framebuffer.attachments[_main_camera_pass_post_process_buffer_even].format = RHI_FORMAT_R16G16B16A16_SFLOAT;
    for (int attachment_index = _main_camera_pass_custom_attachment_count;
         attachment_index < _main_camera_pass_custom_attachment_count + _main_camera_pass_post_process_attachment_count;
         ++attachment_index)
    {
        m_Rhi->CreateImage(m_Rhi->GetSwapchainInfo().extent.width,
                           m_Rhi->GetSwapchainInfo().extent.height,
                           m_Framebuffer.attachments[attachment_index].format,
                           RHI_IMAGE_TILING_OPTIMAL,
                           RHI_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | RHI_IMAGE_USAGE_INPUT_ATTACHMENT_BIT |
                               RHI_IMAGE_USAGE_SAMPLED_BIT,
                           RHI_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                           m_Framebuffer.attachments[attachment_index].image,
                           m_Framebuffer.attachments[attachment_index].mem,
                           0,
                           1,
                           1);

        m_Rhi->CreateImageView(m_Framebuffer.attachments[attachment_index].image,
                               m_Framebuffer.attachments[attachment_index].format,
                               RHI_IMAGE_ASPECT_COLOR_BIT,
                               RHI_IMAGE_VIEW_TYPE_2D,
                               1,
                               1,
                               m_Framebuffer.attachments[attachment_index].view);
    }
}

void MainCameraFramebufferResources::SetupGBufferPass()
{
    // Simplified: single-pass G-Buffer render pass (no subpasses).
    // Writes GBufferA, GBufferB, GBufferC, Depth.
    // Final layouts: G-Buffer → SHADER_READ_ONLY_OPTIMAL, Depth → DEPTH_STENCIL_READ_ONLY_OPTIMAL.

    enum
    {
        kAttachment_GBufferA = 0,
        kAttachment_GBufferB = 1,
        kAttachment_GBufferC = 2,
        kAttachment_Depth = 3,
        kAttachmentCount = 4,
    };

    RHIAttachmentDescription attachments[kAttachmentCount] = {};

    // GBufferA
    attachments[kAttachment_GBufferA].format = m_Framebuffer.attachments[_main_camera_pass_gbuffer_a].format;
    attachments[kAttachment_GBufferA].samples = RHI_SAMPLE_COUNT_1_BIT;
    attachments[kAttachment_GBufferA].loadOp = RHI_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[kAttachment_GBufferA].storeOp = RHI_ATTACHMENT_STORE_OP_STORE;
    attachments[kAttachment_GBufferA].stencilLoadOp = RHI_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[kAttachment_GBufferA].stencilStoreOp = RHI_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[kAttachment_GBufferA].initialLayout = RHI_IMAGE_LAYOUT_UNDEFINED;
    attachments[kAttachment_GBufferA].finalLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    // GBufferB
    attachments[kAttachment_GBufferB].format = m_Framebuffer.attachments[_main_camera_pass_gbuffer_b].format;
    attachments[kAttachment_GBufferB].samples = RHI_SAMPLE_COUNT_1_BIT;
    attachments[kAttachment_GBufferB].loadOp = RHI_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[kAttachment_GBufferB].storeOp = RHI_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[kAttachment_GBufferB].stencilLoadOp = RHI_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[kAttachment_GBufferB].stencilStoreOp = RHI_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[kAttachment_GBufferB].initialLayout = RHI_IMAGE_LAYOUT_UNDEFINED;
    attachments[kAttachment_GBufferB].finalLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    // GBufferC
    attachments[kAttachment_GBufferC].format = m_Framebuffer.attachments[_main_camera_pass_gbuffer_c].format;
    attachments[kAttachment_GBufferC].samples = RHI_SAMPLE_COUNT_1_BIT;
    attachments[kAttachment_GBufferC].loadOp = RHI_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[kAttachment_GBufferC].storeOp = RHI_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[kAttachment_GBufferC].stencilLoadOp = RHI_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[kAttachment_GBufferC].stencilStoreOp = RHI_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[kAttachment_GBufferC].initialLayout = RHI_IMAGE_LAYOUT_UNDEFINED;
    attachments[kAttachment_GBufferC].finalLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    // Depth
    attachments[kAttachment_Depth].format = m_Rhi->GetDepthImageInfo().depth_image_format;
    attachments[kAttachment_Depth].samples = RHI_SAMPLE_COUNT_1_BIT;
    attachments[kAttachment_Depth].loadOp = RHI_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[kAttachment_Depth].storeOp = RHI_ATTACHMENT_STORE_OP_STORE;
    attachments[kAttachment_Depth].stencilLoadOp = RHI_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[kAttachment_Depth].stencilStoreOp = RHI_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[kAttachment_Depth].initialLayout = RHI_IMAGE_LAYOUT_UNDEFINED;
    attachments[kAttachment_Depth].finalLayout = RHI_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    // Single subpass: writes to GBufferA, GBufferB, GBufferC, Depth.
    RHIAttachmentReference color_refs[3] = {};
    color_refs[0].attachment = kAttachment_GBufferA;
    color_refs[0].layout = RHI_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color_refs[1].attachment = kAttachment_GBufferB;
    color_refs[1].layout = RHI_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color_refs[2].attachment = kAttachment_GBufferC;
    color_refs[2].layout = RHI_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    RHIAttachmentReference depth_ref {};
    depth_ref.attachment = kAttachment_Depth;
    depth_ref.layout = RHI_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    RHISubpassDescription subpass {};
    subpass.pipelineBindPoint = RHI_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 3;
    subpass.pColorAttachments = color_refs;
    subpass.pDepthStencilAttachment = &depth_ref;

    RHISubpassDescription subpasses[1] = {subpass};

    RHIRenderPassCreateInfo ci {};
    ci.sType = RHI_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    ci.attachmentCount = kAttachmentCount;
    ci.pAttachments = attachments;
    ci.subpassCount = 1;
    ci.pSubpasses = subpasses;
    ci.dependencyCount = 0;
    ci.pDependencies = nullptr;

    if (m_Rhi->CreateRenderPass(&ci, m_GBufferRenderPass) != RHI_SUCCESS)
    {
        throw std::runtime_error("MainCameraFramebufferResources: create GBuffer render pass failed");
    }
}

void MainCameraFramebufferResources::SetupDeferredLightingPass()
{
    // Simplified: single-pass Deferred Lighting + Sky render pass (no subpasses).
    // Reads GBufferA, GBufferB, GBufferC, Depth (as input attachments).
    // Writes BackupOdd (HDR output).

    enum
    {
        kAttachment_GBufferA = 0,
        kAttachment_GBufferB = 1,
        kAttachment_GBufferC = 2,
        kAttachment_Depth = 3,
        kAttachment_BackupOdd = 4,
        kAttachmentCount = 5,
    };

    RHIAttachmentDescription attachments[kAttachmentCount] = {};

    // GBufferA (input only, LOAD)
    attachments[kAttachment_GBufferA].format = m_Framebuffer.attachments[_main_camera_pass_gbuffer_a].format;
    attachments[kAttachment_GBufferA].samples = RHI_SAMPLE_COUNT_1_BIT;
    attachments[kAttachment_GBufferA].loadOp = RHI_ATTACHMENT_LOAD_OP_LOAD;
    attachments[kAttachment_GBufferA].storeOp = RHI_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[kAttachment_GBufferA].stencilLoadOp = RHI_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[kAttachment_GBufferA].stencilStoreOp = RHI_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[kAttachment_GBufferA].initialLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    attachments[kAttachment_GBufferA].finalLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    // GBufferB (input only, LOAD)
    attachments[kAttachment_GBufferB].format = m_Framebuffer.attachments[_main_camera_pass_gbuffer_b].format;
    attachments[kAttachment_GBufferB].samples = RHI_SAMPLE_COUNT_1_BIT;
    attachments[kAttachment_GBufferB].loadOp = RHI_ATTACHMENT_LOAD_OP_LOAD;
    attachments[kAttachment_GBufferB].storeOp = RHI_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[kAttachment_GBufferB].stencilLoadOp = RHI_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[kAttachment_GBufferB].stencilStoreOp = RHI_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[kAttachment_GBufferB].initialLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    attachments[kAttachment_GBufferB].finalLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    // GBufferC (input only, LOAD)
    attachments[kAttachment_GBufferC].format = m_Framebuffer.attachments[_main_camera_pass_gbuffer_c].format;
    attachments[kAttachment_GBufferC].samples = RHI_SAMPLE_COUNT_1_BIT;
    attachments[kAttachment_GBufferC].loadOp = RHI_ATTACHMENT_LOAD_OP_LOAD;
    attachments[kAttachment_GBufferC].storeOp = RHI_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[kAttachment_GBufferC].stencilLoadOp = RHI_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[kAttachment_GBufferC].stencilStoreOp = RHI_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[kAttachment_GBufferC].initialLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    attachments[kAttachment_GBufferC].finalLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    // Depth (input only, LOAD)
    attachments[kAttachment_Depth].format = m_Rhi->GetDepthImageInfo().depth_image_format;
    attachments[kAttachment_Depth].samples = RHI_SAMPLE_COUNT_1_BIT;
    attachments[kAttachment_Depth].loadOp = RHI_ATTACHMENT_LOAD_OP_LOAD;
    attachments[kAttachment_Depth].storeOp = RHI_ATTACHMENT_STORE_OP_STORE;
    attachments[kAttachment_Depth].stencilLoadOp = RHI_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[kAttachment_Depth].stencilStoreOp = RHI_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[kAttachment_Depth].initialLayout = RHI_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    attachments[kAttachment_Depth].finalLayout = RHI_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    // BackupOdd (output, CLEAR)
    attachments[kAttachment_BackupOdd].format =
        m_Framebuffer.attachments[_main_camera_pass_backup_buffer_odd].format;
    attachments[kAttachment_BackupOdd].samples = RHI_SAMPLE_COUNT_1_BIT;
    attachments[kAttachment_BackupOdd].loadOp = RHI_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[kAttachment_BackupOdd].storeOp = RHI_ATTACHMENT_STORE_OP_STORE;
    attachments[kAttachment_BackupOdd].stencilLoadOp = RHI_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[kAttachment_BackupOdd].stencilStoreOp = RHI_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[kAttachment_BackupOdd].initialLayout = RHI_IMAGE_LAYOUT_UNDEFINED;
    attachments[kAttachment_BackupOdd].finalLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    // Single subpass: reads GBufferA/B/C + Depth as input, writes BackupOdd as color.
    // DX12 note: do NOT include Depth in input_refs. In DX12, a resource cannot
    // be both an SRV (for input attachment reads) and a DSV (for depth testing)
    // at the same time. The deferred lighting shader reads depth via SRV bound by
    // RefreshDeferredLightingInputAttachments (separate descriptor), not via
    // input attachments. Depth is only the depth stencil attachment here.
    const bool is_dx12 = (m_Rhi != nullptr && m_Rhi->getGraphicsAPI() == GraphicsAPI::DirectX12);

    RHIAttachmentReference input_refs[4] = {};
    input_refs[0].attachment = kAttachment_GBufferA;
    input_refs[0].layout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    input_refs[1].attachment = kAttachment_GBufferB;
    input_refs[1].layout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    input_refs[2].attachment = kAttachment_GBufferC;
    input_refs[2].layout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    uint32_t input_attachment_count = 3;
    if (!is_dx12)
    {
        // Vulkan: depth as input attachment is allowed and required by the render pass.
        input_refs[3].attachment = kAttachment_Depth;
        input_refs[3].layout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        input_attachment_count = 4;
    }

    RHIAttachmentReference color_refs[1] = {};
    color_refs[0].attachment = kAttachment_BackupOdd;
    color_refs[0].layout = RHI_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    RHIAttachmentReference depth_ref {};
    depth_ref.attachment = kAttachment_Depth;
    depth_ref.layout = RHI_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    RHISubpassDescription subpass {};
    subpass.pipelineBindPoint = RHI_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.inputAttachmentCount = input_attachment_count;
    subpass.pInputAttachments = input_refs;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = color_refs;
    subpass.pDepthStencilAttachment = &depth_ref;

    RHISubpassDescription subpasses[1] = {subpass};

    // Dependency: GBufferPass → DeferredLightingPass
    RHISubpassDependency dependency {};
    dependency.srcSubpass = RHI_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = RHI_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = RHI_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | RHI_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = RHI_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependency.dstAccessMask = RHI_ACCESS_SHADER_READ_BIT | RHI_ACCESS_COLOR_ATTACHMENT_READ_BIT;

    RHIRenderPassCreateInfo ci {};
    ci.sType = RHI_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    ci.attachmentCount = kAttachmentCount;
    ci.pAttachments = attachments;
    ci.subpassCount = 1;
    ci.pSubpasses = subpasses;
    ci.dependencyCount = 1;
    ci.pDependencies = &dependency;

    if (m_Rhi->CreateRenderPass(&ci, m_DeferredLightingRenderPass) != RHI_SUCCESS)
    {
        throw std::runtime_error("MainCameraFramebufferResources: create DeferredLighting render pass failed");
    }
}

void MainCameraFramebufferResources::SetupForwardLightingPass()
{
    // Simplified: single-pass Forward Lighting render pass (no subpasses).
    // Reads/writes BackupOdd (HDR, transparent objects).
    // Reads Depth (for depth testing).

    enum
    {
        kAttachment_BackupOdd = 0,
        kAttachment_Depth = 1,
        kAttachmentCount = 2,
    };

    RHIAttachmentDescription attachments[kAttachmentCount] = {};

    // BackupOdd (read/write, LOAD)
    attachments[kAttachment_BackupOdd].format =
        m_Framebuffer.attachments[_main_camera_pass_backup_buffer_odd].format;
    attachments[kAttachment_BackupOdd].samples = RHI_SAMPLE_COUNT_1_BIT;
    attachments[kAttachment_BackupOdd].loadOp = RHI_ATTACHMENT_LOAD_OP_LOAD;
    attachments[kAttachment_BackupOdd].storeOp = RHI_ATTACHMENT_STORE_OP_STORE;
    attachments[kAttachment_BackupOdd].stencilLoadOp = RHI_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[kAttachment_BackupOdd].stencilStoreOp = RHI_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[kAttachment_BackupOdd].initialLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    attachments[kAttachment_BackupOdd].finalLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    // Depth (read, LOAD)
    attachments[kAttachment_Depth].format = m_Rhi->GetDepthImageInfo().depth_image_format;
    attachments[kAttachment_Depth].samples = RHI_SAMPLE_COUNT_1_BIT;
    attachments[kAttachment_Depth].loadOp = RHI_ATTACHMENT_LOAD_OP_LOAD;
    attachments[kAttachment_Depth].storeOp = RHI_ATTACHMENT_STORE_OP_STORE;
    attachments[kAttachment_Depth].stencilLoadOp = RHI_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[kAttachment_Depth].stencilStoreOp = RHI_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[kAttachment_Depth].initialLayout = RHI_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    attachments[kAttachment_Depth].finalLayout = RHI_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    // Single subpass: reads/writes BackupOdd, reads Depth.
    RHIAttachmentReference color_refs[1] = {};
    color_refs[0].attachment = kAttachment_BackupOdd;
    color_refs[0].layout = RHI_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    RHIAttachmentReference depth_ref {};
    depth_ref.attachment = kAttachment_Depth;
    depth_ref.layout = RHI_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    RHISubpassDescription subpass {};
    subpass.pipelineBindPoint = RHI_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = color_refs;
    subpass.pDepthStencilAttachment = &depth_ref;

    RHISubpassDescription subpasses[1] = {subpass};

    // Dependency: DeferredLightingPass → ForwardLightingPass
    RHISubpassDependency dependency {};
    dependency.srcSubpass = RHI_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = RHI_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = RHI_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = RHI_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependency.dstAccessMask = RHI_ACCESS_COLOR_ATTACHMENT_READ_BIT;

    RHIRenderPassCreateInfo ci {};
    ci.sType = RHI_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    ci.attachmentCount = kAttachmentCount;
    ci.pAttachments = attachments;
    ci.subpassCount = 1;
    ci.pSubpasses = subpasses;
    ci.dependencyCount = 1;
    ci.pDependencies = &dependency;

    if (m_Rhi->CreateRenderPass(&ci, m_ForwardLightingRenderPass) != RHI_SUCCESS)
    {
        throw std::runtime_error("MainCameraFramebufferResources: create ForwardLighting render pass failed");
    }
}

void MainCameraFramebufferResources::SetupRenderPass2()
{
    // UE-style: create TWO simple render passes (no subpasses).
    //
    // HDR render pass:  for color_grading and fxaa.
    //   Attachment format = R16G16B16A16_SFLOAT (matches backup_odd/backup_even).
    // LDR render pass:  for combine_ui (writes to swapchain).
    //   Attachment format = swapchain image format (LDR).

    // ---- HDR render pass (color_grading / fxaa) ----
    // finalLayout = SHADER_READ_ONLY_OPTIMAL so that after the render pass ends,
    // the attachment is already in a shader-readable layout (no extra barrier needed).
    {
        RHIAttachmentDescription att {};
        att.format = RHI_FORMAT_R16G16B16A16_SFLOAT;  // matches backup_odd / backup_even
        att.samples = RHI_SAMPLE_COUNT_1_BIT;
        att.loadOp = RHI_ATTACHMENT_LOAD_OP_CLEAR;
        att.storeOp = RHI_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp = RHI_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = RHI_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout = RHI_IMAGE_LAYOUT_UNDEFINED;
        att.finalLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        // Simple subpass: one color attachment (attachment index 0).
        RHIAttachmentReference color_ref {};
        color_ref.attachment = 0;
        color_ref.layout = RHI_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        RHISubpassDescription subpass {};
        subpass.pipelineBindPoint = RHI_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &color_ref;

        RHISubpassDescription subpasses[1] = {subpass};

        RHIRenderPassCreateInfo ci {};
        ci.sType = RHI_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        ci.attachmentCount = 1;
        ci.pAttachments = &att;
        ci.subpassCount = 1;
        ci.pSubpasses = subpasses;
        ci.dependencyCount = 0;
        ci.pDependencies = nullptr;

        if (m_Rhi->CreateRenderPass(&ci, m_Rp2HdrRenderPass) != RHI_SUCCESS)
        {
            throw std::runtime_error("MainCameraFramebufferResources: create RP2 HDR render pass failed");
        }
    }

    // ---- LDR render pass (combine_ui) ----
    {
        RHIAttachmentDescription att {};
        att.format = m_Rhi->GetSwapchainInfo().image_format;  // LDR swapchain format
        att.samples = RHI_SAMPLE_COUNT_1_BIT;
        att.loadOp = RHI_ATTACHMENT_LOAD_OP_CLEAR;
        att.storeOp = RHI_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp = RHI_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = RHI_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout = RHI_IMAGE_LAYOUT_UNDEFINED;
        att.finalLayout = RHI_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        // Simple subpass: one color attachment (attachment index 0).
        RHIAttachmentReference color_ref {};
        color_ref.attachment = 0;
        color_ref.layout = RHI_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        RHISubpassDescription subpass {};
        subpass.pipelineBindPoint = RHI_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &color_ref;

        RHISubpassDescription subpasses[1] = {subpass};

        RHIRenderPassCreateInfo ci {};
        ci.sType = RHI_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        ci.attachmentCount = 1;
        ci.pAttachments = &att;
        ci.subpassCount = 1;
        ci.pSubpasses = subpasses;
        ci.dependencyCount = 0;
        ci.pDependencies = nullptr;

        if (m_Rhi->CreateRenderPass(&ci, m_Rp2LdrRenderPass) != RHI_SUCCESS)
        {
            throw std::runtime_error("MainCameraFramebufferResources: create RP2 LDR render pass failed");
        }
    }
}

void MainCameraFramebufferResources::SetupFramebuffers()
{
    // RP1 – Simplified: 3 independent framebuffers (no subpasses).

    // GBufferPass framebuffer (writes GBufferA, GBufferB, GBufferC, Depth)
    {
        RHIImageView* attachments[4] = {
            m_Framebuffer.attachments[_main_camera_pass_gbuffer_a].view,
            m_Framebuffer.attachments[_main_camera_pass_gbuffer_b].view,
            m_Framebuffer.attachments[_main_camera_pass_gbuffer_c].view,
            m_Rhi->GetDepthImageInfo().depth_image_view,
        };

        RHIFramebufferCreateInfo ci {};
        ci.sType = RHI_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        ci.renderPass = m_GBufferRenderPass;
        ci.attachmentCount = 4;
        ci.pAttachments = attachments;
        ci.width = m_Rhi->GetSwapchainInfo().extent.width;
        ci.height = m_Rhi->GetSwapchainInfo().extent.height;
        ci.layers = 1;

        if (m_Rhi->CreateFramebuffer(&ci, m_GBufferFramebuffer) != RHI_SUCCESS)
        {
            throw std::runtime_error("MainCameraFramebufferResources: create GBuffer framebuffer failed");
        }
    }

    // DeferredLightingPass framebuffer (reads GBufferA/B/C + Depth, writes BackupOdd)
    {
        RHIImageView* attachments[5] = {
            m_Framebuffer.attachments[_main_camera_pass_gbuffer_a].view,   // input
            m_Framebuffer.attachments[_main_camera_pass_gbuffer_b].view,   // input
            m_Framebuffer.attachments[_main_camera_pass_gbuffer_c].view,   // input
            m_Rhi->GetDepthImageInfo().depth_image_view,                   // input
            m_Framebuffer.attachments[_main_camera_pass_backup_buffer_odd].view,  // output
        };

        RHIFramebufferCreateInfo ci {};
        ci.sType = RHI_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        ci.renderPass = m_DeferredLightingRenderPass;
        ci.attachmentCount = 5;
        ci.pAttachments = attachments;
        ci.width = m_Rhi->GetSwapchainInfo().extent.width;
        ci.height = m_Rhi->GetSwapchainInfo().extent.height;
        ci.layers = 1;

        if (m_Rhi->CreateFramebuffer(&ci, m_DeferredLightingFramebuffer) != RHI_SUCCESS)
        {
            throw std::runtime_error("MainCameraFramebufferResources: create DeferredLighting framebuffer failed");
        }
    }

    // ForwardLightingPass framebuffer (reads/writes BackupOdd, reads Depth)
    {
        RHIImageView* attachments[2] = {
            m_Framebuffer.attachments[_main_camera_pass_backup_buffer_odd].view,  // read/write
            m_Rhi->GetDepthImageInfo().depth_image_view,                          // input
        };

        RHIFramebufferCreateInfo ci {};
        ci.sType = RHI_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        ci.renderPass = m_ForwardLightingRenderPass;
        ci.attachmentCount = 2;
        ci.pAttachments = attachments;
        ci.width = m_Rhi->GetSwapchainInfo().extent.width;
        ci.height = m_Rhi->GetSwapchainInfo().extent.height;
        ci.layers = 1;

        if (m_Rhi->CreateFramebuffer(&ci, m_ForwardLightingFramebuffer) != RHI_SUCCESS)
        {
            throw std::runtime_error("MainCameraFramebufferResources: create ForwardLighting framebuffer failed");
        }
    }

    // RP2 step 0: color_grading framebuffer
    // CRITICAL: SRV reads backup_odd (RP1 output: deferred + sky),
    //           RTV must write to a DIFFERENT texture to avoid read-write hazard
    //           with loadOp=CLEAR (which would zero backup_odd before shader samples it).
    // Output = backup_even (no FXAA) or post_odd (FXAA enabled).
    {
        uint32_t output_attachment =
            m_EnableFxaa ? _main_camera_pass_post_process_buffer_odd : _main_camera_pass_backup_buffer_even;
        RHIImageView* att[1] = { m_Framebuffer.attachments[output_attachment].view };

        RHIFramebufferCreateInfo fb_ci {};
        fb_ci.sType = RHI_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb_ci.renderPass = m_Rp2HdrRenderPass;  // HDR format render pass
        fb_ci.attachmentCount = 1;
        fb_ci.pAttachments = att;
        fb_ci.width = m_Rhi->GetSwapchainInfo().extent.width;
        fb_ci.height = m_Rhi->GetSwapchainInfo().extent.height;
        fb_ci.layers = 1;

        if (m_Rhi->CreateFramebuffer(&fb_ci, m_Rp2ColorGradingFramebuffer) != RHI_SUCCESS)
        {
            throw std::runtime_error("MainCameraFramebufferResources: create RP2 color_grading framebuffer failed");
        }
    }

    // RP2 step 1: fxaa framebuffer (output = backup_even, only if FXAA enabled)
    // FXAA also uses HDR render pass (reads post_odd, writes backup_even, both HDR format).
    if (m_EnableFxaa)
    {
        RHIImageView* att[1] = { m_Framebuffer.attachments[_main_camera_pass_backup_buffer_even].view };

        RHIFramebufferCreateInfo fb_ci {};
        fb_ci.sType = RHI_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb_ci.renderPass = m_Rp2HdrRenderPass;  // HDR format render pass
        fb_ci.attachmentCount = 1;
        fb_ci.pAttachments = att;
        fb_ci.width = m_Rhi->GetSwapchainInfo().extent.width;
        fb_ci.height = m_Rhi->GetSwapchainInfo().extent.height;
        fb_ci.layers = 1;

        if (m_Rhi->CreateFramebuffer(&fb_ci, m_Rp2FxaaFramebuffer) != RHI_SUCCESS)
        {
            throw std::runtime_error("MainCameraFramebufferResources: create RP2 fxaa framebuffer failed");
        }
    }

    // RP2 step 2: combine_ui framebuffers (one per swapchain image, output = swapchain)
    // Combine UI uses LDR render pass (swapchain format R8G8B8A8_UNORM).
    m_SwapchainFramebuffers.resize(m_Rhi->GetSwapchainInfo().imageViews.size());
    for (size_t i = 0; i < m_SwapchainFramebuffers.size(); ++i)
    {
        RHIImageView* att[1] = { m_Rhi->GetSwapchainInfo().imageViews[i] };

        RHIFramebufferCreateInfo fb_ci {};
        fb_ci.sType = RHI_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb_ci.renderPass = m_Rp2LdrRenderPass;  // LDR format render pass
        fb_ci.attachmentCount = 1;
        fb_ci.pAttachments = att;
        fb_ci.width = m_Rhi->GetSwapchainInfo().extent.width;
        fb_ci.height = m_Rhi->GetSwapchainInfo().extent.height;
        fb_ci.layers = 1;

        RHIFramebuffer* framebuffer = nullptr;
        if (m_Rhi->CreateFramebuffer(&fb_ci, framebuffer) != RHI_SUCCESS)
        {
            throw std::runtime_error("MainCameraFramebufferResources: create RP2 combine_ui framebuffer failed");
        }
        m_SwapchainFramebuffers[i] = framebuffer;
    }
}
