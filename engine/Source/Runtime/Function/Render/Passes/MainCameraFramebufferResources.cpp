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
    SetupRenderPass1();
    SetupRenderPass2();
    SetupFramebuffers();

    m_Initialized = true;
    LOG_INFO(ZRender,
             "MainCameraFramebufferResources: RP1+RP2 ready ({}x{}, fxaa={}, swapchain_images={})",
             m_Rhi->GetSwapchainInfo().extent.width,
             m_Rhi->GetSwapchainInfo().extent.height,
             m_EnableFxaa ? "on" : "off",
             m_SwapchainFramebuffers.size());
    return true;
}

void MainCameraFramebufferResources::Shutdown()
{
    DestroyFramebuffers();
    DestroyAttachments();

    m_Rp1RenderPass = nullptr;
    m_Framebuffer.render_pass = nullptr;
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
        m_Rp1Framebuffer = nullptr;
        m_SwapchainFramebuffers.clear();
        return;
    }

    if (m_Rp1Framebuffer != nullptr)
    {
        m_Rhi->DestroyFramebuffer(m_Rp1Framebuffer);
        m_Rp1Framebuffer = nullptr;
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
            m_Rhi->CreateImage(m_Rhi->GetSwapchainInfo().extent.width,
                               m_Rhi->GetSwapchainInfo().extent.height,
                               m_Framebuffer.attachments[buffer_index].format,
                               RHI_IMAGE_TILING_OPTIMAL,
                               RHI_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | RHI_IMAGE_USAGE_INPUT_ATTACHMENT_BIT |
                                   RHI_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
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

void MainCameraFramebufferResources::SetupRenderPass1()
{
    enum
    {
        kRP1_GBufferA = 0,
        kRP1_GBufferB = 1,
        kRP1_GBufferC = 2,
        kRP1_BackupOdd = 3,
        kRP1_Depth = 4,
        kRP1_AttachmentCount = 5,
    };

    RHIAttachmentDescription attachments[kRP1_AttachmentCount] = {};

    RHIAttachmentDescription& gbuffer_normal_attachment_description = attachments[kRP1_GBufferA];
    gbuffer_normal_attachment_description.format = m_Framebuffer.attachments[_main_camera_pass_gbuffer_a].format;
    gbuffer_normal_attachment_description.samples = RHI_SAMPLE_COUNT_1_BIT;
    gbuffer_normal_attachment_description.loadOp = RHI_ATTACHMENT_LOAD_OP_CLEAR;
    gbuffer_normal_attachment_description.storeOp = RHI_ATTACHMENT_STORE_OP_STORE;
    gbuffer_normal_attachment_description.stencilLoadOp = RHI_ATTACHMENT_LOAD_OP_DONT_CARE;
    gbuffer_normal_attachment_description.stencilStoreOp = RHI_ATTACHMENT_STORE_OP_DONT_CARE;
    gbuffer_normal_attachment_description.initialLayout = RHI_IMAGE_LAYOUT_UNDEFINED;
    gbuffer_normal_attachment_description.finalLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    RHIAttachmentDescription& gbuffer_metallic_attachment_description = attachments[kRP1_GBufferB];
    gbuffer_metallic_attachment_description.format =
        m_Framebuffer.attachments[_main_camera_pass_gbuffer_b].format;
    gbuffer_metallic_attachment_description.samples = RHI_SAMPLE_COUNT_1_BIT;
    gbuffer_metallic_attachment_description.loadOp = RHI_ATTACHMENT_LOAD_OP_CLEAR;
    gbuffer_metallic_attachment_description.storeOp = RHI_ATTACHMENT_STORE_OP_DONT_CARE;
    gbuffer_metallic_attachment_description.stencilLoadOp = RHI_ATTACHMENT_LOAD_OP_DONT_CARE;
    gbuffer_metallic_attachment_description.stencilStoreOp = RHI_ATTACHMENT_STORE_OP_DONT_CARE;
    gbuffer_metallic_attachment_description.initialLayout = RHI_IMAGE_LAYOUT_UNDEFINED;
    gbuffer_metallic_attachment_description.finalLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    RHIAttachmentDescription& gbuffer_albedo_attachment_description = attachments[kRP1_GBufferC];
    gbuffer_albedo_attachment_description.format = m_Framebuffer.attachments[_main_camera_pass_gbuffer_c].format;
    gbuffer_albedo_attachment_description.samples = RHI_SAMPLE_COUNT_1_BIT;
    gbuffer_albedo_attachment_description.loadOp = RHI_ATTACHMENT_LOAD_OP_CLEAR;
    gbuffer_albedo_attachment_description.storeOp = RHI_ATTACHMENT_STORE_OP_DONT_CARE;
    gbuffer_albedo_attachment_description.stencilLoadOp = RHI_ATTACHMENT_LOAD_OP_DONT_CARE;
    gbuffer_albedo_attachment_description.stencilStoreOp = RHI_ATTACHMENT_STORE_OP_DONT_CARE;
    gbuffer_albedo_attachment_description.initialLayout = RHI_IMAGE_LAYOUT_UNDEFINED;
    gbuffer_albedo_attachment_description.finalLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    RHIAttachmentDescription& backup_odd_attachment_description = attachments[kRP1_BackupOdd];
    backup_odd_attachment_description.format =
        m_Framebuffer.attachments[_main_camera_pass_backup_buffer_odd].format;
    backup_odd_attachment_description.samples = RHI_SAMPLE_COUNT_1_BIT;
    backup_odd_attachment_description.loadOp = RHI_ATTACHMENT_LOAD_OP_CLEAR;
    backup_odd_attachment_description.storeOp = RHI_ATTACHMENT_STORE_OP_STORE;
    backup_odd_attachment_description.stencilLoadOp = RHI_ATTACHMENT_LOAD_OP_DONT_CARE;
    backup_odd_attachment_description.stencilStoreOp = RHI_ATTACHMENT_STORE_OP_DONT_CARE;
    backup_odd_attachment_description.initialLayout = RHI_IMAGE_LAYOUT_UNDEFINED;
    backup_odd_attachment_description.finalLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    RHIAttachmentDescription& depth_attachment_description = attachments[kRP1_Depth];
    depth_attachment_description.format = m_Rhi->GetDepthImageInfo().depth_image_format;
    depth_attachment_description.samples = RHI_SAMPLE_COUNT_1_BIT;
    depth_attachment_description.loadOp = RHI_ATTACHMENT_LOAD_OP_CLEAR;
    depth_attachment_description.storeOp = RHI_ATTACHMENT_STORE_OP_STORE;
    depth_attachment_description.stencilLoadOp = RHI_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth_attachment_description.stencilStoreOp = RHI_ATTACHMENT_STORE_OP_DONT_CARE;
    depth_attachment_description.initialLayout = RHI_IMAGE_LAYOUT_UNDEFINED;
    depth_attachment_description.finalLayout = RHI_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    RHISubpassDescription subpasses[_main_camera_rp1_subpass_count] = {};

    RHIAttachmentReference base_pass_color_attachments_reference[3] = {};
    base_pass_color_attachments_reference[0].attachment = kRP1_GBufferA;
    base_pass_color_attachments_reference[0].layout = RHI_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    base_pass_color_attachments_reference[1].attachment = kRP1_GBufferB;
    base_pass_color_attachments_reference[1].layout = RHI_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    base_pass_color_attachments_reference[2].attachment = kRP1_GBufferC;
    base_pass_color_attachments_reference[2].layout = RHI_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    RHIAttachmentReference base_pass_depth_attachment_reference {};
    base_pass_depth_attachment_reference.attachment = kRP1_Depth;
    base_pass_depth_attachment_reference.layout = RHI_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    RHISubpassDescription& base_pass = subpasses[_main_camera_subpass_basepass];
    base_pass.pipelineBindPoint = RHI_PIPELINE_BIND_POINT_GRAPHICS;
    base_pass.colorAttachmentCount =
        sizeof(base_pass_color_attachments_reference) / sizeof(base_pass_color_attachments_reference[0]);
    base_pass.pColorAttachments = base_pass_color_attachments_reference;
    base_pass.pDepthStencilAttachment = &base_pass_depth_attachment_reference;

    RHIAttachmentReference deferred_lighting_pass_input_attachments_reference[4] = {};
    deferred_lighting_pass_input_attachments_reference[0].attachment = kRP1_GBufferA;
    deferred_lighting_pass_input_attachments_reference[0].layout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    deferred_lighting_pass_input_attachments_reference[1].attachment = kRP1_GBufferB;
    deferred_lighting_pass_input_attachments_reference[1].layout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    deferred_lighting_pass_input_attachments_reference[2].attachment = kRP1_GBufferC;
    deferred_lighting_pass_input_attachments_reference[2].layout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    deferred_lighting_pass_input_attachments_reference[3].attachment = kRP1_Depth;
    deferred_lighting_pass_input_attachments_reference[3].layout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    RHIAttachmentReference deferred_lighting_pass_color_attachment_reference[1] = {};
    deferred_lighting_pass_color_attachment_reference[0].attachment = kRP1_BackupOdd;
    deferred_lighting_pass_color_attachment_reference[0].layout = RHI_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    RHISubpassDescription& deferred_lighting_pass = subpasses[_main_camera_subpass_deferred_lighting];
    deferred_lighting_pass.pipelineBindPoint = RHI_PIPELINE_BIND_POINT_GRAPHICS;
    deferred_lighting_pass.inputAttachmentCount = 4;
    deferred_lighting_pass.pInputAttachments = deferred_lighting_pass_input_attachments_reference;
    deferred_lighting_pass.colorAttachmentCount = 1;
    deferred_lighting_pass.pColorAttachments = deferred_lighting_pass_color_attachment_reference;

    RHIAttachmentReference forward_lighting_pass_color_attachments_reference[1] = {};
    forward_lighting_pass_color_attachments_reference[0].attachment = kRP1_BackupOdd;
    forward_lighting_pass_color_attachments_reference[0].layout = RHI_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    RHIAttachmentReference forward_lighting_pass_depth_attachment_reference {};
    forward_lighting_pass_depth_attachment_reference.attachment = kRP1_Depth;
    forward_lighting_pass_depth_attachment_reference.layout = RHI_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    RHISubpassDescription& forward_lighting_pass = subpasses[_main_camera_subpass_forward_lighting];
    forward_lighting_pass.pipelineBindPoint = RHI_PIPELINE_BIND_POINT_GRAPHICS;
    forward_lighting_pass.colorAttachmentCount = 1;
    forward_lighting_pass.pColorAttachments = forward_lighting_pass_color_attachments_reference;
    forward_lighting_pass.pDepthStencilAttachment = &forward_lighting_pass_depth_attachment_reference;

    RHISubpassDependency dependencies[3] = {};

    RHISubpassDependency& deferred_lighting_pass_depend_on_shadow_map_pass = dependencies[0];
    deferred_lighting_pass_depend_on_shadow_map_pass.srcSubpass = RHI_SUBPASS_EXTERNAL;
    deferred_lighting_pass_depend_on_shadow_map_pass.dstSubpass = _main_camera_subpass_deferred_lighting;
    deferred_lighting_pass_depend_on_shadow_map_pass.srcStageMask = RHI_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deferred_lighting_pass_depend_on_shadow_map_pass.dstStageMask = RHI_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deferred_lighting_pass_depend_on_shadow_map_pass.srcAccessMask = RHI_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deferred_lighting_pass_depend_on_shadow_map_pass.dstAccessMask = RHI_ACCESS_SHADER_READ_BIT;

    RHISubpassDependency& deferred_lighting_pass_depend_on_base_pass = dependencies[1];
    deferred_lighting_pass_depend_on_base_pass.srcSubpass = _main_camera_subpass_basepass;
    deferred_lighting_pass_depend_on_base_pass.dstSubpass = _main_camera_subpass_deferred_lighting;
    deferred_lighting_pass_depend_on_base_pass.srcStageMask =
        RHI_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | RHI_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deferred_lighting_pass_depend_on_base_pass.dstStageMask =
        RHI_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | RHI_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deferred_lighting_pass_depend_on_base_pass.srcAccessMask =
        RHI_ACCESS_SHADER_WRITE_BIT | RHI_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deferred_lighting_pass_depend_on_base_pass.dstAccessMask =
        RHI_ACCESS_SHADER_READ_BIT | RHI_ACCESS_COLOR_ATTACHMENT_READ_BIT;
    deferred_lighting_pass_depend_on_base_pass.dependencyFlags = RHI_DEPENDENCY_BY_REGION_BIT;

    RHISubpassDependency& forward_lighting_pass_depend_on_deferred_lighting_pass = dependencies[2];
    forward_lighting_pass_depend_on_deferred_lighting_pass.srcSubpass = _main_camera_subpass_deferred_lighting;
    forward_lighting_pass_depend_on_deferred_lighting_pass.dstSubpass = _main_camera_subpass_forward_lighting;
    forward_lighting_pass_depend_on_deferred_lighting_pass.srcStageMask =
        RHI_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | RHI_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    forward_lighting_pass_depend_on_deferred_lighting_pass.dstStageMask =
        RHI_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | RHI_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    forward_lighting_pass_depend_on_deferred_lighting_pass.srcAccessMask =
        RHI_ACCESS_SHADER_WRITE_BIT | RHI_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    forward_lighting_pass_depend_on_deferred_lighting_pass.dstAccessMask =
        RHI_ACCESS_SHADER_READ_BIT | RHI_ACCESS_COLOR_ATTACHMENT_READ_BIT;
    forward_lighting_pass_depend_on_deferred_lighting_pass.dependencyFlags = RHI_DEPENDENCY_BY_REGION_BIT;

    RHIRenderPassCreateInfo renderpass_create_info {};
    renderpass_create_info.sType = RHI_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderpass_create_info.attachmentCount = kRP1_AttachmentCount;
    renderpass_create_info.pAttachments = attachments;
    renderpass_create_info.subpassCount = _main_camera_rp1_subpass_count;
    renderpass_create_info.pSubpasses = subpasses;
    renderpass_create_info.dependencyCount = sizeof(dependencies) / sizeof(dependencies[0]);
    renderpass_create_info.pDependencies = dependencies;

    if (m_Rhi->CreateRenderPass(&renderpass_create_info, m_Rp1RenderPass) != RHI_SUCCESS)
    {
        throw std::runtime_error("MainCameraFramebufferResources: create RP1 render pass failed");
    }
}

void MainCameraFramebufferResources::SetupRenderPass2()
{
    enum
    {
        kRP2_BackupOdd = 0,
        kRP2_BackupEven = 1,
        kRP2_PostOdd = 2,
        kRP2_Swapchain = 3,
        kRP2_AttachmentCount = 4,
    };

    RHIAttachmentDescription attachments[kRP2_AttachmentCount] = {};

    RHIAttachmentDescription& backup_odd_attachment = attachments[kRP2_BackupOdd];
    backup_odd_attachment.format =
        m_Framebuffer.attachments[_main_camera_pass_backup_buffer_odd].format;
    backup_odd_attachment.samples = RHI_SAMPLE_COUNT_1_BIT;
    backup_odd_attachment.loadOp = RHI_ATTACHMENT_LOAD_OP_LOAD;
    backup_odd_attachment.storeOp = RHI_ATTACHMENT_STORE_OP_STORE;
    backup_odd_attachment.stencilLoadOp = RHI_ATTACHMENT_LOAD_OP_DONT_CARE;
    backup_odd_attachment.stencilStoreOp = RHI_ATTACHMENT_STORE_OP_DONT_CARE;
    backup_odd_attachment.initialLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    backup_odd_attachment.finalLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    RHIAttachmentDescription& backup_even_attachment = attachments[kRP2_BackupEven];
    backup_even_attachment.format =
        m_Framebuffer.attachments[_main_camera_pass_backup_buffer_even].format;
    backup_even_attachment.samples = RHI_SAMPLE_COUNT_1_BIT;
    backup_even_attachment.loadOp = RHI_ATTACHMENT_LOAD_OP_LOAD;
    backup_even_attachment.storeOp = RHI_ATTACHMENT_STORE_OP_STORE;
    backup_even_attachment.stencilLoadOp = RHI_ATTACHMENT_LOAD_OP_DONT_CARE;
    backup_even_attachment.stencilStoreOp = RHI_ATTACHMENT_STORE_OP_DONT_CARE;
    backup_even_attachment.initialLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    backup_even_attachment.finalLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    RHIAttachmentDescription& post_odd_attachment = attachments[kRP2_PostOdd];
    post_odd_attachment.format =
        m_Framebuffer.attachments[_main_camera_pass_post_process_buffer_odd].format;
    post_odd_attachment.samples = RHI_SAMPLE_COUNT_1_BIT;
    post_odd_attachment.loadOp = RHI_ATTACHMENT_LOAD_OP_CLEAR;
    post_odd_attachment.storeOp = RHI_ATTACHMENT_STORE_OP_DONT_CARE;
    post_odd_attachment.stencilLoadOp = RHI_ATTACHMENT_LOAD_OP_DONT_CARE;
    post_odd_attachment.stencilStoreOp = RHI_ATTACHMENT_STORE_OP_DONT_CARE;
    post_odd_attachment.initialLayout = RHI_IMAGE_LAYOUT_UNDEFINED;
    post_odd_attachment.finalLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    RHIAttachmentDescription& swapchain_attachment = attachments[kRP2_Swapchain];
    swapchain_attachment.format = m_Rhi->GetSwapchainInfo().image_format;
    swapchain_attachment.samples = RHI_SAMPLE_COUNT_1_BIT;
    swapchain_attachment.loadOp = RHI_ATTACHMENT_LOAD_OP_CLEAR;
    swapchain_attachment.storeOp = RHI_ATTACHMENT_STORE_OP_STORE;
    swapchain_attachment.stencilLoadOp = RHI_ATTACHMENT_LOAD_OP_DONT_CARE;
    swapchain_attachment.stencilStoreOp = RHI_ATTACHMENT_STORE_OP_DONT_CARE;
    swapchain_attachment.initialLayout = RHI_IMAGE_LAYOUT_UNDEFINED;
    swapchain_attachment.finalLayout = RHI_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    RHISubpassDescription subpasses[_main_camera_rp2_subpass_count] = {};

    RHIAttachmentReference color_grading_input {};
    color_grading_input.attachment = kRP2_BackupEven;
    color_grading_input.layout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    RHIAttachmentReference color_grading_output {};
    color_grading_output.attachment = m_EnableFxaa ? kRP2_PostOdd : kRP2_BackupOdd;
    color_grading_output.layout = RHI_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    RHISubpassDescription& color_grading_pass = subpasses[_main_camera_subpass_color_grading];
    color_grading_pass.pipelineBindPoint = RHI_PIPELINE_BIND_POINT_GRAPHICS;
    color_grading_pass.inputAttachmentCount = 1;
    color_grading_pass.pInputAttachments = &color_grading_input;
    color_grading_pass.colorAttachmentCount = 1;
    color_grading_pass.pColorAttachments = &color_grading_output;

    RHIAttachmentReference fxaa_input {};
    fxaa_input.attachment = m_EnableFxaa ? kRP2_PostOdd : kRP2_BackupEven;
    fxaa_input.layout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    RHIAttachmentReference fxaa_output {};
    fxaa_output.attachment = kRP2_BackupOdd;
    fxaa_output.layout = RHI_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    RHISubpassDescription& fxaa_pass = subpasses[_main_camera_subpass_fxaa];
    fxaa_pass.pipelineBindPoint = RHI_PIPELINE_BIND_POINT_GRAPHICS;
    fxaa_pass.inputAttachmentCount = 1;
    fxaa_pass.pInputAttachments = &fxaa_input;
    fxaa_pass.colorAttachmentCount = 1;
    fxaa_pass.pColorAttachments = &fxaa_output;

    RHIAttachmentReference ui_output {};
    ui_output.attachment = kRP2_BackupEven;
    ui_output.layout = RHI_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    uint32_t ui_preserve_attachment = kRP2_BackupOdd;

    RHISubpassDescription& ui_pass = subpasses[_main_camera_subpass_ui];
    ui_pass.pipelineBindPoint = RHI_PIPELINE_BIND_POINT_GRAPHICS;
    ui_pass.colorAttachmentCount = 1;
    ui_pass.pColorAttachments = &ui_output;
    ui_pass.preserveAttachmentCount = 1;
    ui_pass.pPreserveAttachments = &ui_preserve_attachment;

    RHIAttachmentReference combine_ui_inputs[2] = {};
    combine_ui_inputs[0].attachment = kRP2_BackupOdd;
    combine_ui_inputs[0].layout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    combine_ui_inputs[1].attachment = kRP2_BackupEven;
    combine_ui_inputs[1].layout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    RHIAttachmentReference combine_ui_output {};
    combine_ui_output.attachment = kRP2_Swapchain;
    combine_ui_output.layout = RHI_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    RHISubpassDescription& combine_ui_pass = subpasses[_main_camera_subpass_combine_ui];
    combine_ui_pass.pipelineBindPoint = RHI_PIPELINE_BIND_POINT_GRAPHICS;
    combine_ui_pass.inputAttachmentCount = 2;
    combine_ui_pass.pInputAttachments = combine_ui_inputs;
    combine_ui_pass.colorAttachmentCount = 1;
    combine_ui_pass.pColorAttachments = &combine_ui_output;

    RHISubpassDependency dependencies[5] = {};

    dependencies[0].srcSubpass = RHI_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = _main_camera_subpass_color_grading;
    dependencies[0].srcStageMask = RHI_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | RHI_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].dstStageMask = RHI_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | RHI_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].srcAccessMask = RHI_ACCESS_SHADER_WRITE_BIT | RHI_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[0].dstAccessMask = RHI_ACCESS_SHADER_READ_BIT | RHI_ACCESS_COLOR_ATTACHMENT_READ_BIT;
    dependencies[0].dependencyFlags = RHI_DEPENDENCY_BY_REGION_BIT;

    dependencies[1].srcSubpass = _main_camera_subpass_color_grading;
    dependencies[1].dstSubpass = _main_camera_subpass_fxaa;
    dependencies[1].srcStageMask = RHI_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | RHI_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[1].dstStageMask = RHI_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | RHI_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[1].srcAccessMask = RHI_ACCESS_SHADER_WRITE_BIT | RHI_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = RHI_ACCESS_SHADER_READ_BIT | RHI_ACCESS_COLOR_ATTACHMENT_READ_BIT;
    dependencies[1].dependencyFlags = RHI_DEPENDENCY_BY_REGION_BIT;

    dependencies[2].srcSubpass = _main_camera_subpass_fxaa;
    dependencies[2].dstSubpass = _main_camera_subpass_ui;
    dependencies[2].srcStageMask = RHI_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | RHI_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[2].dstStageMask = RHI_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | RHI_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[2].srcAccessMask = RHI_ACCESS_SHADER_WRITE_BIT | RHI_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[2].dstAccessMask = RHI_ACCESS_SHADER_READ_BIT | RHI_ACCESS_COLOR_ATTACHMENT_READ_BIT;
    dependencies[2].dependencyFlags = RHI_DEPENDENCY_BY_REGION_BIT;

    dependencies[3].srcSubpass = _main_camera_subpass_ui;
    dependencies[3].dstSubpass = _main_camera_subpass_combine_ui;
    dependencies[3].srcStageMask = RHI_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | RHI_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[3].dstStageMask = RHI_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | RHI_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[3].srcAccessMask = RHI_ACCESS_SHADER_WRITE_BIT | RHI_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[3].dstAccessMask = RHI_ACCESS_SHADER_READ_BIT | RHI_ACCESS_COLOR_ATTACHMENT_READ_BIT;
    dependencies[3].dependencyFlags = RHI_DEPENDENCY_BY_REGION_BIT;

    dependencies[4].srcSubpass = RHI_SUBPASS_EXTERNAL;
    dependencies[4].dstSubpass = _main_camera_subpass_combine_ui;
    dependencies[4].srcStageMask = RHI_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | RHI_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[4].dstStageMask = RHI_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[4].srcAccessMask = RHI_ACCESS_SHADER_WRITE_BIT | RHI_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[4].dstAccessMask = RHI_ACCESS_SHADER_READ_BIT;
    dependencies[4].dependencyFlags = RHI_DEPENDENCY_BY_REGION_BIT;

    RHIRenderPassCreateInfo renderpass_create_info {};
    renderpass_create_info.sType = RHI_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderpass_create_info.attachmentCount = kRP2_AttachmentCount;
    renderpass_create_info.pAttachments = attachments;
    renderpass_create_info.subpassCount = _main_camera_rp2_subpass_count;
    renderpass_create_info.pSubpasses = subpasses;
    renderpass_create_info.dependencyCount = sizeof(dependencies) / sizeof(dependencies[0]);
    renderpass_create_info.pDependencies = dependencies;

    if (m_Rhi->CreateRenderPass(&renderpass_create_info, m_Framebuffer.render_pass) != RHI_SUCCESS)
    {
        throw std::runtime_error("MainCameraFramebufferResources: create RP2 render pass failed");
    }
}

void MainCameraFramebufferResources::SetupFramebuffers()
{
    {
        RHIImageView* rp1_attachments[5] = {
            m_Framebuffer.attachments[_main_camera_pass_gbuffer_a].view,
            m_Framebuffer.attachments[_main_camera_pass_gbuffer_b].view,
            m_Framebuffer.attachments[_main_camera_pass_gbuffer_c].view,
            m_Framebuffer.attachments[_main_camera_pass_backup_buffer_odd].view,
            m_Rhi->GetDepthImageInfo().depth_image_view,
        };

        RHIFramebufferCreateInfo fb_ci {};
        fb_ci.sType = RHI_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb_ci.renderPass = m_Rp1RenderPass;
        fb_ci.attachmentCount = 5;
        fb_ci.pAttachments = rp1_attachments;
        fb_ci.width = m_Rhi->GetSwapchainInfo().extent.width;
        fb_ci.height = m_Rhi->GetSwapchainInfo().extent.height;
        fb_ci.layers = 1;

        if (m_Rhi->CreateFramebuffer(&fb_ci, m_Rp1Framebuffer) != RHI_SUCCESS)
        {
            throw std::runtime_error("MainCameraFramebufferResources: create RP1 framebuffer failed");
        }
    }

    m_SwapchainFramebuffers.resize(m_Rhi->GetSwapchainInfo().imageViews.size());
    for (size_t i = 0; i < m_SwapchainFramebuffers.size(); ++i)
    {
        RHIImageView* rp2_attachments[4] = {
            m_Framebuffer.attachments[_main_camera_pass_backup_buffer_odd].view,
            m_Framebuffer.attachments[_main_camera_pass_backup_buffer_even].view,
            m_Framebuffer.attachments[_main_camera_pass_post_process_buffer_odd].view,
            m_Rhi->GetSwapchainInfo().imageViews[i],
        };

        RHIFramebufferCreateInfo fb_ci {};
        fb_ci.sType = RHI_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb_ci.renderPass = m_Framebuffer.render_pass;
        fb_ci.attachmentCount = 4;
        fb_ci.pAttachments = rp2_attachments;
        fb_ci.width = m_Rhi->GetSwapchainInfo().extent.width;
        fb_ci.height = m_Rhi->GetSwapchainInfo().extent.height;
        fb_ci.layers = 1;

        RHIFramebuffer* framebuffer = nullptr;
        if (m_Rhi->CreateFramebuffer(&fb_ci, framebuffer) != RHI_SUCCESS)
        {
            throw std::runtime_error("MainCameraFramebufferResources: create RP2 framebuffer failed");
        }
        m_SwapchainFramebuffers[i] = framebuffer;
    }
}
