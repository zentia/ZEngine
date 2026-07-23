#include "CommonPCH/pch.h"
#include "Runtime/Function/Render/Interface/Metal/MetalRHI.h"
#include "Runtime/Function/Render/Interface/Metal/MetalBindlessTextureManager.h"

#include "Runtime/Function/Render/WindowSystem.h"

#include <algorithm>

std::vector<std::type_index> MetalRHI::GetDependencies() const
{
    return {GET_SYSTEM_TYPE(WindowSystem)};
}

bool MetalRHI::Initialize()
{
#ifdef __APPLE__
    m_Device = MTLCreateSystemDefaultDevice();
    if (m_Device == nil)
    {
        return false;
    }
    m_CommandQueue = [m_Device newCommandQueue];
    if (m_CommandQueue == nil)
    {
        return false;
    }

    // ---- Bindless capability probe ----
    // Tier2 argument buffers are required for large texture arrays.
    MTLArgumentBuffersTier tier = [m_Device argumentBuffersSupport];
    if (tier >= MTLArgumentBuffersTier2)
    {
        // Query device limits for bindless capacity.
        // Metal doesn't expose a "max sampled images" query like Vulkan;
        // we use the same default as DX12/Vulkan (16384 on desktop-class,
        // 4096 on mobile). Apple GPUs support up to 65535 textures in
        // argument buffers on Tier2.
#if TARGET_OS_IOS && !TARGET_OS_MACCATALYST
        m_MaxBindlessSampledImages  = 4096;
        m_MaxBindlessStorageBuffers = 256;
#else
        m_MaxBindlessSampledImages  = 16384;
        m_MaxBindlessStorageBuffers = 1024;
#endif

        m_BindlessTextureManager = new MetalBindlessTextureManager();
        if (!m_BindlessTextureManager->Initialize(m_Device, m_MaxBindlessSampledImages))
        {
            LOG_WARNING(ZRender,
                        "MetalBindlessTextureManager init failed -- demoting bindless support to OFF");
            delete m_BindlessTextureManager;
        m_BindlessTextureManager = nullptr;
            m_MaxBindlessSampledImages  = 0;
            m_MaxBindlessStorageBuffers = 0;
        }
        else
        {
            m_BindlessSupported = true;
            LOG_INFO(ZRender,
                     "MetalRHI: bindless textures ENABLED (capacity = {}, Tier2)",
                     m_MaxBindlessSampledImages);
        }
    }
    else
    {
        LOG_INFO(ZRender,
                 "MetalRHI: Argument Buffer Tier1 only -- bindless textures DISABLED");
    }

    return true;
#else
    return false;
#endif
}

void MetalRHI::Shutdown()
{
#ifdef __APPLE__
    // Tear down bindless manager before releasing the device.
    if (m_BindlessTextureManager)
    {
        m_BindlessTextureManager->Shutdown();
        delete m_BindlessTextureManager;
        m_BindlessTextureManager = nullptr;
    }
    m_BindlessSupported           = false;
    m_MaxBindlessSampledImages  = 0;
    m_MaxBindlessStorageBuffers = 0;

#if !__has_feature(objc_arc)
    [m_CommandQueue release];
    [m_Device release];
#endif
    m_CommandQueue = nil;
    m_Device        = nil;
#endif
}

MetalRHI::~MetalRHI() = default;

void MetalRHI::PrepareContext() {}
bool MetalRHI::IsPointLightShadowEnabled() { return false; }
void MetalRHI::NotifyWindowFocusGained() {}
void MetalRHI::DestroyRenderPass(RHIRenderPass* /*renderPass*/) {}

bool MetalRHI::AllocateCommandBuffers(const RHICommandBufferAllocateInfo*, RHICommandBuffer*& pCommandBuffers)
{
    pCommandBuffers = nullptr;
    return false;
}

bool MetalRHI::AllocateDescriptorSets(const RHIDescriptorSetAllocateInfo*, RHIDescriptorSet*& pDescriptorSets)
{
    pDescriptorSets = nullptr;
    return false;
}

void MetalRHI::CreateSwapchain() {}
void MetalRHI::RecreateSwapchain() {}
void MetalRHI::CreateSwapchainImageViews() {}
void MetalRHI::CreateFramebufferImageAndView() {}
RHISampler* MetalRHI::GetOrCreateDefaultSampler(RHIDefaultSamplerType) { return nullptr; }
RHISampler* MetalRHI::GetOrCreateMipmapSampler(uint32_t, uint32_t) { return nullptr; }
RHIShader* MetalRHI::CreateShaderModule(const std::vector<unsigned char>&) { return nullptr; }

RHIShader* MetalRHI::CreateShaderModuleFromFile(const std::string&,
                                                ShaderStage,
                                                const std::vector<std::string>&,
                                                const ShaderMacros&,
                                                std::vector<uint8_t>& output_binary,
                                                const std::string& /*entry_point*/,
                                                bool /*embed_debug*/)
{
    (void)output_binary;
    return nullptr;
}

RHIShader* MetalRHI::CreateShaderModuleFromSource(const std::string&, ShaderStage, const std::string&, const std::vector<std::string>&, const ShaderMacros&)
{
    return nullptr;
}

void MetalRHI::CreateBuffer(RHIDeviceSize, RHIBufferUsageFlags, RHIMemoryPropertyFlags, RHIBuffer*& buffer, RHIDeviceMemory*& buffer_memory)
{
    buffer        = nullptr;
    buffer_memory = nullptr;
}

void MetalRHI::CreateBufferAndInitialize(RHIBufferUsageFlags, RHIMemoryPropertyFlags, RHIBuffer*& buffer, RHIDeviceMemory*& buffer_memory, RHIDeviceSize, void*, int)
{
    buffer        = nullptr;
    buffer_memory = nullptr;
}

void MetalRHI::CopyBuffer(RHIBuffer*, RHIBuffer*, RHIDeviceSize, RHIDeviceSize, RHIDeviceSize) {}

void MetalRHI::CreateImage(uint32_t, uint32_t, RHIFormat, RHIImageTiling, RHIImageUsageFlags, RHIMemoryPropertyFlags, RHIImage*& image, RHIDeviceMemory*& memory, RHIImageCreateFlags, uint32_t, uint32_t)
{
    image  = nullptr;
    memory = nullptr;
}

void MetalRHI::CreateImageView(RHIImage*, RHIFormat, RHIImageAspectFlags, RHIImageViewType, uint32_t, uint32_t, RHIImageView*& image_view)
{
    image_view = nullptr;
}

void MetalRHI::CreateGlobalImage(RHIImage*& image, RHIImageView*& image_view, void*, uint32_t, uint32_t, void*, RHIFormat, uint32_t)
{
    image      = nullptr;
    image_view = nullptr;
}

void MetalRHI::CreateCubeMap(RHIImage*& image, RHIImageView*& image_view, void*, uint32_t, uint32_t, std::array<void*, 6>, RHIFormat, uint32_t)
{
    image      = nullptr;
    image_view = nullptr;
}

void MetalRHI::CreateCommandPool() {}

bool MetalRHI::CreateCommandPool(const RHICommandPoolCreateInfo*, RHICommandPool*& pCommandPool)
{
    pCommandPool = nullptr;
    return false;
}

bool MetalRHI::CreateDescriptorPool(const RHIDescriptorPoolCreateInfo*, RHIDescriptorPool*& pDescriptorPool)
{
    pDescriptorPool = nullptr;
    return false;
}

bool MetalRHI::CreateDescriptorSetLayout(const RHIDescriptorSetLayoutCreateInfo*, RHIDescriptorSetLayout*& pSetLayout)
{
    pSetLayout = nullptr;
    return false;
}

bool MetalRHI::CreateFence(const RHIFenceCreateInfo*, RHIFence*& pFence)
{
    pFence = nullptr;
    return false;
}

bool MetalRHI::CreateFramebuffer(const RHIFramebufferCreateInfo*, RHIFramebuffer*& pFramebuffer)
{
    pFramebuffer = nullptr;
    return false;
}

bool MetalRHI::CreateGraphicsPipelines(RHIPipelineCache*, uint32_t, const RHIGraphicsPipelineCreateInfo*, RHIPipeline*& pPipelines)
{
    pPipelines = nullptr;
    return false;
}

bool MetalRHI::CreateComputePipelines(RHIPipelineCache*, uint32_t, const RHIComputePipelineCreateInfo*, RHIPipeline*& pPipelines)
{
    pPipelines = nullptr;
    return false;
}

bool MetalRHI::CreatePipelineLayout(const RHIPipelineLayoutCreateInfo*, RHIPipelineLayout*& pPipelineLayout)
{
    pPipelineLayout = nullptr;
    return false;
}

bool MetalRHI::CreateRenderPass(const RHIRenderPassCreateInfo*, RHIRenderPass*& pRenderPass)
{
    pRenderPass = nullptr;
    return false;
}

bool MetalRHI::CreateSampler(const RHISamplerCreateInfo*, RHISampler*& pSampler)
{
    pSampler = nullptr;
    return false;
}

bool MetalRHI::CreateSemaphore(const RHISemaphoreCreateInfo*, RHISemaphore*& pSemaphore)
{
    pSemaphore = nullptr;
    return false;
}

bool MetalRHI::WaitForFencesPFN(uint32_t, RHIFence* const*, RHIBool32, uint64_t) { return true; }
bool MetalRHI::ResetFencesPFN(uint32_t, RHIFence* const*) { return true; }
bool MetalRHI::ResetCommandPoolPFN(RHICommandPool*, RHICommandPoolResetFlags) { return true; }
bool MetalRHI::BeginCommandBufferPFN(RHICommandBuffer*, const RHICommandBufferBeginInfo*) { return true; }
bool MetalRHI::EndCommandBufferPFN(RHICommandBuffer*) { return true; }
void MetalRHI::CmdBeginRenderPassPFN(RHICommandBuffer*, const RHIRenderPassBeginInfo*, RHISubpassContents) {}
void MetalRHI::CmdNextSubpassPFN(RHICommandBuffer*, RHISubpassContents) {}
void MetalRHI::CmdEndRenderPassPFN(RHICommandBuffer*) {}
void MetalRHI::CmdBindPipelinePFN(RHICommandBuffer*, RHIPipelineBindPoint, RHIPipeline*) {}
void MetalRHI::CmdSetViewportPFN(RHICommandBuffer*, uint32_t, uint32_t, const RHIViewport*) {}
void MetalRHI::CmdSetScissorPFN(RHICommandBuffer*, uint32_t, uint32_t, const RHIRect2D*) {}
void MetalRHI::CmdBindVertexBuffersPFN(RHICommandBuffer*, uint32_t, uint32_t, RHIBuffer* const*, const RHIDeviceSize*) {}
void MetalRHI::CmdBindIndexBufferPFN(RHICommandBuffer*, RHIBuffer*, RHIDeviceSize, RHIIndexType) {}
void MetalRHI::CmdBindDescriptorSetsPFN(RHICommandBuffer*, RHIPipelineBindPoint, RHIPipelineLayout*, uint32_t, uint32_t, const RHIDescriptorSet* const*, uint32_t, const uint32_t*) {}
void MetalRHI::CmdDrawIndexedPFN(RHICommandBuffer*, uint32_t, uint32_t, uint32_t, int32_t, uint32_t) {}
void MetalRHI::CmdClearAttachmentsPFN(RHICommandBuffer*, uint32_t, const RHIClearAttachment*, uint32_t, const RHIClearRect*) {}

bool MetalRHI::BeginCommandBuffer(RHICommandBuffer*, const RHICommandBufferBeginInfo*) { return true; }
void MetalRHI::CmdCopyImageToBuffer(RHICommandBuffer*, RHIImage*, RHIImageLayout, RHIBuffer*, uint32_t, const RHIBufferImageCopy*) {}
void MetalRHI::CmdCopyImageToImage(RHICommandBuffer*, RHIImage*, RHIImageAspectFlagBits, RHIImage*, RHIImageAspectFlagBits, uint32_t, uint32_t) {}
void MetalRHI::CmdBlitImage(RHICommandBuffer*, RHIImage*, RHIImageLayout, RHIImage*, RHIImageLayout, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, RHIFilter) {}
void MetalRHI::CmdCopyBuffer(RHICommandBuffer*, RHIBuffer*, RHIBuffer*, uint32_t, RHIBufferCopy*) {}
void MetalRHI::CmdDraw(RHICommandBuffer*, uint32_t, uint32_t, uint32_t, uint32_t) {}
void MetalRHI::CmdDispatch(RHICommandBuffer*, uint32_t, uint32_t, uint32_t) {}
void MetalRHI::CmdDispatchIndirect(RHICommandBuffer*, RHIBuffer*, RHIDeviceSize) {}
void MetalRHI::CmdPipelineBarrier(RHICommandBuffer*, RHIPipelineStageFlags, RHIPipelineStageFlags, RHIDependencyFlags, uint32_t, const RHIMemoryBarrier*, uint32_t, const RHIBufferMemoryBarrier*, uint32_t, const RHIImageMemoryBarrier*) {}
bool MetalRHI::EndCommandBuffer(RHICommandBuffer*) { return true; }
void MetalRHI::UpdateDescriptorSets(uint32_t, const RHIWriteDescriptorSet*, uint32_t, const RHICopyDescriptorSet*) {}
bool MetalRHI::QueueSubmit(RHIQueue*, uint32_t, const RHISubmitInfo*, RHIFence*) { return true; }
bool MetalRHI::QueueWaitIdle(RHIQueue*) { return true; }
void MetalRHI::ResetCommandPool() {}
void MetalRHI::WaitForFences() {}

void MetalRHI::GetPhysicalDeviceProperties(RHIPhysicalDeviceProperties*) {}
RHICommandBuffer* MetalRHI::GetCurrentCommandBuffer() const { return m_CurrentCommandBuffer; }
RHICommandBuffer* const* MetalRHI::GetCommandBufferList() const { return m_CommandBuffers; }
RHICommandPool* MetalRHI::GetCommandPoor() const { return m_RhiCommandPool; }
RHIDescriptorPool* MetalRHI::GetDescriptorPoor() const { return m_DescriptorPool; }
RHIFence* const* MetalRHI::GetFenceList() const { return m_RhiIsFrameInFlightFences; }
QueueFamilyIndices MetalRHI::GetQueueFamilyIndices() const { return m_QueueIndices; }
RHIQueue* MetalRHI::GetGraphicsQueue() const { return m_GraphicsQueue; }
RHIQueue* MetalRHI::GetComputeQueue() const { return m_ComputeQueue; }
RHISwapChainDesc MetalRHI::GetSwapchainInfo()
{
    RHISwapChainDesc desc {};
    desc.extent         = m_SwapchainExtent;
    desc.image_format   = m_SwapchainImageFormat;
    desc.viewport       = m_Viewports;
    desc.viewport_count = static_cast<uint32_t>(m_ViewportCount);
    desc.scissor        = m_Scissors;
    desc.imageViews     = m_SwapchainImageviews;
    return desc;
}

RHIDepthImageDesc MetalRHI::GetDepthImageInfo() const
{
    RHIDepthImageDesc desc {};
    desc.depth_image_format = m_DepthImageFormat;
    desc.depth_image_view   = m_DepthImageView;
    return desc;
}
uint8_t MetalRHI::GetMaxFramesInFlight() const { return k_max_frames_in_flight; }
uint8_t MetalRHI::GetCurrentFrameIndex() const { return m_CurrentFrameIndex; }
void MetalRHI::SetCurrentFrameIndex(uint8_t index) { m_CurrentFrameIndex = index % k_max_frames_in_flight; }

RHICommandBuffer* MetalRHI::BeginSingleTimeCommands() { return nullptr; }
void MetalRHI::EndSingleTimeCommands(RHICommandBuffer*) {}
bool MetalRHI::PrepareBeforePass(std::function<void()> passUpdateAfterRecreateSwapchain)
{
    if (passUpdateAfterRecreateSwapchain)
    {
        passUpdateAfterRecreateSwapchain();
    }
    return true;
}
void MetalRHI::SubmitRendering(std::function<void()> passUpdateAfterRecreateSwapchain)
{
    if (passUpdateAfterRecreateSwapchain)
    {
        passUpdateAfterRecreateSwapchain();
    }
}
void MetalRHI::PushEvent(RHICommandBuffer*, const char*, const float*) {}
void MetalRHI::PopEvent(RHICommandBuffer*) {}

void MetalRHI::clear()
{
    // Tear down bindless manager before general cleanup.
    if (m_BindlessTextureManager)
    {
        m_BindlessTextureManager->Shutdown();
        delete m_BindlessTextureManager;
        m_BindlessTextureManager = nullptr;
    }
    m_BindlessSupported           = false;
    m_MaxBindlessSampledImages  = 0;
    m_MaxBindlessStorageBuffers = 0;
}
void MetalRHI::ClearSwapchain() {}
void MetalRHI::DestroyDefaultSampler(RHIDefaultSamplerType) {}
void MetalRHI::DestroyMipmappedSampler() {}
void MetalRHI::DestroyShaderModule(RHIShader*) {}
void MetalRHI::DestroySemaphore(RHISemaphore*) {}
void MetalRHI::DestroySampler(RHISampler*) {}
void MetalRHI::DestroyInstance(RHIInstance*) {}
void MetalRHI::DestroyImageView(RHIImageView*) {}
void MetalRHI::DestroyImage(RHIImage*) {}
void MetalRHI::DestroyFramebuffer(RHIFramebuffer*) {}
void MetalRHI::DestroyFence(RHIFence*) {}
void MetalRHI::DestroyDevice() {}
void MetalRHI::DestroyCommandPool(RHICommandPool*) {}
void MetalRHI::DestroyBuffer(RHIBuffer*& buffer) { buffer = nullptr; }
void MetalRHI::FreeCommandBuffers(RHICommandPool*, uint32_t, RHICommandBuffer*) {}
void MetalRHI::FreeMemory(RHIDeviceMemory*& memory) { memory = nullptr; }
bool MetalRHI::MapMemory(RHIDeviceMemory*, RHIDeviceSize, RHIDeviceSize, RHIMemoryMapFlags, void** ppData)
{
    if (ppData)
    {
        *ppData = nullptr;
    }
    return false;
}
void MetalRHI::UnmapMemory(RHIDeviceMemory*) {}
void MetalRHI::InvalidateMappedMemoryRanges(void*, RHIDeviceMemory*, RHIDeviceSize, RHIDeviceSize) {}
void MetalRHI::FlushMappedMemoryRanges(void*, RHIDeviceMemory*, RHIDeviceSize, RHIDeviceSize) {}

RHISemaphore*& MetalRHI::GetTextureCopySemaphore(uint32_t)
{
    static RHISemaphore* null_semaphore = nullptr;
    return null_semaphore;
}

void MetalRHI::RegisterViewport(const int viewport_id, const RHIViewport& viewport)
{
    if (viewport_id >= 0 && viewport_id < m_ViewportCount)
    {
        m_Viewports[viewport_id] = viewport;
    }
}

void MetalRHI::UpdateViewport(ViewportType viewport_id, const RHIViewport& viewport)
{
    const int index = static_cast<int>(viewport_id);
    if (index >= 0 && index < m_ViewportCount)
    {
        m_Viewports[index] = viewport;
    }
}

RHIViewport* MetalRHI::GetViewport(ViewportType viewport_id)
{
    const int index = static_cast<int>(viewport_id);
    if (index < 0 || index >= m_ViewportCount)
    {
        return nullptr;
    }
    return &m_Viewports[index];
}

void MetalRHI::CreateViewportRenderTexture(const std::string&, uint32_t, uint32_t) {}
void MetalRHI::UpdateViewportRenderTexture(const std::string&, uint32_t, uint32_t) {}
void MetalRHI::DestroyViewportRenderTexture(const std::string&) {}
RHI::ViewportRenderTexture* MetalRHI::GetViewportRenderTexture(const std::string& viewport_id)
{
    auto it = m_ViewportRenderTextures.find(viewport_id);
    return it == m_ViewportRenderTextures.end() ? nullptr : &it->second;
}

// =====================================================================
// Bindless overrides
// =====================================================================

RHIBindlessTextureManager* MetalRHI::getBindlessTextureManager()
{
    return m_BindlessTextureManager;
}

bool MetalRHI::supportsBindlessTextures() const
{
    return m_BindlessSupported;
}

uint32_t MetalRHI::maxBindlessSampledImages() const
{
    return m_MaxBindlessSampledImages;
}

uint32_t MetalRHI::maxBindlessStorageBuffers() const
{
    return m_MaxBindlessStorageBuffers;
}

void MetalRHI::CmdSetBindlessIndexPFN(RHICommandBuffer*    /*commandBuffer*/,
                                      RHIPipelineBindPoint /*pipelineBindPoint*/,
                                      RHIPipelineLayout*   /*layout*/,
                                      uint32_t             packed_index)
{
#ifdef __APPLE__
    if (!m_BindlessSupported || m_BindlessTextureManager == nullptr)
    {
        return;
    }

    // Push the packed 32-bit bindless index through
    // setVertexBytes / setFragmentBytes on the current frame's
    // render command encoder. This mirrors Vulkan's push-constant
    // and DX12's root-constant delivery.
    //
    // The 4-byte payload is: low 16 bits = texture index,
    // high 16 bits = sampler index (see BindlessIndex::Pack).
    const uint32_t frame = m_CurrentFrameIndex;
    if (frame >= k_max_frames_in_flight)
    {
        return;
    }

    id<MTLRenderCommandEncoder> encoder = m_RenderEncoders[frame];
    if (encoder == nil)
    {
        return;
    }

    // Bind the argument buffer at the canonical slot (0) for both
    // vertex and fragment stages, then push the index constant at
    // slot 1.
    const NSUInteger buffer_index = MetalBindlessTextureManager::kBindlessBufferIndex;
    const NSUInteger index_buffer_index = MetalBindlessTextureManager::kBindlessIndexBufferIndex;

    [encoder setVertexBuffer:m_BindlessTextureManager->getArgumentBuffer()
                      offset:0
                     atIndex:buffer_index];
    [encoder setFragmentBuffer:m_BindlessTextureManager->getArgumentBuffer()
                        offset:0
                       atIndex:buffer_index];

    [encoder setVertexBytes:&packed_index
                     length:sizeof(packed_index)
                    atIndex:index_buffer_index];
    [encoder setFragmentBytes:&packed_index
                       length:sizeof(packed_index)
                      atIndex:index_buffer_index];

    // Ensure residency of all allocated textures before any draw.
    m_BindlessTextureManager->ensureResidency(encoder);
#endif
}
