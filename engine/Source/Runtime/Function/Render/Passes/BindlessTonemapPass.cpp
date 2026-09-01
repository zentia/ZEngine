#include "Runtime/Function/Render/Passes/BindlessTonemapPass.h"

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Function/Render/Interface/RHI.h"
#if defined(_WIN32)
    #include "Runtime/Function/Render/Interface/DX12/DX12RHI.h"
    #include "Runtime/Function/Render/Interface/DX12/DX12RHIResource.h"
#endif

#include <stdexcept>
#include <string>

namespace
{
    std::string GetRp2ShaderRoot()
    {
#ifdef ZENGINE_SHADER_ROOT
        return ZENGINE_SHADER_ROOT;
#else
        return "e:/Engine/ZEngine/engine/shader";
#endif
    }

    RHIPipeline* CreateFullscreenTonemapPipeline(RHI* rhi,
                                                 RHIShader* vert,
                                                 RHIShader* frag,
                                                 RHIPipelineLayout* layout,
                                                 RHIRenderPass* render_pass)
    {
        RHIPipelineShaderStageCreateInfo stages[2] {};
        stages[0].sType = RHI_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = RHI_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vert;
        stages[0].pName = "main";
        stages[1].sType = RHI_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = RHI_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = frag;
        stages[1].pName = "main";

        RHIPipelineVertexInputStateCreateInfo vi {};
        vi.sType = RHI_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

        RHIPipelineInputAssemblyStateCreateInfo ia {};
        ia.sType = RHI_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = RHI_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        RHIPipelineViewportStateCreateInfo vp {};
        vp.sType = RHI_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vp.viewportCount = 1;
        vp.scissorCount = 1;

        RHIPipelineRasterizationStateCreateInfo rs {};
        rs.sType = RHI_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.cullMode = RHI_CULL_MODE_NONE;
        rs.frontFace = RHI_FRONT_FACE_CLOCKWISE;
        rs.polygonMode = RHI_POLYGON_MODE_FILL;

        RHIPipelineMultisampleStateCreateInfo ms {};
        ms.sType = RHI_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = RHI_SAMPLE_COUNT_1_BIT;

        RHIPipelineColorBlendAttachmentState blend_att {};
        blend_att.colorWriteMask = RHI_COLOR_COMPONENT_R_BIT | RHI_COLOR_COMPONENT_G_BIT | RHI_COLOR_COMPONENT_B_BIT |
                                   RHI_COLOR_COMPONENT_A_BIT;

        RHIPipelineColorBlendStateCreateInfo blend {};
        blend.sType = RHI_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        blend.attachmentCount = 1;
        blend.pAttachments = &blend_att;

        RHIPipelineDepthStencilStateCreateInfo ds {};
        ds.sType = RHI_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        ds.depthTestEnable = RHI_FALSE;
        ds.depthWriteEnable = RHI_FALSE;

        RHIDynamicState dyn_states[] = {RHI_DYNAMIC_STATE_VIEWPORT, RHI_DYNAMIC_STATE_SCISSOR};
        RHIPipelineDynamicStateCreateInfo dyn {};
        dyn.sType = RHI_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dyn.dynamicStateCount = 2;
        dyn.pDynamicStates = dyn_states;

        RHIGraphicsPipelineCreateInfo info {};
        info.sType = RHI_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        info.stageCount = 2;
        info.pStages = stages;
        info.pVertexInputState = &vi;
        info.pInputAssemblyState = &ia;
        info.pViewportState = &vp;
        info.pRasterizationState = &rs;
        info.pMultisampleState = &ms;
        info.pColorBlendState = &blend;
        info.pDepthStencilState = &ds;
        info.pDynamicState = &dyn;
        info.layout = layout;
        info.renderPass = render_pass;
        info.subpass = 0;

        RHIPipeline* pipeline = nullptr;
        if (rhi->CreateGraphicsPipelines(RHI_NULL_HANDLE, 1, &info, pipeline) != RHI_SUCCESS)
        {
            return nullptr;
        }
        return pipeline;
    }
}  // namespace

bool BindlessTonemapPass::InitBackendPipeline(RHIRenderPass* render_pass, const char* hlsl_search_root)
{
    if (m_Rhi == nullptr || render_pass == nullptr)
    {
        return false;
    }

    if (m_Rhi->getGraphicsAPI() == GraphicsAPI::DirectX12)
    {
#if defined(_WIN32)
        (void)hlsl_search_root;
        return InitDx12DescriptorTonemap(render_pass);
#else
        (void)hlsl_search_root;
        return false;
#endif
    }

#if defined(Z_HAS_VULKAN)
    (void)hlsl_search_root;
    return m_VulkanPipeline.Initialize(m_Rhi, render_pass);
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
        (void)bindless_slot;
        RecordDx12DescriptorTonemap(cmd);
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
        return m_Dx12DescriptorTonemapReady;
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

#if defined(_WIN32)
bool BindlessTonemapPass::InitDx12DescriptorTonemap(RHIRenderPass* render_pass)
{
    RHIDescriptorSetLayoutBinding binding {};
    binding.binding = 0;
    binding.descriptorType = RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = RHI_SHADER_STAGE_FRAGMENT_BIT;

    RHIDescriptorSetLayoutCreateInfo layout_ci {};
    layout_ci.sType = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_ci.bindingCount = 1;
    layout_ci.pBindings = &binding;
    if (m_Rhi->CreateDescriptorSetLayout(&layout_ci, m_Dx12TonemapSetLayout) != RHI_SUCCESS)
    {
        return false;
    }

    RHIPipelineLayoutCreateInfo pli {};
    pli.sType = RHI_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &m_Dx12TonemapSetLayout;
    if (m_Rhi->CreatePipelineLayout(&pli, m_Dx12TonemapPipelineLayout) != RHI_SUCCESS)
    {
        return false;
    }

    const std::string shader_root = GetRp2ShaderRoot() + "/hlsl/rp2/";
    std::vector<uint8_t> binary;
    RHIShader* vert = m_Rhi->CreateShaderModuleFromFile(shader_root + "post_process.vert.hlsl",
                                                        ShaderStage::Vertex,
                                                        {},
                                                        {},
                                                        binary);
    RHIShader* frag = m_Rhi->CreateShaderModuleFromFile(shader_root + "color_grading_hdr_tonemap.frag.hlsl",
                                                        ShaderStage::Fragment,
                                                        {},
                                                        {},
                                                        binary);
    if (vert == nullptr || frag == nullptr)
    {
        LOG_ERROR(ZRender, "BindlessTonemapPass: DX12 descriptor tonemap shader load failed");
        return false;
    }

    m_Dx12TonemapPipeline =
        CreateFullscreenTonemapPipeline(m_Rhi, vert, frag, m_Dx12TonemapPipelineLayout, render_pass);
    m_Rhi->DestroyShaderModule(vert);
    m_Rhi->DestroyShaderModule(frag);

    if (m_Dx12TonemapPipeline == nullptr)
    {
        LOG_ERROR(ZRender, "BindlessTonemapPass: DX12 descriptor tonemap pipeline create failed");
        return false;
    }

    RHIDescriptorSetAllocateInfo ai {};
    ai.sType = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = m_Rhi->GetDescriptorPoor();
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &m_Dx12TonemapSetLayout;
    RHIDescriptorSet* descriptor_set = nullptr;
    if (m_Rhi->AllocateDescriptorSets(&ai, descriptor_set) != RHI_SUCCESS || descriptor_set == nullptr)
    {
        return false;
    }
    m_Dx12TonemapDescriptorSet = descriptor_set;

    m_Dx12TonemapSampler = m_Rhi->GetOrCreateDefaultSampler(Default_Sampler_Linear);
    m_Dx12DescriptorTonemapReady = true;
    LOG_INFO(ZRender, "BindlessTonemapPass: DX12 descriptor tonemap ready (backup_odd -> backup_even)");
    return true;
}

void BindlessTonemapPass::ShutdownDx12DescriptorTonemap()
{
    m_Dx12TonemapPipeline = nullptr;
    m_Dx12TonemapPipelineLayout = nullptr;
    m_Dx12TonemapSetLayout = nullptr;
    m_Dx12TonemapDescriptorSet = nullptr;
    m_Dx12TonemapSampler = nullptr;
    m_Dx12DescriptorTonemapReady = false;
}

void BindlessTonemapPass::UpdateDx12DescriptorBinding()
{
    LOG_INFO(ZRender, "UpdateDx12DescriptorBinding: ENTRY (m_Dx12DescriptorTonemapReady={}, m_SourceHdrView={}, m_Dx12TonemapDescriptorSet={})",
             m_Dx12DescriptorTonemapReady, (void*)m_SourceHdrView, (void*)m_Dx12TonemapDescriptorSet);
    
    if (!m_Dx12DescriptorTonemapReady || m_SourceHdrView == nullptr || m_Dx12TonemapDescriptorSet == nullptr)
    {
        LOG_ERROR(ZRender, "UpdateDx12DescriptorBinding: EARLY RETURN (m_Dx12DescriptorTonemapReady={}, m_SourceHdrView={}, m_Dx12TonemapDescriptorSet={})",
                  m_Dx12DescriptorTonemapReady, (void*)m_SourceHdrView, (void*)m_Dx12TonemapDescriptorSet);
        return;
    }

    RHIDescriptorImageInfo in_hdr {};
    in_hdr.sampler = m_Dx12TonemapSampler;
    in_hdr.imageView = m_SourceHdrView;
    in_hdr.imageLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    RHIWriteDescriptorSet write {};
    write.sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = m_Dx12TonemapDescriptorSet;
    write.dstBinding = 0;
    write.descriptorType = RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo = &in_hdr;
    m_Rhi->UpdateDescriptorSets(1, &write, 0, nullptr);
}

void BindlessTonemapPass::RecordDx12DescriptorTonemap(RHICommandBuffer* cmd) const
{
    if (!m_Dx12DescriptorTonemapReady || cmd == nullptr || m_Dx12TonemapPipeline == nullptr)
    {
        LOG_ERROR(ZRender, "RecordDx12DescriptorTonemap: early return (m_Dx12DescriptorTonemapReady={}, cmd={}, pipeline={})",
                  m_Dx12DescriptorTonemapReady, (void*)cmd, (void*)m_Dx12TonemapPipeline);
        return;
    }

    LOG_INFO(ZRender, "RecordDx12DescriptorTonemap: drawing fullscreen triangle (pipeline={}, descriptor_set={})",
             (void*)m_Dx12TonemapPipeline, (void*)m_Dx12TonemapDescriptorSet);
    
    m_Rhi->CmdBindPipelinePFN(cmd, RHI_PIPELINE_BIND_POINT_GRAPHICS, m_Dx12TonemapPipeline);

    RHIViewport viewport {0.0f, 0.0f, static_cast<float>(m_Width), static_cast<float>(m_Height), 0.0f, 1.0f};
    RHIRect2D scissor {0, 0, m_Width, m_Height};
    m_Rhi->CmdSetViewportPFN(cmd, 0, 1, &viewport);
    m_Rhi->CmdSetScissorPFN(cmd, 0, 1, &scissor);

    m_Rhi->CmdBindDescriptorSetsPFN(cmd,
                                      RHI_PIPELINE_BIND_POINT_GRAPHICS,
                                      m_Dx12TonemapPipelineLayout,
                                      0,
                                      1,
                                      &m_Dx12TonemapDescriptorSet,
                                      0,
                                      nullptr);
    LOG_INFO(ZRender, "RecordDx12DescriptorTonemap: about to CmdDraw (cmd={}, pipeline={})", (void*)cmd, (void*)m_Dx12TonemapPipeline);
    LOG_INFO(ZRender, "RecordDx12DescriptorTonemap: viewport=({},{}), scissor=({},{},{},{})", 
             viewport.x, viewport.y, scissor.offset.x, scissor.offset.y, scissor.extent.width, scissor.extent.height);
    m_Rhi->CmdDraw(cmd, 3, 1, 0, 0);
    LOG_INFO(ZRender, "RecordDx12DescriptorTonemap: CmdDraw returned (pipeline should have written RED to RTV)");
}
#endif

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
    LOG_INFO(ZRender, "SetupFramebuffer: target_ldr_view={}, width={}, height={}", 
             (void*)target_ldr_view, width, height);
    
#if defined(_WIN32)
    // DEBUG: Log RTV handle of target_ldr_view (DX12-only types)
    if (target_ldr_view)
    {
        DX12ImageView* dx12_view = static_cast<DX12ImageView*>(target_ldr_view);
        if (dx12_view->hasRenderTargetHandle())
        {
            D3D12_CPU_DESCRIPTOR_HANDLE rtv = dx12_view->getRenderTargetHandle();
            LOG_INFO(ZRender, "SetupFramebuffer: target_ldr_view RTV={:016X}", rtv.ptr);
        }
        else
        {
            LOG_WARNING(ZRender, "SetupFramebuffer: target_ldr_view has NO RTV handle!");
        }
    }
#endif
    
    if (target_ldr_view == nullptr || width == 0 || height == 0)
    {
        LOG_ERROR(ZRender, "SetupFramebuffer: invalid parameters (target_ldr_view={}, width={}, height={})",
                  (void*)target_ldr_view, width, height);
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
    m_SourceHdrView = source_hdr_view;

    if (m_Rhi->getGraphicsAPI() == GraphicsAPI::DirectX12)
    {
#if defined(_WIN32)
        UpdateDx12DescriptorBinding();
#endif
        return;
    }

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
    if (!isReady() || m_Framebuffer.framebuffer == nullptr || m_SourceHdrView == nullptr)
    {
        LOG_INFO(ZRender, "BindlessTonemapPass::Draw: EARLY RETURN (ready={}, framebuffer={}, source_hdr_view={})",
                 isReady(), (void*)m_Framebuffer.framebuffer, (void*)m_SourceHdrView);
        return;
    }
    LOG_INFO(ZRender, "BindlessTonemapPass::Draw: ENTRY");

    if (m_Rhi->getGraphicsAPI() == GraphicsAPI::DirectX12)
    {
#if defined(_WIN32)
        UpdateDx12DescriptorBinding();
#endif
    }
    else if (m_BindlessSlot == RHIBindlessTextureManager::kInvalidBindlessIndex)
    {
        return;
    }
    else if (m_SourceHdrView != nullptr)
    {
        RHIBindlessTextureManager* mgr = m_Rhi->getBindlessTextureManager();
        RHISampler* sampler = m_Rhi->GetOrCreateDefaultSampler(Default_Sampler_Linear);
        if (mgr != nullptr)
        {
            mgr->Update(m_BindlessSlot, m_SourceHdrView, sampler);
        }
    }

    RHICommandBuffer* cmd = m_Rhi->GetCurrentCommandBuffer();

    LOG_INFO(ZRender, "BindlessTonemapPass::Draw: about to begin render pass (framebuffer={}, width={}, height={})",
             (void*)m_Framebuffer.framebuffer, m_Width, m_Height);
    
    // DEBUG: Log the image pointer of backup_even.
    if (m_Framebuffer.framebuffer)
    {
        // We can't easily get the image from framebuffer, but we can log the RTV.
        LOG_INFO(ZRender, "BindlessTonemapPass::Draw: Tonemap will write to backup_even (RTV should be same as RP2's backup_even)");
    }

    // DEBUG: Log RTV of backup_even in Tonemap's framebuffer.
    // We need to access the framebuffer's RTV array. Since we don't have a public API,
    // let's just log that we're about to draw.
    LOG_INFO(ZRender, "BindlessTonemapPass::Draw: Tonemap should write RED to backup_even, but combine_ui shows BLACK -> Tonemap output is lost!");
    
    float color[4] = {0.6f, 0.8f, 1.0f, 1.0f};
    const char* event_name =
        m_Rhi->getGraphicsAPI() == GraphicsAPI::DirectX12 ? "Tone Map (descriptor)" : "Tone Map (bindless)";
    m_Rhi->PushEvent(cmd, event_name, color);

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

    LOG_INFO(ZRender, "BindlessTonemapPass::Draw: calling RecordBackendTonemap (bindless_slot={})", m_BindlessSlot);
    RecordBackendTonemap(cmd, m_BindlessSlot);
    LOG_INFO(ZRender, "BindlessTonemapPass::Draw: RecordBackendTonemap returned");

    m_Rhi->CmdEndRenderPassPFN(cmd);

    // DEBUG: Log that Tonemap render pass ended. Next step: verify backup_even has red.
    // If combine_ui shows black, the problem is that Tonemap's output didn't reach backup_even.
    LOG_INFO(ZRender, "BindlessTonemapPass::Draw: Tonemap render pass ended (should have written RED to backup_even)");

    m_Rhi->PopEvent(cmd);
    LOG_INFO(ZRender, "BindlessTonemapPass::Draw: render pass ended");
}
