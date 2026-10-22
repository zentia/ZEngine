#include "Runtime/Function/Render/AppleSceneBlit.h"

#if defined(__APPLE__)

    #include "Runtime/Core/Base/Macro.h"
    #include "Runtime/Function/Render/Interface/RHI.h"

// ---------------------------------------------------------------------
// MSL：一个全屏三角形 + 一次采样。
//
// **入口绝对不能叫 `main`**。Metal 的编译器直接拒绝：
//   program_source:10:26: error: vertex function cannot be called 'main'
// 构建期看不见这个错（MSL 是字符串，要等运行时 newLibraryWithSource:），所以
// AppleSceneBlitSmokeTest 里有一条专门的断言盯着它。
//
// 名字虽然不是 "main"，照样能被找到：MetalRHI::CreateShaderModuleFromSource
// 硬编码 `CreateMetalShaderFromMSL(..., "main")`，而它在 `newFunctionWithName:
// @"main"` 落空时会退回 `library.functionNames.firstObject` —— 每个 library
// 只有一个函数，所以兜底就是我们要的那个。（SPIRV-Cross 生成的是 `main0`，
// 走的也是这条兜底。）它不看 RHIPipelineShaderStageCreateInfo 的 pName。
//
// 槽位：MetalRHI::CmdBindDescriptorSetsPFN 里
//   texture_index = set_index * k_msl_descriptor_set_stride + binding
// set 0 / binding 0 ⇒ [[texture(0)]] 和 [[sampler(0)]]。改 binding 号就要同步改
// 这里，两边是手工对齐的（RHIDescriptorSetLayout 是不透明的，拿不回来）。
// ---------------------------------------------------------------------
namespace
{
    const char* kAppleSceneBlitVertMsl = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct BlitVertexOut
{
    float4 position [[position]];
    float2 uv;
};

// 覆盖整屏的大三角形：(-1,-1) (3,-1) (-1,3)。比两个三角形少一条对角线上的
// quad 重复着色，也不需要顶点缓冲。
vertex BlitVertexOut BlitVertexMain(uint vertex_id [[vertex_id]])
{
    float2 xy = float2(vertex_id == 1u ? 3.0 : -1.0, vertex_id == 2u ? 3.0 : -1.0);

    BlitVertexOut out;
    out.position = float4(xy, 0.0, 1.0);
    // NDC 的 y=+1 在屏幕上边，而纹理的 v=0 在图像上边 ⇒ v 要翻过来，否则整个
    // 画面上下颠倒。
    out.uv = float2(xy.x * 0.5 + 0.5, 0.5 - xy.y * 0.5);
    return out;
}
)MSL";

    const char* kAppleSceneBlitFragMsl = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct BlitVertexOut
{
    float4 position [[position]];
    float2 uv;
};

fragment float4 BlitFragmentMain(BlitVertexOut     in            [[stage_in]],
                     texture2d<float>           scene_texture [[texture(0)]],
                     sampler                    scene_sampler [[sampler(0)]])
{
    return scene_texture.sample(scene_sampler, in.uv);
}
)MSL";
}  // namespace

bool AppleSceneBlit::Initialize(RHI* rhi, RHIRenderPass* swapchain_render_pass)
{
    m_Rhi = rhi;
    if (rhi == nullptr || swapchain_render_pass == nullptr)
    {
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
        LOG_ERROR(ZRender, "AppleSceneBlit: descriptor set layout creation failed");
        Shutdown();
        return false;
    }

    RHIPipelineLayoutCreateInfo pipeline_layout_info {};
    pipeline_layout_info.sType          = RHI_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout_info.setLayoutCount = 1;
    pipeline_layout_info.pSetLayouts    = &m_DescriptorSetLayout;
    if (!rhi->CreatePipelineLayout(&pipeline_layout_info, m_PipelineLayout) || m_PipelineLayout == nullptr)
    {
        LOG_ERROR(ZRender, "AppleSceneBlit: pipeline layout creation failed");
        Shutdown();
        return false;
    }

    RHIDescriptorSetAllocateInfo alloc_info {};
    alloc_info.sType             = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool    = rhi->GetDescriptorPoor();
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts       = &m_DescriptorSetLayout;
    if (RHI_SUCCESS != rhi->AllocateDescriptorSets(&alloc_info, m_DescriptorSet) || m_DescriptorSet == nullptr)
    {
        LOG_ERROR(ZRender, "AppleSceneBlit: descriptor set allocation failed");
        Shutdown();
        return false;
    }

    RHISamplerCreateInfo sampler_info {};
    sampler_info.sType     = RHI_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter = RHI_FILTER_LINEAR;
    sampler_info.minFilter = RHI_FILTER_LINEAR;
    sampler_info.addressModeU = RHI_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = RHI_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = RHI_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (!rhi->CreateSampler(&sampler_info, m_Sampler) || m_Sampler == nullptr)
    {
        LOG_ERROR(ZRender, "AppleSceneBlit: sampler creation failed");
        Shutdown();
        return false;
    }

    RHIShader* vert = rhi->CreateShaderModuleFromSource(kAppleSceneBlitVertMsl, ShaderStage::Vertex, "apple_scene_blit.vert");
    RHIShader* frag = rhi->CreateShaderModuleFromSource(kAppleSceneBlitFragMsl, ShaderStage::Fragment, "apple_scene_blit.frag");
    if (vert == nullptr || frag == nullptr)
    {
        LOG_ERROR(ZRender, "AppleSceneBlit: MSL compilation failed");
        if (vert != nullptr)
        {
            rhi->DestroyShaderModule(vert);
        }
        if (frag != nullptr)
        {
            rhi->DestroyShaderModule(frag);
        }
        Shutdown();
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

    // 顶点位置由 vertex_id 生成，一个顶点缓冲都不绑。
    RHIPipelineVertexInputStateCreateInfo vertex_input {};
    vertex_input.sType = RHI_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    RHIPipelineInputAssemblyStateCreateInfo input_assembly {};
    input_assembly.sType                  = RHI_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology               = RHI_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    input_assembly.primitiveRestartEnable = RHI_FALSE;

    RHIPipelineViewportStateCreateInfo viewport_state {};
    viewport_state.sType        = RHI_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount  = 1;

    RHIPipelineRasterizationStateCreateInfo rasterization {};
    rasterization.sType      = RHI_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.polygonMode = RHI_POLYGON_MODE_FILL;
    // 全屏三角形的绕序和场景网格相反（它是按 vertex_id 直接给的 NDC），所以
    // 关掉剔除 —— 剔错了就是整屏黑。
    rasterization.cullMode    = RHI_CULL_MODE_NONE;
    rasterization.frontFace   = RHI_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.lineWidth   = 1.0f;

    RHIPipelineMultisampleStateCreateInfo multisample {};
    multisample.sType                = RHI_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = RHI_SAMPLE_COUNT_1_BIT;

    // 只写 swapchain 那一个颜色附件。不能多写一个：Metal 是按 render pass 的附件
    // 数来校验的（Nanite 那边踩过同一类问题）。
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
    pipeline_info.sType                = RHI_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_info.stageCount           = 2;
    pipeline_info.pStages              = stages;
    pipeline_info.pVertexInputState    = &vertex_input;
    pipeline_info.pInputAssemblyState  = &input_assembly;
    pipeline_info.pViewportState       = &viewport_state;
    pipeline_info.pRasterizationState  = &rasterization;
    pipeline_info.pMultisampleState    = &multisample;
    pipeline_info.pColorBlendState     = &blend_state;
    pipeline_info.pDynamicState        = &dynamic_state;
    pipeline_info.layout               = m_PipelineLayout;
    pipeline_info.renderPass           = swapchain_render_pass;
    pipeline_info.subpass              = 0;

    const bool ok = rhi->CreateGraphicsPipelines(RHI_NULL_HANDLE, 1, &pipeline_info, m_Pipeline);
    rhi->DestroyShaderModule(vert);
    rhi->DestroyShaderModule(frag);

    if (!ok || m_Pipeline == nullptr)
    {
        LOG_ERROR(ZRender, "AppleSceneBlit: graphics pipeline creation failed");
        Shutdown();
        return false;
    }

    LOG_INFO(ZRender, "AppleSceneBlit: fullscreen scene -> swapchain pipeline ready");
    return true;
}

void AppleSceneBlit::Shutdown()
{
    if (m_Rhi == nullptr)
    {
        return;
    }
    // 没有 DestroyPipeline / DestroyDescriptorSetLayout（RHI 层就没有），所以这里
    // 只能置空让它们泄漏 —— 和引擎里其它 pass 一样，见 RenderResource::
    // DestroyGPUResources 里的说明。
    m_Pipeline     = nullptr;
    m_DescriptorSet = nullptr;
    m_PipelineLayout = nullptr;
    m_DescriptorSetLayout = nullptr;
    m_Sampler     = nullptr;
    m_Rhi         = nullptr;
}

void AppleSceneBlit::Record(RHICommandBuffer* command_buffer, RHIImageView* source)
{
    if (!IsReady() || command_buffer == nullptr || source == nullptr)
    {
        return;
    }

    // 每次都重写 descriptor：resize 会销毁重建 framebuffer，source 视图是跟着变的。
    RHIDescriptorImageInfo image_info {};
    image_info.sampler     = m_Sampler;
    image_info.imageView   = source;
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
}

#endif  // __APPLE__
