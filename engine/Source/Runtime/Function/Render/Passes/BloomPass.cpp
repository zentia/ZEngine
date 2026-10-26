#include "Runtime/Function/Render/Passes/BloomPass.h"

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Function/Render/Interface/Vulkan/VulkanRHI.h"
#include "Runtime/Function/Render/Interface/Vulkan/VulkanUtil.h"
#include "Runtime/Function/Render/RenderCommon.h"
#include "Runtime/Function/Render/RenderPass.h"

#include <bloom_combine_frag.h>
#include <bloom_downsample_frag.h>
#include <bloom_prefilter_frag.h>
#include <bloom_upsample_frag.h>
#include <bloom_vert.h>

#include <algorithm>
#include <cmath>

namespace
{
// Descriptor set layouts. Both are shared by two pipelines each, which is why there are
// only two of them for four stages.
constexpr uint32_t kLayoutSingle = 0;  // tex @0 + ubo @1              (prefilter, downsample)
constexpr uint32_t kLayoutDouble = 1;  // tex @0 + ubo @1 + tex @2     (upsample, combine)

constexpr int kPipelinePrefilter = 0;
constexpr int kPipelineDownsample = 1;
constexpr int kPipelineUpsample = 2;
constexpr int kPipelineCombine = 3;
constexpr int kPipelineCount = 4;

// Below this a mip costs more in draw-call and sampling overhead than it adds to the
// glow, and the downsample kernel starts sampling outside its own footprint.
constexpr uint32_t kMinMipSize = 8;

// Must match the BloomUB block declared in every bloom shader: three vec4s, std140.
// The static_assert is the only thing keeping the C++ side and the five shaders honest.
struct BloomUniformBuffer
{
    float filter_params[4];
    float combine_params[4];
    float texel_size[4];
};
static_assert(sizeof(BloomUniformBuffer) == 48,
              "BloomUniformBuffer must stay 48 bytes to match the shader's std140 BloomUB block");
}  // namespace

// =====================================================================================
// Setup
// =====================================================================================

void BloomPass::Initialize(const RenderPassInitInfo* init_info)
{
    RenderPass::Initialize(nullptr);

    const BloomPassInitInfo* bloom_init_info = static_cast<const BloomPassInitInfo*>(init_info);
    if (bloom_init_info == nullptr)
    {
        LOG_ERROR(ZRender, "bloom: BloomPassInitInfo is null");
        return;
    }
    m_HdrFormat = bloom_init_info->hdr_format;

    SetupRenderPass();
    SetupDescriptorSetLayouts();
    SetupPipelines();
}

void BloomPass::SetupRenderPass()
{
    // One render pass serves the whole chain: a Vulkan render pass carries no size, the
    // framebuffer does, so every mip (and the full-res output) can share it.
    RHIAttachmentDescription color_attachment {};
    color_attachment.flags = 0;
    color_attachment.format = m_HdrFormat;
    color_attachment.samples = RHI_SAMPLE_COUNT_1_BIT;
    // Every mip is fully overwritten, so there is nothing worth loading and nothing to
    // clear -- DONT_CARE / UNDEFINED is both correct and the cheapest option.
    color_attachment.loadOp = RHI_ATTACHMENT_LOAD_OP_DONT_CARE;
    color_attachment.storeOp = RHI_ATTACHMENT_STORE_OP_STORE;
    color_attachment.stencilLoadOp = RHI_ATTACHMENT_LOAD_OP_DONT_CARE;
    color_attachment.stencilStoreOp = RHI_ATTACHMENT_STORE_OP_DONT_CARE;
    color_attachment.initialLayout = RHI_IMAGE_LAYOUT_UNDEFINED;
    // The next mip's sampler is the only consumer, so hand the image straight back as a
    // shader resource. That is what removes every manual barrier from the chain.
    color_attachment.finalLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    RHIAttachmentReference color_reference {};
    color_reference.attachment = 0;
    color_reference.layout = RHI_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    RHISubpassDescription subpass {};
    subpass.flags = 0;
    subpass.pipelineBindPoint = RHI_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.inputAttachmentCount = 0;
    subpass.pInputAttachments = nullptr;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_reference;
    subpass.pResolveAttachments = nullptr;
    subpass.pDepthStencilAttachment = nullptr;
    subpass.preserveAttachmentCount = 0;
    subpass.pPreserveAttachments = nullptr;

    RHIRenderPassCreateInfo render_pass_info {};
    render_pass_info.sType = RHI_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    render_pass_info.pNext = nullptr;
    render_pass_info.flags = 0;
    render_pass_info.attachmentCount = 1;
    render_pass_info.pAttachments = &color_attachment;
    render_pass_info.subpassCount = 1;
    render_pass_info.pSubpasses = &subpass;
    render_pass_info.dependencyCount = 0;
    render_pass_info.pDependencies = nullptr;

    if (!m_Rhi->CreateRenderPass(&render_pass_info, m_Framebuffer.render_pass))
    {
        LOG_ERROR(ZRender, "bloom: render pass creation failed");
    }
}

void BloomPass::SetupDescriptorSetLayouts()
{
    m_DescriptorInfos.resize(2);

    // ---- layout 0: one sampled texture + the shared uniform block -------------------
    {
        RHIDescriptorSetLayoutBinding bindings[2] = {};
        bindings[0].binding = 0;
        bindings[0].descriptorType = RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = RHI_SHADER_STAGE_FRAGMENT_BIT;

        bindings[1].binding = 1;
        bindings[1].descriptorType = RHI_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = RHI_SHADER_STAGE_FRAGMENT_BIT;

        RHIDescriptorSetLayoutCreateInfo create_info {};
        create_info.sType = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        create_info.bindingCount = 2;
        create_info.pBindings = bindings;

        if (RHI_SUCCESS != m_Rhi->CreateDescriptorSetLayout(&create_info, m_DescriptorInfos[kLayoutSingle].layout))
        {
            LOG_ERROR(ZRender, "bloom: single-texture descriptor set layout failed");
        }
    }

    // ---- layout 1: two sampled textures (0 and 2) + the uniform block at 1 ----------
    {
        RHIDescriptorSetLayoutBinding bindings[3] = {};
        bindings[0].binding = 0;
        bindings[0].descriptorType = RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = RHI_SHADER_STAGE_FRAGMENT_BIT;

        bindings[1].binding = 1;
        bindings[1].descriptorType = RHI_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = RHI_SHADER_STAGE_FRAGMENT_BIT;

        bindings[2].binding = 2;
        bindings[2].descriptorType = RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[2].descriptorCount = 1;
        bindings[2].stageFlags = RHI_SHADER_STAGE_FRAGMENT_BIT;

        RHIDescriptorSetLayoutCreateInfo create_info {};
        create_info.sType = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        create_info.bindingCount = 3;
        create_info.pBindings = bindings;

        if (RHI_SUCCESS != m_Rhi->CreateDescriptorSetLayout(&create_info, m_DescriptorInfos[kLayoutDouble].layout))
        {
            LOG_ERROR(ZRender, "bloom: two-texture descriptor set layout failed");
        }
    }
}

void BloomPass::SetupPipelines()
{
    m_RenderPipelines.resize(kPipelineCount);

    // Prefilter and downsample read one texture; upsample and combine read two.
    const uint32_t layout_for_pipeline[kPipelineCount] = {
        kLayoutSingle,  // prefilter
        kLayoutSingle,  // downsample
        kLayoutDouble,  // upsample
        kLayoutDouble,  // combine
    };

    const std::vector<unsigned char>* fragment_shaders[kPipelineCount] = {
        &Z::Shader::BLOOM_PREFILTER_FRAG,
        &Z::Shader::BLOOM_DOWNSAMPLE_FRAG,
        &Z::Shader::BLOOM_UPSAMPLE_FRAG,
        &Z::Shader::BLOOM_COMBINE_FRAG,
    };

    RHIShader* vertex_shader_module = m_Rhi->CreateShaderModule(Z::Shader::BLOOM_VERT);

    for (int i = 0; i < kPipelineCount; ++i)
    {
        RHIDescriptorSetLayout* set_layouts[1] = {m_DescriptorInfos[layout_for_pipeline[i]].layout};

        RHIPipelineLayoutCreateInfo pipeline_layout_create_info {};
        pipeline_layout_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipeline_layout_create_info.setLayoutCount = 1;
        pipeline_layout_create_info.pSetLayouts = set_layouts;

        if (RHI_SUCCESS != m_Rhi->CreatePipelineLayout(&pipeline_layout_create_info, m_RenderPipelines[i].layout))
        {
            LOG_ERROR(ZRender, "bloom: pipeline layout {} failed", i);
            continue;
        }

        RHIShader* fragment_shader_module = m_Rhi->CreateShaderModule(*fragment_shaders[i]);

        RHIPipelineShaderStageCreateInfo shader_stages[2] = {};
        shader_stages[0].sType = RHI_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        shader_stages[0].stage = RHI_SHADER_STAGE_VERTEX_BIT;
        shader_stages[0].module = vertex_shader_module;
        shader_stages[0].pName = "main";

        shader_stages[1].sType = RHI_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        shader_stages[1].stage = RHI_SHADER_STAGE_FRAGMENT_BIT;
        shader_stages[1].module = fragment_shader_module;
        shader_stages[1].pName = "main";

        // No vertex buffer: bloom.vert synthesises the fullscreen triangle from
        // gl_VertexIndex, exactly like post_process.vert / fxaa.vert.
        RHIPipelineVertexInputStateCreateInfo vertex_input_state {};
        vertex_input_state.sType = RHI_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertex_input_state.vertexBindingDescriptionCount = 0;
        vertex_input_state.pVertexBindingDescriptions = nullptr;
        vertex_input_state.vertexAttributeDescriptionCount = 0;
        vertex_input_state.pVertexAttributeDescriptions = nullptr;

        RHIPipelineInputAssemblyStateCreateInfo input_assembly {};
        input_assembly.sType = RHI_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        input_assembly.topology = RHI_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        input_assembly.primitiveRestartEnable = RHI_FALSE;

        RHIPipelineViewportStateCreateInfo viewport_state {};
        viewport_state.sType = RHI_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport_state.viewportCount = 1;
        viewport_state.scissorCount = 1;

        RHIPipelineRasterizationStateCreateInfo rasterization_state {};
        rasterization_state.sType = RHI_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterization_state.depthClampEnable = RHI_FALSE;
        rasterization_state.rasterizerDiscardEnable = RHI_FALSE;
        rasterization_state.polygonMode = RHI_POLYGON_MODE_FILL;
        rasterization_state.lineWidth = 1.0f;
        rasterization_state.cullMode = RHI_CULL_MODE_NONE;
        rasterization_state.frontFace = RHI_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterization_state.depthBiasEnable = RHI_FALSE;

        RHIPipelineMultisampleStateCreateInfo multisample_state {};
        multisample_state.sType = RHI_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample_state.sampleShadingEnable = RHI_FALSE;
        multisample_state.rasterizationSamples = RHI_SAMPLE_COUNT_1_BIT;

        // No blending anywhere: the upsample pass adds its two inputs inside the shader
        // precisely so no backend has to carry an additive blend state.
        RHIPipelineColorBlendAttachmentState color_blend_attachment {};
        color_blend_attachment.colorWriteMask = RHI_COLOR_COMPONENT_R_BIT | RHI_COLOR_COMPONENT_G_BIT |
                                                RHI_COLOR_COMPONENT_B_BIT | RHI_COLOR_COMPONENT_A_BIT;
        color_blend_attachment.blendEnable = RHI_FALSE;

        RHIPipelineColorBlendStateCreateInfo color_blend_state {};
        color_blend_state.sType = RHI_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        color_blend_state.logicOpEnable = RHI_FALSE;
        color_blend_state.attachmentCount = 1;
        color_blend_state.pAttachments = &color_blend_attachment;

        RHIPipelineDepthStencilStateCreateInfo depth_stencil_state {};
        depth_stencil_state.sType = RHI_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depth_stencil_state.depthTestEnable = RHI_FALSE;
        depth_stencil_state.depthWriteEnable = RHI_FALSE;
        depth_stencil_state.stencilTestEnable = RHI_FALSE;

        // Viewport and scissor are dynamic so one pipeline can render every mip size.
        RHIDynamicState dynamic_states[] = {RHI_DYNAMIC_STATE_VIEWPORT, RHI_DYNAMIC_STATE_SCISSOR};
        RHIPipelineDynamicStateCreateInfo dynamic_state {};
        dynamic_state.sType = RHI_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic_state.dynamicStateCount = 2;
        dynamic_state.pDynamicStates = dynamic_states;

        RHIGraphicsPipelineCreateInfo pipeline_info {};
        pipeline_info.sType = RHI_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipeline_info.stageCount = 2;
        pipeline_info.pStages = shader_stages;
        pipeline_info.pVertexInputState = &vertex_input_state;
        pipeline_info.pInputAssemblyState = &input_assembly;
        pipeline_info.pViewportState = &viewport_state;
        pipeline_info.pRasterizationState = &rasterization_state;
        pipeline_info.pMultisampleState = &multisample_state;
        pipeline_info.pColorBlendState = &color_blend_state;
        pipeline_info.pDepthStencilState = &depth_stencil_state;
        pipeline_info.pDynamicState = &dynamic_state;
        pipeline_info.layout = m_RenderPipelines[i].layout;
        pipeline_info.renderPass = m_Framebuffer.render_pass;
        pipeline_info.subpass = 0;
        pipeline_info.basePipelineHandle = RHI_NULL_HANDLE;

        if (RHI_SUCCESS != m_Rhi->CreateGraphicsPipelines(RHI_NULL_HANDLE, 1, &pipeline_info, m_RenderPipelines[i].pipeline))
        {
            LOG_ERROR(ZRender, "bloom: graphics pipeline {} failed", i);
        }

        m_Rhi->DestroyShaderModule(fragment_shader_module);
    }

    m_Rhi->DestroyShaderModule(vertex_shader_module);
}

// =====================================================================================
// Resources
// =====================================================================================

void BloomPass::RebuildTargets(uint32_t width, uint32_t height)
{
    DestroyTargets();
    m_Ready = false;

    if (width == 0 || height == 0 || m_Framebuffer.render_pass == nullptr)
    {
        return;
    }

    // How many half-res steps actually fit before the mip stops being worth anything.
    const uint32_t requested = std::max<uint32_t>(1u, m_Settings.mip_count);
    uint32_t mip_count = 0;
    for (uint32_t i = 1; i <= requested; ++i)
    {
        const uint32_t mip_width = std::max<uint32_t>(width >> i, 1u);
        const uint32_t mip_height = std::max<uint32_t>(height >> i, 1u);
        if (mip_width < kMinMipSize || mip_height < kMinMipSize)
        {
            break;
        }
        ++mip_count;
    }
    if (mip_count == 0)
    {
        LOG_WARNING(ZRender, "bloom: {}x{} is too small for a bloom chain", width, height);
        return;
    }

    auto create_target = [this](MipTarget& target, uint32_t target_width, uint32_t target_height) {
        m_Rhi->CreateImage(target_width,
                           target_height,
                           m_HdrFormat,
                           RHI_IMAGE_TILING_OPTIMAL,
                           RHI_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | RHI_IMAGE_USAGE_SAMPLED_BIT,
                           RHI_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                           target.image,
                           target.memory,
                           0,
                           1,
                           1);
        m_Rhi->CreateImageView(target.image,
                               m_HdrFormat,
                               RHI_IMAGE_ASPECT_COLOR_BIT,
                               RHI_IMAGE_VIEW_TYPE_2D,
                               1,
                               1,
                               target.view);

        RHIFramebufferCreateInfo framebuffer_info {};
        framebuffer_info.sType = RHI_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebuffer_info.renderPass = m_Framebuffer.render_pass;
        framebuffer_info.attachmentCount = 1;
        framebuffer_info.pAttachments = &target.view;
        framebuffer_info.width = target_width;
        framebuffer_info.height = target_height;
        framebuffer_info.layers = 1;
        m_Rhi->CreateFramebuffer(&framebuffer_info, target.framebuffer);

        target.width = target_width;
        target.height = target_height;
    };

    m_Down.resize(mip_count);
    for (uint32_t i = 0; i < mip_count; ++i)
    {
        create_target(m_Down[i],
                      std::max<uint32_t>(width >> (i + 1), 1u),
                      std::max<uint32_t>(height >> (i + 1), 1u));
    }

    // U[j] mirrors D[j] in size. The coarsest level has no U of its own -- it is the
    // start of the up chain and is read straight out of D.
    if (mip_count > 1)
    {
        m_Up.resize(mip_count - 1);
        for (uint32_t j = 0; j + 1 < mip_count; ++j)
        {
            create_target(m_Up[j], m_Down[j].width, m_Down[j].height);
        }
    }
    else
    {
        m_Up.clear();
    }

    create_target(m_Output, width, height);

    m_Width = width;
    m_Height = height;
}

void BloomPass::DestroyTargets()
{
    auto destroy_target = [this](MipTarget& target) {
        if (target.framebuffer != nullptr)
        {
            m_Rhi->DestroyFramebuffer(target.framebuffer);
            target.framebuffer = nullptr;
        }
        if (target.view != nullptr)
        {
            m_Rhi->DestroyImageView(target.view);
            target.view = nullptr;
        }
        if (target.image != nullptr)
        {
            m_Rhi->DestroyImage(target.image);
            target.image = nullptr;
        }
        target.memory = nullptr;
        target.width = 0;
        target.height = 0;
    };

    for (MipTarget& target : m_Down)
    {
        destroy_target(target);
    }
    for (MipTarget& target : m_Up)
    {
        destroy_target(target);
    }
    destroy_target(m_Output);

    m_Down.clear();
    m_Up.clear();
    m_Ready = false;
}

BloomPass::PassBinding BloomPass::AllocateBinding(uint32_t layout_index,
                                                  RHIImageView* texture0,
                                                  RHIImageView* texture2,
                                                  float src_width,
                                                  float src_height,
                                                  float dst_width,
                                                  float dst_height)
{
    PassBinding binding {};

    RHIDescriptorSetAllocateInfo allocate_info {};
    allocate_info.sType = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate_info.descriptorPool = m_Rhi->GetDescriptorPoor();
    allocate_info.descriptorSetCount = 1;
    allocate_info.pSetLayouts = &m_DescriptorInfos[layout_index].layout;

    if (RHI_SUCCESS != m_Rhi->AllocateDescriptorSets(&allocate_info, binding.set) || binding.set == nullptr)
    {
        LOG_ERROR(ZRender, "bloom: descriptor set allocation failed");
        return binding;
    }

    // The knee is forced non-zero because the shader divides by it (0.25 / knee).
    const float knee = std::max(m_Settings.knee, 1e-4f);

    BloomUniformBuffer ubo {};
    ubo.filter_params[0] = m_Settings.threshold;
    ubo.filter_params[1] = m_Settings.threshold - knee;
    ubo.filter_params[2] = knee * 2.0f;
    ubo.filter_params[3] = 0.25f / knee;
    ubo.combine_params[0] = m_Settings.intensity;
    ubo.combine_params[1] = m_Settings.scatter;
    ubo.combine_params[2] = m_Settings.clamp_max;
    ubo.combine_params[3] = 0.0f;
    ubo.texel_size[0] = 1.0f / std::max(src_width, 1.0f);
    ubo.texel_size[1] = 1.0f / std::max(src_height, 1.0f);
    ubo.texel_size[2] = 1.0f / std::max(dst_width, 1.0f);
    ubo.texel_size[3] = 1.0f / std::max(dst_height, 1.0f);

    // NOTE: the RHI exposes no per-frame upload for uniform buffers (no MapMemory in
    // RHI.h), so the block is created pre-filled. Changing the settings therefore
    // recreates the buffers -- fine for editor-tunable values, and the reason
    // SetSettings() must not be called from inside a frame.
    m_Rhi->CreateBufferAndInitialize(RHI_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                     RHI_MEMORY_PROPERTY_HOST_VISIBLE_BIT | RHI_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                     binding.ubo,
                                     binding.ubo_memory,
                                     static_cast<RHIDeviceSize>(sizeof(BloomUniformBuffer)),
                                     &ubo,
                                     static_cast<int>(sizeof(BloomUniformBuffer)));

    RHIDescriptorImageInfo image0_info {};
    image0_info.sampler = m_Rhi->GetOrCreateDefaultSampler(Default_Sampler_Linear);
    image0_info.imageView = texture0;
    image0_info.imageLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    RHIDescriptorBufferInfo ubo_info {};
    ubo_info.offset = 0;
    ubo_info.range = static_cast<RHIDeviceSize>(sizeof(BloomUniformBuffer));
    ubo_info.buffer = binding.ubo;

    RHIWriteDescriptorSet writes[3] = {};

    writes[0].sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = binding.set;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo = &image0_info;

    writes[1].sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = binding.set;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = RHI_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[1].pBufferInfo = &ubo_info;

    uint32_t write_count = 2;
    RHIDescriptorImageInfo image2_info {};
    if (texture2 != nullptr)
    {
        image2_info.sampler = m_Rhi->GetOrCreateDefaultSampler(Default_Sampler_Linear);
        image2_info.imageView = texture2;
        image2_info.imageLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        writes[2].sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet = binding.set;
        writes[2].dstBinding = 2;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType = RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[2].pImageInfo = &image2_info;
        write_count = 3;
    }

    m_Rhi->UpdateDescriptorSets(static_cast<uint32_t>(write_count), writes, 0, nullptr);
    return binding;
}

void BloomPass::DestroyBinding(PassBinding& binding)
{
    // The descriptor set itself is not destroyed individually: this RHI only exposes
    // pool-level allocation, and the set goes away with the pool.
    if (binding.ubo != nullptr)
    {
        m_Rhi->DestroyBuffer(binding.ubo);
        binding.ubo = nullptr;
    }
    binding.ubo_memory = nullptr;
    binding.set = nullptr;
}

void BloomPass::RebuildBindings(RHIImageView* scene_hdr_input)
{
    DestroyBindings();
    m_Ready = false;

    if (scene_hdr_input == nullptr || m_Down.empty() || m_Output.view == nullptr)
    {
        return;
    }
    m_SceneInput = scene_hdr_input;

    const size_t mip_count = m_Down.size();
    const float scene_width = static_cast<float>(m_Width);
    const float scene_height = static_cast<float>(m_Height);

    // ---- prefilter: scene (full res) -> D[0] ----------------------------------------
    m_PrefilterBinding = AllocateBinding(kLayoutSingle,
                                         m_SceneInput,
                                         nullptr,
                                         scene_width,
                                         scene_height,
                                         static_cast<float>(m_Down[0].width),
                                         static_cast<float>(m_Down[0].height));

    // ---- downsample: D[i-1] -> D[i] --------------------------------------------------
    m_DownBindings.clear();
    for (size_t i = 1; i < mip_count; ++i)
    {
        m_DownBindings.push_back(AllocateBinding(kLayoutSingle,
                                                 m_Down[i - 1].view,
                                                 nullptr,
                                                 static_cast<float>(m_Down[i - 1].width),
                                                 static_cast<float>(m_Down[i - 1].height),
                                                 static_cast<float>(m_Down[i].width),
                                                 static_cast<float>(m_Down[i].height)));
    }

    // ---- upsample: coarse accumulation + same-size down mip -> U[j] ------------------
    // Coarse to fine, because U[j] reads U[j + 1]. The coarsest level is seeded straight
    // from D[mip_count - 1].
    m_UpBindings.clear();
    m_UpBindings.resize(m_Up.size());
    for (size_t j = m_Up.size(); j-- > 0;)
    {
        const MipTarget& lower = (j + 1 == mip_count - 1) ? m_Down[mip_count - 1] : m_Up[j + 1];
        m_UpBindings[j] = AllocateBinding(kLayoutDouble,
                                          lower.view,
                                          m_Down[j].view,
                                          static_cast<float>(lower.width),
                                          static_cast<float>(lower.height),
                                          static_cast<float>(m_Up[j].width),
                                          static_cast<float>(m_Up[j].height));
    }

    // ---- combine: scene + finished bloom -> output ------------------------------------
    // With a single mip there is no up chain, so the bloom is D[0] itself.
    RHIImageView* bloom_view = m_Up.empty() ? m_Down[0].view : m_Up[0].view;
    m_CombineBinding = AllocateBinding(
        kLayoutDouble, m_SceneInput, bloom_view, scene_width, scene_height, scene_width, scene_height);

    m_Ready = true;
}

void BloomPass::DestroyBindings()
{
    DestroyBinding(m_PrefilterBinding);
    for (PassBinding& binding : m_DownBindings)
    {
        DestroyBinding(binding);
    }
    for (PassBinding& binding : m_UpBindings)
    {
        DestroyBinding(binding);
    }
    DestroyBinding(m_CombineBinding);

    m_DownBindings.clear();
    m_UpBindings.clear();
    m_Ready = false;
}

// =====================================================================================
// Public API
// =====================================================================================

void BloomPass::UpdateAfterFramebufferRecreate(uint32_t width, uint32_t height, RHIImageView* scene_hdr_input)
{
    const bool size_changed = (width != m_Width) || (height != m_Height);
    const bool chain_missing = m_Down.empty() || m_Output.view == nullptr;

    if (size_changed || chain_missing)
    {
        RebuildTargets(width, height);
        RebuildBindings(scene_hdr_input);
        return;
    }

    // Same size: only re-point the scene input (the swapchain's HDR buffer was just
    // recreated underneath us) and keep every mip we already own.
    if (m_SceneInput != scene_hdr_input || !m_Ready)
    {
        RebuildBindings(scene_hdr_input);
    }
}

void BloomPass::SetSettings(const BloomSettings& settings)
{
    const bool needs_rebuild =
        (settings.mip_count != m_Settings.mip_count) || (settings.threshold != m_Settings.threshold) ||
        (settings.knee != m_Settings.knee) || (settings.intensity != m_Settings.intensity) ||
        (settings.scatter != m_Settings.scatter) || (settings.clamp_max != m_Settings.clamp_max);

    m_Settings = settings;

    if (needs_rebuild && m_Width != 0 && m_Height != 0)
    {
        // mip_count changes the chain itself; everything else only changes the uniform
        // block, but both go through the same rebuild because the UBOs are baked at
        // creation time (see the note in AllocateBinding).
        RebuildTargets(m_Width, m_Height);
        RebuildBindings(m_SceneInput);
    }
}

void BloomPass::Shutdown()
{
    DestroyBindings();
    DestroyTargets();

    if (m_Framebuffer.render_pass != nullptr)
    {
        m_Rhi->DestroyRenderPass(m_Framebuffer.render_pass);
        m_Framebuffer.render_pass = nullptr;
    }
}

// =====================================================================================
// Draw
// =====================================================================================

void BloomPass::DrawStage(int pipeline_index, const PassBinding& binding, const MipTarget& target)
{
    RHICommandBuffer* command_buffer = m_Rhi->GetCurrentCommandBuffer();
    if (command_buffer == nullptr || binding.set == nullptr || target.framebuffer == nullptr ||
        m_RenderPipelines[static_cast<size_t>(pipeline_index)].pipeline == nullptr)
    {
        return;
    }

    // loadOp is DONT_CARE, so no clear value is required (or allowed) here.
    RHIRenderPassBeginInfo begin_info {};
    begin_info.sType = RHI_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    begin_info.renderPass = m_Framebuffer.render_pass;
    begin_info.framebuffer = target.framebuffer;
    begin_info.renderArea.offset = {0, 0};
    begin_info.renderArea.extent = {target.width, target.height};
    begin_info.clearValueCount = 0;
    begin_info.pClearValues = nullptr;

    m_Rhi->CmdBeginRenderPassPFN(command_buffer, &begin_info, RHI_SUBPASS_CONTENTS_INLINE);

    RHIViewport viewport = {
        0.0f, 0.0f, static_cast<float>(target.width), static_cast<float>(target.height), 0.0f, 1.0f};
    RHIRect2D scissor = {{0, 0}, {target.width, target.height}};

    m_Rhi->CmdBindPipelinePFN(command_buffer,
                              RHI_PIPELINE_BIND_POINT_GRAPHICS,
                              m_RenderPipelines[static_cast<size_t>(pipeline_index)].pipeline);
    m_Rhi->CmdSetViewportPFN(command_buffer, 0, 1, &viewport);
    m_Rhi->CmdSetScissorPFN(command_buffer, 0, 1, &scissor);
    m_Rhi->CmdBindDescriptorSetsPFN(command_buffer,
                                    RHI_PIPELINE_BIND_POINT_GRAPHICS,
                                    m_RenderPipelines[static_cast<size_t>(pipeline_index)].layout,
                                    0,
                                    1,
                                    &binding.set,
                                    0,
                                    nullptr);

    m_Rhi->CmdDraw(command_buffer, 3, 1, 0, 0);

    m_Rhi->CmdEndRenderPassPFN(command_buffer);
}

void BloomPass::Draw()
{
    if (!m_Ready || !m_Settings.enabled)
    {
        return;
    }

    RHICommandBuffer* command_buffer = m_Rhi->GetCurrentCommandBuffer();
    if (command_buffer == nullptr || m_Down.empty())
    {
        return;
    }

    const float debug_color[4] = {1.0f, 0.85f, 0.2f, 1.0f};
    m_Rhi->PushEvent(command_buffer, "Bloom", debug_color);

    // 1. Threshold + half-res extraction.
    DrawStage(kPipelinePrefilter, m_PrefilterBinding, m_Down[0]);

    // 2. Downsample the rest of the chain.
    for (size_t i = 0; i < m_DownBindings.size(); ++i)
    {
        DrawStage(kPipelineDownsample, m_DownBindings[i], m_Down[i + 1]);
    }

    // 3. Upsample back up, coarse -> fine. U[j] is written before U[j-1] reads it.
    for (size_t j = m_UpBindings.size(); j-- > 0;)
    {
        DrawStage(kPipelineUpsample, m_UpBindings[j], m_Up[j]);
    }

    // 4. Composite onto the scene at full resolution.
    DrawStage(kPipelineCombine, m_CombineBinding, m_Output);

    m_Rhi->PopEvent(command_buffer);
}
