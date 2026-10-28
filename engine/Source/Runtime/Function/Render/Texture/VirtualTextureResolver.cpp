#include "VirtualTextureResolver.h"

#include "Runtime/Function/Render/Interface/RHI.h"
#include "VirtualTextureManager.h"
#include "VirtualTextureResource.h"

#include <cstring>

// 生成的 SPIR-V 头缺失（环境没有 glslangValidator）⇒ 退化成"没有 shader"，Resolver 的
// Initialize 返回 false，而不是让整个 ZRuntime 编译不过。与 NaniteRenderer 同一手法。
#if ZENGINE_VT_GPU && defined(__has_include)
    #if __has_include(<vt_resolve_comp.h>)
        #include <vt_resolve_comp.h>
        #define ZENGINE_VT_HAS_RESOLVE_SPIRV 1
    #endif
#endif
#ifndef ZENGINE_VT_HAS_RESOLVE_SPIRV
namespace Z
{
namespace Shader
{
    static const std::vector<unsigned char> VT_RESOLVE_COMP {};
}
}  // namespace Z::Shader
#endif

namespace
{
    // 与 vt_resolve.comp 的 _vt_resolve_params 逐字段对应（std140：uvec4/uvec4/vec4，
    // 全是 16 字节对齐的向量，无填充）。
    struct ResolveParams
    {
        uint32_t lookup[4];  // page_size, pool_width_pages, pages_x, pages_y
        uint32_t extent[4];  // width, height, mode, unused
        float fallback[4];
    };
    static_assert(sizeof(ResolveParams) == 48, "ResolveParams must match the std140 block in vt_resolve.comp");

    constexpr uint32_t kBindingParams = 0;
    constexpr uint32_t kBindingPageTable = 1;
    constexpr uint32_t kBindingPhysicalPool = 2;
    constexpr uint32_t kBindingOutput = 3;
    constexpr uint32_t kBindingCount = 4;

    constexpr RHIMemoryPropertyFlags kHostMemory =
        RHI_MEMORY_PROPERTY_HOST_VISIBLE_BIT | RHI_MEMORY_PROPERTY_HOST_COHERENT_BIT;
}  // namespace

VirtualTextureResolver::~VirtualTextureResolver()
{
    Shutdown();
}

bool VirtualTextureResolver::Initialize(RHI* rhi)
{
#if !ZENGINE_VT_GPU
    (void)rhi;
    return false;
#else
    Shutdown();
    if (rhi == nullptr || Z::Shader::VT_RESOLVE_COMP.empty())
    {
        return false;
    }
    // compute 解析器只有 Vulkan 版：它把结果写进 STORAGE_BUFFER，而 DX12 RHI 里 STORAGE_BUFFER 描述符
    // 一律建成 SRV（只读，见 ToDX12DescriptorRangeType），根本写不进去；加 UAV 需要改 RHI 的描述符模型，
    // 不属于 VT 的范围。DX12 的 GPU 消费者验证由 VirtualTextureBlitter（fragment 阶段）承担。
    if (!VirtualTextureUsesVulkanPath(rhi))
    {
        return false;
    }
    m_Rhi = rhi;

    // 采样器：NEAREST + clamp。VT 查找只用 texelFetch，但 combined image sampler 描述符
    // 需要一个合法的 sampler 句柄。**不是**为了双线性：跨页边界的双线性会读到相邻物理
    // 槽位里毫不相干的页（页池没有 gutter），见 vt_common.h 头注释。
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

    m_Rhi->CreateBuffer(256,  // 见 VirtualTextureBlitter 的 kParamsBufferBytes 注释（Vulkan 上无害）
                        RHI_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                        kHostMemory,
                        m_ParamsBuffer,
                        m_ParamsMemory);
    if (m_ParamsBuffer == nullptr || m_ParamsMemory == nullptr)
    {
        Shutdown();
        return false;
    }

    RHIDescriptorSetLayoutBinding bindings[kBindingCount] = {};
    bindings[kBindingParams] = {kBindingParams, RHI_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, RHI_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[kBindingPageTable] = {
        kBindingPageTable, RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, RHI_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[kBindingPhysicalPool] = {
        kBindingPhysicalPool, RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, RHI_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[kBindingOutput] = {kBindingOutput, RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, RHI_SHADER_STAGE_COMPUTE_BIT, nullptr};

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

    RHIShader* shader = m_Rhi->CreateShaderModule(Z::Shader::VT_RESOLVE_COMP);
    if (shader == nullptr)
    {
        Shutdown();
        return false;
    }
    RHIPipelineShaderStageCreateInfo stage {};
    stage.sType = RHI_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = RHI_SHADER_STAGE_COMPUTE_BIT;
    stage.module = shader;
    stage.pName = "main";
    RHIComputePipelineCreateInfo pipeline_info {};
    pipeline_info.sType = RHI_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeline_info.pStages = &stage;
    pipeline_info.layout = m_PipelineLayout;
    const bool pipeline_ok = m_Rhi->CreateComputePipelines(RHI_NULL_HANDLE, 1, &pipeline_info, m_Pipeline);
    m_Rhi->DestroyShaderModule(shader);
    if (!pipeline_ok || m_Pipeline == nullptr)
    {
        Shutdown();
        return false;
    }

    m_Ready = true;
    return true;
#endif
}

void VirtualTextureResolver::Shutdown()
{
    if (m_Rhi != nullptr)
    {
        if (m_OutputMemory != nullptr)
        {
            m_Rhi->FreeMemory(m_OutputMemory);
        }
        if (m_OutputBuffer != nullptr)
        {
            m_Rhi->DestroyBuffer(m_OutputBuffer);
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
    m_OutputMemory = nullptr;
    m_OutputBuffer = nullptr;
    m_OutputCapacity = 0;
    m_ParamsMemory = nullptr;
    m_ParamsBuffer = nullptr;
    m_Sampler = nullptr;
    // 管线 / 布局 / 描述符集：RHI 没有对应的销毁入口（已知缺口，同 NaniteRenderer），
    // 只丢句柄。descriptor_set 归还进池，不单独释放。
    m_Pipeline = nullptr;
    m_PipelineLayout = nullptr;
    m_SetLayout = nullptr;
    m_DescriptorSet = nullptr;
    m_Ready = false;
    m_Rhi = nullptr;
}

bool VirtualTextureResolver::EnsureOutputBuffer(uint64_t bytes)
{
    if (m_OutputBuffer != nullptr && m_OutputCapacity >= bytes)
    {
        return true;
    }
    if (m_OutputMemory != nullptr)
    {
        m_Rhi->FreeMemory(m_OutputMemory);
        m_OutputMemory = nullptr;
    }
    if (m_OutputBuffer != nullptr)
    {
        m_Rhi->DestroyBuffer(m_OutputBuffer);
        m_OutputBuffer = nullptr;
    }
    m_Rhi->CreateBuffer(bytes, RHI_BUFFER_USAGE_STORAGE_BUFFER_BIT, kHostMemory, m_OutputBuffer, m_OutputMemory);
    if (m_OutputBuffer == nullptr || m_OutputMemory == nullptr)
    {
        m_OutputCapacity = 0;
        return false;
    }
    m_OutputCapacity = bytes;
    return true;
}

bool VirtualTextureResolver::Resolve(VirtualTextureManager& manager,
                                     uint64_t handle,
                                     Mode mode,
                                     const float fallback_rgba[4],
                                     std::vector<VirtualTextureResolvedTexel>& out)
{
    out.clear();
#if !ZENGINE_VT_GPU
    (void)manager;
    (void)handle;
    (void)mode;
    (void)fallback_rgba;
    return false;
#else
    if (!m_Ready || m_Rhi == nullptr)
    {
        return false;
    }

    auto resource = manager.GetVirtualTexture(handle);
    auto pool = manager.getPhysicalTexturePool();
    if (!resource || !pool || pool->getPhysicalTextureView() == nullptr)
    {
        return false;
    }
    // 池图从没传过任何页 ⇒ 它还是 UNDEFINED 布局，描述符里声明 SHADER_READ_ONLY 去读
    // 它是非法的（走 path 注册的 VT 页表项会标驻留但没有像素，正好落在这里）。
    // 没有可读的东西，如实失败，而不是读未定义内容。
    if (!pool->hasContent())
    {
        return false;
    }

    const uint32_t width = resource->getVirtualWidth();
    const uint32_t height = resource->getVirtualHeight();
    if (width == 0 || height == 0)
    {
        return false;
    }
    const size_t texel_count = static_cast<size_t>(width) * height;

    // 从未驻留过 ⇒ 没有页表纹理（懒创建）。整张 VT 按"全不驻留"在 CPU 侧给出，
    // 而不是让 GPU 绑一个空图像。
    RHIImageView* page_table_view = resource->getPageTableTextureView();
    if (page_table_view == nullptr)
    {
        const uint32_t fallback_packed =
            (static_cast<uint32_t>(fallback_rgba[0] * 255.0f + 0.5f) & 0xFFu) |
            ((static_cast<uint32_t>(fallback_rgba[1] * 255.0f + 0.5f) & 0xFFu) << 8) |
            ((static_cast<uint32_t>(fallback_rgba[2] * 255.0f + 0.5f) & 0xFFu) << 16) |
            ((static_cast<uint32_t>(fallback_rgba[3] * 255.0f + 0.5f) & 0xFFu) << 24);
        out.assign(texel_count, VirtualTextureResolvedTexel {fallback_packed, 0u});
        return true;
    }

    const uint64_t output_bytes = static_cast<uint64_t>(texel_count) * sizeof(VirtualTextureResolvedTexel);
    if (!EnsureOutputBuffer(output_bytes))
    {
        return false;
    }

    // ---- 参数 ----
    ResolveParams params {};
    params.lookup[0] = pool->getPageSize();
    params.lookup[1] = pool->getPoolWidthPages();
    params.lookup[2] = resource->getPagesX();
    params.lookup[3] = resource->getPagesY();
    params.extent[0] = width;
    params.extent[1] = height;
    params.extent[2] = static_cast<uint32_t>(mode);
    std::memcpy(params.fallback, fallback_rgba, sizeof(params.fallback));

    void* mapped = nullptr;
    if (!m_Rhi->MapMemory(m_ParamsMemory, 0, sizeof(ResolveParams), 0, &mapped) || mapped == nullptr)
    {
        return false;
    }
    std::memcpy(mapped, &params, sizeof(ResolveParams));
    m_Rhi->UnmapMemory(m_ParamsMemory);

    // ---- 描述符：页表 / 页池 / 输出，每次调用重写（图像 / buffer 可能已重建）----
    RHIDescriptorBufferInfo params_info {};
    params_info.buffer = m_ParamsBuffer;
    params_info.offset = 0;
    params_info.range = sizeof(ResolveParams);

    RHIDescriptorImageInfo table_info {};
    table_info.sampler = m_Sampler;
    table_info.imageView = page_table_view;
    table_info.imageLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    RHIDescriptorImageInfo pool_info {};
    pool_info.sampler = m_Sampler;
    pool_info.imageView = pool->getPhysicalTextureView();
    pool_info.imageLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    RHIDescriptorBufferInfo output_info {};
    output_info.buffer = m_OutputBuffer;
    output_info.offset = 0;
    output_info.range = output_bytes;

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
    writes[kBindingOutput].descriptorType = RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[kBindingOutput].pBufferInfo = &output_info;
    m_Rhi->UpdateDescriptorSets(kBindingCount, writes, 0, nullptr);

    // ---- 录制 + 提交（single-time，内部 QueueWaitIdle，所以返回即完成）----
    RHICommandBuffer* command_buffer = m_Rhi->BeginSingleTimeCommands();
    if (command_buffer == nullptr)
    {
        return false;
    }

    m_Rhi->CmdBindPipelinePFN(command_buffer, RHI_PIPELINE_BIND_POINT_COMPUTE, m_Pipeline);
    m_Rhi->CmdBindDescriptorSetsPFN(
        command_buffer, RHI_PIPELINE_BIND_POINT_COMPUTE, m_PipelineLayout, 0, 1, &m_DescriptorSet, 0, nullptr);
    m_Rhi->CmdDispatch(command_buffer, (width + 7) / 8, (height + 7) / 8, 1);

    // compute 写 SSBO → host 读：EndSingleTimeCommands 的 QueueWaitIdle 保证执行完成，
    // 但主机可见性还要一道 memory barrier（host-coherent 内存也一样要求）。
    RHIMemoryBarrier to_host {};
    to_host.sType = RHI_STRUCTURE_TYPE_MEMORY_BARRIER;
    to_host.srcAccessMask = RHI_ACCESS_SHADER_WRITE_BIT;
    to_host.dstAccessMask = RHI_ACCESS_HOST_READ_BIT;
    m_Rhi->CmdPipelineBarrier(command_buffer,
                              RHI_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                              RHI_PIPELINE_STAGE_HOST_BIT,
                              0,
                              1,
                              &to_host,
                              0,
                              nullptr,
                              0,
                              nullptr);
    m_Rhi->EndSingleTimeCommands(command_buffer);

    // ---- 读回 ----
    void* result = nullptr;
    if (!m_Rhi->MapMemory(m_OutputMemory, 0, output_bytes, 0, &result) || result == nullptr)
    {
        return false;
    }
    const auto* texels = static_cast<const VirtualTextureResolvedTexel*>(result);
    out.assign(texels, texels + texel_count);
    m_Rhi->UnmapMemory(m_OutputMemory);
    return true;
#endif
}
