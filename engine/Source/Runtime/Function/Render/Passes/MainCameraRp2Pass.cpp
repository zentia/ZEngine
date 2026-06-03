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
    RHIRenderPass* rp2 = m_FbResources->getRP2RenderPass();

    RHIShader* post_vert = LoadShader("post_process.vert.hlsl", ShaderStage::Vertex);
    RHIShader* color_frag = LoadShader("color_grading.frag.hlsl", ShaderStage::Fragment);
    RHIShader* fxaa_frag = LoadShader("fxaa.frag.hlsl", ShaderStage::Fragment);
    RHIShader* combine_frag = LoadShader("combine_ui.frag.hlsl", ShaderStage::Fragment);

    if (post_vert == nullptr || color_frag == nullptr || fxaa_frag == nullptr || combine_frag == nullptr)
    {
        throw std::runtime_error("MainCameraRp2Pass: failed to load RP2 shaders");
    }

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
            rp2,
            _main_camera_subpass_color_grading);
    }

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
        m_RenderPipelines[_pipeline_fxaa].pipeline = CreateFullscreenPostPipeline(m_Rhi,
                                                                                  post_vert,
                                                                                  fxaa_frag,
                                                                                  m_RenderPipelines[_pipeline_fxaa].layout,
                                                                                  rp2,
                                                                                  _main_camera_subpass_fxaa);
    }

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
            rp2,
            _main_camera_subpass_combine_ui);
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
        in_color.imageView = backup_even;
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
        RHIDescriptorImageInfo scene {};
        scene.sampler = nearest;
        scene.imageView = backup_odd;
        scene.imageLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        RHIDescriptorImageInfo ui {};
        ui.sampler = nearest;
        ui.imageView = backup_even;
        ui.imageLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

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

    const auto& swapchain_fbs = m_FbResources->getRP2Framebuffers();
    if (swapchain_image_index >= swapchain_fbs.size() || swapchain_fbs[swapchain_image_index] == nullptr)
    {
        return;
    }

    constexpr float k_scene_clear_r = 0.29f;
    constexpr float k_scene_clear_g = 0.345f;
    constexpr float k_scene_clear_b = 0.435f;

    RHIRenderPassBeginInfo begin_info {};
    begin_info.sType = RHI_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    begin_info.renderPass = m_FbResources->getRP2RenderPass();
    begin_info.framebuffer = swapchain_fbs[swapchain_image_index];
    begin_info.renderArea.offset = {0, 0};
    begin_info.renderArea.extent = m_Rhi->GetSwapchainInfo().extent;

    RHIClearValue clear_values[4];
    clear_values[0].color = {{k_scene_clear_r, k_scene_clear_g, k_scene_clear_b, 1.0f}};
    clear_values[1].color = {{k_scene_clear_r, k_scene_clear_g, k_scene_clear_b, 1.0f}};
    clear_values[2].color = {{k_scene_clear_r, k_scene_clear_g, k_scene_clear_b, 1.0f}};
    clear_values[3].color = {{k_scene_clear_r, k_scene_clear_g, k_scene_clear_b, 1.0f}};
    begin_info.clearValueCount = 4;
    begin_info.pClearValues = clear_values;

    RHICommandBuffer* cmd = m_Rhi->GetCurrentCommandBuffer();
    m_Rhi->CmdBeginRenderPassPFN(cmd, &begin_info, RHI_SUBPASS_CONTENTS_INLINE);

    float color[4] = {0.8f, 0.6f, 1.0f, 1.0f};
    m_Rhi->PushEvent(cmd, "Color Grading", color);
    DrawColorGrading();
    m_Rhi->PopEvent(cmd);

    m_Rhi->CmdNextSubpassPFN(cmd, RHI_SUBPASS_CONTENTS_INLINE);

    m_Rhi->PushEvent(cmd, "FXAA", color);
    DrawFxaa();
    m_Rhi->PopEvent(cmd);

    m_Rhi->CmdNextSubpassPFN(cmd, RHI_SUBPASS_CONTENTS_INLINE);

    RHIClearAttachment clear_attachments[1] {};
    clear_attachments[0].aspectMask = RHI_IMAGE_ASPECT_COLOR_BIT;
    clear_attachments[0].colorAttachment = 0;
    clear_attachments[0].clearValue.color.float32[0] = 0.0f;
    clear_attachments[0].clearValue.color.float32[1] = 0.0f;
    clear_attachments[0].clearValue.color.float32[2] = 0.0f;
    clear_attachments[0].clearValue.color.float32[3] = 0.0f;

    RHIClearRect clear_rects[1] {};
    clear_rects[0].rect.offset.x = 0;
    clear_rects[0].rect.offset.y = 0;
    clear_rects[0].rect.extent.width = m_Rhi->GetSwapchainInfo().extent.width;
    clear_rects[0].rect.extent.height = m_Rhi->GetSwapchainInfo().extent.height;
    m_Rhi->CmdClearAttachmentsPFN(cmd, 1, clear_attachments, 1, clear_rects);

    if (m_UiPass != nullptr)
    {
        m_UiPass->Draw();
    }

    for (const std::function<void()>& callback : post_ui_callbacks)
    {
        if (callback)
        {
            callback();
        }
    }

    m_Rhi->CmdNextSubpassPFN(cmd, RHI_SUBPASS_CONTENTS_INLINE);

    m_Rhi->PushEvent(cmd, "Combine UI", color);
    DrawCombineUi();
    m_Rhi->PopEvent(cmd);

    m_Rhi->CmdEndRenderPassPFN(cmd);
}
