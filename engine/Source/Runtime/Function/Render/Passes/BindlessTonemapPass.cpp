#include "Runtime/Function/Render/Passes/BindlessTonemapPass.h"

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Function/Render/Interface/RHI.h"
#if defined(_WIN32)
    #include "Runtime/Function/Render/Interface/DX12/DX12RHI.h"
#endif

#include <stdexcept>
#include <string>

bool BindlessTonemapPass::InitBackendPipeline(RHIRenderPass* render_pass, const char* hlsl_search_root)
{
    if (m_Rhi == nullptr || render_pass == nullptr)
    {
        return false;
    }

    if (m_Rhi->getGraphicsAPI() == GraphicsAPI::DirectX12)
    {
#if defined(_WIN32)
        const std::string root = hlsl_search_root != nullptr ? hlsl_search_root : "";
        return m_Dx12Pipeline.Initialize(m_Rhi.get(), render_pass, root);
#else
        (void)hlsl_search_root;
        return false;
#endif
    }

#if defined(Z_HAS_VULKAN)
    (void)hlsl_search_root;
    return m_VulkanPipeline.Initialize(m_Rhi.get(), render_pass);
#else
    (void)hlsl_search_root;
    return false;
#endif
}

void BindlessTonemapPass::RecordBackendTonemap(RHICommandBuffer* cmd, uint32_t bindless_slot) const
{
    if (m_Rhi == nullptr || cmd == nullptr)
    {
        return;
    }

    if (m_Rhi->getGraphicsAPI() == GraphicsAPI::DirectX12)
    {
#if defined(_WIN32)
        if (auto* dx12_rhi = dynamic_cast<DX12RHI*>(m_Rhi.get()))
        {
            dx12_rhi->SetBindlessDescriptorHeaps();
        }
        m_Dx12Pipeline.RecordTonemap(cmd, m_Width, m_Height, bindless_slot, BindlessBlitSampler::LinearClamp);
#endif
        return;
    }

#if defined(Z_HAS_VULKAN)
    m_VulkanPipeline.RecordTonemap(cmd, m_Width, m_Height, bindless_slot, 0);
#endif
}

bool BindlessTonemapPass::isReady() const
{
    if (m_Rhi == nullptr)
    {
        return false;
    }
    if (m_Rhi->getGraphicsAPI() == GraphicsAPI::DirectX12)
    {
#if defined(_WIN32)
        return m_Dx12Pipeline.isReady();
#else
        return false;
#endif
    }
#if defined(Z_HAS_VULKAN)
    return m_VulkanPipeline.isReady();
#else
    return false;
#endif
}

void BindlessTonemapPass::Initialize(const RenderPassInitInfo* init_info)
{
    RenderPass::Initialize(nullptr);

    const BindlessTonemapPassInitInfo* info = static_cast<const BindlessTonemapPassInitInfo*>(init_info);
    if (info == nullptr)
    {
        LOG_ERROR(ZRender, "BindlessTonemapPass::initialize: init_info is null");
        return;
    }

    m_Width = info->width;
    m_Height = info->height;

    SetupRenderPass(info->target_ldr_format);

    if (!InitBackendPipeline(m_Framebuffer.render_pass, info->hlsl_search_root))
    {
        LOG_ERROR(ZRender, "BindlessTonemapPass::initialize: backend tonemap pipeline failed");
        return;
    }

    SetupFramebuffer(info->target_ldr_view, info->width, info->height);

    UpdateAfterFramebufferRecreate(info->source_hdr_view, info->target_ldr_view, info->width, info->height);
}

void BindlessTonemapPass::SetupRenderPass(RHIFormat target_format)
{
    RHIAttachmentDescription color_attachment {};
    color_attachment.format = target_format;
    color_attachment.samples = RHI_SAMPLE_COUNT_1_BIT;
    color_attachment.loadOp = RHI_ATTACHMENT_LOAD_OP_CLEAR;
    color_attachment.storeOp = RHI_ATTACHMENT_STORE_OP_STORE;
    color_attachment.stencilLoadOp = RHI_ATTACHMENT_LOAD_OP_DONT_CARE;
    color_attachment.stencilStoreOp = RHI_ATTACHMENT_STORE_OP_DONT_CARE;
    color_attachment.initialLayout = RHI_IMAGE_LAYOUT_UNDEFINED;
    color_attachment.finalLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    RHIAttachmentReference color_ref {};
    color_ref.attachment = 0;
    color_ref.layout = RHI_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    RHISubpassDescription subpass {};
    subpass.pipelineBindPoint = RHI_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;

    RHISubpassDependency external_in {};
    external_in.srcSubpass = RHI_SUBPASS_EXTERNAL;
    external_in.dstSubpass = 0;
    external_in.srcStageMask = RHI_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    external_in.dstStageMask = RHI_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    external_in.srcAccessMask = RHI_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    external_in.dstAccessMask = RHI_ACCESS_SHADER_READ_BIT;

    RHISubpassDependency external_out {};
    external_out.srcSubpass = 0;
    external_out.dstSubpass = RHI_SUBPASS_EXTERNAL;
    external_out.srcStageMask = RHI_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    external_out.dstStageMask = RHI_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    external_out.srcAccessMask = RHI_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    external_out.dstAccessMask = RHI_ACCESS_SHADER_READ_BIT;

    RHISubpassDependency deps[2] = {external_in, external_out};

    RHIRenderPassCreateInfo rp_ci {};
    rp_ci.sType = RHI_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp_ci.attachmentCount = 1;
    rp_ci.pAttachments = &color_attachment;
    rp_ci.subpassCount = 1;
    rp_ci.pSubpasses = &subpass;
    rp_ci.dependencyCount = 2;
    rp_ci.pDependencies = deps;

    if (m_Rhi->CreateRenderPass(&rp_ci, m_Framebuffer.render_pass) != RHI_SUCCESS)
    {
        throw std::runtime_error("BindlessTonemapPass: createRenderPass failed");
    }
}

void BindlessTonemapPass::SetupFramebuffer(RHIImageView* target_ldr_view, uint32_t width, uint32_t height)
{
    if (target_ldr_view == nullptr || width == 0 || height == 0)
    {
        return;
    }

    RHIImageView* attachments[1] = {target_ldr_view};

    RHIFramebufferCreateInfo fb_ci {};
    fb_ci.sType = RHI_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fb_ci.renderPass = m_Framebuffer.render_pass;
    fb_ci.attachmentCount = 1;
    fb_ci.pAttachments = attachments;
    fb_ci.width = width;
    fb_ci.height = height;
    fb_ci.layers = 1;

    RHIFramebuffer* framebuffer = nullptr;
    if (m_Rhi->CreateFramebuffer(&fb_ci, framebuffer) != RHI_SUCCESS || framebuffer == nullptr)
    {
        throw std::runtime_error("BindlessTonemapPass: createFramebuffer failed");
    }

    m_Framebuffer.framebuffer = framebuffer;
    m_Framebuffer.width = static_cast<int>(width);
    m_Framebuffer.height = static_cast<int>(height);
}

void BindlessTonemapPass::DestroyFramebuffer()
{
    if (m_Framebuffer.framebuffer != nullptr)
    {
        m_Rhi->DestroyFramebuffer(m_Framebuffer.framebuffer);
        m_Framebuffer.framebuffer = nullptr;
    }
}

void BindlessTonemapPass::UpdateAfterFramebufferRecreate(RHIImageView* source_hdr_view,
                                                         RHIImageView* target_ldr_view,
                                                         uint32_t width,
                                                         uint32_t height)
{
    DestroyFramebuffer();
    SetupFramebuffer(target_ldr_view, width, height);
    m_Width = width;
    m_Height = height;

    if (!m_Rhi->supportsBindlessTextures() || source_hdr_view == nullptr)
    {
        return;
    }

    RHIBindlessTextureManager* mgr = m_Rhi->getBindlessTextureManager();
    if (mgr == nullptr)
    {
        return;
    }

    RHISampler* sampler = m_Rhi->GetOrCreateDefaultSampler(Default_Sampler_Linear);

    if (m_BindlessSlot == RHIBindlessTextureManager::kInvalidBindlessIndex)
    {
        m_BindlessSlot = mgr->allocate(source_hdr_view, sampler);
        if (m_BindlessSlot == RHIBindlessTextureManager::kInvalidBindlessIndex)
        {
            LOG_ERROR(ZRender, "BindlessTonemapPass::updateAfterFramebufferRecreate: bindless allocate failed");
            return;
        }
        LOG_INFO(ZRender, "BindlessTonemapPass: allocated bindless slot {}", m_BindlessSlot);
    }
    else
    {
        mgr->Update(m_BindlessSlot, source_hdr_view, sampler);
    }
}

void BindlessTonemapPass::Draw()
{
    if (!isReady() || m_Framebuffer.framebuffer == nullptr)
    {
        return;
    }
    if (m_BindlessSlot == RHIBindlessTextureManager::kInvalidBindlessIndex)
    {
        return;
    }

    RHICommandBuffer* cmd = m_Rhi->GetCurrentCommandBuffer();

    float color[4] = {0.6f, 0.8f, 1.0f, 1.0f};
    m_Rhi->PushEvent(cmd, "Tone Map (bindless)", color);

    RHIRenderPassBeginInfo bi {};
    bi.sType = RHI_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    bi.renderPass = m_Framebuffer.render_pass;
    bi.framebuffer = m_Framebuffer.framebuffer;
    bi.renderArea.offset = {0, 0};
    bi.renderArea.extent.width = m_Width;
    bi.renderArea.extent.height = m_Height;

    RHIClearValue clear_value {};
    clear_value.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    bi.clearValueCount = 1;
    bi.pClearValues = &clear_value;

    m_Rhi->CmdBeginRenderPassPFN(cmd, &bi, RHI_SUBPASS_CONTENTS_INLINE);

    RecordBackendTonemap(cmd, m_BindlessSlot);

    m_Rhi->CmdEndRenderPassPFN(cmd);

    m_Rhi->PopEvent(cmd);
}
