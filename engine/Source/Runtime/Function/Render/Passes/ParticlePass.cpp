#include "Runtime/Function/Render/Passes/ParticlePass.h"

#include "Runtime/Function/Render/Interface/Vulkan/VulkanRHI.h"
#include "Runtime/Function/Render/Interface/Vulkan/VulkanUtil.h"
#include "Runtime/Function/Render/RenderCamera.h"
#include "Runtime/Function/Render/RenderSystem.h"
#include "core/base/Macro.h"
#include "particle_emit_comp.h"
#include "particle_kickoff_comp.h"
#include "particle_simulate_comp.h"

#include <fstream>
#include <particlebillboard_frag.h>
#include <particlebillboard_vert.h>

void ParticleEmitterBufferBatch::FreeUpBatch(std::shared_ptr<RHI> rhi)
{
    rhi->FreeMemory(m_CounterHostMemory);
    rhi->FreeMemory(m_PositionHostMemory);
    rhi->FreeMemory(m_PositionDeviceMemory);
    rhi->FreeMemory(m_CounterDeviceMemory);
    rhi->FreeMemory(m_IndirectDispatchArgumentMemory);
    rhi->FreeMemory(m_AliveListMemory);
    rhi->FreeMemory(m_AliveListNextMemory);
    rhi->FreeMemory(m_DeadListMemory);
    rhi->FreeMemory(m_ParticleComponentResMemory);
    rhi->FreeMemory(m_PositionRenderMemory);

    rhi->DestroyBuffer(m_PositionRenderBuffer);
    rhi->DestroyBuffer(m_PositionDeviceBuffer);
    rhi->DestroyBuffer(m_PositionHostBuffer);
    rhi->DestroyBuffer(m_CounterDeviceBuffer);
    rhi->DestroyBuffer(m_CounterHostBuffer);
    rhi->DestroyBuffer(m_IndirectDispatchArgumentBuffer);
    rhi->DestroyBuffer(m_AliveListBuffer);
    rhi->DestroyBuffer(m_AliveListNextBuffer);
    rhi->DestroyBuffer(m_DeadListBuffer);
    rhi->DestroyBuffer(m_ParticleComponentResBuffer);
}

void ParticlePass::CopyNormalAndDepthImage()
{
    uint8_t index = (m_Rhi->GetCurrentFrameIndex() + m_Rhi->GetMaxFramesInFlight() - 1) % m_Rhi->GetMaxFramesInFlight();

    m_Rhi->WaitForFencesPFN(1, &(m_Rhi->GetFenceList()[index]), VK_TRUE, UINT64_MAX);

    RHICommandBufferBeginInfo command_buffer_begin_info {};
    command_buffer_begin_info.sType = RHI_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    command_buffer_begin_info.flags = 0;
    command_buffer_begin_info.pInheritanceInfo = nullptr;

    bool res_begin_command_buffer = m_Rhi->BeginCommandBufferPFN(m_CopyCommandBuffer, &command_buffer_begin_info);
    assert(RHI_SUCCESS == res_begin_command_buffer);

    float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    m_Rhi->PushEvent(m_CopyCommandBuffer, "Copy Depth Image for Particle", color);

    // depth image
    RHIImageSubresourceRange subresourceRange = {RHI_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    RHIImageMemoryBarrier imagememorybarrier {};
    imagememorybarrier.sType = RHI_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    imagememorybarrier.srcQueueFamilyIndex = RHI_QUEUE_FAMILY_IGNORED;
    imagememorybarrier.dstQueueFamilyIndex = RHI_QUEUE_FAMILY_IGNORED;
    imagememorybarrier.subresourceRange = subresourceRange;
    {
        // Transition destination depth image to TRANSFER_DST_OPTIMAL
        // Use UNDEFINED as oldLayout to allow transition from any previous layout (content will be discarded)
        imagememorybarrier.oldLayout = RHI_IMAGE_LAYOUT_UNDEFINED;
        imagememorybarrier.newLayout = RHI_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        imagememorybarrier.srcAccessMask = 0;  // No source access needed when transitioning from UNDEFINED
        imagememorybarrier.dstAccessMask = RHI_ACCESS_TRANSFER_WRITE_BIT;
        imagememorybarrier.image = m_DstDepthImage;

        m_Rhi->CmdPipelineBarrier(m_CopyCommandBuffer,
                                  RHI_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                  RHI_PIPELINE_STAGE_TRANSFER_BIT,
                                  0,
                                  0,
                                  nullptr,
                                  0,
                                  nullptr,
                                  1,
                                  &imagememorybarrier);

        // Transition source depth image from DEPTH_STENCIL_ATTACHMENT_OPTIMAL to TRANSFER_SRC_OPTIMAL
        imagememorybarrier.oldLayout = RHI_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        imagememorybarrier.newLayout = RHI_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        imagememorybarrier.srcAccessMask = RHI_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        imagememorybarrier.dstAccessMask = RHI_ACCESS_TRANSFER_READ_BIT;
        imagememorybarrier.image = m_SrcDepthImage;

        m_Rhi->CmdPipelineBarrier(m_CopyCommandBuffer,
                                  RHI_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                                  RHI_PIPELINE_STAGE_TRANSFER_BIT,
                                  0,
                                  0,
                                  nullptr,
                                  0,
                                  nullptr,
                                  1,
                                  &imagememorybarrier);

        m_Rhi->CmdCopyImageToImage(m_CopyCommandBuffer,
                                   m_SrcDepthImage,
                                   RHI_IMAGE_ASPECT_DEPTH_BIT,
                                   m_DstDepthImage,
                                   RHI_IMAGE_ASPECT_DEPTH_BIT,
                                   m_Rhi->GetSwapchainInfo().extent.width,
                                   m_Rhi->GetSwapchainInfo().extent.height);

        // Transition source depth image back to DEPTH_STENCIL_ATTACHMENT_OPTIMAL
        imagememorybarrier.oldLayout = RHI_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        imagememorybarrier.newLayout = RHI_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        imagememorybarrier.srcAccessMask = RHI_ACCESS_TRANSFER_READ_BIT;
        imagememorybarrier.dstAccessMask =
            RHI_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | RHI_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

        m_Rhi->CmdPipelineBarrier(m_CopyCommandBuffer,
                                  RHI_PIPELINE_STAGE_TRANSFER_BIT,
                                  RHI_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                                  0,
                                  0,
                                  nullptr,
                                  0,
                                  nullptr,
                                  1,
                                  &imagememorybarrier);

        // Transition destination depth image to SHADER_READ_ONLY_OPTIMAL
        imagememorybarrier.image = m_DstDepthImage;
        imagememorybarrier.oldLayout = RHI_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        imagememorybarrier.newLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imagememorybarrier.srcAccessMask = RHI_ACCESS_TRANSFER_WRITE_BIT;
        imagememorybarrier.dstAccessMask = RHI_ACCESS_SHADER_READ_BIT;

        m_Rhi->CmdPipelineBarrier(m_CopyCommandBuffer,
                                  RHI_PIPELINE_STAGE_TRANSFER_BIT,
                                  RHI_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                  0,
                                  0,
                                  nullptr,
                                  0,
                                  nullptr,
                                  1,
                                  &imagememorybarrier);
    }

    m_Rhi->PopEvent(m_CopyCommandBuffer);  // end depth image copy label

    m_Rhi->PushEvent(m_CopyCommandBuffer, "Copy Normal Image for Particle", color);

    // color image
    subresourceRange = {RHI_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    imagememorybarrier.subresourceRange = subresourceRange;
    {
        // Transition destination normal image to TRANSFER_DST_OPTIMAL
        // Use UNDEFINED as oldLayout to allow transition from any previous layout (content will be discarded)
        imagememorybarrier.oldLayout = RHI_IMAGE_LAYOUT_UNDEFINED;
        imagememorybarrier.newLayout = RHI_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        imagememorybarrier.srcAccessMask = 0;  // No source access needed when transitioning from UNDEFINED
        imagememorybarrier.dstAccessMask = RHI_ACCESS_TRANSFER_WRITE_BIT;
        imagememorybarrier.image = m_DstNormalImage;

        m_Rhi->CmdPipelineBarrier(m_CopyCommandBuffer,
                                  RHI_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                  RHI_PIPELINE_STAGE_TRANSFER_BIT,
                                  0,
                                  0,
                                  nullptr,
                                  0,
                                  nullptr,
                                  1,
                                  &imagememorybarrier);

        // Transition source normal image from COLOR_ATTACHMENT_OPTIMAL to TRANSFER_SRC_OPTIMAL
        imagememorybarrier.oldLayout = RHI_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        imagememorybarrier.newLayout = RHI_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        imagememorybarrier.srcAccessMask = RHI_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        imagememorybarrier.dstAccessMask = RHI_ACCESS_TRANSFER_READ_BIT;
        imagememorybarrier.image = m_SrcNormalImage;

        m_Rhi->CmdPipelineBarrier(m_CopyCommandBuffer,
                                  RHI_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                  RHI_PIPELINE_STAGE_TRANSFER_BIT,
                                  0,
                                  0,
                                  nullptr,
                                  0,
                                  nullptr,
                                  1,
                                  &imagememorybarrier);

        m_Rhi->CmdCopyImageToImage(m_CopyCommandBuffer,
                                   m_SrcNormalImage,
                                   RHI_IMAGE_ASPECT_COLOR_BIT,
                                   m_DstNormalImage,
                                   RHI_IMAGE_ASPECT_COLOR_BIT,
                                   m_Rhi->GetSwapchainInfo().extent.width,
                                   m_Rhi->GetSwapchainInfo().extent.height);

        // Transition source normal image back to COLOR_ATTACHMENT_OPTIMAL
        imagememorybarrier.oldLayout = RHI_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        imagememorybarrier.newLayout = RHI_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        imagememorybarrier.srcAccessMask = RHI_ACCESS_TRANSFER_READ_BIT;
        imagememorybarrier.dstAccessMask = RHI_ACCESS_COLOR_ATTACHMENT_READ_BIT | RHI_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        m_Rhi->CmdPipelineBarrier(m_CopyCommandBuffer,
                                  RHI_PIPELINE_STAGE_TRANSFER_BIT,
                                  RHI_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                  0,
                                  0,
                                  nullptr,
                                  0,
                                  nullptr,
                                  1,
                                  &imagememorybarrier);

        // Transition destination normal image to GENERAL layout
        imagememorybarrier.image = m_DstNormalImage;
        imagememorybarrier.oldLayout = RHI_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        imagememorybarrier.newLayout = RHI_IMAGE_LAYOUT_GENERAL;
        imagememorybarrier.srcAccessMask = RHI_ACCESS_TRANSFER_WRITE_BIT;
        imagememorybarrier.dstAccessMask = RHI_ACCESS_SHADER_READ_BIT;

        m_Rhi->CmdPipelineBarrier(m_CopyCommandBuffer,
                                  RHI_PIPELINE_STAGE_TRANSFER_BIT,
                                  RHI_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                  0,
                                  0,
                                  nullptr,
                                  0,
                                  nullptr,
                                  1,
                                  &imagememorybarrier);
    }

    m_Rhi->PopEvent(m_CopyCommandBuffer);

    bool res_end_command_buffer = m_Rhi->EndCommandBufferPFN(m_CopyCommandBuffer);
    assert(RHI_SUCCESS == res_end_command_buffer);

    bool res_reset_fences = m_Rhi->ResetFencesPFN(1, &m_Rhi->GetFenceList()[index]);
    assert(RHI_SUCCESS == res_reset_fences);

    RHIPipelineStageFlags wait_stages[] = {RHI_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    RHISubmitInfo submit_info = {};
    submit_info.sType = RHI_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.waitSemaphoreCount = 1;
    submit_info.pWaitSemaphores = &(m_Rhi->GetTextureCopySemaphore(index));
    submit_info.pWaitDstStageMask = wait_stages;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &m_CopyCommandBuffer;
    submit_info.signalSemaphoreCount = 0;
    submit_info.pSignalSemaphores = nullptr;
    bool res_queue_submit =
        m_Rhi->QueueSubmit(m_Rhi->GetGraphicsQueue(), 1, &submit_info, m_Rhi->GetFenceList()[index]);
    assert(RHI_SUCCESS == res_queue_submit);

    m_Rhi->QueueWaitIdle(m_Rhi->GetGraphicsQueue());
}

void ParticlePass::UpdateAfterFramebufferRecreate()
{
    m_Rhi->DestroyImage(m_DstDepthImage);
    m_Rhi->FreeMemory(m_DstDepthImageMemory);

    m_Rhi->CreateImage(m_Rhi->GetSwapchainInfo().extent.width,
                       m_Rhi->GetSwapchainInfo().extent.height,
                       m_Rhi->GetDepthImageInfo().depth_image_format,
                       RHI_IMAGE_TILING_OPTIMAL,
                       RHI_IMAGE_USAGE_SAMPLED_BIT | RHI_IMAGE_USAGE_TRANSFER_DST_BIT,
                       RHI_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                       m_DstDepthImage,
                       m_DstDepthImageMemory,
                       0,
                       1,
                       1);

    m_Rhi->DestroyImage(m_DstNormalImage);
    m_Rhi->FreeMemory(m_DstNormalImageMemory);

    m_Rhi->CreateImage(m_Rhi->GetSwapchainInfo().extent.width,
                       m_Rhi->GetSwapchainInfo().extent.height,
                       RHI_FORMAT_R8G8B8A8_UNORM,
                       RHI_IMAGE_TILING_OPTIMAL,
                       RHI_IMAGE_USAGE_STORAGE_BIT | RHI_IMAGE_USAGE_TRANSFER_DST_BIT,
                       RHI_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                       m_DstNormalImage,
                       m_DstNormalImageMemory,
                       0,
                       1,
                       1);

    m_Rhi->CreateImageView(m_DstDepthImage,
                           m_Rhi->GetDepthImageInfo().depth_image_format,
                           RHI_IMAGE_ASPECT_DEPTH_BIT,
                           RHI_IMAGE_VIEW_TYPE_2D,
                           1,
                           1,
                           m_SrcDepthImageView);

    m_Rhi->CreateImageView(m_DstNormalImage,
                           RHI_FORMAT_R8G8B8A8_UNORM,
                           RHI_IMAGE_ASPECT_COLOR_BIT,
                           RHI_IMAGE_VIEW_TYPE_2D,
                           1,
                           1,
                           m_SrcNormalImageView);

    // Note: UpdateDescriptorSet() is called later when textures are loaded via EnsureAttachmentsInitialized()
}

void ParticlePass::Draw()
{
    constexpr ViewportType k_viewports[] = {ViewportType::game, ViewportType::scene};

    for (ViewportType viewport_type : k_viewports)
    {
        auto* viewport = m_Rhi->GetViewport(viewport_type);
        if (!viewport || viewport->width <= 0.0f || viewport->height <= 0.0f)
            continue;

        m_ParticleCollisionPerframeStorageBufferObject =
            m_ParticleCollisionPerframeStorageBufferObjects[static_cast<size_t>(viewport_type)];
        memcpy(m_SceneUniformBufferMapped,
               &m_ParticleCollisionPerframeStorageBufferObject,
               sizeof(ParticleCollisionPerframeStorageBufferObject));

        m_ParticlebillboardPerframeStorageBufferObject =
            m_ParticlebillboardPerframeStorageBufferObjects[static_cast<size_t>(viewport_type)];
        memcpy(m_ParticleBillboardUniformBufferMapped,
               &m_ParticlebillboardPerframeStorageBufferObject,
               sizeof(m_ParticlebillboardPerframeStorageBufferObject));

        m_ViewportParams = *viewport;
        UpdateUniformBuffer(viewport_type);

        RHIRect2D scissor = m_Rhi->GetSwapchainInfo().scissor[static_cast<size_t>(viewport_type)];

        for (int i = 0; i < m_EmitterCount; ++i)
        {
            float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
            m_Rhi->PushEvent(m_RenderCommandBuffer, "ParticleBillboard", color);

            m_Rhi->CmdBindPipelinePFN(
                m_RenderCommandBuffer, RHI_PIPELINE_BIND_POINT_GRAPHICS, m_RenderPipelines[1].pipeline);
            m_Rhi->CmdSetViewportPFN(m_RenderCommandBuffer, 0, 1, viewport);
            m_Rhi->CmdSetScissorPFN(m_RenderCommandBuffer, 0, 1, &scissor);
            m_Rhi->CmdBindDescriptorSetsPFN(m_RenderCommandBuffer,
                                            RHI_PIPELINE_BIND_POINT_GRAPHICS,
                                            m_RenderPipelines[1].layout,
                                            0,
                                            1,
                                            &m_DescriptorInfos[i * 3 + 2].descriptor_set,
                                            0,
                                            NULL);

            m_Rhi->CmdDraw(m_RenderCommandBuffer, 4, m_EmitterBufferBatches[i].m_NumParticle, 0, 0);

            m_Rhi->PopEvent(m_RenderCommandBuffer);
        }
    }
}

void ParticlePass::SetupAttachments()
{
    // SRP-style lazy initialization: texture resources are loaded later when ParticleManager is ready
    // Only create depth and normal images here (they don't depend on ParticleManager)

    m_Rhi->CreateImage(m_Rhi->GetSwapchainInfo().extent.width,
                       m_Rhi->GetSwapchainInfo().extent.height,
                       m_Rhi->GetDepthImageInfo().depth_image_format,
                       RHI_IMAGE_TILING_OPTIMAL,
                       RHI_IMAGE_USAGE_SAMPLED_BIT | RHI_IMAGE_USAGE_TRANSFER_DST_BIT,
                       RHI_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                       m_DstDepthImage,
                       m_DstDepthImageMemory,
                       0,
                       1,
                       1);

    m_Rhi->CreateImage(m_Rhi->GetSwapchainInfo().extent.width,
                       m_Rhi->GetSwapchainInfo().extent.height,
                       RHI_FORMAT_R8G8B8A8_UNORM,
                       RHI_IMAGE_TILING_OPTIMAL,
                       RHI_IMAGE_USAGE_STORAGE_BIT | RHI_IMAGE_USAGE_TRANSFER_DST_BIT,
                       RHI_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                       m_DstNormalImage,
                       m_DstNormalImageMemory,
                       0,
                       1,
                       1);

    m_Rhi->CreateImageView(m_DstDepthImage,
                           m_Rhi->GetDepthImageInfo().depth_image_format,
                           RHI_IMAGE_ASPECT_DEPTH_BIT,
                           RHI_IMAGE_VIEW_TYPE_2D,
                           1,
                           1,
                           m_SrcDepthImageView);

    m_Rhi->CreateImageView(m_DstNormalImage,
                           RHI_FORMAT_R8G8B8A8_UNORM,
                           RHI_IMAGE_ASPECT_COLOR_BIT,
                           RHI_IMAGE_VIEW_TYPE_2D,
                           1,
                           1,
                           m_SrcNormalImageView);
}

void ParticlePass::EnsureAttachmentsInitialized()
{
    // SRP-style lazy initialization: load texture resources when ParticleManager is ready
    if (m_AttachmentsInitialized)
    {
        return;
    }

    // Check if ParticleManager is available and initialized
    if (!m_ParticleManager || !m_ParticleManager->IsInitialized())
    {
        return;
    }

    // Load billboard texture
    {
        std::shared_ptr<TextureData> m_ParticleBillboardTextureResource = m_RenderResource->LoadTextureHDR(
            m_ParticleManager->GetGlobalParticleRes()->m_ParticleBillboardTexturePath);
        m_Rhi->CreateGlobalImage(m_ParticleBillboardTextureImage,
                                 m_ParticleBillboardTextureImageView,
                                 m_ParticleBillboardTextureVmaAllocation,
                                 m_ParticleBillboardTextureResource->m_Width,
                                 m_ParticleBillboardTextureResource->m_Height,
                                 m_ParticleBillboardTextureResource->m_Pixels,
                                 m_ParticleBillboardTextureResource->m_Format);
    }

    // Load Z texture
    {
        std::shared_ptr<TextureData> m_ZengineLogoTextureResource = m_RenderResource->LoadTexture(
            m_ParticleManager->GetGlobalParticleRes()->m_ZengineLogoTexturePath, true);
        m_Rhi->CreateGlobalImage(m_ZengineLogoTextureImage,
                                 m_ZengineLogoTextureImageView,
                                 m_ZengineLogoTextureVmaAllocation,
                                 m_ZengineLogoTextureResource->m_Width,
                                 m_ZengineLogoTextureResource->m_Height,
                                 m_ZengineLogoTextureResource->m_Pixels,
                                 m_ZengineLogoTextureResource->m_Format);
    }

    m_AttachmentsInitialized = true;
}

void ParticlePass::SetupParticleDescriptorSet()
{
    // SRP-style lazy initialization: ensure texture resources are loaded when ParticleManager is ready
    EnsureAttachmentsInitialized();

    for (int eid = 0; eid < m_EmitterCount; ++eid)
    {
        RHIDescriptorSetAllocateInfo particlebillboard_global_descriptor_set_alloc_info;
        particlebillboard_global_descriptor_set_alloc_info.sType = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        particlebillboard_global_descriptor_set_alloc_info.pNext = NULL;
        particlebillboard_global_descriptor_set_alloc_info.descriptorPool = m_Rhi->GetDescriptorPoor();
        particlebillboard_global_descriptor_set_alloc_info.descriptorSetCount = 1;
        particlebillboard_global_descriptor_set_alloc_info.pSetLayouts = &m_DescriptorInfos[2].layout;

        if (RHI_SUCCESS != m_Rhi->AllocateDescriptorSets(&particlebillboard_global_descriptor_set_alloc_info,
                                                         m_DescriptorInfos[eid * 3 + 2].descriptor_set))
        {
            throw std::runtime_error("allocate particle billboard global descriptor set");
        }

        RHIDescriptorBufferInfo particlebillboard_perframe_storage_buffer_info = {};
        particlebillboard_perframe_storage_buffer_info.offset = 0;
        particlebillboard_perframe_storage_buffer_info.range = RHI_WHOLE_SIZE;
        particlebillboard_perframe_storage_buffer_info.buffer = m_ParticleBillboardUniformBuffer;

        RHIDescriptorBufferInfo particlebillboard_perdrawcall_storage_buffer_info = {};
        particlebillboard_perdrawcall_storage_buffer_info.offset = 0;
        particlebillboard_perdrawcall_storage_buffer_info.range = RHI_WHOLE_SIZE;
        particlebillboard_perdrawcall_storage_buffer_info.buffer =
            m_EmitterBufferBatches[eid].m_PositionRenderBuffer;

        RHIWriteDescriptorSet particlebillboard_descriptor_writes_info[3];

        particlebillboard_descriptor_writes_info[0].sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        particlebillboard_descriptor_writes_info[0].pNext = NULL;
        particlebillboard_descriptor_writes_info[0].dstSet = m_DescriptorInfos[eid * 3 + 2].descriptor_set;
        particlebillboard_descriptor_writes_info[0].dstBinding = 0;
        particlebillboard_descriptor_writes_info[0].dstArrayElement = 0;
        particlebillboard_descriptor_writes_info[0].descriptorType = RHI_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        particlebillboard_descriptor_writes_info[0].descriptorCount = 1;
        particlebillboard_descriptor_writes_info[0].pBufferInfo = &particlebillboard_perframe_storage_buffer_info;

        particlebillboard_descriptor_writes_info[1].sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        particlebillboard_descriptor_writes_info[1].pNext = NULL;
        particlebillboard_descriptor_writes_info[1].dstSet = m_DescriptorInfos[eid * 3 + 2].descriptor_set;
        particlebillboard_descriptor_writes_info[1].dstBinding = 1;
        particlebillboard_descriptor_writes_info[1].dstArrayElement = 0;
        particlebillboard_descriptor_writes_info[1].descriptorType = RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        particlebillboard_descriptor_writes_info[1].descriptorCount = 1;
        particlebillboard_descriptor_writes_info[1].pBufferInfo = &particlebillboard_perdrawcall_storage_buffer_info;

        RHISampler* sampler;
        RHISamplerCreateInfo samplerCreateInfo {};
        samplerCreateInfo.sType = RHI_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerCreateInfo.maxAnisotropy = 1.0f;
        samplerCreateInfo.anisotropyEnable = true;
        samplerCreateInfo.magFilter = RHI_FILTER_LINEAR;
        samplerCreateInfo.minFilter = RHI_FILTER_LINEAR;
        samplerCreateInfo.mipmapMode = RHI_SAMPLER_MIPMAP_MODE_LINEAR;
        samplerCreateInfo.addressModeU = RHI_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerCreateInfo.addressModeV = RHI_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerCreateInfo.addressModeW = RHI_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerCreateInfo.mipLodBias = 0.0f;
        samplerCreateInfo.compareOp = RHI_COMPARE_OP_NEVER;
        samplerCreateInfo.minLod = 0.0f;
        samplerCreateInfo.maxLod = 0.0f;
        samplerCreateInfo.borderColor = RHI_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        if (RHI_SUCCESS != m_Rhi->CreateSampler(&samplerCreateInfo, sampler))
        {
            throw std::runtime_error("create sampler error");
        }

        RHIDescriptorImageInfo particle_texture_image_info = {};
        particle_texture_image_info.sampler = sampler;
        particle_texture_image_info.imageView = m_ParticleBillboardTextureImageView;
        particle_texture_image_info.imageLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        particlebillboard_descriptor_writes_info[2].sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        particlebillboard_descriptor_writes_info[2].pNext = NULL;
        particlebillboard_descriptor_writes_info[2].dstSet = m_DescriptorInfos[eid * 3 + 2].descriptor_set;
        particlebillboard_descriptor_writes_info[2].dstBinding = 2;
        particlebillboard_descriptor_writes_info[2].dstArrayElement = 0;
        particlebillboard_descriptor_writes_info[2].descriptorType = RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        particlebillboard_descriptor_writes_info[2].descriptorCount = 1;
        particlebillboard_descriptor_writes_info[2].pImageInfo = &particle_texture_image_info;

        m_Rhi->UpdateDescriptorSets(3, particlebillboard_descriptor_writes_info, 0, NULL);
    }
}

void ParticlePass::SetEmitterCount(int count)
{
    for (int i = 0; i < m_EmitterBufferBatches.size(); ++i)
    {
        m_EmitterBufferBatches[i].FreeUpBatch(m_Rhi);
    }

    m_EmitterCount = count;
    m_EmitterBufferBatches.resize(m_EmitterCount);
}

void ParticlePass::CreateEmitter(int id, const ParticleEmitterDesc& desc)
{
    const VkDeviceSize counterBufferSize = sizeof(ParticleCounter);
    ParticleCounter counter;
    counter.alive_count = m_EmitterBufferBatches[id].m_NumParticle;
    counter.dead_count = s_MaxParticles - m_EmitterBufferBatches[id].m_NumParticle;
    counter.emit_count = 0;
    counter.alive_count_after_sim = m_EmitterBufferBatches[id].m_NumParticle;

    if constexpr (s_VerboseParticleAliveInfo)
    {
        LOG_INFO(ZParticle, "Emitter {} info:", id);
        LOG_INFO(ZParticle,
                 "Dead {}, Alive {}, After sim {}, Emit {}",
                 counter.dead_count,
                 counter.alive_count,
                 counter.alive_count_after_sim,
                 counter.emit_count);
    }

    {
        const VkDeviceSize indirectArgumentSize = sizeof(IndirectArgumemt);
        struct IndirectArgumemt indirectargument = {};
        indirectargument.alive_flap_bit = 1;
        m_Rhi->CreateBufferAndInitialize(RHI_BUFFER_USAGE_STORAGE_BUFFER_BIT | RHI_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                                         RHI_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                                         m_EmitterBufferBatches[id].m_IndirectDispatchArgumentBuffer,
                                         m_EmitterBufferBatches[id].m_IndirectDispatchArgumentMemory,
                                         indirectArgumentSize,
                                         &indirectargument,
                                         indirectArgumentSize);

        const VkDeviceSize aliveListSize = 4 * sizeof(uint32_t) * s_MaxParticles;
        std::vector<int> aliveindices(s_MaxParticles * 4, 0);
        for (int i = 0; i < s_MaxParticles; ++i)
            aliveindices[i * 4] = i;

        m_Rhi->CreateBufferAndInitialize(RHI_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                         RHI_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                                         m_EmitterBufferBatches[id].m_AliveListBuffer,
                                         m_EmitterBufferBatches[id].m_AliveListMemory,
                                         aliveListSize,
                                         aliveindices.data(),
                                         aliveListSize);

        m_Rhi->CreateBufferAndInitialize(RHI_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                         RHI_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                         m_EmitterBufferBatches[id].m_AliveListNextBuffer,
                                         m_EmitterBufferBatches[id].m_AliveListNextMemory,
                                         aliveListSize);

        const VkDeviceSize deadListSize = 4 * sizeof(uint32_t) * s_MaxParticles;
        std::vector<int32_t> deadindices(s_MaxParticles * 4, 0);
        for (int32_t i = 0; i < s_MaxParticles; ++i)
            deadindices[i * 4] = s_MaxParticles - 1 - i;

        m_Rhi->CreateBufferAndInitialize(RHI_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                         RHI_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                                         m_EmitterBufferBatches[id].m_DeadListBuffer,
                                         m_EmitterBufferBatches[id].m_DeadListMemory,
                                         deadListSize,
                                         deadindices.data(),
                                         deadListSize);
    }

    RHIFence* fence = nullptr;
    ParticleCounter counterNext {};
    {
        m_Rhi->CreateBufferAndInitialize(RHI_BUFFER_USAGE_TRANSFER_SRC_BIT | RHI_BUFFER_USAGE_TRANSFER_DST_BIT,
                                         RHI_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                                         m_EmitterBufferBatches[id].m_CounterHostBuffer,
                                         m_EmitterBufferBatches[id].m_CounterHostMemory,
                                         counterBufferSize,
                                         &counter,
                                         sizeof(counter));

        // Flush writes to host visible buffer
        void* mapped;

        m_Rhi->MapMemory(m_EmitterBufferBatches[id].m_CounterHostMemory, 0, RHI_WHOLE_SIZE, 0, &mapped);

        m_Rhi->FlushMappedMemoryRanges(nullptr, m_EmitterBufferBatches[id].m_CounterHostMemory, 0, RHI_WHOLE_SIZE);

        m_Rhi->UnmapMemory(m_EmitterBufferBatches[id].m_CounterHostMemory);

        m_Rhi->CreateBufferAndInitialize(RHI_BUFFER_USAGE_STORAGE_BUFFER_BIT | RHI_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                             RHI_BUFFER_USAGE_TRANSFER_DST_BIT,
                                         RHI_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                         m_EmitterBufferBatches[id].m_CounterDeviceBuffer,
                                         m_EmitterBufferBatches[id].m_CounterDeviceMemory,
                                         counterBufferSize);

        // Copy to staging buffer
        RHICommandBufferAllocateInfo cmdBufAllocateInfo {};
        cmdBufAllocateInfo.sType = RHI_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cmdBufAllocateInfo.commandPool = m_Rhi->GetCommandPoor();
        cmdBufAllocateInfo.level = RHI_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmdBufAllocateInfo.commandBufferCount = 1;
        RHICommandBuffer* copyCmd;
        if (RHI_SUCCESS != m_Rhi->AllocateCommandBuffers(&cmdBufAllocateInfo, copyCmd))
        {
            throw std::runtime_error("alloc command buffer");
        }

        RHICommandBufferBeginInfo cmdBufInfo {};
        cmdBufInfo.sType = RHI_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        if (RHI_SUCCESS != m_Rhi->BeginCommandBuffer(copyCmd, &cmdBufInfo))
        {
            throw std::runtime_error("begin command buffer");
        }

        RHIBufferCopy copyRegion = {};
        copyRegion.srcOffset = 0;
        copyRegion.dstOffset = 0;
        copyRegion.size = counterBufferSize;
        m_Rhi->CmdCopyBuffer(copyCmd,
                             m_EmitterBufferBatches[id].m_CounterHostBuffer,
                             m_EmitterBufferBatches[id].m_CounterDeviceBuffer,
                             1,
                             &copyRegion);

        if (RHI_SUCCESS != m_Rhi->EndCommandBuffer(copyCmd))
        {
            throw std::runtime_error("buffer copy");
        }

        RHISubmitInfo submitInfo {};
        submitInfo.sType = RHI_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &copyCmd;
        RHIFenceCreateInfo fenceInfo {};
        fenceInfo.sType = RHI_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = 0;
        if (RHI_SUCCESS != m_Rhi->CreateFence(&fenceInfo, fence))
        {
            throw std::runtime_error("create fence");
        }

        // Submit to the queue
        if (RHI_SUCCESS != m_Rhi->QueueSubmit(m_Rhi->GetComputeQueue(), 1, &submitInfo, fence))
        {
            throw std::runtime_error("queue submit");
        }

        if (RHI_SUCCESS != m_Rhi->WaitForFencesPFN(1, &fence, RHI_TRUE, UINT64_MAX))
        {
            throw std::runtime_error("wait fence submit");
        }

        m_Rhi->DestroyFence(fence);
        m_Rhi->FreeCommandBuffers(m_Rhi->GetCommandPoor(), 1, copyCmd);
    }

    const VkDeviceSize staggingBuferSize = s_MaxParticles * sizeof(Particle);
    m_EmitterBufferBatches[id].m_EmitterDesc = desc;

    // fill in data
    {
        m_Rhi->CreateBufferAndInitialize(RHI_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                         RHI_MEMORY_PROPERTY_HOST_VISIBLE_BIT | RHI_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                         m_EmitterBufferBatches[id].m_ParticleComponentResBuffer,
                                         m_EmitterBufferBatches[id].m_ParticleComponentResMemory,
                                         sizeof(ParticleEmitterDesc),
                                         &m_EmitterBufferBatches[id].m_EmitterDesc,
                                         sizeof(ParticleEmitterDesc));

        if (RHI_SUCCESS != m_Rhi->MapMemory(m_EmitterBufferBatches[id].m_ParticleComponentResMemory,
                                            0,
                                            RHI_WHOLE_SIZE,
                                            0,
                                            &m_EmitterBufferBatches[id].m_EmitterDescMapped))
        {
            throw std::runtime_error("map emitter component res buffer");
        }

        m_Rhi->CreateBufferAndInitialize(RHI_BUFFER_USAGE_TRANSFER_SRC_BIT | RHI_BUFFER_USAGE_TRANSFER_DST_BIT,
                                         RHI_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                                         m_EmitterBufferBatches[id].m_PositionHostBuffer,
                                         m_EmitterBufferBatches[id].m_PositionHostMemory,
                                         staggingBuferSize);

        // Flush writes to host visible buffer
        void* mapped;
        m_Rhi->MapMemory(m_EmitterBufferBatches[id].m_PositionHostMemory, 0, RHI_WHOLE_SIZE, 0, &mapped);

        m_Rhi->FlushMappedMemoryRanges(nullptr, m_EmitterBufferBatches[id].m_PositionHostMemory, 0, RHI_WHOLE_SIZE);

        m_Rhi->UnmapMemory(m_EmitterBufferBatches[id].m_PositionHostMemory);

        m_Rhi->CreateBufferAndInitialize(RHI_BUFFER_USAGE_STORAGE_BUFFER_BIT | RHI_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                             RHI_BUFFER_USAGE_TRANSFER_DST_BIT,
                                         RHI_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                         m_EmitterBufferBatches[id].m_PositionDeviceBuffer,
                                         m_EmitterBufferBatches[id].m_PositionDeviceMemory,
                                         staggingBuferSize);

        m_Rhi->CreateBufferAndInitialize(RHI_BUFFER_USAGE_STORAGE_BUFFER_BIT | RHI_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                             RHI_BUFFER_USAGE_TRANSFER_DST_BIT,
                                         RHI_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                         m_EmitterBufferBatches[id].m_PositionRenderBuffer,
                                         m_EmitterBufferBatches[id].m_PositionRenderMemory,
                                         staggingBuferSize);

        // Copy to staging buffer
        RHICommandBufferAllocateInfo cmdBufAllocateInfo {};
        cmdBufAllocateInfo.sType = RHI_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cmdBufAllocateInfo.commandPool = m_Rhi->GetCommandPoor();
        cmdBufAllocateInfo.level = RHI_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmdBufAllocateInfo.commandBufferCount = 1;
        RHICommandBuffer* copyCmd;
        if (RHI_SUCCESS != m_Rhi->AllocateCommandBuffers(&cmdBufAllocateInfo, copyCmd))
        {
            throw std::runtime_error("alloc command buffer");
        }
        RHICommandBufferBeginInfo cmdBufInfo {};
        cmdBufInfo.sType = RHI_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        if (RHI_SUCCESS != m_Rhi->BeginCommandBuffer(copyCmd, &cmdBufInfo))
        {
            throw std::runtime_error("begin command buffer");
        }

        RHIBufferCopy copyRegion = {};
        copyRegion.srcOffset = 0;
        copyRegion.dstOffset = 0;
        copyRegion.size = staggingBuferSize;
        m_Rhi->CmdCopyBuffer(copyCmd,
                             m_EmitterBufferBatches[id].m_PositionHostBuffer,
                             m_EmitterBufferBatches[id].m_PositionDeviceBuffer,
                             1,
                             &copyRegion);

        if (RHI_SUCCESS != m_Rhi->EndCommandBuffer(copyCmd))
        {
            throw std::runtime_error("buffer copy");
        }

        RHISubmitInfo submitInfo {};
        submitInfo.sType = RHI_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &copyCmd;
        RHIFenceCreateInfo fenceInfo {};
        fenceInfo.sType = RHI_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = 0;
        if (RHI_SUCCESS != m_Rhi->CreateFence(&fenceInfo, fence))
        {
            throw std::runtime_error("create fence");
        }

        // Submit to the queue
        if (RHI_SUCCESS != m_Rhi->QueueSubmit(m_Rhi->GetComputeQueue(), 1, &submitInfo, fence))
        {
            throw std::runtime_error("queue submit");
        }

        if (RHI_SUCCESS != m_Rhi->WaitForFencesPFN(1, &fence, RHI_TRUE, UINT64_MAX))
        {
            throw std::runtime_error("wait fence submit");
        }

        m_Rhi->DestroyFence(fence);
        m_Rhi->FreeCommandBuffers(m_Rhi->GetCommandPoor(), 1, copyCmd);
    }
}

void ParticlePass::InitializeEmitters()
{
    AllocateDescriptorSet();
    UpdateDescriptorSet();
    SetupParticleDescriptorSet();
}

void ParticlePass::SetupParticlePass()
{
    PrepareUniformBuffer();
    SetupDescriptorSetLayout();
    SetupPipelines();
    SetupAttachments();

    RHICommandBufferAllocateInfo cmdBufAllocateInfo {};
    cmdBufAllocateInfo.sType = RHI_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdBufAllocateInfo.commandPool = m_Rhi->GetCommandPoor();
    cmdBufAllocateInfo.level = RHI_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdBufAllocateInfo.commandBufferCount = 1;
    if (RHI_SUCCESS != m_Rhi->AllocateCommandBuffers(&cmdBufAllocateInfo, m_ComputeCommandBuffer))
        throw std::runtime_error("alloc compute command buffer");
    if (RHI_SUCCESS != m_Rhi->AllocateCommandBuffers(&cmdBufAllocateInfo, m_CopyCommandBuffer))
        throw std::runtime_error("alloc copy command buffer");

    RHIFenceCreateInfo fenceCreateInfo {};
    fenceCreateInfo.sType = RHI_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceCreateInfo.flags = 0;
    if (RHI_SUCCESS != m_Rhi->CreateFence(&fenceCreateInfo, m_Fence))
        throw std::runtime_error("create fence");
}

void ParticlePass::Initialize(const RenderPassInitInfo* init_info)
{
    RenderPass::Initialize(nullptr);

    const ParticlePassInitInfo* _init_info = static_cast<const ParticlePassInitInfo*>(init_info);
    m_ParticleManager = _init_info->m_ParticleManager;
}

void ParticlePass::SetupDescriptorSetLayout()
{
    m_DescriptorInfos.resize(3);

    // compute descriptor sets
    {
        RHIDescriptorSetLayoutBinding particle_layout_bindings[11] = {};
        {
            RHIDescriptorSetLayoutBinding& uniform_layout_bingding = particle_layout_bindings[0];
            uniform_layout_bingding.binding = 0;
            uniform_layout_bingding.descriptorType = RHI_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            uniform_layout_bingding.descriptorCount = 1;
            uniform_layout_bingding.stageFlags = RHI_SHADER_STAGE_COMPUTE_BIT;
        }

        {
            RHIDescriptorSetLayoutBinding& storage_position_layout_binding = particle_layout_bindings[1];
            storage_position_layout_binding.binding = 1;
            storage_position_layout_binding.descriptorType = RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            storage_position_layout_binding.descriptorCount = 1;
            storage_position_layout_binding.stageFlags = RHI_SHADER_STAGE_COMPUTE_BIT;
        }

        {
            RHIDescriptorSetLayoutBinding& storage_counter_layout_binding = particle_layout_bindings[2];
            storage_counter_layout_binding.binding = 2;
            storage_counter_layout_binding.descriptorType = RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            storage_counter_layout_binding.descriptorCount = 1;
            storage_counter_layout_binding.stageFlags = RHI_SHADER_STAGE_COMPUTE_BIT;
        }

        {
            RHIDescriptorSetLayoutBinding& storage_indirectargument_layout_binding = particle_layout_bindings[3];
            storage_indirectargument_layout_binding.binding = 3;
            storage_indirectargument_layout_binding.descriptorType = RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            storage_indirectargument_layout_binding.descriptorCount = 1;
            storage_indirectargument_layout_binding.stageFlags = RHI_SHADER_STAGE_COMPUTE_BIT;
        }

        {
            RHIDescriptorSetLayoutBinding& alive_list_layout_binding = particle_layout_bindings[4];
            alive_list_layout_binding.binding = 4;
            alive_list_layout_binding.descriptorType = RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            alive_list_layout_binding.descriptorCount = 1;
            alive_list_layout_binding.stageFlags = RHI_SHADER_STAGE_COMPUTE_BIT;
        }

        {
            RHIDescriptorSetLayoutBinding& dead_list_layout_binding = particle_layout_bindings[5];
            dead_list_layout_binding.binding = 5;
            dead_list_layout_binding.descriptorType = RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            dead_list_layout_binding.descriptorCount = 1;
            dead_list_layout_binding.stageFlags = RHI_SHADER_STAGE_COMPUTE_BIT;
        }

        {
            RHIDescriptorSetLayoutBinding& alive_list_next_layout_binding = particle_layout_bindings[6];
            alive_list_next_layout_binding.binding = 6;
            alive_list_next_layout_binding.descriptorType = RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            alive_list_next_layout_binding.descriptorCount = 1;
            alive_list_next_layout_binding.stageFlags = RHI_SHADER_STAGE_COMPUTE_BIT;
        }

        {
            RHIDescriptorSetLayoutBinding& particle_res_layout_binding = particle_layout_bindings[7];
            particle_res_layout_binding.binding = 7;
            particle_res_layout_binding.descriptorType = RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            particle_res_layout_binding.descriptorCount = 1;
            particle_res_layout_binding.stageFlags = RHI_SHADER_STAGE_COMPUTE_BIT;
        }

        {
            RHIDescriptorSetLayoutBinding& scene_uniformbuffer_layout_binding = particle_layout_bindings[8];
            scene_uniformbuffer_layout_binding.binding = 8;
            scene_uniformbuffer_layout_binding.descriptorType = RHI_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            scene_uniformbuffer_layout_binding.descriptorCount = 1;
            scene_uniformbuffer_layout_binding.stageFlags = RHI_SHADER_STAGE_COMPUTE_BIT;
        }

        {
            RHIDescriptorSetLayoutBinding& storage_render_position_layout_binding = particle_layout_bindings[9];
            storage_render_position_layout_binding.binding = 9;
            storage_render_position_layout_binding.descriptorType = RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            storage_render_position_layout_binding.descriptorCount = 1;
            storage_render_position_layout_binding.stageFlags = RHI_SHADER_STAGE_COMPUTE_BIT;
        }

        {
            RHIDescriptorSetLayoutBinding& zengine_texture_layout_binding = particle_layout_bindings[10];
            zengine_texture_layout_binding.binding = 10;
            zengine_texture_layout_binding.descriptorType = RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            zengine_texture_layout_binding.descriptorCount = 1;
            zengine_texture_layout_binding.stageFlags = RHI_SHADER_STAGE_COMPUTE_BIT;
        }

        RHIDescriptorSetLayoutCreateInfo particle_descriptor_layout_create_info;
        particle_descriptor_layout_create_info.sType = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        particle_descriptor_layout_create_info.pNext = NULL;
        particle_descriptor_layout_create_info.flags = 0;
        particle_descriptor_layout_create_info.bindingCount =
            sizeof(particle_layout_bindings) / sizeof(particle_layout_bindings[0]);
        particle_descriptor_layout_create_info.pBindings = particle_layout_bindings;

        if (RHI_SUCCESS !=
            m_Rhi->CreateDescriptorSetLayout(&particle_descriptor_layout_create_info, m_DescriptorInfos[0].layout))
        {
            throw std::runtime_error("setup particle compute Descriptor done");
        }
        LOG_INFO(ZParticle, "setup particle compute Descriptor done");
    }
    // scene depth and normal binding
    {
        RHIDescriptorSetLayoutBinding scene_global_layout_bindings[2] = {};

        RHIDescriptorSetLayoutBinding& gbuffer_normal_global_layout_input_attachment_binding =
            scene_global_layout_bindings[0];
        gbuffer_normal_global_layout_input_attachment_binding.binding = 0;
        gbuffer_normal_global_layout_input_attachment_binding.descriptorType = RHI_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        gbuffer_normal_global_layout_input_attachment_binding.descriptorCount = 1;
        gbuffer_normal_global_layout_input_attachment_binding.stageFlags = RHI_SHADER_STAGE_COMPUTE_BIT;

        RHIDescriptorSetLayoutBinding& gbuffer_depth_global_layout_input_attachment_binding =
            scene_global_layout_bindings[1];
        gbuffer_depth_global_layout_input_attachment_binding.binding = 1;
        gbuffer_depth_global_layout_input_attachment_binding.descriptorType =
            RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        gbuffer_depth_global_layout_input_attachment_binding.descriptorCount = 1;
        gbuffer_depth_global_layout_input_attachment_binding.stageFlags = RHI_SHADER_STAGE_COMPUTE_BIT;

        RHIDescriptorSetLayoutCreateInfo gbuffer_lighting_global_layout_create_info;
        gbuffer_lighting_global_layout_create_info.sType = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        gbuffer_lighting_global_layout_create_info.pNext = NULL;
        gbuffer_lighting_global_layout_create_info.flags = 0;
        gbuffer_lighting_global_layout_create_info.bindingCount =
            sizeof(scene_global_layout_bindings) / sizeof(scene_global_layout_bindings[0]);
        gbuffer_lighting_global_layout_create_info.pBindings = scene_global_layout_bindings;

        if (RHI_SUCCESS !=
            m_Rhi->CreateDescriptorSetLayout(&gbuffer_lighting_global_layout_create_info, m_DescriptorInfos[1].layout))
            throw std::runtime_error("create scene normal and depth global layout");
    }

    {
        RHIDescriptorSetLayoutBinding particlebillboard_global_layout_bindings[3];

        RHIDescriptorSetLayoutBinding& particlebillboard_global_layout_perframe_storage_buffer_binding =
            particlebillboard_global_layout_bindings[0];
        particlebillboard_global_layout_perframe_storage_buffer_binding.binding = 0;
        particlebillboard_global_layout_perframe_storage_buffer_binding.descriptorType =
            RHI_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        particlebillboard_global_layout_perframe_storage_buffer_binding.descriptorCount = 1;
        particlebillboard_global_layout_perframe_storage_buffer_binding.stageFlags = RHI_SHADER_STAGE_VERTEX_BIT;
        particlebillboard_global_layout_perframe_storage_buffer_binding.pImmutableSamplers = NULL;

        RHIDescriptorSetLayoutBinding& particlebillboard_global_layout_perdrawcall_storage_buffer_binding =
            particlebillboard_global_layout_bindings[1];
        particlebillboard_global_layout_perdrawcall_storage_buffer_binding.binding = 1;
        particlebillboard_global_layout_perdrawcall_storage_buffer_binding.descriptorType =
            RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        particlebillboard_global_layout_perdrawcall_storage_buffer_binding.descriptorCount = 1;
        particlebillboard_global_layout_perdrawcall_storage_buffer_binding.stageFlags = RHI_SHADER_STAGE_VERTEX_BIT;
        particlebillboard_global_layout_perdrawcall_storage_buffer_binding.pImmutableSamplers = NULL;

        RHIDescriptorSetLayoutBinding& particlebillboard_global_layout_texture_binding =
            particlebillboard_global_layout_bindings[2];
        particlebillboard_global_layout_texture_binding.binding = 2;
        particlebillboard_global_layout_texture_binding.descriptorType = RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        particlebillboard_global_layout_texture_binding.descriptorCount = 1;
        particlebillboard_global_layout_texture_binding.stageFlags = RHI_SHADER_STAGE_FRAGMENT_BIT;
        particlebillboard_global_layout_texture_binding.pImmutableSamplers = NULL;

        RHIDescriptorSetLayoutCreateInfo particlebillboard_global_layout_create_info;
        particlebillboard_global_layout_create_info.sType = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        particlebillboard_global_layout_create_info.pNext = NULL;
        particlebillboard_global_layout_create_info.flags = 0;
        particlebillboard_global_layout_create_info.bindingCount = 3;
        particlebillboard_global_layout_create_info.pBindings = particlebillboard_global_layout_bindings;

        if (RHI_SUCCESS != m_Rhi->CreateDescriptorSetLayout(&particlebillboard_global_layout_create_info,
                                                            m_DescriptorInfos[2].layout))
        {
            throw std::runtime_error("create particle billboard global layout");
        }
    }
}

void ParticlePass::SetupPipelines()
{
    m_RenderPipelines.resize(2);

    // compute pipeline
    {
        RHIDescriptorSetLayout* descriptorset_layouts[2] = {m_DescriptorInfos[0].layout, m_DescriptorInfos[1].layout};
        RHIPipelineLayoutCreateInfo pipeline_layout_create_info {};
        pipeline_layout_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipeline_layout_create_info.setLayoutCount = sizeof(descriptorset_layouts) / sizeof(descriptorset_layouts[0]);
        pipeline_layout_create_info.pSetLayouts = descriptorset_layouts;

        if (m_Rhi->CreatePipelineLayout(&pipeline_layout_create_info, m_RenderPipelines[0].layout) != RHI_SUCCESS)
            throw std::runtime_error("create compute pass pipe layout");
        LOG_INFO(ZParticle, "compute pipe layout done");
    }
    /*
    VkPipelineCache           pipelineCache;
    VkPipelineCacheCreateInfo pipelineCacheCreateInfo = {};
    pipelineCacheCreateInfo.sType                     = RHI_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    if (RHI_SUCCESS != vkCreatePipelineCache(m_VulkanRhi->m_Device, &pipelineCacheCreateInfo, nullptr,
    &pipelineCache))
    {
        throw std::runtime_error("create particle cache");
    }*/

    struct SpecializationData
    {
        uint32_t BUFFER_ELEMENT_COUNT = 32;
    } specializationData;

    VkSpecializationMapEntry specializationMapEntry {};
    specializationMapEntry.constantID = 0;
    specializationMapEntry.offset = 0;
    specializationMapEntry.size = sizeof(uint32_t);

    VkSpecializationInfo specializationInfo {};
    specializationInfo.mapEntryCount = 1;
    specializationInfo.pMapEntries = &specializationMapEntry;
    specializationInfo.dataSize = sizeof(specializationData);
    specializationInfo.pData = &specializationData;

    RHIComputePipelineCreateInfo computePipelineCreateInfo {};

    computePipelineCreateInfo.sType = RHI_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    computePipelineCreateInfo.layout = m_RenderPipelines[0].layout;
    computePipelineCreateInfo.flags = 0;

    RHIPipelineShaderStageCreateInfo shaderStage = {};
    shaderStage.sType = RHI_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStage.stage = RHI_SHADER_STAGE_COMPUTE_BIT;
    shaderStage.pName = "main";

    {
        LOG_INFO(ZParticle, "  [kickoff] createShaderModule begin (size = {})", PARTICLE_KICKOFF_COMP.size());
        shaderStage.module = m_Rhi->CreateShaderModule(PARTICLE_KICKOFF_COMP);
        shaderStage.pSpecializationInfo = nullptr;
        LOG_INFO(ZParticle, "  [kickoff] createShaderModule done (module = {})", static_cast<const void*>(shaderStage.module));
        assert(shaderStage.module != RHI_NULL_HANDLE);

        computePipelineCreateInfo.pStages = &shaderStage;
        LOG_INFO(ZParticle, "  [kickoff] createComputePipelines begin");
        if (RHI_SUCCESS != m_Rhi->CreateComputePipelines(
                               /*pipelineCache*/ nullptr, 1, &computePipelineCreateInfo, m_KickoffPipeline))
        {
            throw std::runtime_error("create particle kickoff pipe");
        }
        LOG_INFO(ZParticle, "  [kickoff] createComputePipelines done");
    }

    {
        LOG_INFO(ZParticle, "  [emit] createShaderModule begin (size = {})", PARTICLE_EMIT_COMP.size());
        shaderStage.module = m_Rhi->CreateShaderModule(PARTICLE_EMIT_COMP);
        shaderStage.pSpecializationInfo = nullptr;
        LOG_INFO(ZParticle, "  [emit] createShaderModule done");
        assert(shaderStage.module != RHI_NULL_HANDLE);

        computePipelineCreateInfo.pStages = &shaderStage;
        if (RHI_SUCCESS != m_Rhi->CreateComputePipelines(
                               /*pipelineCache*/ nullptr, 1, &computePipelineCreateInfo, m_EmitPipeline))
        {
            throw std::runtime_error("create particle emit pipe");
        }
        LOG_INFO(ZParticle, "  [emit] createComputePipelines done");
    }

    {
        LOG_INFO(ZParticle, "  [simulate] createShaderModule begin (size = {})", PARTICLE_SIMULATE_COMP.size());
        shaderStage.module = m_Rhi->CreateShaderModule(PARTICLE_SIMULATE_COMP);
        shaderStage.pSpecializationInfo = nullptr;
        LOG_INFO(ZParticle, "  [simulate] createShaderModule done");
        assert(shaderStage.module != RHI_NULL_HANDLE);

        computePipelineCreateInfo.pStages = &shaderStage;

        if (RHI_SUCCESS != m_Rhi->CreateComputePipelines(
                               /*pipelineCache*/ nullptr, 1, &computePipelineCreateInfo, m_SimulatePipeline))
        {
            throw std::runtime_error("create particle simulate pipe");
        }
        LOG_INFO(ZParticle, "  [simulate] createComputePipelines done");
    }

    LOG_INFO(ZParticle, "  particle compute pipelines all created; entering billboard graphics pipeline setup");

    // particle billboard
    {
        RHIDescriptorSetLayout* descriptorset_layouts[1] = {m_DescriptorInfos[2].layout};
        RHIPipelineLayoutCreateInfo pipeline_layout_create_info {};
        pipeline_layout_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipeline_layout_create_info.setLayoutCount = 1;
        pipeline_layout_create_info.pSetLayouts = descriptorset_layouts;

        if (m_Rhi->CreatePipelineLayout(&pipeline_layout_create_info, m_RenderPipelines[1].layout) != RHI_SUCCESS)
        {
            throw std::runtime_error("create particle billboard pipeline layout");
        }

        RHIShader* vert_shader_module = m_Rhi->CreateShaderModule(PARTICLEBILLBOARD_VERT);
        RHIShader* frag_shader_module = m_Rhi->CreateShaderModule(PARTICLEBILLBOARD_FRAG);

        RHIPipelineShaderStageCreateInfo vert_pipeline_shader_stage_create_info {};
        vert_pipeline_shader_stage_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        vert_pipeline_shader_stage_create_info.stage = RHI_SHADER_STAGE_VERTEX_BIT;
        vert_pipeline_shader_stage_create_info.module = vert_shader_module;
        vert_pipeline_shader_stage_create_info.pName = "main";

        RHIPipelineShaderStageCreateInfo frag_pipeline_shader_stage_create_info {};
        frag_pipeline_shader_stage_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        frag_pipeline_shader_stage_create_info.stage = RHI_SHADER_STAGE_FRAGMENT_BIT;
        frag_pipeline_shader_stage_create_info.module = frag_shader_module;
        frag_pipeline_shader_stage_create_info.pName = "main";

        RHIPipelineShaderStageCreateInfo shader_stages[] = {vert_pipeline_shader_stage_create_info,
                                                            frag_pipeline_shader_stage_create_info};

        RHIPipelineVertexInputStateCreateInfo vertex_input_state_create_info {};
        vertex_input_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertex_input_state_create_info.vertexBindingDescriptionCount = 0;
        vertex_input_state_create_info.pVertexBindingDescriptions = NULL;
        vertex_input_state_create_info.vertexAttributeDescriptionCount = 0;
        vertex_input_state_create_info.pVertexAttributeDescriptions = NULL;

        RHIPipelineInputAssemblyStateCreateInfo input_assembly_create_info {};
        input_assembly_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        input_assembly_create_info.topology = RHI_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        input_assembly_create_info.primitiveRestartEnable = RHI_FALSE;

        RHIPipelineViewportStateCreateInfo viewport_state_create_info {};
        viewport_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport_state_create_info.viewportCount = 1;
        viewport_state_create_info.pViewports = m_Rhi->GetSwapchainInfo().viewport;
        viewport_state_create_info.scissorCount = 1;
        viewport_state_create_info.pScissors = m_Rhi->GetSwapchainInfo().scissor;

        RHIPipelineRasterizationStateCreateInfo rasterization_state_create_info {};
        rasterization_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterization_state_create_info.depthClampEnable = RHI_FALSE;
        rasterization_state_create_info.rasterizerDiscardEnable = RHI_FALSE;
        rasterization_state_create_info.polygonMode = RHI_POLYGON_MODE_FILL;
        rasterization_state_create_info.lineWidth = 1.0f;
        rasterization_state_create_info.cullMode = RHI_CULL_MODE_NONE;
        rasterization_state_create_info.frontFace = RHI_FRONT_FACE_CLOCKWISE;
        rasterization_state_create_info.depthBiasEnable = RHI_FALSE;
        rasterization_state_create_info.depthBiasConstantFactor = 0.0f;
        rasterization_state_create_info.depthBiasClamp = 0.0f;
        rasterization_state_create_info.depthBiasSlopeFactor = 0.0f;

        RHIPipelineMultisampleStateCreateInfo multisample_state_create_info {};
        multisample_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample_state_create_info.sampleShadingEnable = RHI_FALSE;
        multisample_state_create_info.rasterizationSamples = RHI_SAMPLE_COUNT_1_BIT;

        RHIPipelineColorBlendAttachmentState color_blend_attachments[1] = {};
        color_blend_attachments[0].colorWriteMask = RHI_COLOR_COMPONENT_R_BIT | RHI_COLOR_COMPONENT_G_BIT |
                                                    RHI_COLOR_COMPONENT_B_BIT | RHI_COLOR_COMPONENT_A_BIT;
        color_blend_attachments[0].blendEnable = RHI_TRUE;
        color_blend_attachments[0].srcColorBlendFactor = RHI_BLEND_FACTOR_ONE;
        color_blend_attachments[0].dstColorBlendFactor = RHI_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        color_blend_attachments[0].colorBlendOp = RHI_BLEND_OP_ADD;
        color_blend_attachments[0].srcAlphaBlendFactor = RHI_BLEND_FACTOR_ONE;
        color_blend_attachments[0].dstAlphaBlendFactor = RHI_BLEND_FACTOR_ZERO;
        color_blend_attachments[0].alphaBlendOp = RHI_BLEND_OP_ADD;

        RHIPipelineColorBlendStateCreateInfo color_blend_state_create_info = {};
        color_blend_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        color_blend_state_create_info.logicOpEnable = RHI_FALSE;
        color_blend_state_create_info.logicOp = RHI_LOGIC_OP_COPY;
        color_blend_state_create_info.attachmentCount =
            sizeof(color_blend_attachments) / sizeof(color_blend_attachments[0]);
        color_blend_state_create_info.pAttachments = &color_blend_attachments[0];
        color_blend_state_create_info.blendConstants[0] = 0.0f;
        color_blend_state_create_info.blendConstants[1] = 0.0f;
        color_blend_state_create_info.blendConstants[2] = 0.0f;
        color_blend_state_create_info.blendConstants[3] = 0.0f;

        RHIPipelineDepthStencilStateCreateInfo depth_stencil_create_info {};
        depth_stencil_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depth_stencil_create_info.depthTestEnable = RHI_TRUE;
        depth_stencil_create_info.depthWriteEnable = RHI_FALSE;
        depth_stencil_create_info.depthCompareOp = RHI_COMPARE_OP_LESS;
        depth_stencil_create_info.depthBoundsTestEnable = RHI_FALSE;
        depth_stencil_create_info.stencilTestEnable = RHI_FALSE;

        RHIDynamicState dynamic_states[] = {RHI_DYNAMIC_STATE_VIEWPORT, RHI_DYNAMIC_STATE_SCISSOR};

        RHIPipelineDynamicStateCreateInfo dynamic_state_create_info {};
        dynamic_state_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic_state_create_info.dynamicStateCount = 2;
        dynamic_state_create_info.pDynamicStates = dynamic_states;

        RHIGraphicsPipelineCreateInfo pipelineInfo {};
        pipelineInfo.sType = RHI_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.stageCount = 2;
        pipelineInfo.pStages = shader_stages;
        pipelineInfo.pVertexInputState = &vertex_input_state_create_info;
        pipelineInfo.pInputAssemblyState = &input_assembly_create_info;
        pipelineInfo.pViewportState = &viewport_state_create_info;
        pipelineInfo.pRasterizationState = &rasterization_state_create_info;
        pipelineInfo.pMultisampleState = &multisample_state_create_info;
        pipelineInfo.pColorBlendState = &color_blend_state_create_info;
        pipelineInfo.pDepthStencilState = &depth_stencil_create_info;
        pipelineInfo.layout = m_RenderPipelines[1].layout;
        pipelineInfo.renderPass = m_RenderPass;
        pipelineInfo.subpass = _main_camera_subpass_forward_lighting;
        pipelineInfo.basePipelineHandle = RHI_NULL_HANDLE;
        pipelineInfo.pDynamicState = &dynamic_state_create_info;

        if (m_Rhi->CreateGraphicsPipelines(RHI_NULL_HANDLE, 1, &pipelineInfo, m_RenderPipelines[1].pipeline) !=
            RHI_SUCCESS)
        {
            throw std::runtime_error("create particle billboard graphics pipeline");
        }

        m_Rhi->DestroyShaderModule(vert_shader_module);
        m_Rhi->DestroyShaderModule(frag_shader_module);
    }
}

void ParticlePass::AllocateDescriptorSet()
{
    RHIDescriptorSetAllocateInfo particle_descriptor_set_alloc_info;
    particle_descriptor_set_alloc_info.sType = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    particle_descriptor_set_alloc_info.descriptorPool = m_Rhi->GetDescriptorPoor();

    m_DescriptorInfos.resize(3 * m_EmitterCount);
    for (int eid = 0; eid < m_EmitterCount; ++eid)
    {
        particle_descriptor_set_alloc_info.pSetLayouts = &m_DescriptorInfos[0].layout;
        particle_descriptor_set_alloc_info.descriptorSetCount = 1;
        particle_descriptor_set_alloc_info.pNext = NULL;

        if (RHI_SUCCESS != m_Rhi->AllocateDescriptorSets(&particle_descriptor_set_alloc_info,
                                                         m_DescriptorInfos[eid * 3].descriptor_set))
            throw std::runtime_error("allocate compute descriptor set");
        particle_descriptor_set_alloc_info.pSetLayouts = &m_DescriptorInfos[1].layout;
        particle_descriptor_set_alloc_info.descriptorSetCount = 1;
        particle_descriptor_set_alloc_info.pNext = NULL;

        if (RHI_SUCCESS != m_Rhi->AllocateDescriptorSets(&particle_descriptor_set_alloc_info,
                                                         m_DescriptorInfos[eid * 3 + 1].descriptor_set))
            LOG_INFO(ZParticle, "allocate normal and depth descriptor set done");
    }
}

void ParticlePass::UpdateDescriptorSet()
{
    // SRP-style lazy initialization: ensure texture resources are loaded when ParticleManager is ready
    EnsureAttachmentsInitialized();

    for (int eid = 0; eid < m_EmitterCount; ++eid)
    {
        // compute part
        {
            std::vector<RHIWriteDescriptorSet> computeWriteDescriptorSets {{}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}};

            RHIDescriptorBufferInfo uniformbufferDescriptor = {m_ComputeUniformBuffer, 0, RHI_WHOLE_SIZE};
            {
                RHIWriteDescriptorSet& descriptorset = computeWriteDescriptorSets[0];
                descriptorset.sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                descriptorset.dstSet = m_DescriptorInfos[eid * 3].descriptor_set;
                descriptorset.descriptorType = RHI_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                descriptorset.dstBinding = 0;
                descriptorset.pBufferInfo = &uniformbufferDescriptor;
                descriptorset.descriptorCount = 1;
            }

            RHIDescriptorBufferInfo positionBufferDescriptor = {
                m_EmitterBufferBatches[eid].m_PositionDeviceBuffer, 0, RHI_WHOLE_SIZE};
            {
                RHIWriteDescriptorSet& descriptorset = computeWriteDescriptorSets[1];
                descriptorset.sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                descriptorset.dstSet = m_DescriptorInfos[eid * 3].descriptor_set;
                descriptorset.descriptorType = RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                descriptorset.dstBinding = 1;
                descriptorset.pBufferInfo = &positionBufferDescriptor;
                descriptorset.descriptorCount = 1;
            }

            RHIDescriptorBufferInfo counterBufferDescriptor = {
                m_EmitterBufferBatches[eid].m_CounterDeviceBuffer, 0, RHI_WHOLE_SIZE};
            {
                RHIWriteDescriptorSet& descriptorset = computeWriteDescriptorSets[2];
                descriptorset.sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                descriptorset.dstSet = m_DescriptorInfos[eid * 3].descriptor_set;
                descriptorset.descriptorType = RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                descriptorset.dstBinding = 2;
                descriptorset.pBufferInfo = &counterBufferDescriptor;
                descriptorset.descriptorCount = 1;
            }

            RHIDescriptorBufferInfo indirectArgumentBufferDescriptor = {
                m_EmitterBufferBatches[eid].m_IndirectDispatchArgumentBuffer, 0, RHI_WHOLE_SIZE};
            {
                RHIWriteDescriptorSet& descriptorset = computeWriteDescriptorSets[3];
                descriptorset.sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                descriptorset.dstSet = m_DescriptorInfos[eid * 3].descriptor_set;
                descriptorset.descriptorType = RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                descriptorset.dstBinding = 3;
                descriptorset.pBufferInfo = &indirectArgumentBufferDescriptor;
                descriptorset.descriptorCount = 1;
            }

            RHIDescriptorBufferInfo aliveListBufferDescriptor = {
                m_EmitterBufferBatches[eid].m_AliveListBuffer, 0, RHI_WHOLE_SIZE};
            {
                RHIWriteDescriptorSet& descriptorset = computeWriteDescriptorSets[4];
                descriptorset.sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                descriptorset.dstSet = m_DescriptorInfos[eid * 3].descriptor_set;
                descriptorset.descriptorType = RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                descriptorset.dstBinding = 4;
                descriptorset.pBufferInfo = &aliveListBufferDescriptor;
                descriptorset.descriptorCount = 1;
            }

            RHIDescriptorBufferInfo deadListBufferDescriptor = {
                m_EmitterBufferBatches[eid].m_DeadListBuffer, 0, RHI_WHOLE_SIZE};
            {
                RHIWriteDescriptorSet& descriptorset = computeWriteDescriptorSets[5];
                descriptorset.sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                descriptorset.dstSet = m_DescriptorInfos[eid * 3].descriptor_set;
                descriptorset.descriptorType = RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                descriptorset.dstBinding = 5;
                descriptorset.pBufferInfo = &deadListBufferDescriptor;
                descriptorset.descriptorCount = 1;
            }

            RHIDescriptorBufferInfo aliveListNextBufferDescriptor = {
                m_EmitterBufferBatches[eid].m_AliveListNextBuffer, 0, RHI_WHOLE_SIZE};
            {
                RHIWriteDescriptorSet& descriptorset = computeWriteDescriptorSets[6];
                descriptorset.sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                descriptorset.dstSet = m_DescriptorInfos[eid * 3].descriptor_set;
                descriptorset.descriptorType = RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                descriptorset.dstBinding = 6;
                descriptorset.pBufferInfo = &aliveListNextBufferDescriptor;
                descriptorset.descriptorCount = 1;
            }

            RHIDescriptorBufferInfo particleComponentResBufferDescriptor = {
                m_EmitterBufferBatches[eid].m_ParticleComponentResBuffer, 0, RHI_WHOLE_SIZE};
            {
                RHIWriteDescriptorSet& descriptorset = computeWriteDescriptorSets[7];
                descriptorset.sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                descriptorset.dstSet = m_DescriptorInfos[eid * 3].descriptor_set;
                descriptorset.descriptorType = RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                descriptorset.dstBinding = 7;
                descriptorset.pBufferInfo = &particleComponentResBufferDescriptor;
                descriptorset.descriptorCount = 1;
            }

            RHIDescriptorBufferInfo particleSceneUniformBufferDescriptor = {m_SceneUniformBuffer, 0, RHI_WHOLE_SIZE};
            {
                RHIWriteDescriptorSet& descriptorset = computeWriteDescriptorSets[8];
                descriptorset.sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                descriptorset.dstSet = m_DescriptorInfos[eid * 3].descriptor_set;
                descriptorset.descriptorType = RHI_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                descriptorset.dstBinding = 8;
                descriptorset.pBufferInfo = &particleSceneUniformBufferDescriptor;
                descriptorset.descriptorCount = 1;
            }

            RHIDescriptorBufferInfo positionRenderbufferDescriptor = {
                m_EmitterBufferBatches[eid].m_PositionRenderBuffer, 0, RHI_WHOLE_SIZE};
            {
                RHIWriteDescriptorSet& descriptorset = computeWriteDescriptorSets[9];
                descriptorset.sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                descriptorset.dstSet = m_DescriptorInfos[eid * 3].descriptor_set;
                descriptorset.descriptorType = RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                descriptorset.dstBinding = 9;
                descriptorset.pBufferInfo = &positionRenderbufferDescriptor;
                descriptorset.descriptorCount = 1;
            }

            RHISampler* sampler;
            RHISamplerCreateInfo samplerCreateInfo {};
            samplerCreateInfo.sType = RHI_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
            samplerCreateInfo.maxAnisotropy = 1.0f;
            samplerCreateInfo.anisotropyEnable = true;
            samplerCreateInfo.magFilter = RHI_FILTER_LINEAR;
            samplerCreateInfo.minFilter = RHI_FILTER_LINEAR;
            samplerCreateInfo.mipmapMode = RHI_SAMPLER_MIPMAP_MODE_LINEAR;
            samplerCreateInfo.addressModeU = RHI_SAMPLER_ADDRESS_MODE_REPEAT;
            samplerCreateInfo.addressModeV = RHI_SAMPLER_ADDRESS_MODE_REPEAT;
            samplerCreateInfo.addressModeW = RHI_SAMPLER_ADDRESS_MODE_REPEAT;
            samplerCreateInfo.mipLodBias = 0.0f;
            samplerCreateInfo.compareOp = RHI_COMPARE_OP_NEVER;
            samplerCreateInfo.minLod = 0.0f;
            samplerCreateInfo.maxLod = 0.0f;
            samplerCreateInfo.borderColor = RHI_BORDER_COLOR_FLOAT_OPAQUE_WHITE;

            if (RHI_SUCCESS != m_Rhi->CreateSampler(&samplerCreateInfo, sampler))
            {
                throw std::runtime_error("create sampler error");
            }

            RHIDescriptorImageInfo zengine_texture_image_info = {};
            zengine_texture_image_info.sampler = sampler;
            zengine_texture_image_info.imageView = m_ZengineLogoTextureImageView;
            zengine_texture_image_info.imageLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            {
                RHIWriteDescriptorSet& descriptorset = computeWriteDescriptorSets[10];
                descriptorset.sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                descriptorset.dstSet = m_DescriptorInfos[eid * 3].descriptor_set;
                descriptorset.descriptorType = RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                descriptorset.dstBinding = 10;
                descriptorset.pImageInfo = &zengine_texture_image_info;
                descriptorset.descriptorCount = 1;
            }

            m_Rhi->UpdateDescriptorSets(
                static_cast<uint32_t>(computeWriteDescriptorSets.size()), computeWriteDescriptorSets.data(), 0, NULL);
        }
        {
            RHIWriteDescriptorSet descriptor_input_attachment_writes_info[2] = {{}, {}};

            RHIDescriptorImageInfo gbuffer_normal_descriptor_image_info = {};
            gbuffer_normal_descriptor_image_info.sampler = nullptr;
            gbuffer_normal_descriptor_image_info.imageView = m_SrcNormalImageView;
            gbuffer_normal_descriptor_image_info.imageLayout = RHI_IMAGE_LAYOUT_GENERAL;
            {
                RHIWriteDescriptorSet& gbuffer_normal_descriptor_input_attachment_write_info =
                    descriptor_input_attachment_writes_info[0];
                gbuffer_normal_descriptor_input_attachment_write_info.sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                gbuffer_normal_descriptor_input_attachment_write_info.pNext = NULL;
                gbuffer_normal_descriptor_input_attachment_write_info.dstSet =
                    m_DescriptorInfos[eid * 3 + 1].descriptor_set;
                gbuffer_normal_descriptor_input_attachment_write_info.dstBinding = 0;
                gbuffer_normal_descriptor_input_attachment_write_info.dstArrayElement = 0;
                gbuffer_normal_descriptor_input_attachment_write_info.descriptorType =
                    RHI_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                gbuffer_normal_descriptor_input_attachment_write_info.descriptorCount = 1;
                gbuffer_normal_descriptor_input_attachment_write_info.pImageInfo =
                    &gbuffer_normal_descriptor_image_info;
            }

            RHISampler* sampler;
            RHISamplerCreateInfo samplerCreateInfo {};
            samplerCreateInfo.sType = RHI_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
            samplerCreateInfo.maxAnisotropy = 1.0f;
            samplerCreateInfo.anisotropyEnable = true;
            samplerCreateInfo.magFilter = RHI_FILTER_NEAREST;
            samplerCreateInfo.minFilter = RHI_FILTER_NEAREST;
            samplerCreateInfo.mipmapMode = RHI_SAMPLER_MIPMAP_MODE_LINEAR;
            samplerCreateInfo.addressModeU = RHI_SAMPLER_ADDRESS_MODE_REPEAT;
            samplerCreateInfo.addressModeV = RHI_SAMPLER_ADDRESS_MODE_REPEAT;
            samplerCreateInfo.addressModeW = RHI_SAMPLER_ADDRESS_MODE_REPEAT;
            samplerCreateInfo.mipLodBias = 0.0f;
            samplerCreateInfo.compareOp = RHI_COMPARE_OP_NEVER;
            samplerCreateInfo.minLod = 0.0f;
            samplerCreateInfo.maxLod = 0.0f;
            samplerCreateInfo.borderColor = RHI_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
            if (RHI_SUCCESS != m_Rhi->CreateSampler(&samplerCreateInfo, sampler))
            {
                throw std::runtime_error("create sampler error");
            }

            RHIDescriptorImageInfo depth_descriptor_image_info = {};
            depth_descriptor_image_info.sampler = sampler;
            depth_descriptor_image_info.imageView = m_SrcDepthImageView;
            depth_descriptor_image_info.imageLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            {
                RHIWriteDescriptorSet& depth_descriptor_input_attachment_write_info =
                    descriptor_input_attachment_writes_info[1];
                depth_descriptor_input_attachment_write_info.sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                depth_descriptor_input_attachment_write_info.pNext = NULL;
                depth_descriptor_input_attachment_write_info.dstSet = m_DescriptorInfos[eid * 3 + 1].descriptor_set;
                depth_descriptor_input_attachment_write_info.dstBinding = 1;
                depth_descriptor_input_attachment_write_info.dstArrayElement = 0;
                depth_descriptor_input_attachment_write_info.descriptorType =
                    RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                depth_descriptor_input_attachment_write_info.descriptorCount = 1;
                depth_descriptor_input_attachment_write_info.pImageInfo = &depth_descriptor_image_info;
            }

            m_Rhi->UpdateDescriptorSets(sizeof(descriptor_input_attachment_writes_info) /
                                            sizeof(descriptor_input_attachment_writes_info[0]),
                                        descriptor_input_attachment_writes_info,
                                        0,
                                        NULL);
        }
    }
}

void ParticlePass::Simulate()
{
    for (auto i : m_EmitterTickIndices)
    {
        RHICommandBufferBeginInfo cmdBufInfo {};
        cmdBufInfo.sType = RHI_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

        // particle compute pass
        if (RHI_SUCCESS != m_Rhi->BeginCommandBuffer(m_ComputeCommandBuffer, &cmdBufInfo))
        {
            throw std::runtime_error("begin command buffer");
        }

        float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        m_Rhi->PushEvent(m_ComputeCommandBuffer, "Particle compute", color);
        m_Rhi->PushEvent(m_ComputeCommandBuffer, "Particle Kickoff", color);

        m_Rhi->CmdBindPipelinePFN(m_ComputeCommandBuffer, RHI_PIPELINE_BIND_POINT_COMPUTE, m_KickoffPipeline);
        RHIDescriptorSet* descriptorsets[2] = {m_DescriptorInfos[i * 3].descriptor_set,
                                               m_DescriptorInfos[i * 3 + 1].descriptor_set};
        m_Rhi->CmdBindDescriptorSetsPFN(m_ComputeCommandBuffer,
                                        RHI_PIPELINE_BIND_POINT_COMPUTE,
                                        m_RenderPipelines[0].layout,
                                        0,
                                        2,
                                        descriptorsets,
                                        0,
                                        0);

        m_Rhi->CmdDispatch(m_ComputeCommandBuffer, 1, 1, 1);

        m_Rhi->PopEvent(m_ComputeCommandBuffer);  // end particle kickoff label

        RHIBufferMemoryBarrier bufferBarrier {};
        bufferBarrier.sType = RHI_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        bufferBarrier.buffer = m_EmitterBufferBatches[i].m_CounterDeviceBuffer;
        bufferBarrier.size = RHI_WHOLE_SIZE;
        bufferBarrier.srcAccessMask = RHI_ACCESS_SHADER_WRITE_BIT;
        bufferBarrier.dstAccessMask = RHI_ACCESS_SHADER_READ_BIT;
        bufferBarrier.srcQueueFamilyIndex = RHI_QUEUE_FAMILY_IGNORED;
        bufferBarrier.dstQueueFamilyIndex = RHI_QUEUE_FAMILY_IGNORED;

        m_Rhi->CmdPipelineBarrier(m_ComputeCommandBuffer,
                                  RHI_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                  RHI_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                  0,
                                  0,
                                  nullptr,
                                  1,
                                  &bufferBarrier,
                                  0,
                                  nullptr);

        bufferBarrier.buffer = m_EmitterBufferBatches[i].m_IndirectDispatchArgumentBuffer;
        bufferBarrier.size = RHI_WHOLE_SIZE;
        bufferBarrier.srcAccessMask = RHI_ACCESS_SHADER_WRITE_BIT;
        bufferBarrier.dstAccessMask = RHI_ACCESS_INDIRECT_COMMAND_READ_BIT | RHI_ACCESS_SHADER_READ_BIT;
        bufferBarrier.srcQueueFamilyIndex = RHI_QUEUE_FAMILY_IGNORED;
        bufferBarrier.dstQueueFamilyIndex = RHI_QUEUE_FAMILY_IGNORED;

        m_Rhi->CmdPipelineBarrier(m_ComputeCommandBuffer,
                                  RHI_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                  RHI_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                  0,
                                  0,
                                  nullptr,
                                  1,
                                  &bufferBarrier,
                                  0,
                                  nullptr);

        m_Rhi->PushEvent(m_ComputeCommandBuffer, "Particle Emit", color);

        m_Rhi->CmdBindPipelinePFN(m_ComputeCommandBuffer, RHI_PIPELINE_BIND_POINT_COMPUTE, m_EmitPipeline);

        m_Rhi->CmdDispatchIndirect(m_ComputeCommandBuffer,
                                   m_EmitterBufferBatches[i].m_IndirectDispatchArgumentBuffer,
                                   s_ArgumentOffsetEmit);

        m_Rhi->PopEvent(m_ComputeCommandBuffer);  // end particle emit label

        bufferBarrier.buffer = m_EmitterBufferBatches[i].m_PositionDeviceBuffer;
        bufferBarrier.size = RHI_WHOLE_SIZE;
        bufferBarrier.srcAccessMask = RHI_ACCESS_SHADER_WRITE_BIT;
        bufferBarrier.dstAccessMask = RHI_ACCESS_SHADER_READ_BIT;
        bufferBarrier.srcQueueFamilyIndex = RHI_QUEUE_FAMILY_IGNORED;
        bufferBarrier.dstQueueFamilyIndex = RHI_QUEUE_FAMILY_IGNORED;

        m_Rhi->CmdPipelineBarrier(m_ComputeCommandBuffer,
                                  RHI_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                  RHI_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                  0,
                                  0,
                                  nullptr,
                                  1,
                                  &bufferBarrier,
                                  0,
                                  nullptr);

        bufferBarrier.buffer = m_EmitterBufferBatches[i].m_PositionRenderBuffer;
        bufferBarrier.size = RHI_WHOLE_SIZE;
        bufferBarrier.srcAccessMask = RHI_ACCESS_SHADER_WRITE_BIT;
        bufferBarrier.dstAccessMask = RHI_ACCESS_SHADER_READ_BIT;
        bufferBarrier.srcQueueFamilyIndex = RHI_QUEUE_FAMILY_IGNORED;
        bufferBarrier.dstQueueFamilyIndex = RHI_QUEUE_FAMILY_IGNORED;

        m_Rhi->CmdPipelineBarrier(m_ComputeCommandBuffer,
                                  RHI_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                  RHI_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                  0,
                                  0,
                                  nullptr,
                                  1,
                                  &bufferBarrier,
                                  0,
                                  nullptr);

        bufferBarrier.buffer = m_EmitterBufferBatches[i].m_CounterDeviceBuffer;
        bufferBarrier.size = RHI_WHOLE_SIZE;
        bufferBarrier.srcAccessMask = RHI_ACCESS_SHADER_WRITE_BIT;
        bufferBarrier.dstAccessMask = RHI_ACCESS_SHADER_READ_BIT;
        bufferBarrier.srcQueueFamilyIndex = RHI_QUEUE_FAMILY_IGNORED;
        bufferBarrier.dstQueueFamilyIndex = RHI_QUEUE_FAMILY_IGNORED;

        m_Rhi->CmdPipelineBarrier(m_ComputeCommandBuffer,
                                  RHI_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                  RHI_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                  0,
                                  0,
                                  nullptr,
                                  1,
                                  &bufferBarrier,
                                  0,
                                  nullptr);

        bufferBarrier.buffer = m_EmitterBufferBatches[i].m_AliveListBuffer;
        bufferBarrier.size = RHI_WHOLE_SIZE;
        bufferBarrier.srcAccessMask = RHI_ACCESS_SHADER_WRITE_BIT;
        bufferBarrier.dstAccessMask = RHI_ACCESS_SHADER_READ_BIT;
        bufferBarrier.srcQueueFamilyIndex = RHI_QUEUE_FAMILY_IGNORED;
        bufferBarrier.dstQueueFamilyIndex = RHI_QUEUE_FAMILY_IGNORED;

        m_Rhi->CmdPipelineBarrier(m_ComputeCommandBuffer,
                                  RHI_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                  RHI_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                  0,
                                  0,
                                  nullptr,
                                  1,
                                  &bufferBarrier,
                                  0,
                                  nullptr);

        bufferBarrier.buffer = m_EmitterBufferBatches[i].m_DeadListBuffer;
        bufferBarrier.size = RHI_WHOLE_SIZE;
        bufferBarrier.srcAccessMask = RHI_ACCESS_SHADER_WRITE_BIT;
        bufferBarrier.dstAccessMask = RHI_ACCESS_SHADER_READ_BIT;
        bufferBarrier.srcQueueFamilyIndex = RHI_QUEUE_FAMILY_IGNORED;
        bufferBarrier.dstQueueFamilyIndex = RHI_QUEUE_FAMILY_IGNORED;

        m_Rhi->CmdPipelineBarrier(m_ComputeCommandBuffer,
                                  RHI_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                  RHI_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                  0,
                                  0,
                                  nullptr,
                                  1,
                                  &bufferBarrier,
                                  0,
                                  nullptr);

        bufferBarrier.buffer = m_EmitterBufferBatches[i].m_AliveListNextBuffer;
        bufferBarrier.size = RHI_WHOLE_SIZE;
        bufferBarrier.srcAccessMask = RHI_ACCESS_SHADER_WRITE_BIT;
        bufferBarrier.dstAccessMask = RHI_ACCESS_SHADER_READ_BIT;
        bufferBarrier.srcQueueFamilyIndex = RHI_QUEUE_FAMILY_IGNORED;
        bufferBarrier.dstQueueFamilyIndex = RHI_QUEUE_FAMILY_IGNORED;

        m_Rhi->CmdPipelineBarrier(m_ComputeCommandBuffer,
                                  RHI_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                  RHI_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                  0,
                                  0,
                                  nullptr,
                                  1,
                                  &bufferBarrier,
                                  0,
                                  nullptr);

        m_Rhi->PushEvent(m_ComputeCommandBuffer, "Particle Simulate", color);

        m_Rhi->CmdBindPipelinePFN(m_ComputeCommandBuffer, RHI_PIPELINE_BIND_POINT_COMPUTE, m_SimulatePipeline);
        m_Rhi->CmdDispatchIndirect(m_ComputeCommandBuffer,
                                   m_EmitterBufferBatches[i].m_IndirectDispatchArgumentBuffer,
                                   s_ArgumentOffsetSimulate);

        m_Rhi->PopEvent(m_ComputeCommandBuffer);  // end particle simulate label

        if (RHI_SUCCESS != m_Rhi->EndCommandBuffer(m_ComputeCommandBuffer))
        {
            throw std::runtime_error("end command buffer");
        }
        m_Rhi->ResetFencesPFN(1, &m_Fence);

        RHISubmitInfo computeSubmitInfo {};
        computeSubmitInfo.sType = RHI_STRUCTURE_TYPE_SUBMIT_INFO;
        computeSubmitInfo.pWaitDstStageMask = 0;
        computeSubmitInfo.commandBufferCount = 1;
        computeSubmitInfo.pCommandBuffers = &m_ComputeCommandBuffer;

        if (RHI_SUCCESS != m_Rhi->QueueSubmit(m_Rhi->GetComputeQueue(), 1, &computeSubmitInfo, m_Fence))
        {
            throw std::runtime_error("compute queue submit");
        }

        if (RHI_SUCCESS != m_Rhi->WaitForFencesPFN(1, &m_Fence, RHI_TRUE, UINT64_MAX))
        {
            throw std::runtime_error("wait for fence");
        }

        if (RHI_SUCCESS != m_Rhi->BeginCommandBuffer(m_ComputeCommandBuffer, &cmdBufInfo))
        {
            throw std::runtime_error("begin command buffer");
        }

        m_Rhi->PushEvent(m_ComputeCommandBuffer, "Copy Particle Counter Buffer", color);

        // Barrier to ensure that shader writes are finished before buffer is read back from GPU
        bufferBarrier.srcAccessMask = RHI_ACCESS_SHADER_WRITE_BIT;
        bufferBarrier.dstAccessMask = RHI_ACCESS_TRANSFER_READ_BIT;
        bufferBarrier.buffer = m_EmitterBufferBatches[i].m_CounterDeviceBuffer;
        bufferBarrier.size = RHI_WHOLE_SIZE;
        bufferBarrier.srcQueueFamilyIndex = RHI_QUEUE_FAMILY_IGNORED;
        bufferBarrier.dstQueueFamilyIndex = RHI_QUEUE_FAMILY_IGNORED;

        m_Rhi->CmdPipelineBarrier(m_ComputeCommandBuffer,
                                  RHI_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                  RHI_PIPELINE_STAGE_TRANSFER_BIT,
                                  0,
                                  0,
                                  nullptr,
                                  1,
                                  &bufferBarrier,
                                  0,
                                  nullptr);
        // Read back to host visible buffer

        RHIBufferCopy copyRegion {};
        copyRegion.srcOffset = 0;
        copyRegion.dstOffset = 0;
        copyRegion.size = sizeof(ParticleCounter);

        m_Rhi->CmdCopyBuffer(m_ComputeCommandBuffer,
                             m_EmitterBufferBatches[i].m_CounterDeviceBuffer,
                             m_EmitterBufferBatches[i].m_CounterHostBuffer,
                             1,
                             &copyRegion);

        // Barrier to ensure that buffer copy is finished before host reading from it
        bufferBarrier.srcAccessMask = RHI_ACCESS_TRANSFER_WRITE_BIT;
        bufferBarrier.dstAccessMask = RHI_ACCESS_HOST_READ_BIT;
        bufferBarrier.buffer = m_EmitterBufferBatches[i].m_CounterHostBuffer;
        bufferBarrier.size = RHI_WHOLE_SIZE;
        bufferBarrier.srcQueueFamilyIndex = RHI_QUEUE_FAMILY_IGNORED;
        bufferBarrier.dstQueueFamilyIndex = RHI_QUEUE_FAMILY_IGNORED;

        m_Rhi->CmdPipelineBarrier(m_ComputeCommandBuffer,
                                  RHI_PIPELINE_STAGE_TRANSFER_BIT,
                                  RHI_PIPELINE_STAGE_HOST_BIT,
                                  0,
                                  0,
                                  nullptr,
                                  1,
                                  &bufferBarrier,
                                  0,
                                  nullptr);

        m_Rhi->PopEvent(m_ComputeCommandBuffer);  // end particle counter copy label

        m_Rhi->PopEvent(m_ComputeCommandBuffer);  // end particle compute label

        if (RHI_SUCCESS != m_Rhi->EndCommandBuffer(m_ComputeCommandBuffer))
        {
            throw std::runtime_error("end command buffer");
        }

        // Submit compute work
        m_Rhi->ResetFencesPFN(1, &m_Fence);
        computeSubmitInfo = {};
        const VkPipelineStageFlags waitStageMask = RHI_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        computeSubmitInfo.sType = RHI_STRUCTURE_TYPE_SUBMIT_INFO;
        computeSubmitInfo.pWaitDstStageMask = &waitStageMask;
        computeSubmitInfo.commandBufferCount = 1;
        computeSubmitInfo.pCommandBuffers = &m_ComputeCommandBuffer;

        if (RHI_SUCCESS != m_Rhi->QueueSubmit(m_Rhi->GetComputeQueue(), 1, &computeSubmitInfo, m_Fence))
        {
            throw std::runtime_error("compute queue submit");
        }

        if (RHI_SUCCESS != m_Rhi->WaitForFencesPFN(1, &m_Fence, RHI_TRUE, UINT64_MAX))
        {
            throw std::runtime_error("wait for fence");
        }

        m_Rhi->QueueWaitIdle(m_Rhi->GetComputeQueue());

        // Make device writes visible to the host
        void* mapped;
        m_Rhi->MapMemory(m_EmitterBufferBatches[i].m_CounterHostMemory, 0, RHI_WHOLE_SIZE, 0, &mapped);

        m_Rhi->InvalidateMappedMemoryRanges(
            nullptr, m_EmitterBufferBatches[i].m_CounterHostMemory, 0, RHI_WHOLE_SIZE);

        // Copy to output
        ParticleCounter counterNext {};
        memcpy(&counterNext, mapped, sizeof(ParticleCounter));
        m_Rhi->UnmapMemory(m_EmitterBufferBatches[i].m_CounterHostMemory);

        if constexpr (s_VerboseParticleAliveInfo)
            LOG_INFO(ZParticle,
                     "{} {} {} {}",
                     counterNext.dead_count,
                     counterNext.alive_count,
                     counterNext.alive_count_after_sim,
                     counterNext.emit_count);
        m_EmitterBufferBatches[i].m_NumParticle = counterNext.alive_count_after_sim;
    }
    m_EmitterTickIndices.clear();
    m_EmitterTransformIndices.clear();
}

void ParticlePass::PrepareUniformBuffer()
{
    RHIDeviceMemory* d_mem;
    m_Rhi->CreateBuffer(sizeof(m_ParticleCollisionPerframeStorageBufferObject),
                        RHI_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                        RHI_MEMORY_PROPERTY_HOST_VISIBLE_BIT | RHI_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        m_SceneUniformBuffer,
                        d_mem);

    if (RHI_SUCCESS != m_Rhi->MapMemory(d_mem, 0, RHI_WHOLE_SIZE, 0, &m_SceneUniformBufferMapped))
    {
        throw std::runtime_error("map billboard uniform buffer");
    }
    RHIDeviceMemory* d_uniformdmemory;

    m_Rhi->CreateBufferAndInitialize(RHI_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                     RHI_MEMORY_PROPERTY_HOST_VISIBLE_BIT | RHI_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                     m_ComputeUniformBuffer,
                                     d_uniformdmemory,
                                     sizeof(m_Ubo));

    if (RHI_SUCCESS != m_Rhi->MapMemory(d_uniformdmemory, 0, RHI_WHOLE_SIZE, 0, &m_ParticleComputeBufferMapped))
    {
        throw std::runtime_error("map buffer");
    }

    GlobalParticleRes* global_res = m_ParticleManager->GetGlobalParticleRes();

    m_Ubo.emit_gap = global_res->m_EmitGap;
    m_Ubo.time_step = global_res->m_TimeStep;
    m_Ubo.max_life = global_res->m_MaxLife;
    m_Ubo.gravity = global_res->m_Gravity;
    std::random_device r;
    std::seed_seq seed {r()};
    m_RandomEngine.seed(seed);
    float rnd0 = m_RandomEngine.uniformDistribution<float>(0, 1000) * 0.001f;
    float rnd1 = m_RandomEngine.uniformDistribution<float>(0, 1000) * 0.001f;
    float rnd2 = m_RandomEngine.uniformDistribution<float>(0, 1000) * 0.001f;
    m_Ubo.pack = Vector4 {rnd0, static_cast<float>(m_Rhi->GetCurrentFrameIndex()), rnd1, rnd2};
    m_Ubo.xemit_count = 100000;

    m_ViewportParams = *m_Rhi->GetSwapchainInfo().viewport;
    m_Ubo.viewport.x = m_ViewportParams.x;
    m_Ubo.viewport.y = m_ViewportParams.y;
    m_Ubo.viewport.z = m_ViewportParams.width;
    m_Ubo.viewport.w = m_ViewportParams.height;
    m_Ubo.extent.x = m_Rhi->GetSwapchainInfo().scissor->extent.width;
    m_Ubo.extent.y = m_Rhi->GetSwapchainInfo().scissor->extent.height;

    memcpy(m_ParticleComputeBufferMapped, &m_Ubo, sizeof(m_Ubo));

    {
        RHIDeviceMemory* d_mem;
        m_Rhi->CreateBuffer(sizeof(m_ParticlebillboardPerframeStorageBufferObject),
                            RHI_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                            RHI_MEMORY_PROPERTY_HOST_VISIBLE_BIT | RHI_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                            m_ParticleBillboardUniformBuffer,
                            d_mem);

        if (RHI_SUCCESS != m_Rhi->MapMemory(d_mem, 0, RHI_WHOLE_SIZE, 0, &m_ParticleBillboardUniformBufferMapped))
        {
            throw std::runtime_error("map billboard uniform buffer");
        }
    }
}

void ParticlePass::UpdateEmitterTransform()
{
    for (ParticleEmitterTransformDesc& transform_desc : m_EmitterTransformIndices)
    {
        int index = transform_desc.m_Id;
        m_EmitterBufferBatches[index].m_EmitterDesc.m_Position = transform_desc.m_Position;
        m_EmitterBufferBatches[index].m_EmitterDesc.m_Rotation = transform_desc.m_Rotation;

        memcpy(m_EmitterBufferBatches[index].m_EmitterDescMapped,
               &m_EmitterBufferBatches[index].m_EmitterDesc,
               sizeof(ParticleEmitterDesc));
    }
}

void ParticlePass::UpdateUniformBuffer(ViewportType viewport_type)
{
    std::random_device r;
    std::seed_seq seed {r()};
    m_RandomEngine.seed(seed);
    float rnd0 = m_RandomEngine.uniformDistribution<float>(0, 1000) * 0.001f;
    float rnd1 = m_RandomEngine.uniformDistribution<float>(0, 1000) * 0.001f;
    float rnd2 = m_RandomEngine.uniformDistribution<float>(0, 1000) * 0.001f;
    m_Ubo.pack = Vector4 {rnd0, rnd1, rnd2, static_cast<float>(GET_SYSTEM(RHI)->GetCurrentFrameIndex())};

    m_Ubo.viewport.x = static_cast<uint32_t>(m_ViewportParams.x);
    m_Ubo.viewport.y = static_cast<uint32_t>(m_ViewportParams.y);
    m_Ubo.viewport.z = static_cast<uint32_t>(m_ViewportParams.width);
    m_Ubo.viewport.w = static_cast<uint32_t>(m_ViewportParams.height);
    m_Ubo.extent.x = static_cast<uint32_t>(m_ViewportParams.width);
    m_Ubo.extent.y = static_cast<uint32_t>(m_ViewportParams.height);

    auto camera = GET_SYSTEM(RenderSystem)->GetRenderCamera(viewport_type);
    m_Ubo.extent.z = camera ? camera->m_Znear : 0.1f;
    m_Ubo.extent.w = camera ? camera->m_Zfar : 1000.0f;
    memcpy(m_ParticleComputeBufferMapped, &m_Ubo, sizeof(m_Ubo));
}

void ParticlePass::PreparePassData(std::shared_ptr<RenderResourceBase> render_resource)
{
    // SRP-style lazy initialization: ensure texture resources are loaded when ParticleManager is ready
    EnsureAttachmentsInitialized();

    const RenderResource* vulkan_resource = static_cast<const RenderResource*>(render_resource.get());
    if (vulkan_resource)
    {
        m_ParticleCollisionPerframeStorageBufferObjects =
            vulkan_resource->m_ParticleCollisionPerframeStorageBufferObjects;
        m_ParticleCollisionPerframeStorageBufferObject =
            vulkan_resource->m_ParticleCollisionPerframeStorageBufferObject;

        m_ParticlebillboardPerframeStorageBufferObjects =
            vulkan_resource->m_ParticlebillboardPerframeStorageBufferObjects;
        m_ParticlebillboardPerframeStorageBufferObject =
            vulkan_resource->m_ParticlebillboardPerframeStorageBufferObject;

        UpdateEmitterTransform();
    }
}

void ParticlePass::SetDepthAndNormalImage(RHIImage* depth_image, RHIImage* normal_image)
{
    m_SrcDepthImage = depth_image;
    m_SrcNormalImage = normal_image;
}

void ParticlePass::SetRenderCommandBufferHandle(RHICommandBuffer* command_buffer)
{
    m_RenderCommandBuffer = command_buffer;
}

void ParticlePass::SetRenderPassHandle(RHIRenderPass* render_pass)
{
    m_RenderPass = render_pass;
}

void ParticlePass::SetTickIndices(const std::vector<ParticleEmitterID>& tick_indices)
{
    m_EmitterTickIndices = tick_indices;
}

void ParticlePass::SetTransformIndices(const std::vector<ParticleEmitterTransformDesc>& transform_indices)
{
    m_EmitterTransformIndices = transform_indices;
}