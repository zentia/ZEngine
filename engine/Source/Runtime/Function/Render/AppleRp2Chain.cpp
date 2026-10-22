#include "Runtime/Function/Render/AppleRp2Chain.h"

#if defined(__APPLE__)

    #include "Runtime/Core/Base/Macro.h"
    #include "Runtime/Function/Render/Interface/RHI.h"
    #include "Runtime/Function/Render/Passes/MainCameraFramebufferResources.h"
    #include "Runtime/Function/Render/RenderPass.h"

// ---------------------------------------------------------------------
// MSL：一个全屏三角形 + 一次采样 + 显示变换。
//
// 入口**绝对不能叫 `main`**（Metal 编译器直接拒绝），这点 AppleSceneBlit.cpp 里
// 踩过并记录在案；同时 MetalRHI 的 `CreateShaderModuleFromSource` 硬编码找
// "main"，落空时退回 `library.functionNames.firstObject` —— 所以每个 library
// 里只能有一个函数，否则兜底会取到错的那个。本文件每个字符串只有一个入口。
//
// 槽位：set 0 / binding 0 ⇒ [[texture(0)]] + [[sampler(0)]]，和
// AppleSceneBlit 同一套手工约定。
//
// uv 映射**必须和 AppleSceneBlit 完全一致**（`v = 0.5 - y*0.5`）。这样 RP2 是
// 一次恒等变换：读 backup_odd 的哪个 texel 就往 backup_even 的哪个 texel 写，
// blit 之后再用同样的约定读出来 —— 不管 Metal 的帧缓冲原点到底在哪一边，
// 三段都同向就不会翻。
// ---------------------------------------------------------------------
namespace
{
    const char* kAppleRp2VertMsl = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct Rp2VertexOut
{
    float4 position [[position]];
    float2 uv;
};

vertex Rp2VertexOut Rp2VertexMain(uint vertex_id [[vertex_id]])
{
    float2 xy = float2(vertex_id == 1u ? 3.0 : -1.0, vertex_id == 2u ? 3.0 : -1.0);

    Rp2VertexOut out;
    out.position = float4(xy, 0.0, 1.0);
    out.uv = float2(xy.x * 0.5 + 0.5, 0.5 - xy.y * 0.5);
    return out;
}
)MSL";

    // Uncharted2 tonemap —— 逐系数和 engine/shader/glsl/tone_mapping.frag 一致，
    // 也和 AppleSceneBlit 里那段一字不差。
    //
    // 为什么搬到这里：这一步以前**只能**由 blit 代劳（RP2 在 Apple 上不存在），
    // 于是"把离屏结果搬到屏幕"和"把 HDR 压成 LDR"两件事被焊在一次采样里。RP2
    // 接上之后显示变换回到它该在的位置，blit 退化成纯搬运。
    //
    // 源图是 RP1 的 backup_odd（R16G16B16A16_SFLOAT，线性 HDR），目标是
    // backup_even（同样 HDR 格式，但装的是 LDR 值 —— 后面 blit 直接搬，不再
    // 变换）。不做的话：>1 的地方硬切成纯白、其余整体偏暗，而且不报任何错。
    const char* kAppleRp2FragMsl = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct Rp2VertexOut
{
    float4 position [[position]];
    float2 uv;
};

static float3 Uncharted2Tonemap(float3 x)
{
    const float A = 0.15;
    const float B = 0.50;
    const float C = 0.10;
    const float D = 0.20;
    const float E = 0.02;
    const float F = 0.30;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}

fragment float4 Rp2FragmentMain(Rp2VertexOut     in            [[stage_in]],
                                texture2d<float> scene_texture [[texture(0)]],
                                sampler          scene_sampler [[sampler(0)]])
{
    float3 color = scene_texture.sample(scene_sampler, in.uv).rgb;

    color = Uncharted2Tonemap(color * 4.5);
    color = color * (1.0 / Uncharted2Tonemap(float3(11.2)));
    color = pow(color, float3(1.0 / 2.2));

    return float4(color, 1.0);
}
)MSL";
}  // namespace

bool AppleRp2Chain::Initialize(RHI* rhi, MainCameraFramebufferResources* framebuffers)
{
    m_Rhi = rhi;
    m_Framebuffers = framebuffers;

    if (rhi == nullptr || framebuffers == nullptr)
    {
        LOG_ERROR(ZRender, "AppleRp2Chain: rhi or framebuffer resources is null");
        return false;
    }

    // RP2 的 HDR render pass（backup_odd / backup_even，R16G16B16A16_SFLOAT）。
    // 它是 MainCameraFramebufferResources 建的，且 **resize 时不重建**
    // （`UpdateAfterFramebufferRecreate` 只重建 image/framebuffer），所以管线建好
    // 之后一直有效。
    RHIRenderPass* hdr_render_pass = framebuffers->getRp2HdrRenderPass();
    if (hdr_render_pass == nullptr)
    {
        LOG_ERROR(ZRender, "AppleRp2Chain: RP2 HDR render pass is null");
        return false;
    }

    RHIDescriptorSetLayoutBinding binding {};
    binding.binding         = 0;
    binding.descriptorType  = RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags      = RHI_SHADER_STAGE_FRAGMENT_BIT;

    RHIDescriptorSetLayoutCreateInfo layout_info {};
    layout_info.sType        = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_info.bindingCount = 1;
    layout_info.pBindings    = &binding;
    if (!rhi->CreateDescriptorSetLayout(&layout_info, m_DescriptorSetLayout) || m_DescriptorSetLayout == nullptr)
    {
        LOG_ERROR(ZRender, "AppleRp2Chain: descriptor set layout creation failed");
        return false;
    }

    RHIPipelineLayoutCreateInfo pipeline_layout_info {};
    pipeline_layout_info.sType          = RHI_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout_info.setLayoutCount = 1;
    pipeline_layout_info.pSetLayouts    = &m_DescriptorSetLayout;
    if (!rhi->CreatePipelineLayout(&pipeline_layout_info, m_PipelineLayout) || m_PipelineLayout == nullptr)
    {
        LOG_ERROR(ZRender, "AppleRp2Chain: pipeline layout creation failed");
        return false;
    }

    RHIDescriptorSetAllocateInfo alloc_info {};
    alloc_info.sType             = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool    = rhi->GetDescriptorPoor();
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts       = &m_DescriptorSetLayout;
    if (RHI_SUCCESS != rhi->AllocateDescriptorSets(&alloc_info, m_DescriptorSet) || m_DescriptorSet == nullptr)
    {
        LOG_ERROR(ZRender, "AppleRp2Chain: descriptor set allocation failed");
        return false;
    }

    RHISamplerCreateInfo sampler_info {};
    sampler_info.sType         = RHI_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter     = RHI_FILTER_LINEAR;
    sampler_info.minFilter     = RHI_FILTER_LINEAR;
    sampler_info.addressModeU  = RHI_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV  = RHI_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW  = RHI_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (!rhi->CreateSampler(&sampler_info, m_Sampler) || m_Sampler == nullptr)
    {
        LOG_ERROR(ZRender, "AppleRp2Chain: sampler creation failed");
        return false;
    }

    RHIShader* vert = rhi->CreateShaderModuleFromSource(kAppleRp2VertMsl, ShaderStage::Vertex, "apple_rp2.vert");
    RHIShader* frag = rhi->CreateShaderModuleFromSource(kAppleRp2FragMsl, ShaderStage::Fragment, "apple_rp2.frag");
    if (vert == nullptr || frag == nullptr)
    {
        LOG_ERROR(ZRender, "AppleRp2Chain: MSL compilation failed");
        if (vert != nullptr)
        {
            rhi->DestroyShaderModule(vert);
        }
        if (frag != nullptr)
        {
            rhi->DestroyShaderModule(frag);
        }
        return false;
    }

    RHIPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType  = RHI_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = RHI_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName  = "main";
    stages[1].sType  = RHI_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = RHI_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName  = "main";

    RHIPipelineVertexInputStateCreateInfo vertex_input {};
    vertex_input.sType = RHI_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    RHIPipelineInputAssemblyStateCreateInfo input_assembly {};
    input_assembly.sType                  = RHI_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology               = RHI_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    input_assembly.primitiveRestartEnable = RHI_FALSE;

    RHIPipelineViewportStateCreateInfo viewport_state {};
    viewport_state.sType         = RHI_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount  = 1;

    RHIPipelineRasterizationStateCreateInfo rasterization {};
    rasterization.sType        = RHI_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.polygonMode  = RHI_POLYGON_MODE_FILL;
    // 全屏三角形是按 vertex_id 直接给 NDC，绕序和场景网格相反 ⇒ 关掉剔除。
    rasterization.cullMode     = RHI_CULL_MODE_NONE;
    rasterization.frontFace    = RHI_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.lineWidth    = 1.0f;

    RHIPipelineMultisampleStateCreateInfo multisample {};
    multisample.sType                = RHI_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = RHI_SAMPLE_COUNT_1_BIT;

    // 只写那一个颜色附件。Metal 按 render pass 的附件数校验，多写一个就直接失败。
    RHIPipelineColorBlendAttachmentState blend {};
    blend.blendEnable    = RHI_FALSE;
    blend.colorWriteMask = RHI_COLOR_COMPONENT_R_BIT | RHI_COLOR_COMPONENT_G_BIT |
                           RHI_COLOR_COMPONENT_B_BIT | RHI_COLOR_COMPONENT_A_BIT;

    RHIPipelineColorBlendStateCreateInfo blend_state {};
    blend_state.sType           = RHI_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend_state.attachmentCount = 1;
    blend_state.pAttachments    = &blend;

    RHIDynamicState dynamic_states[] = {RHI_DYNAMIC_STATE_VIEWPORT, RHI_DYNAMIC_STATE_SCISSOR};
    RHIPipelineDynamicStateCreateInfo dynamic_state {};
    dynamic_state.sType             = RHI_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic_state.dynamicStateCount = 2;
    dynamic_state.pDynamicStates    = dynamic_states;

    RHIGraphicsPipelineCreateInfo pipeline_info {};
    pipeline_info.sType               = RHI_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_info.stageCount          = 2;
    pipeline_info.pStages             = stages;
    pipeline_info.pVertexInputState   = &vertex_input;
    pipeline_info.pInputAssemblyState = &input_assembly;
    pipeline_info.pViewportState      = &viewport_state;
    pipeline_info.pRasterizationState = &rasterization;
    pipeline_info.pMultisampleState   = &multisample;
    pipeline_info.pColorBlendState    = &blend_state;
    pipeline_info.pDynamicState       = &dynamic_state;
    pipeline_info.layout              = m_PipelineLayout;
    pipeline_info.renderPass          = hdr_render_pass;
    pipeline_info.subpass             = 0;

    const bool ok = rhi->CreateGraphicsPipelines(RHI_NULL_HANDLE, 1, &pipeline_info, m_Pipeline);
    rhi->DestroyShaderModule(vert);
    rhi->DestroyShaderModule(frag);

    if (!ok || m_Pipeline == nullptr)
    {
        LOG_ERROR(ZRender, "AppleRp2Chain: graphics pipeline creation failed");
        return false;
    }

    m_Ready = true;
    LOG_INFO(ZRender, "AppleRp2Chain: offscreen post chain (backup_odd -> backup_even) ready");
    return true;
}

void AppleRp2Chain::Shutdown()
{
    // 和 AppleSceneBlit 一样：RHI 层没有 DestroyPipeline / DestroyDescriptorSetLayout，
    // 只能置空，见 RenderResource::DestroyGPUResources 里的说明。
    m_Ready = false;
    m_Pipeline            = nullptr;
    m_DescriptorSet       = nullptr;
    m_PipelineLayout      = nullptr;
    m_DescriptorSetLayout = nullptr;
    m_Sampler             = nullptr;
    m_Framebuffers        = nullptr;
    m_Rhi                 = nullptr;
}

bool AppleRp2Chain::CanRender() const
{
    if (!m_Ready || m_Rhi == nullptr || m_Framebuffers == nullptr)
    {
        return false;
    }

    // 每帧重取：resize 会销毁重建 framebuffer / attachment，句柄跟着变。
    if (m_Framebuffers->getRp2ColorGradingFramebuffer() == nullptr)
    {
        return false;
    }
    if (m_Framebuffers->getAttachmentView(_main_camera_pass_backup_buffer_odd) == nullptr)
    {
        return false;
    }

    const RHIExtent2D extent = m_Rhi->GetSwapchainInfo().extent;
    return extent.width > 0 && extent.height > 0;
}

RHIImageView* AppleRp2Chain::GetOutputView() const
{
    if (!CanRender())
    {
        return nullptr;
    }
    // RP2 step 0 的输出：无 FXAA 时是 backup_even。
    return m_Framebuffers->getAttachmentView(_main_camera_pass_backup_buffer_even);
}

void AppleRp2Chain::WriteSceneDescriptor(RHIImageView* source_view)
{
    RHIDescriptorImageInfo image_info {};
    image_info.sampler     = m_Sampler;
    image_info.imageView   = source_view;
    image_info.imageLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    RHIWriteDescriptorSet write {};
    write.sType           = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet          = m_DescriptorSet;
    write.dstBinding      = 0;
    write.dstArrayElement = 0;
    write.descriptorCount = 1;
    write.descriptorType  = RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo      = &image_info;
    m_Rhi->UpdateDescriptorSets(1, &write, 0, nullptr);
}

void AppleRp2Chain::Record(RHICommandBuffer* command_buffer)
{
    if (command_buffer == nullptr || !CanRender())
    {
        return;
    }

    // 每帧重取：resize 会销毁重建 framebuffer / attachment，句柄是跟着变的。
    RHIFramebuffer* framebuffer = m_Framebuffers->getRp2ColorGradingFramebuffer();
    RHIImageView*   source_view = m_Framebuffers->getAttachmentView(_main_camera_pass_backup_buffer_odd);

    const RHIExtent2D extent = m_Rhi->GetSwapchainInfo().extent;

    WriteSceneDescriptor(source_view);

    RHIClearValue clear_value {};
    clear_value.color.float32[0] = 0.0f;
    clear_value.color.float32[1] = 0.0f;
    clear_value.color.float32[2] = 0.0f;
    clear_value.color.float32[3] = 1.0f;

    RHIRenderPassBeginInfo begin_info {};
    begin_info.sType           = RHI_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    begin_info.renderPass      = m_Framebuffers->getRp2HdrRenderPass();
    begin_info.framebuffer     = framebuffer;
    begin_info.renderArea      = {{0, 0}, extent};
    begin_info.clearValueCount = 1;
    begin_info.pClearValues    = &clear_value;

    m_Rhi->CmdBeginRenderPassPFN(command_buffer, &begin_info, RHI_SUBPASS_CONTENTS_INLINE);

    const RHIViewport viewport {
        0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f};
    const RHIRect2D scissor {{0, 0}, extent.width, extent.height};
    m_Rhi->CmdSetViewportPFN(command_buffer, 0, 1, &viewport);
    m_Rhi->CmdSetScissorPFN(command_buffer, 0, 1, &scissor);

    m_Rhi->CmdBindPipelinePFN(command_buffer, RHI_PIPELINE_BIND_POINT_GRAPHICS, m_Pipeline);
    m_Rhi->CmdBindDescriptorSetsPFN(command_buffer,
                                    RHI_PIPELINE_BIND_POINT_GRAPHICS,
                                    m_PipelineLayout,
                                    0,
                                    1,
                                    &m_DescriptorSet,
                                    0,
                                    nullptr);
    m_Rhi->CmdDraw(command_buffer, 3, 1, 0, 0);

    m_Rhi->CmdEndRenderPassPFN(command_buffer);
}

#endif  // __APPLE__
