#include "Runtime/Function/Render/Passes/TAAPass.h"

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Function/Render/Interface/Vulkan/VulkanRHI.h"
#include "Runtime/Function/Render/Interface/Vulkan/VulkanUtil.h"
#include "Runtime/Function/Render/RenderCommon.h"
#include "Runtime/Function/Render/RenderPass.h"

#include <bloom_vert.h>
#include <taa_frag.h>

#include <cmath>

namespace
{
// Must match the TAAUB block declared in taa.frag: two mat4s + two vec4s, std140.
struct TAAUniformBuffer
{
    float clip_to_world[16];       // column-major
    float world_to_prev_clip[16];  // column-major
    float params[4];
    float resolution[4];
};
static_assert(sizeof(TAAUniformBuffer) == 160,
              "TAAUniformBuffer must stay 160 bytes to match the shader's std140 TAAUB block");

// Matrix4x4 is row-major (m_Mat[row][col], clip = M * v) while a GLSL mat4 in a uniform
// block is column-major, so the bytes have to be transposed on the way in -- otherwise
// `ubo.clip_to_world * clip` silently evaluates Mᵀ·v. Same trick NaniteGPUPackMatrix uses.
void PackMatrixColumnMajor(const Matrix4x4& source, float* destination)
{
    for (int row = 0; row < 4; ++row)
    {
        for (int col = 0; col < 4; ++col)
        {
            destination[col * 4 + row] = source.m_Mat[row][col];
        }
    }
}
}  // namespace

// =====================================================================================
// Setup
// =====================================================================================

void TAAPass::Initialize(const RenderPassInitInfo* init_info)
{
    RenderPass::Initialize(nullptr);

    const TAAPassInitInfo* taa_init_info = static_cast<const TAAPassInitInfo*>(init_info);
    if (taa_init_info == nullptr)
    {
        LOG_ERROR(ZRender, "taa: TAAPassInitInfo is null");
        return;
    }
    m_HdrFormat = taa_init_info->hdr_format;

    SetupRenderPass();
    SetupDescriptorSetLayout();
    SetupPipeline();
}

void TAAPass::SetupRenderPass()
{
    // Two colour attachments, one draw, no depth:
    //   [0] the stable resolve target  -- what bloom / tone mapping sample
    //   [1] this frame's history slot  -- what the NEXT frame samples
    // The pass only ever overwrites both, so there is nothing to load or clear.
    RHIAttachmentDescription color_attachments[2] = {};
    for (RHIAttachmentDescription& attachment : color_attachments)
    {
        attachment.flags = 0;
        attachment.format = m_HdrFormat;
        attachment.samples = RHI_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = RHI_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.storeOp = RHI_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = RHI_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp = RHI_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = RHI_IMAGE_LAYOUT_UNDEFINED;
        // Both are read by a sampler afterwards (the resolve immediately, the history one
        // frame later), so both hand off shader-read-ready.
        attachment.finalLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }

    RHIAttachmentReference color_attachment_references[2] = {};
    color_attachment_references[0].attachment = 0;
    color_attachment_references[0].layout = RHI_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color_attachment_references[1].attachment = 1;
    color_attachment_references[1].layout = RHI_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    RHISubpassDescription subpass {};
    subpass.pipelineBindPoint = RHI_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 2;
    subpass.pColorAttachments = &color_attachment_references[0];
    subpass.pDepthStencilAttachment = nullptr;
    subpass.preserveAttachmentCount = 0;
    subpass.pPreserveAttachments = nullptr;

    RHIRenderPassCreateInfo render_pass_info {};
    render_pass_info.sType = RHI_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    render_pass_info.attachmentCount = 2;
    render_pass_info.pAttachments = &color_attachments[0];
    render_pass_info.subpassCount = 1;
    render_pass_info.pSubpasses = &subpass;
    render_pass_info.dependencyCount = 0;
    render_pass_info.pDependencies = nullptr;

    if (!m_Rhi->CreateRenderPass(&render_pass_info, m_Framebuffer.render_pass))
    {
        LOG_ERROR(ZRender, "taa: render pass creation failed");
    }
}

void TAAPass::SetupDescriptorSetLayout()
{
    m_DescriptorInfos.resize(1);

    // binding 0 = current colour, 1 = history, 2 = depth, 3 = TAAUB.
    RHIDescriptorSetLayoutBinding bindings[4] = {};
    for (uint32_t i = 0; i < 3; ++i)
    {
        bindings[i].binding = i;
        bindings[i].descriptorType = RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = RHI_SHADER_STAGE_FRAGMENT_BIT;
    }
    bindings[3].binding = 3;
    bindings[3].descriptorType = RHI_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[3].descriptorCount = 1;
    bindings[3].stageFlags = RHI_SHADER_STAGE_FRAGMENT_BIT;

    RHIDescriptorSetLayoutCreateInfo create_info {};
    create_info.sType = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    create_info.bindingCount = 4;
    create_info.pBindings = bindings;

    if (RHI_SUCCESS != m_Rhi->CreateDescriptorSetLayout(&create_info, m_DescriptorInfos[0].layout))
    {
        LOG_ERROR(ZRender, "taa: descriptor set layout failed");
    }
}

void TAAPass::SetupPipeline()
{
    m_RenderPipelines.resize(1);

    RHIDescriptorSetLayout* set_layouts[1] = {m_DescriptorInfos[0].layout};

    RHIPipelineLayoutCreateInfo pipeline_layout_create_info {};
    pipeline_layout_create_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout_create_info.setLayoutCount = 1;
    pipeline_layout_create_info.pSetLayouts = set_layouts;

    if (RHI_SUCCESS != m_Rhi->CreatePipelineLayout(&pipeline_layout_create_info, m_RenderPipelines[0].layout))
    {
        LOG_ERROR(ZRender, "taa: pipeline layout failed");
        return;
    }

    RHIShader* vertex_shader_module = m_Rhi->CreateShaderModule(Z::Shader::BLOOM_VERT);
    RHIShader* fragment_shader_module = m_Rhi->CreateShaderModule(Z::Shader::TAA_FRAG);

    RHIPipelineShaderStageCreateInfo shader_stages[2] = {};
    shader_stages[0].sType = RHI_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shader_stages[0].stage = RHI_SHADER_STAGE_VERTEX_BIT;
    shader_stages[0].module = vertex_shader_module;
    shader_stages[0].pName = "main";

    shader_stages[1].sType = RHI_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shader_stages[1].stage = RHI_SHADER_STAGE_FRAGMENT_BIT;
    shader_stages[1].module = fragment_shader_module;
    shader_stages[1].pName = "main";

    RHIPipelineVertexInputStateCreateInfo vertex_input_state {};
    vertex_input_state.sType = RHI_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    RHIPipelineInputAssemblyStateCreateInfo input_assembly {};
    input_assembly.sType = RHI_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = RHI_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    input_assembly.primitiveRestartEnable = RHI_FALSE;

    // Viewport + scissor are dynamic even though there is only one size: it keeps the
    // pass independent of the swapchain extent at pipeline-creation time.
    RHIDynamicState dynamic_states[] = {RHI_DYNAMIC_STATE_VIEWPORT, RHI_DYNAMIC_STATE_SCISSOR};
    RHIPipelineDynamicStateCreateInfo dynamic_state {};
    dynamic_state.sType = RHI_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic_state.dynamicStateCount = 2;
    dynamic_state.pDynamicStates = dynamic_states;

    RHIPipelineViewportStateCreateInfo viewport_state {};
    viewport_state.sType = RHI_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;

    RHIPipelineRasterizationStateCreateInfo rasterization_state {};
    rasterization_state.sType = RHI_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization_state.polygonMode = RHI_POLYGON_MODE_FILL;
    rasterization_state.lineWidth = 1.0f;
    rasterization_state.cullMode = RHI_CULL_MODE_NONE;
    rasterization_state.frontFace = RHI_FRONT_FACE_COUNTER_CLOCKWISE;

    RHIPipelineMultisampleStateCreateInfo multisample_state {};
    multisample_state.sType = RHI_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample_state.rasterizationSamples = RHI_SAMPLE_COUNT_1_BIT;

    RHIPipelineColorBlendAttachmentState color_blend_attachments[2] = {};
    for (RHIPipelineColorBlendAttachmentState& blend : color_blend_attachments)
    {
        blend.colorWriteMask = RHI_COLOR_COMPONENT_R_BIT | RHI_COLOR_COMPONENT_G_BIT |
                               RHI_COLOR_COMPONENT_B_BIT | RHI_COLOR_COMPONENT_A_BIT;
        blend.blendEnable = RHI_FALSE;
    }

    RHIPipelineColorBlendStateCreateInfo color_blend_state {};
    color_blend_state.sType = RHI_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blend_state.attachmentCount = 2;
    color_blend_state.pAttachments = &color_blend_attachments[0];

    RHIPipelineDepthStencilStateCreateInfo depth_stencil_state {};
    depth_stencil_state.sType = RHI_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil_state.depthTestEnable = RHI_FALSE;
    depth_stencil_state.depthWriteEnable = RHI_FALSE;
    depth_stencil_state.stencilTestEnable = RHI_FALSE;

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
    pipeline_info.layout = m_RenderPipelines[0].layout;
    pipeline_info.renderPass = m_Framebuffer.render_pass;
    pipeline_info.subpass = 0;

    if (RHI_SUCCESS != m_Rhi->CreateGraphicsPipelines(RHI_NULL_HANDLE, 1, &pipeline_info, m_RenderPipelines[0].pipeline))
    {
        LOG_ERROR(ZRender, "taa: graphics pipeline failed");
    }

    m_Rhi->DestroyShaderModule(fragment_shader_module);
    m_Rhi->DestroyShaderModule(vertex_shader_module);
}

// =====================================================================================
// Resources
// =====================================================================================

void TAAPass::RebuildTargets(uint32_t width, uint32_t height)
{
    DestroyTargets();
    m_Ready = false;

    if (width == 0 || height == 0 || m_Framebuffer.render_pass == nullptr)
    {
        return;
    }

    auto create_target = [this, width, height](HistoryTarget& target) {
        m_Rhi->CreateImage(width,
                           height,
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
    };

    create_target(m_Output);
    create_target(m_History[0]);
    create_target(m_History[1]);

    // One framebuffer per history slot: attachment 0 is always the resolve, attachment 1
    // is whichever history slot this frame writes.
    for (uint32_t i = 0; i < 2; ++i)
    {
        RHIImageView* attachments[2] = {m_Output.view, m_History[i].view};

        RHIFramebufferCreateInfo framebuffer_info {};
        framebuffer_info.sType = RHI_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebuffer_info.renderPass = m_Framebuffer.render_pass;
        framebuffer_info.attachmentCount = 2;
        framebuffer_info.pAttachments = &attachments[0];
        framebuffer_info.width = width;
        framebuffer_info.height = height;
        framebuffer_info.layers = 1;
        m_Rhi->CreateFramebuffer(&framebuffer_info, m_ResolveFramebuffers[i]);
    }

    m_Width = width;
    m_Height = height;
    // A new history chain has no valid contents.
    m_ResetPending = true;
}

void TAAPass::DestroyTargets()
{
    for (RHIFramebuffer*& framebuffer : m_ResolveFramebuffers)
    {
        if (framebuffer != nullptr)
        {
            m_Rhi->DestroyFramebuffer(framebuffer);
            framebuffer = nullptr;
        }
    }

    auto destroy_target = [this](HistoryTarget& target) {
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
    };

    destroy_target(m_History[0]);
    destroy_target(m_History[1]);
    destroy_target(m_Output);
    m_Ready = false;
}

void TAAPass::RebuildDescriptor(RHIImageView* scene_hdr_input, RHIImageView* depth_input)
{
    DestroyDescriptor();
    m_Ready = false;

    if (scene_hdr_input == nullptr || depth_input == nullptr || m_Output.view == nullptr ||
        m_History[0].view == nullptr)
    {
        return;
    }
    m_SceneInput = scene_hdr_input;
    m_DepthInput = depth_input;

    RHIDescriptorSetAllocateInfo allocate_info {};
    allocate_info.sType = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate_info.descriptorPool = m_Rhi->GetDescriptorPoor();
    allocate_info.descriptorSetCount = 1;
    allocate_info.pSetLayouts = &m_DescriptorInfos[0].layout;

    if (RHI_SUCCESS != m_Rhi->AllocateDescriptorSets(&allocate_info, m_DescriptorSet) || m_DescriptorSet == nullptr)
    {
        LOG_ERROR(ZRender, "taa: descriptor set allocation failed");
        return;
    }
    m_DescriptorInfos[0].descriptor_set = m_DescriptorSet;

    UploadUniforms();
    m_UboRingReady = true;
    m_Ready = true;
}

void TAAPass::DestroyDescriptor()
{
    for (FrameUbo& ubo : m_UboRing)
    {
        if (ubo.buffer != nullptr)
        {
            m_Rhi->DestroyBuffer(ubo.buffer);
            ubo.buffer = nullptr;
        }
        ubo.memory = nullptr;
    }
    // The set itself dies with the pool; this RHI exposes no per-set destroy.
    m_DescriptorSet = nullptr;
    m_UboRingReady = false;
}

void TAAPass::UploadUniforms()
{
    if (m_DescriptorSet == nullptr)
    {
        return;
    }

    // Rotate first so the buffer being replaced is the oldest one in the ring.
    m_UboCursor = (m_UboCursor + 1u) % kUboRingSize;
    FrameUbo& slot = m_UboRing[m_UboCursor];
    if (slot.buffer != nullptr)
    {
        m_Rhi->DestroyBuffer(slot.buffer);
        slot.buffer = nullptr;
        slot.memory = nullptr;
    }

    // The reprojection has to use the SAME (jittered) view-projection the scene was
    // rendered with -- that is what UpdatePerFrame was handed -- and the previous frame's
    // one. On the first frame (or after a reset) there is no previous, so the previous is
    // taken equal to the current: prev_uv then lands exactly on this pixel and the reset
    // flag discards the uninitialised history anyway.
    const Matrix4x4& previous =
        m_HasPrevious ? m_PreviousViewProjection : m_CurrentViewProjection;

    TAAUniformBuffer ubo {};
    PackMatrixColumnMajor(m_CurrentViewProjection.inverse(), ubo.clip_to_world);
    PackMatrixColumnMajor(previous, ubo.world_to_prev_clip);
    ubo.params[0] = m_Settings.blend;
    ubo.params[1] = (m_ResetPending || !m_HasPrevious) ? 1.0f : 0.0f;
    ubo.params[2] = m_Settings.variance_gamma;
    ubo.params[3] = m_Settings.reversed_z ? 1.0f : 0.0f;
    ubo.resolution[0] = static_cast<float>(m_Width);
    ubo.resolution[1] = static_cast<float>(m_Height);
    ubo.resolution[2] = 1.0f / std::max<float>(static_cast<float>(m_Width), 1.0f);
    ubo.resolution[3] = 1.0f / std::max<float>(static_cast<float>(m_Height), 1.0f);

    m_Rhi->CreateBufferAndInitialize(RHI_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                     RHI_MEMORY_PROPERTY_HOST_VISIBLE_BIT | RHI_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                     slot.buffer,
                                     slot.memory,
                                     static_cast<RHIDeviceSize>(sizeof(TAAUniformBuffer)),
                                     &ubo,
                                     static_cast<int>(sizeof(TAAUniformBuffer)));

    // The current colour and depth only change on resize; the history alternates every
    // frame and the uniform block is re-created every frame, so both are rewritten here
    // rather than once at setup time.
    RHIDescriptorImageInfo scene_info {};
    scene_info.sampler = m_Rhi->GetOrCreateDefaultSampler(Default_Sampler_Linear);
    scene_info.imageView = m_SceneInput;
    scene_info.imageLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    RHIDescriptorImageInfo history_info {};
    history_info.sampler = m_Rhi->GetOrCreateDefaultSampler(Default_Sampler_Linear);
    // The history read this frame is the slot written LAST frame, i.e. the one this
    // frame's write index is not pointing at.
    history_info.imageView = m_History[1 - m_HistoryWriteIndex].view;
    history_info.imageLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    RHIDescriptorImageInfo depth_info {};
    // Depth must be sampled, not filtered: a linear filter across a depth discontinuity
    // invents a world position halfway between two surfaces and smears the reprojection
    // across every silhouette.
    depth_info.sampler = m_Rhi->GetOrCreateDefaultSampler(Default_Sampler_Nearest);
    depth_info.imageView = m_DepthInput;
    depth_info.imageLayout = RHI_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    RHIDescriptorBufferInfo ubo_info {};
    ubo_info.offset = 0;
    ubo_info.range = static_cast<RHIDeviceSize>(sizeof(TAAUniformBuffer));
    ubo_info.buffer = slot.buffer;

    RHIWriteDescriptorSet writes[4] = {};
    for (uint32_t i = 0; i < 4; ++i)
    {
        writes[i].sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = m_DescriptorSet;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
    }
    writes[0].descriptorType = RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo = &scene_info;
    writes[1].descriptorType = RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].pImageInfo = &history_info;
    writes[2].descriptorType = RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[2].pImageInfo = &depth_info;
    writes[3].descriptorType = RHI_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[3].pBufferInfo = &ubo_info;

    m_Rhi->UpdateDescriptorSets(4, writes, 0, nullptr);
}

// =====================================================================================
// Public API
// =====================================================================================

void TAAPass::UpdateAfterFramebufferRecreate(uint32_t width,
                                             uint32_t height,
                                             RHIImageView* scene_hdr_input,
                                             RHIImageView* depth_input)
{
    const bool size_changed = (width != m_Width) || (height != m_Height);
    const bool chain_missing = m_History[0].view == nullptr || m_Output.view == nullptr;

    if (size_changed || chain_missing)
    {
        RebuildTargets(width, height);
    }

    if (scene_hdr_input != nullptr)
    {
        m_SceneInput = scene_hdr_input;
    }
    if (depth_input != nullptr)
    {
        m_DepthInput = depth_input;
    }

    if (m_Output.view != nullptr && m_History[0].view != nullptr && m_SceneInput != nullptr &&
        m_DepthInput != nullptr)
    {
        RebuildDescriptor(m_SceneInput, m_DepthInput);
    }
}

void TAAPass::UpdatePerFrame(const Matrix4x4& current_view_projection, bool reset)
{
    if (!m_Settings.enabled)
    {
        return;
    }

    if (m_HasPrevious)
    {
        m_PreviousViewProjection = m_CurrentViewProjection;
    }
    m_CurrentViewProjection = current_view_projection;
    m_HasPrevious = true;
    m_ResetPending = m_ResetPending || reset;

    UploadUniforms();
}

RHIImageView* TAAPass::GetOutputImageView() const
{
    // The resolve target is stable across frames -- that is the entire point of writing
    // the history to a second attachment instead of ping-ponging the output.
    return m_Output.view;
}

void TAAPass::Shutdown()
{
    DestroyDescriptor();
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

void TAAPass::Draw()
{
    if (!m_Settings.enabled || !m_Ready || m_DescriptorSet == nullptr || !m_UboRingReady)
    {
        return;
    }

    RHICommandBuffer* command_buffer = m_Rhi->GetCurrentCommandBuffer();
    RHIFramebuffer* framebuffer = m_ResolveFramebuffers[m_HistoryWriteIndex];
    if (command_buffer == nullptr || framebuffer == nullptr || m_RenderPipelines[0].pipeline == nullptr)
    {
        return;
    }

    // The history slot alternates, so its descriptor has to be re-pointed before the draw.
    UploadUniforms();

    RHIRenderPassBeginInfo begin_info {};
    begin_info.sType = RHI_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    begin_info.renderPass = m_Framebuffer.render_pass;
    begin_info.framebuffer = framebuffer;
    begin_info.renderArea.offset = {0, 0};
    begin_info.renderArea.extent = {m_Width, m_Height};
    begin_info.clearValueCount = 0;
    begin_info.pClearValues = nullptr;

    m_Rhi->CmdBeginRenderPassPFN(command_buffer, &begin_info, RHI_SUBPASS_CONTENTS_INLINE);

    RHIViewport viewport = {0.0f, 0.0f, static_cast<float>(m_Width), static_cast<float>(m_Height), 0.0f, 1.0f};
    RHIRect2D scissor = {{0, 0}, {m_Width, m_Height}};

    m_Rhi->CmdBindPipelinePFN(command_buffer, RHI_PIPELINE_BIND_POINT_GRAPHICS, m_RenderPipelines[0].pipeline);
    m_Rhi->CmdSetViewportPFN(command_buffer, 0, 1, &viewport);
    m_Rhi->CmdSetScissorPFN(command_buffer, 0, 1, &scissor);
    m_Rhi->CmdBindDescriptorSetsPFN(command_buffer,
                                    RHI_PIPELINE_BIND_POINT_GRAPHICS,
                                    m_RenderPipelines[0].layout,
                                    0,
                                    1,
                                    &m_DescriptorSet,
                                    0,
                                    nullptr);
    m_Rhi->CmdDraw(command_buffer, 3, 1, 0, 0);

    m_Rhi->CmdEndRenderPassPFN(command_buffer);

    // This frame's history slot is the one next frame reads.
    m_HistoryWriteIndex = 1 - m_HistoryWriteIndex;
    m_ResetPending = false;
}
