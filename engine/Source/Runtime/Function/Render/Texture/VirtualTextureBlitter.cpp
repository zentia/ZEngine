#include "VirtualTextureBlitter.h"

#include "Runtime/Function/Render/Interface/RHI.h"
#include "VirtualTextureManager.h"
#include "VirtualTextureResource.h"

#include <cstring>
#include <string>
#include <vector>

#if ZENGINE_VT_GPU && defined(__has_include)
    #if __has_include(<vt_blit_vert.h>) && __has_include(<vt_blit_frag.h>)
        #include <vt_blit_frag.h>
        #include <vt_blit_vert.h>
        #define ZENGINE_VT_HAS_BLIT_SPIRV 1
    #endif
#endif
#ifndef ZENGINE_VT_HAS_BLIT_SPIRV
namespace Z
{
namespace Shader
{
    static const std::vector<unsigned char> VT_BLIT_VERT {};
    static const std::vector<unsigned char> VT_BLIT_FRAG {};
}
}  // namespace Z::Shader
#endif

namespace
{
    // 与 vt_blit.frag 的 _vt_blit_params 逐字段对应（std140：uvec4/uvec4/vec4，无填充）。
    struct BlitParams
    {
        uint32_t lookup[4];  // page_size, pool_width_pages, pages_x, pages_y
        uint32_t extent[4];  // virtual width, virtual height, unused, unused
        float fallback[4];
    };
    static_assert(sizeof(BlitParams) == 48, "BlitParams must match the std140 block in vt_blit.frag");

    constexpr uint32_t kBindingParams = 0;
    constexpr uint32_t kBindingPageTable = 1;
    constexpr uint32_t kBindingPhysicalPool = 2;
    constexpr uint32_t kBindingCount = 3;
    constexpr RHIDeviceSize kParamsBufferBytes = 256;  // >= sizeof(BlitParams)，且满足 D3D12 的 CBV 256 字节对齐
    static_assert(sizeof(BlitParams) <= kParamsBufferBytes, "params must fit the CBV-aligned buffer");

    // UNORM 而不是 SRGB：页池是 R8G8B8A8_UNORM，texelFetch 得 k/255，写进 UNORM 目标
    // 再按最近取整还原成 k，整条链逐字节可比。SRGB 目标会做一次 linear→sRGB 编码，
    // 把每个字节都改掉。
    constexpr RHIFormat kTargetFormat = RHI_FORMAT_R8G8B8A8_UNORM;

    constexpr RHIMemoryPropertyFlags kHostMemory =
        RHI_MEMORY_PROPERTY_HOST_VISIBLE_BIT | RHI_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    // 读回缓冲的行跨度（单位 texel）。D3D12 的 buffer<-texture 拷贝要求行距是 256 字节的倍数；
    // 把行宽向上取整到 64 texel（= 256 字节），Vulkan 的 bufferRowLength（texel）和 D3D12 的行距就是
    // 同一个数，两个后端用同一份读回布局。取整后行距恰好等于 row_texels * 4，D3D12 不再自己补尾巴。
    uint32_t ReadbackRowTexels(uint32_t width)
    {
        return (width + 63u) & ~63u;
    }

    std::string VtShaderRoot()
    {
#ifdef ZENGINE_SHADER_ROOT
        return ZENGINE_SHADER_ROOT;
#else
        return "e:/Engine/ZEngine/engine/shader";
#endif
    }

    // Vulkan 吃预编译 SPIR-V；其它后端（DX12）由 RHI 在运行时用 DXC 编译 HLSL（hlsl/vt/*.hlsl）。
    bool CreateBlitShaders(RHI* rhi, RHIShader*& vert, RHIShader*& frag)
    {
        vert = nullptr;
        frag = nullptr;
        if (VirtualTextureUsesVulkanPath(rhi))
        {
            vert = rhi->CreateShaderModule(Z::Shader::VT_BLIT_VERT);
            frag = rhi->CreateShaderModule(Z::Shader::VT_BLIT_FRAG);
        }
        else
        {
            const std::string root = VtShaderRoot() + "/hlsl/vt/";
            std::vector<uint8_t> binary;
            vert = rhi->CreateShaderModuleFromFile(root + "vt_blit.vert.hlsl", ShaderStage::Vertex, {}, {}, binary);
            frag = rhi->CreateShaderModuleFromFile(root + "vt_blit.frag.hlsl", ShaderStage::Fragment, {}, {}, binary);
        }
        return vert != nullptr && frag != nullptr;
    }
}  // namespace

VirtualTextureBlitter::~VirtualTextureBlitter()
{
    Shutdown();
}

bool VirtualTextureBlitter::Initialize(RHI* rhi)
{
#if !ZENGINE_VT_GPU
    (void)rhi;
    return false;
#else
    Shutdown();
    // Vulkan 需要预编译的 SPIR-V；DX12 在运行时编译 HLSL，不依赖那两个数组。
    if (rhi == nullptr || !VirtualTextureBackendSupported(rhi) ||
        (VirtualTextureUsesVulkanPath(rhi) && (Z::Shader::VT_BLIT_VERT.empty() || Z::Shader::VT_BLIT_FRAG.empty())))
    {
        return false;
    }
    m_Rhi = rhi;

    // 采样器 NEAREST + clamp：VT 查找只用 texelFetch，描述符只需要一个合法句柄。
    RHISamplerCreateInfo sampler_info {};
    sampler_info.sType = RHI_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter = RHI_FILTER_NEAREST;
    sampler_info.minFilter = RHI_FILTER_NEAREST;
    sampler_info.mipmapMode = RHI_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler_info.addressModeU = RHI_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = RHI_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = RHI_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.minLod = 0.0f;
    sampler_info.maxLod = 0.0f;
    if (!m_Rhi->CreateSampler(&sampler_info, m_Sampler))
    {
        Shutdown();
        return false;
    }

    // CBV 视图的大小按 256 字节对齐（D3D12 硬性要求），而 RHI 在 DX12 上建 CBV 时就是向上取整到 256；
    // buffer 若只有 sizeof(BlitParams)=48 字节，CBV 范围会越过资源末尾（debug layer 报 ERROR #649）。
    // 所以参数缓冲一律按 256 字节分配；Vulkan 无此要求，多出的字节无害。
    m_Rhi->CreateBuffer(
        kParamsBufferBytes, RHI_BUFFER_USAGE_UNIFORM_BUFFER_BIT, kHostMemory, m_ParamsBuffer, m_ParamsMemory);
    if (m_ParamsBuffer == nullptr || m_ParamsMemory == nullptr)
    {
        Shutdown();
        return false;
    }

    // ---- render pass：1 张颜色附件，clear → store，终态 COLOR_ATTACHMENT_OPTIMAL ----
    // 与 Nanite 的离屏 pass 同一手法：不声明 subpass dependency，读回前用显式 barrier
    // 把布局转到 TRANSFER_SRC。
    RHIAttachmentDescription attachment {};
    attachment.format = kTargetFormat;
    attachment.samples = RHI_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = RHI_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = RHI_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = RHI_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = RHI_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = RHI_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = RHI_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    RHIAttachmentReference color_reference {};
    color_reference.attachment = 0;
    color_reference.layout = RHI_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    RHISubpassDescription subpass {};
    subpass.pipelineBindPoint = RHI_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_reference;

    RHIRenderPassCreateInfo render_pass_info {};
    render_pass_info.sType = RHI_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    render_pass_info.attachmentCount = 1;
    render_pass_info.pAttachments = &attachment;
    render_pass_info.subpassCount = 1;
    render_pass_info.pSubpasses = &subpass;
    if (!m_Rhi->CreateRenderPass(&render_pass_info, m_RenderPass))
    {
        Shutdown();
        return false;
    }

    // ---- 描述符布局：params(UBO) / 页表 / 页池，全部只在 fragment 阶段 ----
    RHIDescriptorSetLayoutBinding bindings[kBindingCount] = {};
    bindings[kBindingParams] = {
        kBindingParams, RHI_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, RHI_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    bindings[kBindingPageTable] = {
        kBindingPageTable, RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, RHI_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    bindings[kBindingPhysicalPool] = {
        kBindingPhysicalPool, RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, RHI_SHADER_STAGE_FRAGMENT_BIT, nullptr};

    RHIDescriptorSetLayoutCreateInfo set_layout_info {};
    set_layout_info.sType = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set_layout_info.bindingCount = kBindingCount;
    set_layout_info.pBindings = bindings;
    if (!m_Rhi->CreateDescriptorSetLayout(&set_layout_info, m_SetLayout))
    {
        Shutdown();
        return false;
    }

    RHIPipelineLayoutCreateInfo pipeline_layout_info {};
    pipeline_layout_info.sType = RHI_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout_info.setLayoutCount = 1;
    pipeline_layout_info.pSetLayouts = &m_SetLayout;
    if (!m_Rhi->CreatePipelineLayout(&pipeline_layout_info, m_PipelineLayout))
    {
        Shutdown();
        return false;
    }

    RHIDescriptorSetAllocateInfo alloc_info {};
    alloc_info.sType = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool = m_Rhi->GetDescriptorPoor();
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &m_SetLayout;
    if (RHI_SUCCESS != m_Rhi->AllocateDescriptorSets(&alloc_info, m_DescriptorSet))
    {
        Shutdown();
        return false;
    }

    // ---- 图形管线 ----
    RHIShader* vert = nullptr;
    RHIShader* frag = nullptr;
    if (!CreateBlitShaders(m_Rhi, vert, frag))
    {
        if (vert != nullptr)
        {
            m_Rhi->DestroyShaderModule(vert);
        }
        if (frag != nullptr)
        {
            m_Rhi->DestroyShaderModule(frag);
        }
        Shutdown();
        return false;
    }

    RHIPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = RHI_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = RHI_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].sType = RHI_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = RHI_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";

    // 无顶点输入：位置 / UV 全由 gl_VertexIndex 算出。
    RHIPipelineVertexInputStateCreateInfo vertex_input {};
    vertex_input.sType = RHI_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    RHIPipelineInputAssemblyStateCreateInfo input_assembly {};
    input_assembly.sType = RHI_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = RHI_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    input_assembly.primitiveRestartEnable = RHI_FALSE;

    // viewport / scissor 是动态状态（每次 Render 按目标尺寸设）；这里的静态值只是占位。
    RHIViewport static_viewport {0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
    RHIRect2D static_scissor {};
    static_scissor.extent.width = 1;
    static_scissor.extent.height = 1;
    RHIPipelineViewportStateCreateInfo viewport_state {};
    viewport_state.sType = RHI_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.pViewports = &static_viewport;
    viewport_state.scissorCount = 1;
    viewport_state.pScissors = &static_scissor;

    // CULL_NONE：全屏三角形在 Vulkan 的 y 向下约定里绕序是顺时针，不想让这个测试
    // 依赖"哪个绕序算正面"——验证的是页表查找，不是背面剔除。
    RHIPipelineRasterizationStateCreateInfo rasterization {};
    rasterization.sType = RHI_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.polygonMode = RHI_POLYGON_MODE_FILL;
    rasterization.cullMode = RHI_CULL_MODE_NONE;
    rasterization.frontFace = RHI_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.lineWidth = 1.0f;

    RHIPipelineMultisampleStateCreateInfo multisample {};
    multisample.sType = RHI_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = RHI_SAMPLE_COUNT_1_BIT;

    RHIPipelineColorBlendAttachmentState blend {};
    blend.blendEnable = RHI_FALSE;
    blend.colorWriteMask =
        RHI_COLOR_COMPONENT_R_BIT | RHI_COLOR_COMPONENT_G_BIT | RHI_COLOR_COMPONENT_B_BIT | RHI_COLOR_COMPONENT_A_BIT;
    RHIPipelineColorBlendStateCreateInfo blend_state {};
    blend_state.sType = RHI_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend_state.attachmentCount = 1;
    blend_state.pAttachments = &blend;

    // 没有深度附件：显式给一个全关的深度状态，不传 null（有的后端会解引用）。
    RHIPipelineDepthStencilStateCreateInfo depth_stencil {};
    depth_stencil.sType = RHI_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil.depthTestEnable = RHI_FALSE;
    depth_stencil.depthWriteEnable = RHI_FALSE;
    depth_stencil.depthCompareOp = RHI_COMPARE_OP_ALWAYS;

    RHIDynamicState dynamic_states[] = {RHI_DYNAMIC_STATE_VIEWPORT, RHI_DYNAMIC_STATE_SCISSOR};
    RHIPipelineDynamicStateCreateInfo dynamic_state {};
    dynamic_state.sType = RHI_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic_state.dynamicStateCount = 2;
    dynamic_state.pDynamicStates = dynamic_states;

    RHIGraphicsPipelineCreateInfo pipeline_info {};
    pipeline_info.sType = RHI_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_info.stageCount = 2;
    pipeline_info.pStages = stages;
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &input_assembly;
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &rasterization;
    pipeline_info.pMultisampleState = &multisample;
    pipeline_info.pColorBlendState = &blend_state;
    pipeline_info.pDepthStencilState = &depth_stencil;
    pipeline_info.pDynamicState = &dynamic_state;
    pipeline_info.layout = m_PipelineLayout;
    pipeline_info.renderPass = m_RenderPass;
    pipeline_info.subpass = 0;

    const bool ok = m_Rhi->CreateGraphicsPipelines(RHI_NULL_HANDLE, 1, &pipeline_info, m_Pipeline);
    m_Rhi->DestroyShaderModule(vert);
    m_Rhi->DestroyShaderModule(frag);
    if (!ok || m_Pipeline == nullptr)
    {
        Shutdown();
        return false;
    }

    m_Ready = true;
    return true;
#endif
}

void VirtualTextureBlitter::DestroyTarget()
{
    if (m_Rhi == nullptr)
    {
        return;
    }
    if (m_Framebuffer != nullptr)
    {
        m_Rhi->DestroyFramebuffer(m_Framebuffer);
        m_Framebuffer = nullptr;
    }
    // 逆序销毁：view 先于 image，image 先于 memory。
    if (m_TargetView != nullptr)
    {
        m_Rhi->DestroyImageView(m_TargetView);
        m_TargetView = nullptr;
    }
    if (m_TargetImage != nullptr)
    {
        m_Rhi->DestroyImage(m_TargetImage);
        m_TargetImage = nullptr;
    }
    if (m_TargetMemory != nullptr)
    {
        m_Rhi->FreeMemory(m_TargetMemory);
        m_TargetMemory = nullptr;
    }
    if (m_ReadbackMemory != nullptr)
    {
        m_Rhi->FreeMemory(m_ReadbackMemory);
        m_ReadbackMemory = nullptr;
    }
    if (m_ReadbackBuffer != nullptr)
    {
        m_Rhi->DestroyBuffer(m_ReadbackBuffer);
        m_ReadbackBuffer = nullptr;
    }
    m_TargetWidth = 0;
    m_TargetHeight = 0;
}

void VirtualTextureBlitter::Shutdown()
{
    DestroyTarget();
    if (m_Rhi != nullptr)
    {
        if (m_RenderPass != nullptr)
        {
            m_Rhi->DestroyRenderPass(m_RenderPass);
        }
        if (m_ParamsMemory != nullptr)
        {
            m_Rhi->FreeMemory(m_ParamsMemory);
        }
        if (m_ParamsBuffer != nullptr)
        {
            m_Rhi->DestroyBuffer(m_ParamsBuffer);
        }
        if (m_Sampler != nullptr)
        {
            m_Rhi->DestroySampler(m_Sampler);
        }
    }
    m_RenderPass = nullptr;
    m_ParamsMemory = nullptr;
    m_ParamsBuffer = nullptr;
    m_Sampler = nullptr;
    // 管线 / 布局 / 描述符集：RHI 没有对应销毁入口，只丢句柄。
    m_Pipeline = nullptr;
    m_PipelineLayout = nullptr;
    m_SetLayout = nullptr;
    m_DescriptorSet = nullptr;
    m_Ready = false;
    m_Rhi = nullptr;
}

bool VirtualTextureBlitter::EnsureTarget(uint32_t width, uint32_t height)
{
    if (m_TargetImage != nullptr && m_Framebuffer != nullptr && m_TargetWidth == width && m_TargetHeight == height)
    {
        return true;
    }
    DestroyTarget();

    m_Rhi->CreateImage(width,
                       height,
                       kTargetFormat,
                       RHI_IMAGE_TILING_OPTIMAL,
                       RHI_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | RHI_IMAGE_USAGE_TRANSFER_SRC_BIT,
                       RHI_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                       m_TargetImage,
                       m_TargetMemory,
                       0,
                       1,
                       1);
    if (m_TargetImage == nullptr)
    {
        DestroyTarget();
        return false;
    }
    m_Rhi->CreateImageView(
        m_TargetImage, kTargetFormat, RHI_IMAGE_ASPECT_COLOR_BIT, RHI_IMAGE_VIEW_TYPE_2D, 1, 1, m_TargetView);
    if (m_TargetView == nullptr)
    {
        DestroyTarget();
        return false;
    }

    RHIFramebufferCreateInfo framebuffer_info {};
    framebuffer_info.sType = RHI_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebuffer_info.renderPass = m_RenderPass;
    framebuffer_info.attachmentCount = 1;
    framebuffer_info.pAttachments = &m_TargetView;
    framebuffer_info.width = width;
    framebuffer_info.height = height;
    framebuffer_info.layers = 1;
    if (!m_Rhi->CreateFramebuffer(&framebuffer_info, m_Framebuffer))
    {
        DestroyTarget();
        return false;
    }

    const RHIDeviceSize readback_bytes = static_cast<RHIDeviceSize>(ReadbackRowTexels(width)) * height * 4u;
    m_Rhi->CreateBuffer(readback_bytes, RHI_BUFFER_USAGE_TRANSFER_DST_BIT, kHostMemory, m_ReadbackBuffer, m_ReadbackMemory);
    if (m_ReadbackBuffer == nullptr || m_ReadbackMemory == nullptr)
    {
        DestroyTarget();
        return false;
    }

    m_TargetWidth = width;
    m_TargetHeight = height;
    return true;
}

bool VirtualTextureBlitter::Render(VirtualTextureManager& manager,
                                   uint64_t handle,
                                   uint32_t out_width,
                                   uint32_t out_height,
                                   const float fallback_rgba[4],
                                   std::vector<uint32_t>& out_rgba)
{
    out_rgba.clear();
#if !ZENGINE_VT_GPU
    (void)manager;
    (void)handle;
    (void)out_width;
    (void)out_height;
    (void)fallback_rgba;
    return false;
#else
    if (!m_Ready || m_Rhi == nullptr || out_width == 0 || out_height == 0)
    {
        return false;
    }

    auto resource = manager.GetVirtualTexture(handle);
    auto pool = manager.getPhysicalTexturePool();
    if (!resource || !pool || pool->getPhysicalTextureView() == nullptr)
    {
        return false;
    }
    // 池图从没传过任何页 ⇒ 还是 UNDEFINED 布局，描述符里声明 SHADER_READ_ONLY 去读它
    // 是非法的。如实失败，不读未定义内容。
    if (!pool->hasContent())
    {
        return false;
    }

    const size_t pixel_count = static_cast<size_t>(out_width) * out_height;

    // 从未驻留过 ⇒ 没有页表纹理（懒创建）：整张图按 fallback 在 CPU 侧给出。
    RHIImageView* page_table_view = resource->getPageTableTextureView();
    if (page_table_view == nullptr)
    {
        const uint32_t fallback_packed =
            (static_cast<uint32_t>(fallback_rgba[0] * 255.0f + 0.5f) & 0xFFu) |
            ((static_cast<uint32_t>(fallback_rgba[1] * 255.0f + 0.5f) & 0xFFu) << 8) |
            ((static_cast<uint32_t>(fallback_rgba[2] * 255.0f + 0.5f) & 0xFFu) << 16) |
            ((static_cast<uint32_t>(fallback_rgba[3] * 255.0f + 0.5f) & 0xFFu) << 24);
        out_rgba.assign(pixel_count, fallback_packed);
        return true;
    }

    if (!EnsureTarget(out_width, out_height))
    {
        return false;
    }

    // ---- 参数 ----
    BlitParams params {};
    params.lookup[0] = pool->getPageSize();
    params.lookup[1] = pool->getPoolWidthPages();
    params.lookup[2] = resource->getPagesX();
    params.lookup[3] = resource->getPagesY();
    params.extent[0] = resource->getVirtualWidth();
    params.extent[1] = resource->getVirtualHeight();
    std::memcpy(params.fallback, fallback_rgba, sizeof(params.fallback));

    void* mapped = nullptr;
    if (!m_Rhi->MapMemory(m_ParamsMemory, 0, sizeof(BlitParams), 0, &mapped) || mapped == nullptr)
    {
        return false;
    }
    std::memcpy(mapped, &params, sizeof(BlitParams));
    m_Rhi->UnmapMemory(m_ParamsMemory);

    // ---- 描述符：每次重写（页表 / 页池图像可能已重建）----
    RHIDescriptorBufferInfo params_info {};
    params_info.buffer = m_ParamsBuffer;
    params_info.offset = 0;
    params_info.range = sizeof(BlitParams);

    RHIDescriptorImageInfo table_info {};
    table_info.sampler = m_Sampler;
    table_info.imageView = page_table_view;
    table_info.imageLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    RHIDescriptorImageInfo pool_info {};
    pool_info.sampler = m_Sampler;
    pool_info.imageView = pool->getPhysicalTextureView();
    pool_info.imageLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    RHIWriteDescriptorSet writes[kBindingCount] = {};
    for (uint32_t i = 0; i < kBindingCount; ++i)
    {
        writes[i].sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = m_DescriptorSet;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
    }
    writes[kBindingParams].descriptorType = RHI_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[kBindingParams].pBufferInfo = &params_info;
    writes[kBindingPageTable].descriptorType = RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[kBindingPageTable].pImageInfo = &table_info;
    writes[kBindingPhysicalPool].descriptorType = RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[kBindingPhysicalPool].pImageInfo = &pool_info;
    m_Rhi->UpdateDescriptorSets(kBindingCount, writes, 0, nullptr);

    // ---- 录制：开 pass → 画全屏三角形 → 转布局 → 拷回 ----
    RHICommandBuffer* command_buffer = m_Rhi->BeginSingleTimeCommands();
    if (command_buffer == nullptr)
    {
        return false;
    }

    // clear 成 (0,0,0,0)：fallback 的 alpha 为 0 时无法与"清屏没画到"区分，所以测试用
    // alpha=255 的 fallback —— 见测试注释。
    RHIClearValue clear_value {};
    RHIRenderPassBeginInfo begin_info {};
    begin_info.sType = RHI_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    begin_info.renderPass = m_RenderPass;
    begin_info.framebuffer = m_Framebuffer;
    begin_info.renderArea.offset.x = 0;
    begin_info.renderArea.offset.y = 0;
    begin_info.renderArea.extent.width = out_width;
    begin_info.renderArea.extent.height = out_height;
    begin_info.clearValueCount = 1;
    begin_info.pClearValues = &clear_value;
    m_Rhi->CmdBeginRenderPassPFN(command_buffer, &begin_info, RHI_SUBPASS_CONTENTS_INLINE);

    // 动态 viewport / scissor 不设在 Vulkan 上是未定义行为（真的画不出来）。
    RHIViewport viewport {0.0f, 0.0f, static_cast<float>(out_width), static_cast<float>(out_height), 0.0f, 1.0f};
    m_Rhi->CmdSetViewportPFN(command_buffer, 0, 1, &viewport);
    RHIRect2D scissor {};
    scissor.extent.width = out_width;
    scissor.extent.height = out_height;
    m_Rhi->CmdSetScissorPFN(command_buffer, 0, 1, &scissor);

    m_Rhi->CmdBindPipelinePFN(command_buffer, RHI_PIPELINE_BIND_POINT_GRAPHICS, m_Pipeline);
    m_Rhi->CmdBindDescriptorSetsPFN(
        command_buffer, RHI_PIPELINE_BIND_POINT_GRAPHICS, m_PipelineLayout, 0, 1, &m_DescriptorSet, 0, nullptr);
    m_Rhi->CmdDraw(command_buffer, 3, 1, 0, 0);
    m_Rhi->CmdEndRenderPassPFN(command_buffer);

    // COLOR_ATTACHMENT_OPTIMAL → TRANSFER_SRC_OPTIMAL，然后拷进 host 可见 buffer。
    RHIImageMemoryBarrier to_transfer_src {};
    to_transfer_src.sType = RHI_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_transfer_src.srcAccessMask = RHI_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    to_transfer_src.dstAccessMask = RHI_ACCESS_TRANSFER_READ_BIT;
    to_transfer_src.oldLayout = RHI_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    to_transfer_src.newLayout = RHI_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_transfer_src.srcQueueFamilyIndex = ~0u;
    to_transfer_src.dstQueueFamilyIndex = ~0u;
    to_transfer_src.image = m_TargetImage;
    to_transfer_src.subresourceRange.aspectMask = RHI_IMAGE_ASPECT_COLOR_BIT;
    to_transfer_src.subresourceRange.levelCount = 1;
    to_transfer_src.subresourceRange.layerCount = 1;
    m_Rhi->CmdPipelineBarrier(command_buffer,
                              RHI_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                              RHI_PIPELINE_STAGE_TRANSFER_BIT,
                              0,
                              0,
                              nullptr,
                              0,
                              nullptr,
                              1,
                              &to_transfer_src);

    RHIBufferImageCopy region {};
    region.bufferRowLength = ReadbackRowTexels(out_width);  // texel 数，不是字节；见 ReadbackRowTexels
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = RHI_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent.width = out_width;
    region.imageExtent.height = out_height;
    region.imageExtent.depth = 1;
    m_Rhi->CmdCopyImageToBuffer(
        command_buffer, m_TargetImage, RHI_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_ReadbackBuffer, 1, &region);

    m_Rhi->EndSingleTimeCommands(command_buffer);  // 内部 QueueWaitIdle：返回即完成

    // ---- 读回 ----
    void* result = nullptr;
    const uint32_t row_texels = ReadbackRowTexels(out_width);
    const RHIDeviceSize bytes = static_cast<RHIDeviceSize>(row_texels) * out_height * 4u;
    if (!m_Rhi->MapMemory(m_ReadbackMemory, 0, bytes, 0, &result) || result == nullptr)
    {
        return false;
    }
    const auto* pixels = static_cast<const uint32_t*>(result);
    out_rgba.resize(pixel_count);
    for (uint32_t row = 0; row < out_height; ++row)
    {
        std::memcpy(out_rgba.data() + static_cast<size_t>(row) * out_width,
                    pixels + static_cast<size_t>(row) * row_texels,
                    static_cast<size_t>(out_width) * sizeof(uint32_t));
    }
    m_Rhi->UnmapMemory(m_ReadbackMemory);
    return true;
#endif
}
