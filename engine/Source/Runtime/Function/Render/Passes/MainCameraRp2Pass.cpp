#include "Runtime/Function/Render/Passes/MainCameraRp2Pass.h"

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Function/Render/Interface/DX12/DX12RHI.h"
#include "Runtime/Function/Render/Interface/RHI.h"
#include "Runtime/Function/Render/RenderResource.h"

#include <exception>
#include <string>
#include <vector>

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

    RHIPipeline* CreateFullscreenPostPipeline(RHI* rhi,
                                              RHIShader* vert,
                                              RHIShader* frag,
                                              RHIPipelineLayout* layout,
                                              RHIRenderPass* render_pass,
                                              uint32_t subpass_index)
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
        info.subpass = subpass_index;

        RHIPipeline* pipeline = nullptr;
        if (rhi->CreateGraphicsPipelines(RHI_NULL_HANDLE, 1, &info, pipeline) != RHI_SUCCESS)
        {
            return nullptr;
        }
        return pipeline;
    }
}  // namespace

RHIShader* MainCameraRp2Pass::LoadShader(const char* hlsl_relative_path, ShaderStage stage)
{
    const std::string full_path = GetRp2ShaderRoot() + "/hlsl/rp2/" + hlsl_relative_path;
    std::vector<uint8_t> binary;
    return m_Rhi->CreateShaderModuleFromFile(full_path, stage, {}, {}, binary);
}

bool MainCameraRp2Pass::Initialize(bool enable_fxaa)
{
    if (m_Initialized || m_Rhi == nullptr || m_FbResources == nullptr)
    {
        return false;
    }

    const auto render_resource = static_cast<RenderResource*>(m_RenderResource.get());
    m_GlobalRenderResource = render_resource ? &render_resource->m_GlobalRenderResource : nullptr;
    if (m_GlobalRenderResource == nullptr)
    {
        LOG_ERROR(ZRender,
                  "MainCameraRp2Pass::Initialize: GlobalRenderResource is null "
                  "(call SetCommonInfo with a RenderResource before Initialize)");
        return false;
    }

    m_EnableFxaa = enable_fxaa;
    m_DescriptorInfos.resize(_layout_count);
    m_RenderPipelines.resize(_pipeline_count);

    try
    {
        SetupDescriptorSetLayouts();
        SetupPipelines();
        SetupDescriptorSets();
        EnsureFallbackLutTexture();
        EnsureFallbackUiClearTexture();
        UpdateDescriptorBindings();
    }
    catch (const std::exception& ex)
    {
        LOG_ERROR(ZRender, "MainCameraRp2Pass::Initialize failed: {}", ex.what());
        Shutdown();
        return false;
    }

    if (m_RenderPipelines[_pipeline_color_grading].pipeline == nullptr ||
        m_RenderPipelines[_pipeline_combine_ui].pipeline == nullptr ||
        (m_EnableFxaa && m_RenderPipelines[_pipeline_fxaa].pipeline == nullptr))
    {
        LOG_ERROR(ZRender,
                  "MainCameraRp2Pass::Initialize: one or more RP2 pipelines failed to create "
                  "(color_grading={}, fxaa={}, combine_ui={})",
                  m_RenderPipelines[_pipeline_color_grading].pipeline != nullptr,
                  m_RenderPipelines[_pipeline_fxaa].pipeline != nullptr,
                  m_RenderPipelines[_pipeline_combine_ui].pipeline != nullptr);
        Shutdown();
        return false;
    }

    m_Initialized = true;
    LOG_INFO(ZRender, "MainCameraRp2Pass: initialized (fxaa={})", m_EnableFxaa ? "on" : "off");
    return true;
}

void MainCameraRp2Pass::Shutdown()
{
    if (!m_Initialized)
    {
        return;
    }

    if (m_FallbackLutView != nullptr && m_Rhi != nullptr)
    {
        m_Rhi->DestroyImageView(m_FallbackLutView);
        m_FallbackLutView = nullptr;
    }

    if (m_FallbackUiClearView != nullptr && m_Rhi != nullptr)
    {
        m_Rhi->DestroyImageView(m_FallbackUiClearView);
        m_FallbackUiClearView = nullptr;
    }

    m_RenderPipelines.clear();
    m_DescriptorInfos.clear();
    m_Initialized = false;
}

void MainCameraRp2Pass::EnsureFallbackLutTexture()
{
    if (m_GlobalRenderResource == nullptr || m_Rhi == nullptr)
    {
        return;
    }

    if (m_GlobalRenderResource->m_ColorGradingResource.m_ColorGradingLutTextureImageView != nullptr)
    {
        return;
    }

    RHIImage* image = nullptr;
    // Neutral LUT texel (DX12 color_grading.frag is passthrough; keep for Vulkan parity).
    const uint8_t neutral_lut[] = {128, 128, 128, 255};
    m_Rhi->CreateGlobalImage(image,
                             m_FallbackLutView,
                             nullptr,
                             1,
                             1,
                             const_cast<uint8_t*>(neutral_lut),
                             RHI_FORMAT_R8G8B8A8_UNORM,
                             1);
    m_FallbackSampler = m_Rhi->GetOrCreateDefaultSampler(Default_Sampler_Linear);
}

void MainCameraRp2Pass::EnsureFallbackUiClearTexture()
{
    if (m_FallbackUiClearView != nullptr || m_Rhi == nullptr)
    {
        return;
    }

    RHIImage* image = nullptr;
    const uint8_t clear_rgba[] = {0, 0, 0, 0};
    m_Rhi->CreateGlobalImage(image,
                             m_FallbackUiClearView,
                             nullptr,
                             1,
                             1,
                             const_cast<uint8_t*>(clear_rgba),
                             RHI_FORMAT_R8G8B8A8_UNORM,
                             1);
}

void MainCameraRp2Pass::SetupDescriptorSetLayouts()
{
    {
        RHIDescriptorSetLayoutBinding bindings[2] {};
        bindings[0].binding = 0;
        bindings[0].descriptorType = RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = RHI_SHADER_STAGE_FRAGMENT_BIT;
        bindings[1].binding = 1;
        bindings[1].descriptorType = RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = RHI_SHADER_STAGE_FRAGMENT_BIT;

        RHIDescriptorSetLayoutCreateInfo ci {};
        ci.sType = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ci.bindingCount = 2;
        ci.pBindings = bindings;
        if (m_Rhi->CreateDescriptorSetLayout(&ci, m_DescriptorInfos[_color_grading].layout) != RHI_SUCCESS)
        {
            throw std::runtime_error("MainCameraRp2Pass: color grading layout");
        }
    }

    {
        RHIDescriptorSetLayoutBinding bindings[1] {};
        bindings[0].binding = 0;
        bindings[0].descriptorType = RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = RHI_SHADER_STAGE_FRAGMENT_BIT;

        RHIDescriptorSetLayoutCreateInfo ci {};
        ci.sType = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ci.bindingCount = 1;
        ci.pBindings = bindings;
        if (m_Rhi->CreateDescriptorSetLayout(&ci, m_DescriptorInfos[_fxaa].layout) != RHI_SUCCESS)
        {
            throw std::runtime_error("MainCameraRp2Pass: fxaa layout");
        }
    }

    {
        RHIDescriptorSetLayoutBinding bindings[2] {};
        bindings[0].binding = 0;
        bindings[0].descriptorType = RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = RHI_SHADER_STAGE_FRAGMENT_BIT;
        bindings[1].binding = 1;
        bindings[1].descriptorType = RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = RHI_SHADER_STAGE_FRAGMENT_BIT;

        RHIDescriptorSetLayoutCreateInfo ci {};
        ci.sType = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ci.bindingCount = 2;
        ci.pBindings = bindings;
        if (m_Rhi->CreateDescriptorSetLayout(&ci, m_DescriptorInfos[_combine_ui].layout) != RHI_SUCCESS)
        {
            throw std::runtime_error("MainCameraRp2Pass: combine ui layout");
        }
    }
}

void MainCameraRp2Pass::SetupPipelines()
{
    // UE-style: use separate render passes for HDR (color_grading/fxaa) and LDR (combine_ui).
    // All new render passes are simple (no subpasses), so subpass index is always 0.

    RHIShader* post_vert = LoadShader("post_process.vert.hlsl", ShaderStage::Vertex);
    RHIShader* color_frag = LoadShader("color_grading.frag.hlsl", ShaderStage::Fragment);
    RHIShader* fxaa_frag = LoadShader("fxaa.frag.hlsl", ShaderStage::Fragment);
    RHIShader* combine_frag = LoadShader("combine_ui.frag.hlsl", ShaderStage::Fragment);

    if (post_vert == nullptr || color_frag == nullptr || fxaa_frag == nullptr || combine_frag == nullptr)
    {
        throw std::runtime_error("MainCameraRp2Pass: failed to load RP2 shaders");
    }

    // Color grading: uses HDR render pass (backup_odd/output is HDR).
    {
        RHIDescriptorSetLayout* layouts[] = {m_DescriptorInfos[_color_grading].layout};
        RHIPipelineLayoutCreateInfo pli {};
        pli.sType = RHI_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount = 1;
        pli.pSetLayouts = layouts;
        if (m_Rhi->CreatePipelineLayout(&pli, m_RenderPipelines[_pipeline_color_grading].layout) != RHI_SUCCESS)
        {
            throw std::runtime_error("MainCameraRp2Pass: color grading pipeline layout");
        }
        m_RenderPipelines[_pipeline_color_grading].pipeline = CreateFullscreenPostPipeline(
            m_Rhi,
            post_vert,
            color_frag,
            m_RenderPipelines[_pipeline_color_grading].layout,
            m_FbResources->getRp2HdrRenderPass(),  // HDR render pass
            0);  // No subpasses: index 0
    }

    // FXAA: uses HDR render pass (reads post_odd, writes backup_even, both HDR).
    {
        RHIDescriptorSetLayout* layouts[] = {m_DescriptorInfos[_fxaa].layout};
        RHIPipelineLayoutCreateInfo pli {};
        pli.sType = RHI_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount = 1;
        pli.pSetLayouts = layouts;
        if (m_Rhi->CreatePipelineLayout(&pli, m_RenderPipelines[_pipeline_fxaa].layout) != RHI_SUCCESS)
        {
            throw std::runtime_error("MainCameraRp2Pass: fxaa pipeline layout");
        }
        m_RenderPipelines[_pipeline_fxaa].pipeline = CreateFullscreenPostPipeline(
            m_Rhi,
            post_vert,
            fxaa_frag,
            m_RenderPipelines[_pipeline_fxaa].layout,
            m_FbResources->getRp2HdrRenderPass(),  // HDR render pass
            0);  // No subpasses: index 0
    }

    // Combine UI: uses LDR render pass (writes to swapchain, LDR format).
    {
        RHIDescriptorSetLayout* layouts[] = {m_DescriptorInfos[_combine_ui].layout};
        RHIPipelineLayoutCreateInfo pli {};
        pli.sType = RHI_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount = 1;
        pli.pSetLayouts = layouts;
        if (m_Rhi->CreatePipelineLayout(&pli, m_RenderPipelines[_pipeline_combine_ui].layout) != RHI_SUCCESS)
        {
            throw std::runtime_error("MainCameraRp2Pass: combine ui pipeline layout");
        }
        m_RenderPipelines[_pipeline_combine_ui].pipeline = CreateFullscreenPostPipeline(
            m_Rhi,
            post_vert,
            combine_frag,
            m_RenderPipelines[_pipeline_combine_ui].layout,
            m_FbResources->getRp2LdrRenderPass(),  // LDR render pass
            0);  // No subpasses: index 0
    }

    m_Rhi->DestroyShaderModule(post_vert);
    m_Rhi->DestroyShaderModule(color_frag);
    m_Rhi->DestroyShaderModule(fxaa_frag);
    m_Rhi->DestroyShaderModule(combine_frag);
}

void MainCameraRp2Pass::SetupDescriptorSets()
{
    auto alloc_one = [&](LayoutType layout) {
        RHIDescriptorSetAllocateInfo ai {};
        ai.sType = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = m_Rhi->GetDescriptorPoor();
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &m_DescriptorInfos[layout].layout;
        if (m_Rhi->AllocateDescriptorSets(&ai, m_DescriptorInfos[layout].descriptor_set) != RHI_SUCCESS)
        {
            throw std::runtime_error("MainCameraRp2Pass: allocate descriptor set");
        }
    };

    alloc_one(_color_grading);
    alloc_one(_fxaa);
    alloc_one(_combine_ui);
}

void MainCameraRp2Pass::RefreshColorGradingDescriptorBindings()
{
    if (m_Initialized)
    {
        UpdateDescriptorBindings();
    }
}

void MainCameraRp2Pass::UpdateDescriptorBindings()
{
    if (m_FbResources == nullptr)
    {
        LOG_ERROR(ZRender, "UpdateDescriptorBindings: m_FbResources is null");
        return;
    }

    const auto& attachments = m_FbResources->getAttachments();
    RHISampler* nearest = m_Rhi->GetOrCreateDefaultSampler(Default_Sampler_Nearest);
    RHISampler* linear = m_Rhi->GetOrCreateDefaultSampler(Default_Sampler_Linear);

    RHIImageView* backup_even = attachments[_main_camera_pass_backup_buffer_even].view;
    RHIImageView* backup_odd = attachments[_main_camera_pass_backup_buffer_odd].view;
    RHIImageView* post_odd = attachments[_main_camera_pass_post_process_buffer_odd].view;

    RHIImageView* lut_view =
        m_GlobalRenderResource != nullptr &&
                m_GlobalRenderResource->m_ColorGradingResource.m_ColorGradingLutTextureImageView != nullptr
            ? m_GlobalRenderResource->m_ColorGradingResource.m_ColorGradingLutTextureImageView
            : m_FallbackLutView;
    RHISampler* lut_sampler = m_FallbackSampler != nullptr ? m_FallbackSampler : linear;

    {
        RHIDescriptorImageInfo in_color {};
        in_color.sampler = nearest;
        // FIX: Read from backup_odd (RP1 output: deferred lighting + sky).
        // Previously read backup_even which contained stale/black data —
        // RP1 writes its final result to backup_odd, so RP2 must sample it.
        in_color.imageView = backup_odd;
        in_color.imageLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        RHIDescriptorImageInfo lut {};
        lut.sampler = lut_sampler;
        lut.imageView = lut_view;
        lut.imageLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        RHIWriteDescriptorSet writes[2] {};
        writes[0].sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = m_DescriptorInfos[_color_grading].descriptor_set;
        writes[0].dstBinding = 0;
        writes[0].descriptorType = RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].descriptorCount = 1;
        writes[0].pImageInfo = &in_color;

        writes[1] = writes[0];
        writes[1].dstBinding = 1;
        writes[1].pImageInfo = &lut;
        writes[1].pImageInfo = &lut;

        m_Rhi->UpdateDescriptorSets(2, writes, 0, nullptr);
    }

    {
        // FXAA input: when enabled, reads post_odd (CG writes there).
        // When disabled, this descriptor is unused but keep it valid.
        RHIImageView* fxaa_input = m_EnableFxaa ? post_odd : backup_even;
        RHIDescriptorImageInfo in_tex {};
        in_tex.sampler = linear;
        in_tex.imageView = fxaa_input;
        in_tex.imageLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        RHIWriteDescriptorSet write {};
        write.sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = m_DescriptorInfos[_fxaa].descriptor_set;
        write.dstBinding = 0;
        write.descriptorType = RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1;
        write.pImageInfo = &in_tex;
        m_Rhi->UpdateDescriptorSets(1, &write, 0, nullptr);
    }

    {
        // Combine UI reads the Color Grading output (backup_even when FXAA disabled,
        // backup_even when FXAA enabled) as the scene color, and the UI layer.
        RHIImageView* scene_source = m_EnableFxaa ? backup_even : backup_even;
        RHIDescriptorImageInfo scene {};
        scene.sampler = nearest;
        scene.imageView = scene_source;
        scene.imageLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        // in_ui_color reads the UI layer.
        // On DX12 editor: use a clear (black+transparent) fallback view (editor paints
        // UI on the swapchain after RP2 via ImGui overlay, not via legacy UIPass).
        RHIDescriptorImageInfo ui {};
        ui.sampler = nearest;
        ui.imageLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        if (m_Rhi->getGraphicsAPI() == GraphicsAPI::DirectX12 && m_FallbackUiClearView != nullptr)
        {
            ui.imageView = m_FallbackUiClearView;
        }
        else
        {
            ui.imageView = backup_even;
        }

        RHIWriteDescriptorSet writes[2] {};
        writes[0].sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = m_DescriptorInfos[_combine_ui].descriptor_set;
        writes[0].dstBinding = 0;
        writes[0].descriptorType = RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].descriptorCount = 1;
        writes[0].pImageInfo = &scene;

        writes[1] = writes[0];
        writes[1].dstBinding = 1;
        writes[1].pImageInfo = &ui;

        m_Rhi->UpdateDescriptorSets(2, writes, 0, nullptr);
    }
}

void MainCameraRp2Pass::UpdateAfterFramebufferRecreate()
{
    UpdateDescriptorBindings();
}

void MainCameraRp2Pass::DrawColorGrading()
{
    if (m_RenderPipelines[_pipeline_color_grading].pipeline == nullptr)
    {
        return;
    }

    m_Rhi->CmdBindPipelinePFN(m_Rhi->GetCurrentCommandBuffer(),
                              RHI_PIPELINE_BIND_POINT_GRAPHICS,
                              m_RenderPipelines[_pipeline_color_grading].pipeline);

    const auto extent = m_Rhi->GetSwapchainInfo().extent;
    RHIViewport viewport {0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f};
    RHIRect2D scissor {0, 0, extent.width, extent.height};
    m_Rhi->CmdSetViewportPFN(m_Rhi->GetCurrentCommandBuffer(), 0, 1, &viewport);
    m_Rhi->CmdSetScissorPFN(m_Rhi->GetCurrentCommandBuffer(), 0, 1, &scissor);

    m_Rhi->CmdBindDescriptorSetsPFN(m_Rhi->GetCurrentCommandBuffer(),
                                    RHI_PIPELINE_BIND_POINT_GRAPHICS,
                                    m_RenderPipelines[_pipeline_color_grading].layout,
                                    0,
                                    1,
                                    &m_DescriptorInfos[_color_grading].descriptor_set,
                                    0,
                                    nullptr);
    
    m_Rhi->CmdDraw(m_Rhi->GetCurrentCommandBuffer(), 3, 1, 0, 0);
}

void MainCameraRp2Pass::DrawFxaa()
{
    if (!m_EnableFxaa || m_RenderPipelines[_pipeline_fxaa].pipeline == nullptr)
    {
        return;
    }

    m_Rhi->CmdBindPipelinePFN(m_Rhi->GetCurrentCommandBuffer(),
                              RHI_PIPELINE_BIND_POINT_GRAPHICS,
                              m_RenderPipelines[_pipeline_fxaa].pipeline);

    const auto extent = m_Rhi->GetSwapchainInfo().extent;
    RHIViewport viewport {0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f};
    RHIRect2D scissor {0, 0, extent.width, extent.height};
    m_Rhi->CmdSetViewportPFN(m_Rhi->GetCurrentCommandBuffer(), 0, 1, &viewport);
    m_Rhi->CmdSetScissorPFN(m_Rhi->GetCurrentCommandBuffer(), 0, 1, &scissor);

    m_Rhi->CmdBindDescriptorSetsPFN(m_Rhi->GetCurrentCommandBuffer(),
                                    RHI_PIPELINE_BIND_POINT_GRAPHICS,
                                    m_RenderPipelines[_pipeline_fxaa].layout,
                                    0,
                                    1,
                                    &m_DescriptorInfos[_fxaa].descriptor_set,
                                    0,
                                    nullptr);
    m_Rhi->CmdDraw(m_Rhi->GetCurrentCommandBuffer(), 3, 1, 0, 0);
}

void MainCameraRp2Pass::DrawCombineUi()
{
    if (m_RenderPipelines[_pipeline_combine_ui].pipeline == nullptr)
    {
        return;
    }

    m_Rhi->CmdBindPipelinePFN(m_Rhi->GetCurrentCommandBuffer(),
                              RHI_PIPELINE_BIND_POINT_GRAPHICS,
                              m_RenderPipelines[_pipeline_combine_ui].pipeline);

    const auto extent = m_Rhi->GetSwapchainInfo().extent;
    RHIViewport viewport {0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f};
    RHIRect2D scissor {0, 0, extent.width, extent.height};
    m_Rhi->CmdSetViewportPFN(m_Rhi->GetCurrentCommandBuffer(), 0, 1, &viewport);
    m_Rhi->CmdSetScissorPFN(m_Rhi->GetCurrentCommandBuffer(), 0, 1, &scissor);

    m_Rhi->CmdBindDescriptorSetsPFN(m_Rhi->GetCurrentCommandBuffer(),
                                    RHI_PIPELINE_BIND_POINT_GRAPHICS,
                                    m_RenderPipelines[_pipeline_combine_ui].layout,
                                    0,
                                    1,
                                    &m_DescriptorInfos[_combine_ui].descriptor_set,
                                    0,
                                    nullptr);
    m_Rhi->CmdDraw(m_Rhi->GetCurrentCommandBuffer(), 3, 1, 0, 0);
}

void MainCameraRp2Pass::DrawRP2(uint32_t swapchain_image_index,
                                const std::vector<std::function<void()>>& post_ui_callbacks)
{
    if (!m_Initialized || m_FbResources == nullptr)
    {
        return;
    }

    RHICommandBuffer* cmd = m_Rhi->GetCurrentCommandBuffer();
    const auto& extent = m_Rhi->GetSwapchainInfo().extent;
    float debug_color[4] = {0.8f, 0.6f, 1.0f, 1.0f};

    // DX12 per-frame descriptor fix: UpdateDescriptorBindings() allocates
    // fresh per-frame SRV slots from the CBV/SRV/UAV heap.  Without this
    // call, the ColorGrading pass (Step 0) would use stale GPU handles
    // from a previous frame's partition — WaitForFences resets the
    // per-frame counter, so old handles point to overwritten slots and
    // the shader reads zeros (black screen).
    UpdateDescriptorBindings();

    // =========================================================================
    // UE-style: independent passes with explicit barriers.
    // Each step is a separate BeginRenderPass/EndRenderPass pair.
    // =========================================================================

    // ---- Step 0: Color Grading ----
    // Reads:  backup_odd (RP1 HDR output: deferred lighting + sky, as SRV via descriptor)
    // Writes: backup_even (or post_odd if FXAA enabled, as RTV)
    // Uses HDR render pass (R16G16B16A16_SFLOAT format).
    // NOTE: SRV and RTV MUST be different textures because loadOp=CLEAR
    //       would zero the RTV texture before the fragment shader samples it via SRV.
    {
        RHIRenderPassBeginInfo begin_info {};
        begin_info.sType = RHI_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        begin_info.renderPass = m_FbResources->getRp2HdrRenderPass();  // HDR render pass
        begin_info.framebuffer = m_FbResources->getRp2ColorGradingFramebuffer();
        begin_info.renderArea.offset = {0, 0};
        begin_info.renderArea.extent = extent;
        RHIClearValue clear_values[1] {};
        clear_values[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
        begin_info.clearValueCount = 1;
        begin_info.pClearValues = clear_values;

        m_Rhi->CmdBeginRenderPassPFN(cmd, &begin_info, RHI_SUBPASS_CONTENTS_INLINE);
        m_Rhi->PushEvent(cmd, "Color Grading", debug_color);
        DrawColorGrading();
        m_Rhi->PopEvent(cmd);
        m_Rhi->CmdEndRenderPassPFN(cmd);
    }

    // Barrier: color_grading output (backup_even or post_odd) is already in
    // SHADER_READ_ONLY_OPTIMAL thanks to finalLayout. Only need a memory
    // visibility barrier (no layout transition).
    {
        uint32_t output_att =
            m_EnableFxaa ? _main_camera_pass_post_process_buffer_odd
                          : _main_camera_pass_backup_buffer_even;
        RHIImage* img = m_FbResources->getAttachmentImage(output_att);
        if (img != nullptr)
        {
            RHIImageMemoryBarrier barrier {};
            barrier.sType = RHI_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.pNext = nullptr;
            barrier.srcAccessMask = RHI_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            barrier.dstAccessMask = RHI_ACCESS_SHADER_READ_BIT;
            barrier.oldLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;  // already transitioned by finalLayout
            barrier.newLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;  // no layout change
            barrier.srcQueueFamilyIndex = RHI_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = RHI_QUEUE_FAMILY_IGNORED;
            barrier.image = img;
            RHIImageSubresourceRange range {};
            range.aspectMask = RHI_IMAGE_ASPECT_COLOR_BIT;
            range.baseMipLevel = 0;
            range.levelCount = 1;
            range.baseArrayLayer = 0;
            range.layerCount = 1;
            barrier.subresourceRange = range;

            m_Rhi->CmdPipelineBarrier(cmd,
                                       RHI_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                       RHI_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                       0,
                                       0, nullptr,
                                       0, nullptr,
                                       1, &barrier);
        }
    }

    // ---- Step 1: FXAA (optional) ----
    // Uses HDR render pass (reads post_odd, writes backup_even, both HDR format).
    if (m_EnableFxaa && m_RenderPipelines[_pipeline_fxaa].pipeline != nullptr)
    {
        RHIRenderPassBeginInfo begin_info {};
        begin_info.sType = RHI_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        begin_info.renderPass = m_FbResources->getRp2HdrRenderPass();  // HDR render pass
        begin_info.framebuffer = m_FbResources->getRp2FxaaFramebuffer();
        begin_info.renderArea.offset = {0, 0};
        begin_info.renderArea.extent = extent;
        RHIClearValue clear_values[1] {};
        clear_values[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
        begin_info.clearValueCount = 1;
        begin_info.pClearValues = clear_values;

        m_Rhi->CmdBeginRenderPassPFN(cmd, &begin_info, RHI_SUBPASS_CONTENTS_INLINE);
        m_Rhi->PushEvent(cmd, "FXAA", debug_color);
        DrawFxaa();
        m_Rhi->PopEvent(cmd);
        m_Rhi->CmdEndRenderPassPFN(cmd);
    }

    // Barrier: FXAA output (backup_even) is already in SHADER_READ_ONLY_OPTIMAL
    // thanks to finalLayout. Only need a memory visibility barrier.
    if (m_EnableFxaa)
    {
        RHIImage* img = m_FbResources->getAttachmentImage(_main_camera_pass_backup_buffer_even);
        if (img != nullptr)
        {
            RHIImageMemoryBarrier barrier {};
            barrier.sType = RHI_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.pNext = nullptr;
            barrier.srcAccessMask = RHI_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            barrier.dstAccessMask = RHI_ACCESS_SHADER_READ_BIT;
            barrier.oldLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;  // already transitioned by finalLayout
            barrier.newLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;  // no layout change
            barrier.srcQueueFamilyIndex = RHI_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = RHI_QUEUE_FAMILY_IGNORED;
            barrier.image = img;
            RHIImageSubresourceRange range {};
            range.aspectMask = RHI_IMAGE_ASPECT_COLOR_BIT;
            range.baseMipLevel = 0;
            range.levelCount = 1;
            range.baseArrayLayer = 0;
            range.layerCount = 1;
            barrier.subresourceRange = range;

            m_Rhi->CmdPipelineBarrier(cmd,
                                       RHI_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                       RHI_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                       0,
                                       0, nullptr,
                                       0, nullptr,
                                       1, &barrier);
        }
    }

    // ---- Step 2: UI Clear + Draw ----
    // On DX12, clear backup_even (UI layer) to transparent black.
    // The editor paints UI on the swapchain after RP2, so we skip legacy UIPass.
    // We do this BEFORE combine_ui by writing to backup_even in a small pass,
    // or just clear it as a sub-step. For simplicity, we clear it via a dedicated
    // render pass (or skip if editor overlay handles it).
    // For now, we just draw the legacy UI pass if not on DX12.
    const bool skip_legacy_ui_draw =
        m_Rhi != nullptr && m_Rhi->getGraphicsAPI() == GraphicsAPI::DirectX12;
    if (!skip_legacy_ui_draw && m_UiPass != nullptr)
    {
        // Use a simple pass to clear backup_even, then draw UI.
        // For simplicity, reuse the combine_ui framebuffer but with a clear.
        // Actually, let's just call m_UiPass->Draw() which uses its own render pass.
        m_UiPass->Draw();
    }

    for (const std::function<void()>& callback : post_ui_callbacks)
    {
        if (callback)
        {
            callback();
        }
    }

    // ---- Step 3: Combine UI ----
    // Reads:  backup_odd (scene, as SRV) + backup_even (UI, as SRV)
    // Writes: swapchain image (as RTV)
    // Uses LDR render pass (swapchain format R8G8B8A8_UNORM).
    {
        const auto& swapchain_fbs = m_FbResources->getRP2Framebuffers();
        if (swapchain_image_index >= swapchain_fbs.size() ||
            swapchain_fbs[swapchain_image_index] == nullptr)
        {
            return;
        }

        // Update descriptor bindings so combine_ui reads the latest odd/even contents.
        UpdateDescriptorBindings();

        RHIRenderPassBeginInfo begin_info {};
        begin_info.sType = RHI_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        begin_info.renderPass = m_FbResources->getRp2LdrRenderPass();  // LDR render pass
        begin_info.framebuffer = swapchain_fbs[swapchain_image_index];
        begin_info.renderArea.offset = {0, 0};
        begin_info.renderArea.extent = extent;
        RHIClearValue clear_values[1] {};
        clear_values[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
        begin_info.clearValueCount = 1;
        begin_info.pClearValues = clear_values;

        m_Rhi->CmdBeginRenderPassPFN(cmd, &begin_info, RHI_SUBPASS_CONTENTS_INLINE);
        m_Rhi->PushEvent(cmd, "Combine UI", debug_color);
        DrawCombineUi();
        m_Rhi->PopEvent(cmd);
        m_Rhi->CmdEndRenderPassPFN(cmd);
    }
}
