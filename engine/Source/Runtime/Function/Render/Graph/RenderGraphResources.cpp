#include "Runtime/Function/Render/Graph/RenderGraphResources.h"

#include <algorithm>

// =====================================================================================
// RG -> RHI translation
// =====================================================================================

RHIFormat RGToRHIFormat(RGFormat format)
{
    switch (format)
    {
        case RGFormat::R8_UNORM:
            return RHI_FORMAT_R8_UNORM;
        case RGFormat::R8_UINT:
            return RHI_FORMAT_R8_UINT;
        case RGFormat::R16_UINT:
            return RHI_FORMAT_R16_UINT;
        case RGFormat::R32_UINT:
            return RHI_FORMAT_R32_UINT;
        case RGFormat::R32_SFLOAT:
            return RHI_FORMAT_R32_SFLOAT;
        case RGFormat::R16G16_SFLOAT:
            return RHI_FORMAT_R16G16_SFLOAT;
        case RGFormat::R32G32_SFLOAT:
            return RHI_FORMAT_R32G32_SFLOAT;
        case RGFormat::R8G8B8A8_UNORM:
            return RHI_FORMAT_R8G8B8A8_UNORM;
        case RGFormat::R8G8B8A8_SRGB:
            return RHI_FORMAT_R8G8B8A8_SRGB;
        case RGFormat::B8G8R8A8_UNORM:
            return RHI_FORMAT_B8G8R8A8_UNORM;
        case RGFormat::B8G8R8A8_SRGB:
            return RHI_FORMAT_B8G8R8A8_SRGB;
        case RGFormat::R16G16B16A16_SFLOAT:
            return RHI_FORMAT_R16G16B16A16_SFLOAT;
        case RGFormat::R32G32B32A32_SFLOAT:
            return RHI_FORMAT_R32G32B32A32_SFLOAT;
        case RGFormat::D32_SFLOAT:
            return RHI_FORMAT_D32_SFLOAT;
        case RGFormat::D24_UNORM_S8_UINT:
            return RHI_FORMAT_D24_UNORM_S8_UINT;
        case RGFormat::Unknown:
        default:
            return RHI_FORMAT_UNDEFINED;
    }
}

RHIImageUsageFlags RGToRHIImageUsage(RGTextureUsage usage)
{
    RHIImageUsageFlags flags = 0;
    const auto add = [&](RGTextureUsage bit, RHIImageUsageFlags rhi_bit) {
        if (RGHasUsage(usage, bit))
        {
            flags |= rhi_bit;
        }
    };
    add(RGTextureUsage::RenderTarget, RHI_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    add(RGTextureUsage::DepthStencil, RHI_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
    add(RGTextureUsage::ShaderRead, RHI_IMAGE_USAGE_SAMPLED_BIT);
    add(RGTextureUsage::UnorderedAccess, RHI_IMAGE_USAGE_STORAGE_BIT);
    add(RGTextureUsage::TransferSrc, RHI_IMAGE_USAGE_TRANSFER_SRC_BIT);
    add(RGTextureUsage::TransferDst, RHI_IMAGE_USAGE_TRANSFER_DST_BIT);
    add(RGTextureUsage::InputAttachment, RHI_IMAGE_USAGE_INPUT_ATTACHMENT_BIT);
    return flags;
}

RHIBufferUsageFlags RGToRHIBufferUsage(RGBufferUsage usage)
{
    RHIBufferUsageFlags flags = 0;
    const auto add = [&](RGBufferUsage bit, RHIBufferUsageFlags rhi_bit) {
        if (RGHasUsage(usage, bit))
        {
            flags |= rhi_bit;
        }
    };
    add(RGBufferUsage::Vertex, RHI_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    add(RGBufferUsage::Index, RHI_BUFFER_USAGE_INDEX_BUFFER_BIT);
    add(RGBufferUsage::Uniform, RHI_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    add(RGBufferUsage::ShaderRead, RHI_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    add(RGBufferUsage::UnorderedAccess, RHI_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    add(RGBufferUsage::TransferSrc, RHI_BUFFER_USAGE_TRANSFER_SRC_BIT);
    add(RGBufferUsage::TransferDst, RHI_BUFFER_USAGE_TRANSFER_DST_BIT);
    add(RGBufferUsage::Indirect, RHI_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
    return flags;
}

RGStateMapping RGMapState(RGResourceState state, RGPassType pass_type)
{
    // The pipeline stage is only a hint: DX12's CmdPipelineBarrier drops the stage masks
    // entirely (it resolves hazards through its own per-subresource state machine), so
    // correctness must never depend on it. Vulkan uses it as given.
    const RHIPipelineStageFlags stage = pass_type == RGPassType::Compute
                                            ? RHI_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                                            : RHI_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;

    RGStateMapping mapping;
    mapping.stage = stage;
    switch (state)
    {
        case RGResourceState::RenderTarget:
            mapping.layout = RHI_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            mapping.access = RHI_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | RHI_ACCESS_COLOR_ATTACHMENT_READ_BIT;
            mapping.stage = RHI_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            break;
        case RGResourceState::DepthStencilWrite:
            mapping.layout = RHI_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            mapping.access = RHI_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                             RHI_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
            mapping.stage = RHI_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
            break;
        case RGResourceState::DepthStencilRead:
            mapping.layout = RHI_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            mapping.access = RHI_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | RHI_ACCESS_SHADER_READ_BIT;
            mapping.stage = RHI_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
            break;
        case RGResourceState::ShaderResource:
            mapping.layout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            mapping.access = RHI_ACCESS_SHADER_READ_BIT;
            break;
        case RGResourceState::UnorderedAccess:
            mapping.layout = RHI_IMAGE_LAYOUT_GENERAL;
            mapping.access = RHI_ACCESS_SHADER_READ_BIT | RHI_ACCESS_SHADER_WRITE_BIT;
            break;
        case RGResourceState::CopySource:
            mapping.layout = RHI_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            mapping.access = RHI_ACCESS_TRANSFER_READ_BIT;
            mapping.stage = RHI_PIPELINE_STAGE_TRANSFER_BIT;
            break;
        case RGResourceState::CopyDest:
            mapping.layout = RHI_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            mapping.access = RHI_ACCESS_TRANSFER_WRITE_BIT;
            mapping.stage = RHI_PIPELINE_STAGE_TRANSFER_BIT;
            break;
        case RGResourceState::Present:
            mapping.layout = RHI_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            mapping.access = RHI_ACCESS_COLOR_ATTACHMENT_READ_BIT;
            mapping.stage = RHI_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            break;
        case RGResourceState::Undefined:
        default:
            mapping.layout = RHI_IMAGE_LAYOUT_UNDEFINED;
            mapping.access = 0;
            mapping.stage = RHI_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            break;
    }
    return mapping;
}

// =====================================================================================
// RenderGraphExecuteContext
// =====================================================================================

namespace
{
    bool HandleMatches(const RenderGraph* graph, uint32_t index, uint32_t generation)
    {
        return graph != nullptr && index < graph->GetResourceCount() &&
               graph->GetResources()[index].generation == generation;
    }
}  // namespace

RHIImage* RenderGraphExecuteContext::GetImage(RGTextureHandle handle) const
{
    if (!HandleMatches(m_Graph, handle.index, handle.generation) || m_Textures == nullptr)
    {
        return nullptr;
    }
    return (*m_Textures)[handle.index].image;
}

RHIImageView* RenderGraphExecuteContext::GetImageView(RGTextureHandle handle) const
{
    if (!HandleMatches(m_Graph, handle.index, handle.generation) || m_Textures == nullptr)
    {
        return nullptr;
    }
    return (*m_Textures)[handle.index].view;
}

RHIBuffer* RenderGraphExecuteContext::GetBuffer(RGBufferHandle handle) const
{
    if (!HandleMatches(m_Graph, handle.index, handle.generation) || m_Buffers == nullptr)
    {
        return nullptr;
    }
    return (*m_Buffers)[handle.index].buffer;
}

bool RenderGraphExecuteContext::IsResolved(RGTextureHandle handle) const
{
    return GetImageView(handle) != nullptr;
}

bool RenderGraphExecuteContext::IsResolved(RGBufferHandle handle) const
{
    return GetBuffer(handle) != nullptr;
}

// =====================================================================================
// RenderGraphResourcePool
// =====================================================================================

RenderGraphResourcePool::RenderGraphResourcePool(RHI* rhi)
    : m_Rhi(rhi)
{
}

RenderGraphResourcePool::~RenderGraphResourcePool() { Flush(); }

namespace
{
    // RHI::Destroy* / FreeMemory take `T*&` -- they clear the caller's pointer -- so the
    // values have to be copied into mutable locals first.
    void DestroyTexture(RHI* rhi, const RGPhysicalTexture& physical)
    {
        if (rhi == nullptr || physical.image == nullptr)
        {
            return;
        }
        RHIImageView* view = physical.view;
        RHIImage* image = physical.image;
        RHIDeviceMemory* memory = physical.memory;
        if (view != nullptr)
        {
            rhi->DestroyImageView(view);
        }
        rhi->DestroyImage(image);
        if (memory != nullptr)
        {
            rhi->FreeMemory(memory);
        }
    }

    void DestroyBuffer(RHI* rhi, const RGPhysicalBuffer& physical)
    {
        if (rhi == nullptr || physical.buffer == nullptr)
        {
            return;
        }
        RHIBuffer* buffer = physical.buffer;
        RHIDeviceMemory* memory = physical.memory;
        rhi->DestroyBuffer(buffer);
        if (memory != nullptr)
        {
            rhi->FreeMemory(memory);
        }
    }
}  // namespace

uint64_t RenderGraphResourcePool::KeyOf(const RGTextureDesc& desc)
{
    // FNV-1a over the fields that actually change the allocation.
    uint64_t hash = 1469598103934665603ull;
    const auto mix = [&hash](uint64_t value) {
        hash ^= value;
        hash *= 1099511628211ull;
    };
    mix(desc.width);
    mix(desc.height);
    mix(desc.depth);
    mix(desc.mip_levels);
    mix(desc.array_size);
    mix(static_cast<uint64_t>(desc.format));
    mix(static_cast<uint64_t>(desc.usage));
    return hash;
}

uint64_t RenderGraphResourcePool::KeyOf(const RGBufferDesc& desc)
{
    uint64_t hash = 1469598103934665603ull;
    const auto mix = [&hash](uint64_t value) {
        hash ^= value;
        hash *= 1099511628211ull;
    };
    mix(desc.size_bytes);
    mix(static_cast<uint64_t>(desc.usage));
    return hash;
}

RGPhysicalTexture RenderGraphResourcePool::AcquireTexture(const RGTextureDesc& desc)
{
    if (m_Rhi == nullptr)
    {
        return RGPhysicalTexture {};
    }

    const uint64_t key = KeyOf(desc);
    auto it = m_IdleTextures.find(key);
    if (it != m_IdleTextures.end() && !it->second.empty())
    {
        RGPhysicalTexture physical = it->second.back();
        it->second.pop_back();
        return physical;
    }

    RGPhysicalTexture physical;
    const bool is_depth = desc.format == RGFormat::D32_SFLOAT || desc.format == RGFormat::D24_UNORM_S8_UINT;

    // Transient attachments hint the driver that this memory never needs to be
    // backed by real storage between passes (tile-based GPUs benefit the most).
    RHIImageUsageFlags usage = RGToRHIImageUsage(desc.usage);
    if (desc.transient)
    {
        usage |= RHI_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
    }

    m_Rhi->CreateImage(desc.width,
                       desc.height,
                       RGToRHIFormat(desc.format),
                       RHI_IMAGE_TILING_OPTIMAL,
                       usage,
                       RHI_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                       physical.image,
                       physical.memory,
                       0,
                       desc.array_size,
                       desc.mip_levels);

    if (physical.image != nullptr)
    {
        m_Rhi->CreateImageView(physical.image,
                               RGToRHIFormat(desc.format),
                               is_depth ? (RHI_IMAGE_ASPECT_DEPTH_BIT | RHI_IMAGE_ASPECT_STENCIL_BIT)
                                        : RHI_IMAGE_ASPECT_COLOR_BIT,
                               desc.depth > 1 ? RHI_IMAGE_VIEW_TYPE_3D
                                              : (desc.array_size > 1 ? RHI_IMAGE_VIEW_TYPE_2D_ARRAY
                                                                     : RHI_IMAGE_VIEW_TYPE_2D),
                               1,
                               desc.mip_levels,
                               physical.view);
        ++m_LiveObjectCount;
    }
    return physical;
}

void RenderGraphResourcePool::ReleaseTexture(const RGTextureDesc& desc, const RGPhysicalTexture& physical)
{
    if (m_Rhi == nullptr || physical.image == nullptr)
    {
        return;
    }

    const uint64_t key = KeyOf(desc);
    std::vector<RGPhysicalTexture>& idle = m_IdleTextures[key];
    if (idle.size() >= m_MaxIdlePerKey)
    {
        DestroyTexture(m_Rhi, physical);
        m_LiveObjectCount = m_LiveObjectCount > 0 ? m_LiveObjectCount - 1 : 0;
        return;
    }
    idle.push_back(physical);
}

RGPhysicalBuffer RenderGraphResourcePool::AcquireBuffer(const RGBufferDesc& desc)
{
    if (m_Rhi == nullptr)
    {
        return RGPhysicalBuffer {};
    }

    const uint64_t key = KeyOf(desc);
    auto it = m_IdleBuffers.find(key);
    if (it != m_IdleBuffers.end() && !it->second.empty())
    {
        RGPhysicalBuffer physical = it->second.back();
        it->second.pop_back();
        return physical;
    }

    RGPhysicalBuffer physical;
    m_Rhi->CreateBuffer(static_cast<RHIDeviceSize>(desc.size_bytes),
                        RGToRHIBufferUsage(desc.usage),
                        RHI_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                        physical.buffer,
                        physical.memory);
    if (physical.buffer != nullptr)
    {
        ++m_LiveObjectCount;
    }
    return physical;
}

void RenderGraphResourcePool::ReleaseBuffer(const RGBufferDesc& desc, const RGPhysicalBuffer& physical)
{
    if (m_Rhi == nullptr || physical.buffer == nullptr)
    {
        return;
    }

    const uint64_t key = KeyOf(desc);
    std::vector<RGPhysicalBuffer>& idle = m_IdleBuffers[key];
    if (idle.size() >= m_MaxIdlePerKey)
    {
        DestroyBuffer(m_Rhi, physical);
        m_LiveObjectCount = m_LiveObjectCount > 0 ? m_LiveObjectCount - 1 : 0;
        return;
    }
    idle.push_back(physical);
}

void RenderGraphResourcePool::Flush()
{
    if (m_Rhi == nullptr)
    {
        return;
    }
    for (auto& [key, textures] : m_IdleTextures)
    {
        (void)key;
        for (const RGPhysicalTexture& physical : textures)
        {
            DestroyTexture(m_Rhi, physical);
            m_LiveObjectCount = m_LiveObjectCount > 0 ? m_LiveObjectCount - 1 : 0;
        m_LiveObjectCount = m_LiveObjectCount > 0 ? m_LiveObjectCount - 1 : 0;
        }
    }
    m_IdleTextures.clear();

    for (auto& [key, buffers] : m_IdleBuffers)
    {
        (void)key;
        for (const RGPhysicalBuffer& physical : buffers)
        {
            DestroyBuffer(m_Rhi, physical);
            m_LiveObjectCount = m_LiveObjectCount > 0 ? m_LiveObjectCount - 1 : 0;
        m_LiveObjectCount = m_LiveObjectCount > 0 ? m_LiveObjectCount - 1 : 0;
        }
    }
    m_IdleBuffers.clear();
}

size_t RenderGraphResourcePool::GetIdleTextureCount() const
{
    size_t count = 0;
    for (const auto& [key, textures] : m_IdleTextures)
    {
        (void)key;
        count += textures.size();
    }
    return count;
}

size_t RenderGraphResourcePool::GetIdleBufferCount() const
{
    size_t count = 0;
    for (const auto& [key, buffers] : m_IdleBuffers)
    {
        (void)key;
        count += buffers.size();
    }
    return count;
}

// =====================================================================================
// RenderGraphExecutor
// =====================================================================================

RenderGraphExecutor::RenderGraphExecutor(RHI* rhi, RenderGraphResourcePool* pool)
    : m_Rhi(rhi), m_Pool(pool)
{
}

bool RenderGraphExecutor::Execute(RenderGraph& graph, RHICommandBuffer* command_buffer)
{
    m_LastError.clear();
    m_LastPassCount = 0;
    m_LastBarrierCount = 0;
    m_LastAllocatedTextures = 0;

    if (m_Rhi == nullptr || command_buffer == nullptr)
    {
        m_LastError = "no RHI or command buffer";
        return false;
    }

    const RGCompiledGraph& compiled = graph.Compile();
    if (!compiled.valid)
    {
        m_LastError = compiled.errors.empty() ? "graph failed validation" : compiled.errors.front();
        return false;
    }
    if (m_Pool == nullptr)
    {
        m_LastError = "no resource pool";
        return false;
    }

    const size_t resource_count = graph.GetResourceCount();
    m_Textures.assign(resource_count, RGPhysicalTexture {});
    m_Buffers.assign(resource_count, RGPhysicalBuffer {});
    m_AcquiredTextureDescs.assign(resource_count, RGTextureDesc {});
    m_AcquiredBufferDescs.assign(resource_count, RGBufferDesc {});

    // ---- resolve handles to physical resources -------------------------------------
    for (size_t i = 0; i < resource_count; ++i)
    {
        const RGResource& resource = graph.GetResources()[i];
        if (resource.is_texture)
        {
            if (resource.external)
            {
                m_Textures[i].image = resource.external_texture.image;
                m_Textures[i].view = resource.external_texture.view;
            }
            else
            {
                m_Textures[i] = m_Pool->AcquireTexture(resource.texture_desc);
                m_AcquiredTextureDescs[i] = resource.texture_desc;
                ++m_LastAllocatedTextures;
            }
        }
        else if (resource.external)
        {
            m_Buffers[i].buffer = resource.external_buffer.buffer;
        }
        else
        {
            m_Buffers[i] = m_Pool->AcquireBuffer(resource.buffer_desc);
            m_AcquiredBufferDescs[i] = resource.buffer_desc;
        }
    }

    // ---- record ---------------------------------------------------------------------
    RenderGraphExecuteContext context;
    context.m_Rhi = m_Rhi;
    context.m_CommandBuffer = command_buffer;
    context.m_Graph = &graph;
    context.m_Textures = &m_Textures;
    context.m_Buffers = &m_Buffers;

    const std::vector<uint32_t>& order = compiled.pass_order;
    for (size_t position = 0; position < order.size(); ++position)
    {
        const uint32_t pass_index = order[position];
        const RGPass& pass = graph.GetPasses()[pass_index];

        const size_t barrier_begin = compiled.barrier_offsets[position];
        const size_t barrier_end = compiled.barrier_offsets[position + 1];
        if (barrier_end > barrier_begin)
        {
            RecordBarriers(command_buffer, graph, barrier_begin, barrier_end);
            m_LastBarrierCount += barrier_end - barrier_begin;
        }

        if (m_DebugEvents)
        {
            m_Rhi->PushEvent(command_buffer, pass.name.c_str(), nullptr);
        }

        context.m_PassName = pass.name.c_str();
        context.m_PassIndex = pass_index;
        if (pass.execute)
        {
            pass.execute(context);
        }

        if (m_DebugEvents)
        {
            m_Rhi->PopEvent(command_buffer);
        }

        // Hand transient memory back as soon as the graph is done with it -- the whole
        // point of tracking lifetimes.
        for (size_t i = 0; i < resource_count; ++i)
        {
            const RGResource& resource = graph.GetResources()[i];
            if (resource.external || resource.extracted)
            {
                continue;
            }
            const RGResourceLifetime& lifetime = compiled.lifetimes[i];
            if (lifetime.last_pass != position)
            {
                continue;
            }
            if (resource.is_texture)
            {
                if (m_Textures[i].image != nullptr)
                {
                    m_Pool->ReleaseTexture(m_AcquiredTextureDescs[i], m_Textures[i]);
                    m_Textures[i] = RGPhysicalTexture {};
                }
            }
            else if (m_Buffers[i].buffer != nullptr)
            {
                m_Pool->ReleaseBuffer(m_AcquiredBufferDescs[i], m_Buffers[i]);
                m_Buffers[i] = RGPhysicalBuffer {};
            }
        }
    }

    // Anything still held (culled resources, or ones whose producer was culled) goes
    // back now.
    for (size_t i = 0; i < resource_count; ++i)
    {
        const RGResource& resource = graph.GetResources()[i];
        if (resource.external)
        {
            continue;
        }
        if (resource.is_texture && m_Textures[i].image != nullptr)
        {
            m_Pool->ReleaseTexture(m_AcquiredTextureDescs[i], m_Textures[i]);
            m_Textures[i] = RGPhysicalTexture {};
        }
        else if (!resource.is_texture && m_Buffers[i].buffer != nullptr)
        {
            m_Pool->ReleaseBuffer(m_AcquiredBufferDescs[i], m_Buffers[i]);
            m_Buffers[i] = RGPhysicalBuffer {};
        }
    }

    m_LastPassCount = order.size();
    return true;
}

void RenderGraphExecutor::RecordBarriers(RHICommandBuffer* command_buffer,
                                         const RenderGraph& graph,
                                         size_t barrier_begin,
                                         size_t barrier_end)
{
    if (barrier_begin >= barrier_end)
    {
        return;
    }

    const RGCompiledGraph& compiled = graph.GetCompiled();
    std::vector<RHIImageMemoryBarrier> image_barriers;
    std::vector<RHIBufferMemoryBarrier> buffer_barriers;
    image_barriers.reserve(barrier_end - barrier_begin);
    buffer_barriers.reserve(barrier_end - barrier_begin);

    RHIPipelineStageFlags src_stage = 0;
    RHIPipelineStageFlags dst_stage = 0;

    for (size_t i = barrier_begin; i < barrier_end; ++i)
    {
        const RGBarrier& barrier = compiled.barriers[i];
        const RGResource& resource = graph.GetResources()[barrier.resource_index];
        // The pass that this barrier belongs to decides the destination stage.
        // (Approximation: the graph does not model per-resource stage masks yet.)
        const RGStateMapping before = RGMapState(barrier.before, RGPassType::Render);
        const RGStateMapping after = RGMapState(barrier.after, RGPassType::Render);

        src_stage |= before.stage;
        dst_stage |= after.stage;

        if (barrier.is_texture)
        {
            const bool is_depth = resource.texture_desc.format == RGFormat::D32_SFLOAT ||
                                  resource.texture_desc.format == RGFormat::D24_UNORM_S8_UINT;
            RHIImageMemoryBarrier rhi_barrier {};
            rhi_barrier.sType = RHI_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            rhi_barrier.pNext = nullptr;
            rhi_barrier.srcAccessMask = before.access;
            rhi_barrier.dstAccessMask = after.access;
            rhi_barrier.oldLayout = before.layout;
            rhi_barrier.newLayout = after.layout;
            rhi_barrier.srcQueueFamilyIndex = 0;
            rhi_barrier.dstQueueFamilyIndex = 0;
            rhi_barrier.image = m_Textures[barrier.resource_index].image;
            rhi_barrier.subresourceRange.aspectMask = is_depth
                                                          ? (RHI_IMAGE_ASPECT_DEPTH_BIT |
                                                             RHI_IMAGE_ASPECT_STENCIL_BIT)
                                                          : RHI_IMAGE_ASPECT_COLOR_BIT;
            rhi_barrier.subresourceRange.baseMipLevel = 0;
            rhi_barrier.subresourceRange.levelCount = resource.texture_desc.mip_levels;
            rhi_barrier.subresourceRange.baseArrayLayer = 0;
            rhi_barrier.subresourceRange.layerCount = resource.texture_desc.array_size;
            image_barriers.push_back(rhi_barrier);
        }
        else
        {
            RHIBufferMemoryBarrier rhi_barrier {};
            rhi_barrier.sType = RHI_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            rhi_barrier.pNext = nullptr;
            rhi_barrier.srcAccessMask = before.access;
            rhi_barrier.dstAccessMask = after.access;
            rhi_barrier.srcQueueFamilyIndex = 0;
            rhi_barrier.dstQueueFamilyIndex = 0;
            rhi_barrier.buffer = m_Buffers[barrier.resource_index].buffer;
            rhi_barrier.offset = 0;
            rhi_barrier.size = static_cast<RHIDeviceSize>(resource.buffer_desc.size_bytes);
            buffer_barriers.push_back(rhi_barrier);
        }
    }

    if (src_stage == 0)
    {
        src_stage = RHI_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    }
    if (dst_stage == 0)
    {
        dst_stage = RHI_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    }

    // One call for the whole pass: the graph's main win over hand-written per-pass
    // barriers scattered through every pass file.
    m_Rhi->CmdPipelineBarrier(command_buffer,
                              src_stage,
                              dst_stage,
                              0,
                              0,
                              nullptr,
                              static_cast<uint32_t>(buffer_barriers.size()),
                              buffer_barriers.empty() ? nullptr : buffer_barriers.data(),
                              static_cast<uint32_t>(image_barriers.size()),
                              image_barriers.empty() ? nullptr : image_barriers.data());
}
