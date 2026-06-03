#pragma once

#include "Runtime/Core/Base/EngineSystem.h"
#include "Runtime/Function/Render/Interface/RHI.h"
#include "Runtime/Function/Render/Interface/RHIStruct.h"

#include <EASTL/functional.h>
#include <EASTL/string.h>

#ifdef __APPLE__
    #define Component AppleComponent
    #include <Metal/Metal.h>
    #include <QuartzCore/CAMetalLayer.h>
    #undef Component
#endif

#include <memory>
#include <vector>

class WindowSystem;

class MetalRHI final : public RHI
{
public:
    std::string GetName() const override { return "MetalRHI"; }
    SystemInitPhase GetInitPhase() const override { return SystemInitPhase::Rendering; }
    std::vector<std::type_index> GetDependencies() const override;
    bool Initialize() override;
    void Shutdown() override;

    // Get the graphics API
    GraphicsAPI getGraphicsAPI() const override { return GraphicsAPI::Metal; }

    // Initialize
    virtual void PrepareContext() override final;

    // RHI interface implementation
    virtual bool IsPointLightShadowEnabled() override;

    // Allocate and create
    virtual bool AllocateCommandBuffers(const RHICommandBufferAllocateInfo* pAllocateInfo,
                                        RHICommandBuffer*& pCommandBuffers) override;
    virtual bool AllocateDescriptorSets(const RHIDescriptorSetAllocateInfo* pAllocateInfo,
                                        RHIDescriptorSet*& pDescriptorSets) override;
    virtual void CreateSwapchain() override;
    virtual void RecreateSwapchain() override;
    virtual void CreateSwapchainImageViews() override;
    virtual void CreateFramebufferImageAndView() override;
    virtual RHISampler* GetOrCreateDefaultSampler(RHIDefaultSamplerType type) override;
    virtual RHISampler* GetOrCreateMipmapSampler(uint32_t width, uint32_t height) override;
    virtual RHIShader* CreateShaderModule(const std::vector<unsigned char>& shader_code) override;

    // Shader compilation
    virtual RHIShader* CreateShaderModuleFromFile(const std::string& file_path,
                                                  ShaderStage shader_stage,
                                                  const std::vector<std::string>& include_paths,
                                                  const ShaderMacros& macros,
                                                  std::vector<uint8_t>& output_spirv_code,
                                                  const std::string& entry_point = "main") override;
    virtual RHIShader* CreateShaderModuleFromSource(const std::string& source_code,
                                                    ShaderStage shader_stage,
                                                    const std::string& shader_name = "",
                                                    const std::vector<std::string>& include_paths = {},
                                                    const ShaderMacros& macros = {}) override;

    // Buffer operations
    virtual void CreateBuffer(RHIDeviceSize size,
                              RHIBufferUsageFlags usage,
                              RHIMemoryPropertyFlags properties,
                              RHIBuffer*& buffer,
                              RHIDeviceMemory*& buffer_memory) override;
    virtual void CreateBufferAndInitialize(RHIBufferUsageFlags usage,
                                           RHIMemoryPropertyFlags properties,
                                           RHIBuffer*& buffer,
                                           RHIDeviceMemory*& buffer_memory,
                                           RHIDeviceSize size,
                                           void* data = nullptr,
                                           int datasize = 0) override;
    virtual void CopyBuffer(RHIBuffer* srcBuffer,
                            RHIBuffer* dstBuffer,
                            RHIDeviceSize srcOffset,
                            RHIDeviceSize dstOffset,
                            RHIDeviceSize size) override;

    // Image operations
    virtual void CreateImage(uint32_t image_width,
                             uint32_t image_height,
                             RHIFormat format,
                             RHIImageTiling image_tiling,
                             RHIImageUsageFlags image_usage_flags,
                             RHIMemoryPropertyFlags memory_property_flags,
                             RHIImage*& image,
                             RHIDeviceMemory*& memory,
                             RHIImageCreateFlags image_create_flags,
                             uint32_t array_layers,
                             uint32_t miplevels) override;
    virtual void CreateImageView(RHIImage* image,
                                 RHIFormat format,
                                 RHIImageAspectFlags image_aspect_flags,
                                 RHIImageViewType view_type,
                                 uint32_t layout_count,
                                 uint32_t miplevels,
                                 RHIImageView*& image_view) override;
    virtual void CreateGlobalImage(RHIImage*& image,
                                   RHIImageView*& image_view,
                                   void* image_allocation,
                                   uint32_t texture_image_width,
                                   uint32_t texture_image_height,
                                   void* texture_image_pixels,
                                   RHIFormat texture_image_format,
                                   uint32_t miplevels = 0) override;
    virtual void CreateCubeMap(RHIImage*& image,
                               RHIImageView*& image_view,
                               void* image_allocation,
                               uint32_t texture_image_width,
                               uint32_t texture_image_height,
                               std::array<void*, 6> texture_image_pixels,
                               RHIFormat texture_image_format,
                               uint32_t miplevels) override;

    // Pipeline creation
    virtual void CreateCommandPool() override;
    virtual bool CreateCommandPool(const RHICommandPoolCreateInfo* pCreateInfo, RHICommandPool*& pCommandPool) override;
    virtual bool CreateDescriptorPool(const RHIDescriptorPoolCreateInfo* pCreateInfo,
                                      RHIDescriptorPool*& pDescriptorPool) override;
    virtual bool CreateDescriptorSetLayout(const RHIDescriptorSetLayoutCreateInfo* pCreateInfo,
                                           RHIDescriptorSetLayout*& pSetLayout) override;
    virtual bool CreateFence(const RHIFenceCreateInfo* pCreateInfo, RHIFence*& pFence) override;
    virtual bool CreateFramebuffer(const RHIFramebufferCreateInfo* pCreateInfo, RHIFramebuffer*& pFramebuffer) override;
    virtual bool CreateGraphicsPipelines(RHIPipelineCache* pipelineCache,
                                         uint32_t createInfoCount,
                                         const RHIGraphicsPipelineCreateInfo* pCreateInfos,
                                         RHIPipeline*& pPipelines) override;
    virtual bool CreateComputePipelines(RHIPipelineCache* pipelineCache,
                                        uint32_t createInfoCount,
                                        const RHIComputePipelineCreateInfo* pCreateInfos,
                                        RHIPipeline*& pPipelines) override;
    virtual bool CreatePipelineLayout(const RHIPipelineLayoutCreateInfo* pCreateInfo,
                                      RHIPipelineLayout*& pPipelineLayout) override;
    virtual bool CreateRenderPass(const RHIRenderPassCreateInfo* pCreateInfo, RHIRenderPass*& pRenderPass) override;
    virtual bool CreateSampler(const RHISamplerCreateInfo* pCreateInfo, RHISampler*& pSampler) override;
    virtual bool CreateSemaphore(const RHISemaphoreCreateInfo* pCreateInfo, RHISemaphore*& pSemaphore) override;

    // Command operations (same as DX12RHI)
    virtual bool WaitForFencesPFN(uint32_t fenceCount, RHIFence* const* pFence, RHIBool32 waitAll, uint64_t timeout) override;
    virtual bool ResetFencesPFN(uint32_t fenceCount, RHIFence* const* pFences) override;
    virtual bool ResetCommandPoolPFN(RHICommandPool* commandPool, RHICommandPoolResetFlags flags) override;
    virtual bool BeginCommandBufferPFN(RHICommandBuffer* commandBuffer,
                                       const RHICommandBufferBeginInfo* pBeginInfo) override;
    virtual bool EndCommandBufferPFN(RHICommandBuffer* commandBuffer) override;
    virtual void CmdBeginRenderPassPFN(RHICommandBuffer* commandBuffer,
                                       const RHIRenderPassBeginInfo* pRenderPassBegin,
                                       RHISubpassContents contents) override;
    virtual void CmdNextSubpassPFN(RHICommandBuffer* commandBuffer, RHISubpassContents contents) override;
    virtual void CmdEndRenderPassPFN(RHICommandBuffer* commandBuffer) override;
    virtual void CmdBindPipelinePFN(RHICommandBuffer* commandBuffer,
                                    RHIPipelineBindPoint pipelineBindPoint,
                                    RHIPipeline* pipeline) override;
    virtual void CmdSetViewportPFN(RHICommandBuffer* commandBuffer,
                                   uint32_t firstViewport,
                                   uint32_t viewportCount,
                                   const RHIViewport* pViewports) override;
    virtual void CmdSetScissorPFN(RHICommandBuffer* commandBuffer,
                                  uint32_t firstScissor,
                                  uint32_t scissorCount,
                                  const RHIRect2D* pScissors) override;
    virtual void CmdBindVertexBuffersPFN(RHICommandBuffer* commandBuffer,
                                         uint32_t firstBinding,
                                         uint32_t bindingCount,
                                         RHIBuffer* const* pBuffers,
                                         const RHIDeviceSize* pOffsets) override;
    virtual void CmdBindIndexBufferPFN(RHICommandBuffer* commandBuffer,
                                       RHIBuffer* buffer,
                                       RHIDeviceSize offset,
                                       RHIIndexType indexType) override;
    virtual void CmdBindDescriptorSetsPFN(RHICommandBuffer* commandBuffer,
                                          RHIPipelineBindPoint pipelineBindPoint,
                                          RHIPipelineLayout* layout,
                                          uint32_t firstSet,
                                          uint32_t descriptorSetCount,
                                          const RHIDescriptorSet* const* pDescriptorSets,
                                          uint32_t dynamicOffsetCount,
                                          const uint32_t* pDynamicOffsets) override;
    virtual void CmdDrawIndexedPFN(RHICommandBuffer* commandBuffer,
                                   uint32_t indexCount,
                                   uint32_t instanceCount,
                                   uint32_t firstIndex,
                                   int32_t vertexOffset,
                                   uint32_t firstInstance) override;
    virtual void CmdClearAttachmentsPFN(RHICommandBuffer* commandBuffer,
                                        uint32_t attachmentCount,
                                        const RHIClearAttachment* pAttachments,
                                        uint32_t rectCount,
                                        const RHIClearRect* pRects) override;

    virtual bool BeginCommandBuffer(RHICommandBuffer* commandBuffer,
                                    const RHICommandBufferBeginInfo* pBeginInfo) override;
    virtual void CmdCopyImageToBuffer(RHICommandBuffer* commandBuffer,
                                      RHIImage* srcImage,
                                      RHIImageLayout srcImageLayout,
                                      RHIBuffer* dstBuffer,
                                      uint32_t regionCount,
                                      const RHIBufferImageCopy* pRegions) override;
    virtual void CmdCopyImageToImage(RHICommandBuffer* commandBuffer,
                                     RHIImage* srcImage,
                                     RHIImageAspectFlagBits srcFlag,
                                     RHIImage* dstImage,
                                     RHIImageAspectFlagBits dstFlag,
                                     uint32_t width,
                                     uint32_t height) override;
    virtual void CmdBlitImage(RHICommandBuffer* commandBuffer,
                              RHIImage* srcImage,
                              RHIImageLayout srcImageLayout,
                              RHIImage* dstImage,
                              RHIImageLayout dstImageLayout,
                              uint32_t srcX0,
                              uint32_t srcY0,
                              uint32_t srcX1,
                              uint32_t srcY1,
                              uint32_t dstX0,
                              uint32_t dstY0,
                              uint32_t dstX1,
                              uint32_t dstY1,
                              RHIFilter filter) override;
    virtual void CmdCopyBuffer(RHICommandBuffer* commandBuffer,
                               RHIBuffer* srcBuffer,
                               RHIBuffer* dstBuffer,
                               uint32_t regionCount,
                               RHIBufferCopy* pRegions) override;
    virtual void CmdDraw(RHICommandBuffer* commandBuffer,
                         uint32_t vertexCount,
                         uint32_t instanceCount,
                         uint32_t firstVertex,
                         uint32_t firstInstance) override;
    virtual void CmdDispatch(RHICommandBuffer* commandBuffer,
                             uint32_t groupCountX,
                             uint32_t groupCountY,
                             uint32_t groupCountZ) override;
    virtual void CmdDispatchIndirect(RHICommandBuffer* commandBuffer, RHIBuffer* buffer, RHIDeviceSize offset) override;
    virtual void CmdPipelineBarrier(RHICommandBuffer* commandBuffer,
                                    RHIPipelineStageFlags srcStageMask,
                                    RHIPipelineStageFlags dstStageMask,
                                    RHIDependencyFlags dependencyFlags,
                                    uint32_t memoryBarrierCount,
                                    const RHIMemoryBarrier* pMemoryBarriers,
                                    uint32_t bufferMemoryBarrierCount,
                                    const RHIBufferMemoryBarrier* pBufferMemoryBarriers,
                                    uint32_t imageMemoryBarrierCount,
                                    const RHIImageMemoryBarrier* pImageMemoryBarriers) override;
    virtual bool EndCommandBuffer(RHICommandBuffer* commandBuffer) override;
    virtual void UpdateDescriptorSets(uint32_t descriptorWriteCount,
                                      const RHIWriteDescriptorSet* pDescriptorWrites,
                                      uint32_t descriptorCopyCount,
                                      const RHICopyDescriptorSet* pDescriptorCopies) override;
    virtual bool QueueSubmit(RHIQueue* queue, uint32_t submitCount, const RHISubmitInfo* pSubmits, RHIFence* fence) override;
    virtual bool QueueWaitIdle(RHIQueue* queue) override;
    virtual void ResetCommandPool() override;
    virtual void WaitForFences() override;

    // Query operations
    virtual void GetPhysicalDeviceProperties(RHIPhysicalDeviceProperties* pProperties) override;
    virtual RHICommandBuffer* GetCurrentCommandBuffer() const override;
    virtual RHICommandBuffer* const* GetCommandBufferList() const override;
    virtual RHICommandPool* GetCommandPoor() const override;
    virtual RHIDescriptorPool* GetDescriptorPoor() const override;
    virtual RHIFence* const* GetFenceList() const override;
    virtual QueueFamilyIndices GetQueueFamilyIndices() const override;
    virtual RHIQueue* GetGraphicsQueue() const override;
    virtual RHIQueue* GetComputeQueue() const override;
    virtual RHISwapChainDesc GetSwapchainInfo() override;
    virtual RHIDepthImageDesc GetDepthImageInfo() const override;
    virtual uint8_t GetMaxFramesInFlight() const override;
    virtual uint8_t GetCurrentFrameIndex() const override;
    virtual void SetCurrentFrameIndex(uint8_t index) override;

    // Command write
    virtual RHICommandBuffer* BeginSingleTimeCommands() override;
    virtual void EndSingleTimeCommands(RHICommandBuffer* command_buffer) override;
    virtual bool PrepareBeforePass(std::function<void()> passUpdateAfterRecreateSwapchain) override;
    virtual void SubmitRendering(std::function<void()> passUpdateAfterRecreateSwapchain) override;
    virtual void PushEvent(RHICommandBuffer* commond_buffer, const char* name, const float* color) override;
    virtual void PopEvent(RHICommandBuffer* commond_buffer) override;

    // Destroy operations
    virtual void clear() override;
    virtual void ClearSwapchain() override;
    virtual void DestroyDefaultSampler(RHIDefaultSamplerType type) override;
    virtual void DestroyMipmappedSampler() override;
    virtual void DestroyShaderModule(RHIShader* shader) override;
    virtual void DestroySemaphore(RHISemaphore* semaphore) override;
    virtual void DestroySampler(RHISampler* sampler) override;
    virtual void DestroyInstance(RHIInstance* instance) override;
    virtual void DestroyImageView(RHIImageView* imageView) override;
    virtual void DestroyImage(RHIImage* image) override;
    virtual void DestroyFramebuffer(RHIFramebuffer* framebuffer) override;
    virtual void DestroyFence(RHIFence* fence) override;
    virtual void DestroyDevice() override;
    virtual void DestroyCommandPool(RHICommandPool* commandPool) override;
    virtual void DestroyBuffer(RHIBuffer*& buffer) override;
    virtual void FreeCommandBuffers(RHICommandPool* commandPool,
                                    uint32_t commandBufferCount,
                                    RHICommandBuffer* pCommandBuffers) override;

    // Memory operations
    virtual void FreeMemory(RHIDeviceMemory*& memory) override;
    virtual bool MapMemory(RHIDeviceMemory* memory,
                           RHIDeviceSize offset,
                           RHIDeviceSize size,
                           RHIMemoryMapFlags flags,
                           void** ppData) override;
    virtual void UnmapMemory(RHIDeviceMemory* memory) override;
    virtual void InvalidateMappedMemoryRanges(void* pNext,
                                              RHIDeviceMemory* memory,
                                              RHIDeviceSize offset,
                                              RHIDeviceSize size) override;
    virtual void FlushMappedMemoryRanges(void* pNext, RHIDeviceMemory* memory, RHIDeviceSize offset, RHIDeviceSize size) override;

    // Semaphores
    virtual RHISemaphore*& GetTextureCopySemaphore(uint32_t index) override;

    // Multi-viewport support
    virtual void RegisterViewport(const int viewport_id, const RHIViewport& viewport) override;
    virtual void UpdateViewport(ViewportType viewport_id, const RHIViewport& viewport) override;
    virtual RHIViewport* GetViewport(ViewportType viewport_id) override;

    // Viewport RenderTexture support
    virtual void CreateViewportRenderTexture(const std::string& viewport_id, uint32_t width, uint32_t height) override;
    virtual void UpdateViewportRenderTexture(const std::string& viewport_id, uint32_t width, uint32_t height) override;
    virtual void DestroyViewportRenderTexture(const std::string& viewport_id) override;
    virtual ViewportRenderTexture* GetViewportRenderTexture(const std::string& viewport_id) override;

private:
#ifdef __APPLE__
    // Metal specific members
    id<MTLDevice> m_Device;
    id<MTLCommandQueue> m_CommandQueue;
    CAMetalLayer* m_MetalLayer;
    id<MTLCommandBuffer> m_MetalCommandBuffers[3];  // k_max_frames_in_flight
    id<MTLRenderCommandEncoder> m_RenderEncoders[3];
    id<MTLTexture> m_RenderTargets[3];
    id<MTLTexture> m_DepthStencil;
    id<MTLFence> m_Fences[3];
    dispatch_semaphore_t m_Semaphores[3];
#endif
    uint8_t m_CurrentFrameIndex {0};
    static constexpr uint8_t k_max_frames_in_flight {3};

    // RHI interface members
    RHIQueue* m_GraphicsQueue {nullptr};
    RHIQueue* m_ComputeQueue {nullptr};
    RHIFormat m_SwapchainImageFormat {RHI_FORMAT_UNDEFINED};
    std::vector<RHIImageView*> m_SwapchainImageviews;
    RHIExtent2D m_SwapchainExtent;
    RHIRect2D m_Scissors[2];
    std::string m_ActiveViewportId {"default"};
    std::map<std::string, ViewportRenderTexture> m_ViewportRenderTextures;
    RHIFormat m_DepthImageFormat {RHI_FORMAT_UNDEFINED};
    RHIImageView* m_DepthImageView {nullptr};
    RHIFence* m_RhiIsFrameInFlightFences[k_max_frames_in_flight];
    RHIDescriptorPool* m_DescriptorPool {nullptr};
    RHICommandPool* m_RhiCommandPool {nullptr};
    RHICommandBuffer* m_CommandBuffers[k_max_frames_in_flight];
    RHICommandBuffer* m_CurrentCommandBuffer {nullptr};
    QueueFamilyIndices m_QueueIndices;
};
