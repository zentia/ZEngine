#pragma once

#include "Runtime/Function/Render/Interface/RHI.h"
#include "Runtime/Function/Render/Interface/Vulkan/ShaderCompiler.h"
#include "Runtime/Function/Render/Interface/Vulkan/VulkanBindlessTextureManager.h"
#include "Runtime/Function/Render/Interface/Vulkan/VulkanRHIResource.h"

#include <functional>
#include <map>
#include <memory>
#include <vector>
#include <vma/vk_mem_alloc.h>
#include <vulkan/vulkan.h>

// Vulkan-specific swapchain support details
struct SwapChainSupportDetails
{
    VkSurfaceCapabilitiesKHR capabilities;
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR> presentModes;
};

class VulkanRHI final : public RHI
{
public:
    std::string GetName() const override { return "VulkanRHI"; }
    SystemInitPhase GetInitPhase() const override { return SystemInitPhase::Rendering; }
    std::vector<std::type_index> GetDependencies() const override;
    bool Initialize() override;
    void Shutdown() override {}

    // Get the graphics API
    GraphicsAPI getGraphicsAPI() const override { return GraphicsAPI::Vulkan; }

    // initialize
    virtual void PrepareContext() override final;

    // allocate and create
    bool AllocateCommandBuffers(const RHICommandBufferAllocateInfo* pAllocateInfo,
                                RHICommandBuffer*& pCommandBuffers) override;
    bool AllocateDescriptorSets(const RHIDescriptorSetAllocateInfo* pAllocateInfo,
                                RHIDescriptorSet*& pDescriptorSets) override;
    void CreateSwapchain() override;
    void RecreateSwapchain() override;
    void CreateSwapchainImageViews() override;
    void CreateFramebufferImageAndView() override;
    RHISampler* GetOrCreateDefaultSampler(RHIDefaultSamplerType type) override;
    RHISampler* GetOrCreateMipmapSampler(uint32_t width, uint32_t height) override;
    RHIShader* CreateShaderModule(const std::vector<unsigned char>& shader_code) override;
    RHIShader* CreateShaderModuleFromFile(const std::string& file_path,
                                          ShaderStage shader_stage,
                                          const std::vector<std::string>& include_paths,
                                          const ShaderMacros& macros,
                                          std::vector<uint8_t>& output_binary,
                                          const std::string& entry_point = "main",
                                          bool embed_debug = false) override;
    RHIShader* CreateShaderModuleFromSource(const std::string& source_code,
                                            ShaderStage shader_stage,
                                            const std::string& shader_name = "",
                                            const std::vector<std::string>& include_paths = {},
                                            const ShaderMacros& macros = {}) override;
    void CreateBuffer(RHIDeviceSize size,
                      RHIBufferUsageFlags usage,
                      RHIMemoryPropertyFlags properties,
                      RHIBuffer*& buffer,
                      RHIDeviceMemory*& buffer_memory) override;
    void CreateBufferAndInitialize(RHIBufferUsageFlags usage,
                                   RHIMemoryPropertyFlags properties,
                                   RHIBuffer*& buffer,
                                   RHIDeviceMemory*& buffer_memory,
                                   RHIDeviceSize size,
                                   void* data = nullptr,
                                   int datasize = 0) override;
    bool CreateBufferVMA(void* allocator,
                         const RHIBufferCreateInfo* pBufferCreateInfo,
                         void* pAllocationCreateInfo,
                         RHIBuffer*& pBuffer,
                         void** pAllocation,
                         void* pAllocationInfo) override;
    bool CreateBufferWithAlignmentVMA(void* allocator,
                                      const RHIBufferCreateInfo* pBufferCreateInfo,
                                      void* pAllocationCreateInfo,
                                      RHIDeviceSize minAlignment,
                                      RHIBuffer*& pBuffer,
                                      void** pAllocation,
                                      void* pAllocationInfo) override;
    void CopyBuffer(RHIBuffer* srcBuffer,
                    RHIBuffer* dstBuffer,
                    RHIDeviceSize srcOffset,
                    RHIDeviceSize dstOffset,
                    RHIDeviceSize size) override;
    void CreateImage(uint32_t image_width,
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
    void CreateImageView(RHIImage* image,
                         RHIFormat format,
                         RHIImageAspectFlags image_aspect_flags,
                         RHIImageViewType view_type,
                         uint32_t layout_count,
                         uint32_t miplevels,
                         RHIImageView*& image_view) override;
    void CreateGlobalImage(RHIImage*& image,
                           RHIImageView*& image_view,
                           void* image_allocation,
                           uint32_t texture_image_width,
                           uint32_t texture_image_height,
                           void* texture_image_pixels,
                           RHIFormat texture_image_format,
                           uint32_t miplevels = 0) override;
    void CreateCubeMap(RHIImage*& image,
                       RHIImageView*& image_view,
                       void* image_allocation,
                       uint32_t texture_image_width,
                       uint32_t texture_image_height,
                       std::array<void*, 6> texture_image_pixels,
                       RHIFormat texture_image_format,
                       uint32_t miplevels) override;
    bool CreateCommandPool(const RHICommandPoolCreateInfo* pCreateInfo, RHICommandPool*& pCommandPool) override;
    bool CreateDescriptorPool(const RHIDescriptorPoolCreateInfo* pCreateInfo,
                              RHIDescriptorPool*& pDescriptorPool) override;
    bool CreateDescriptorSetLayout(const RHIDescriptorSetLayoutCreateInfo* pCreateInfo,
                                   RHIDescriptorSetLayout*& pSetLayout) override;
    bool CreateFence(const RHIFenceCreateInfo* pCreateInfo, RHIFence*& pFence) override;
    bool CreateFramebuffer(const RHIFramebufferCreateInfo* pCreateInfo, RHIFramebuffer*& pFramebuffer) override;
    bool CreateGraphicsPipelines(RHIPipelineCache* pipelineCache,
                                 uint32_t createInfoCount,
                                 const RHIGraphicsPipelineCreateInfo* pCreateInfos,
                                 RHIPipeline*& pPipelines) override;
    bool CreateComputePipelines(RHIPipelineCache* pipelineCache,
                                uint32_t createInfoCount,
                                const RHIComputePipelineCreateInfo* pCreateInfos,
                                RHIPipeline*& pPipelines) override;
    bool CreatePipelineLayout(const RHIPipelineLayoutCreateInfo* pCreateInfo,
                              RHIPipelineLayout*& pPipelineLayout) override;
    bool CreateRenderPass(const RHIRenderPassCreateInfo* pCreateInfo, RHIRenderPass*& pRenderPass) override;
    void DestroyRenderPass(RHIRenderPass* renderPass) override;
    bool CreateSampler(const RHISamplerCreateInfo* pCreateInfo, RHISampler*& pSampler) override;
    bool CreateSemaphore(const RHISemaphoreCreateInfo* pCreateInfo, RHISemaphore*& pSemaphore) override;

    // command and command write
    bool WaitForFencesPFN(uint32_t fenceCount, RHIFence* const* pFence, RHIBool32 waitAll, uint64_t timeout) override;
    bool ResetFencesPFN(uint32_t fenceCount, RHIFence* const* pFences) override;
    bool ResetCommandPoolPFN(RHICommandPool* commandPool, RHICommandPoolResetFlags flags) override;
    bool BeginCommandBufferPFN(RHICommandBuffer* commandBuffer, const RHICommandBufferBeginInfo* pBeginInfo) override;
    bool EndCommandBufferPFN(RHICommandBuffer* commandBuffer) override;
    void CmdBeginRenderPassPFN(RHICommandBuffer* commandBuffer,
                               const RHIRenderPassBeginInfo* pRenderPassBegin,
                               RHISubpassContents contents) override;
    void CmdNextSubpassPFN(RHICommandBuffer* commandBuffer, RHISubpassContents contents) override;
    void CmdEndRenderPassPFN(RHICommandBuffer* commandBuffer) override;
    void CmdBindPipelinePFN(RHICommandBuffer* commandBuffer,
                            RHIPipelineBindPoint pipelineBindPoint,
                            RHIPipeline* pipeline) override;
    void CmdSetViewportPFN(RHICommandBuffer* commandBuffer,
                           uint32_t firstViewport,
                           uint32_t viewportCount,
                           const RHIViewport* pViewports) override;
    void CmdSetScissorPFN(RHICommandBuffer* commandBuffer,
                          uint32_t firstScissor,
                          uint32_t scissorCount,
                          const RHIRect2D* pScissors) override;
    void CmdBindVertexBuffersPFN(RHICommandBuffer* commandBuffer,
                                 uint32_t firstBinding,
                                 uint32_t bindingCount,
                                 RHIBuffer* const* pBuffers,
                                 const RHIDeviceSize* pOffsets) override;
    void CmdBindIndexBufferPFN(RHICommandBuffer* commandBuffer,
                               RHIBuffer* buffer,
                               RHIDeviceSize offset,
                               RHIIndexType indexType) override;
    void CmdBindDescriptorSetsPFN(RHICommandBuffer* commandBuffer,
                                  RHIPipelineBindPoint pipelineBindPoint,
                                  RHIPipelineLayout* layout,
                                  uint32_t firstSet,
                                  uint32_t descriptorSetCount,
                                  const RHIDescriptorSet* const* pDescriptorSets,
                                  uint32_t dynamicOffsetCount,
                                  const uint32_t* pDynamicOffsets) override;
    // PR-V1: bindless-index push (Vulkan path).
    // Translates to vkCmdPushConstants on the bindless push-constant
    // range that the pipeline layout reserved (offset=0, size=4,
    // stageFlags=VK_SHADER_STAGE_ALL by convention -- see
    // VulkanBindlessTextureManager helpers for the canonical layout).
    // No-op when the layout pointer is null; never inspects the
    // pipeline-layout internals because Vulkan has no equivalent of
    // DX12PipelineLayout::usesBindless() -- we just push 4 bytes and
    // trust the caller / validation layer to flag a layout that did
    // not declare a matching range.
    void CmdSetBindlessIndexPFN(RHICommandBuffer* commandBuffer,
                                RHIPipelineBindPoint pipelineBindPoint,
                                RHIPipelineLayout* layout,
                                uint32_t packed_index) override;
    void CmdDrawIndexedPFN(RHICommandBuffer* commandBuffer,
                           uint32_t indexCount,
                           uint32_t instanceCount,
                           uint32_t firstIndex,
                           int32_t vertexOffset,
                           uint32_t firstInstance) override;
    void CmdClearAttachmentsPFN(RHICommandBuffer* commandBuffer,
                                uint32_t attachmentCount,
                                const RHIClearAttachment* pAttachments,
                                uint32_t rectCount,
                                const RHIClearRect* pRects) override;

    bool BeginCommandBuffer(RHICommandBuffer* commandBuffer, const RHICommandBufferBeginInfo* pBeginInfo) override;
    void CmdCopyImageToBuffer(RHICommandBuffer* commandBuffer,
                              RHIImage* srcImage,
                              RHIImageLayout srcImageLayout,
                              RHIBuffer* dstBuffer,
                              uint32_t regionCount,
                              const RHIBufferImageCopy* pRegions) override;
    void CmdCopyImageToImage(RHICommandBuffer* commandBuffer,
                             RHIImage* srcImage,
                             RHIImageAspectFlagBits srcFlag,
                             RHIImage* dstImage,
                             RHIImageAspectFlagBits dstFlag,
                             uint32_t width,
                             uint32_t height) override;
    void CmdBlitImage(RHICommandBuffer* commandBuffer,
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
    void CmdCopyBuffer(RHICommandBuffer* commandBuffer,
                       RHIBuffer* srcBuffer,
                       RHIBuffer* dstBuffer,
                       uint32_t regionCount,
                       RHIBufferCopy* pRegions) override;
    void CmdDraw(RHICommandBuffer* commandBuffer,
                 uint32_t vertexCount,
                 uint32_t instanceCount,
                 uint32_t firstVertex,
                 uint32_t firstInstance) override;
    void CmdDispatch(RHICommandBuffer* commandBuffer,
                     uint32_t groupCountX,
                     uint32_t groupCountY,
                     uint32_t groupCountZ) override;
    void CmdDispatchIndirect(RHICommandBuffer* commandBuffer, RHIBuffer* buffer, RHIDeviceSize offset) override;
    void CmdPipelineBarrier(RHICommandBuffer* commandBuffer,
                            RHIPipelineStageFlags srcStageMask,
                            RHIPipelineStageFlags dstStageMask,
                            RHIDependencyFlags dependencyFlags,
                            uint32_t memoryBarrierCount,
                            const RHIMemoryBarrier* pMemoryBarriers,
                            uint32_t bufferMemoryBarrierCount,
                            const RHIBufferMemoryBarrier* pBufferMemoryBarriers,
                            uint32_t imageMemoryBarrierCount,
                            const RHIImageMemoryBarrier* pImageMemoryBarriers) override;
    bool EndCommandBuffer(RHICommandBuffer* commandBuffer) override;
    void UpdateDescriptorSets(uint32_t descriptorWriteCount,
                              const RHIWriteDescriptorSet* pDescriptorWrites,
                              uint32_t descriptorCopyCount,
                              const RHICopyDescriptorSet* pDescriptorCopies) override;
    bool QueueSubmit(RHIQueue* queue, uint32_t submitCount, const RHISubmitInfo* pSubmits, RHIFence* fence) override;
    bool QueueWaitIdle(RHIQueue* queue) override;
    void ResetCommandPool() override;
    void WaitForFences() override;
    bool WaitForFences(uint32_t fenceCount, const RHIFence* const* pFences, RHIBool32 waitAll, uint64_t timeout);

    // query
    void GetPhysicalDeviceProperties(RHIPhysicalDeviceProperties* pProperties) override;
    RHICommandBuffer* GetCurrentCommandBuffer() const override;
    RHICommandBuffer* const* GetCommandBufferList() const override;
    RHICommandPool* GetCommandPoor() const override;
    RHIDescriptorPool* GetDescriptorPoor() const override;
    RHIFence* const* GetFenceList() const override;
    QueueFamilyIndices GetQueueFamilyIndices() const override;
    RHIQueue* GetGraphicsQueue() const override;
    RHIQueue* GetComputeQueue() const override;
    RHISwapChainDesc GetSwapchainInfo() override;
    RHIDepthImageDesc GetDepthImageInfo() const override;
    uint8_t GetMaxFramesInFlight() const override;
    uint8_t GetCurrentFrameIndex() const override;
    void SetCurrentFrameIndex(uint8_t index) override;

    // bindless capability
    bool supportsBindlessTextures() const override { return m_BindlessSupported; }
    uint32_t maxBindlessSampledImages() const override { return m_MaxBindlessSampledImages; }
    uint32_t maxBindlessStorageBuffers() const override { return m_MaxBindlessStorageBuffers; }

    // PR3: cross-backend bindless texture manager. nullptr unless
    // m_BindlessSupported is true and the manager initialized cleanly.
    RHIBindlessTextureManager* getBindlessTextureManager() override { return m_BindlessTextureManager.get(); }

    // command write
    RHICommandBuffer* BeginSingleTimeCommands() override;
    void EndSingleTimeCommands(RHICommandBuffer* command_buffer) override;
    bool PrepareBeforePass(std::function<void()> passUpdateAfterRecreateSwapchain) override;
    void SubmitRendering(std::function<void()> passUpdateAfterRecreateSwapchain) override;
    void PushEvent(RHICommandBuffer* commond_buffer, const char* name, const float* color) override;
    void PopEvent(RHICommandBuffer* commond_buffer) override;

    // destory
    virtual ~VulkanRHI() override final;
    void clear() override;
    void ClearSwapchain() override;
    void DestroyDefaultSampler(RHIDefaultSamplerType type) override;
    void DestroyMipmappedSampler() override;
    void DestroyShaderModule(RHIShader* shader) override;
    void DestroySemaphore(RHISemaphore* semaphore) override;
    void DestroySampler(RHISampler* sampler) override;
    void DestroyInstance(RHIInstance* instance) override;
    void DestroyImageView(RHIImageView* imageView) override;
    void DestroyImage(RHIImage* image) override;
    void DestroyFramebuffer(RHIFramebuffer* framebuffer) override;
    void DestroyFence(RHIFence* fence) override;
    void DestroyDevice() override;
    void DestroyCommandPool(RHICommandPool* commandPool) override;
    void DestroyBuffer(RHIBuffer*& buffer) override;
    void FreeCommandBuffers(RHICommandPool* commandPool,
                            uint32_t commandBufferCount,
                            RHICommandBuffer* pCommandBuffers) override;

    // memory
    void FreeMemory(RHIDeviceMemory*& memory) override;
    bool MapMemory(RHIDeviceMemory* memory,
                   RHIDeviceSize offset,
                   RHIDeviceSize size,
                   RHIMemoryMapFlags flags,
                   void** ppData) override;
    void UnmapMemory(RHIDeviceMemory* memory) override;
    void InvalidateMappedMemoryRanges(void* pNext,
                                      RHIDeviceMemory* memory,
                                      RHIDeviceSize offset,
                                      RHIDeviceSize size) override;
    void
    FlushMappedMemoryRanges(void* pNext, RHIDeviceMemory* memory, RHIDeviceSize offset, RHIDeviceSize size) override;

    // semaphores
    RHISemaphore*& GetTextureCopySemaphore(uint32_t index) override;

    // Multi-viewport support
    void RegisterViewport(const int viewport_id, const RHIViewport& viewport) override;
    void UpdateViewport(ViewportType viewport_id, const RHIViewport& viewport) override;
    RHIViewport* GetViewport(ViewportType viewport_id) override;

    // Viewport RenderTexture support
    void CreateViewportRenderTexture(const std::string& viewport_id, uint32_t width, uint32_t height) override;
    void UpdateViewportRenderTexture(const std::string& viewport_id, uint32_t width, uint32_t height) override;
    void DestroyViewportRenderTexture(const std::string& viewport_id) override;
    ViewportRenderTexture* GetViewportRenderTexture(const std::string& viewport_id) override;

    // VMA allocator access
    VmaAllocator getVmaAllocator() const { return m_AssetsAllocator; }

public:
    static uint8_t const k_max_frames_in_flight {3};

    RHIQueue* m_GraphicsQueue {nullptr};
    RHIQueue* m_ComputeQueue {nullptr};

    RHIFormat m_SwapchainImageFormat {RHI_FORMAT_UNDEFINED};
    std::vector<RHIImageView*> m_SwapchainImageviews;
    RHIExtent2D m_SwapchainExtent;

    // Multi-viewport support
    RHIRect2D m_Scissors[2];
    std::string m_ActiveViewportId {"default"};
    std::map<std::string, ViewportRenderTexture> m_ViewportRenderTextures;

    RHIFormat m_DepthImageFormat {RHI_FORMAT_UNDEFINED};
    RHIImageView* m_DepthImageView = new VulkanImageView();

    RHIFence* m_RhiIsFrameInFlightFences[k_max_frames_in_flight];

    RHIDescriptorPool* m_DescriptorPool = new VulkanDescriptorPool();

    RHICommandPool* m_RhiCommandPool;

    RHICommandBuffer* m_CommandBuffers[k_max_frames_in_flight];
    RHICommandBuffer* m_CurrentCommandBuffer = new VulkanCommandBuffer();

    QueueFamilyIndices m_QueueIndices;

    VkInstance m_Instance {nullptr};
    VkSurfaceKHR m_Surface {nullptr};
    VkPhysicalDevice m_PhysicalDevice {nullptr};
    VkDevice m_Device {nullptr};
    VkQueue m_PresentQueue {nullptr};

    VkSwapchainKHR m_Swapchain {nullptr};
    std::vector<VkImage> m_SwapchainImages;

    RHIImage* m_DepthImage = new VulkanImage();
    VkDeviceMemory m_DepthImageMemory {nullptr};

    std::vector<VkFramebuffer> m_SwapchainFramebuffers;

    // asset allocator use VMA library
    VmaAllocator m_AssetsAllocator;

    // function pointers
    PFN_vkCmdBeginDebugUtilsLabelEXT _vkCmdBeginDebugUtilsLabelEXT;
    PFN_vkCmdEndDebugUtilsLabelEXT _vkCmdEndDebugUtilsLabelEXT;
    PFN_vkWaitForFences _vkWaitForFences;
    PFN_vkResetFences _vkResetFences;
    PFN_vkResetCommandPool _vkResetCommandPool;
    PFN_vkBeginCommandBuffer _vkBeginCommandBuffer;
    PFN_vkEndCommandBuffer _vkEndCommandBuffer;
    PFN_vkCmdBeginRenderPass _vkCmdBeginRenderPass;
    PFN_vkCmdNextSubpass _vkCmdNextSubpass;
    PFN_vkCmdEndRenderPass _vkCmdEndRenderPass;
    PFN_vkCmdBindPipeline _vkCmdBindPipeline;
    PFN_vkCmdSetScissor _vkCmdSetScissor;
    PFN_vkCmdBindVertexBuffers _vkCmdBindVertexBuffers;
    PFN_vkCmdBindIndexBuffer _vkCmdBindIndexBuffer;
    PFN_vkCmdBindDescriptorSets _vkCmdBindDescriptorSets;
    PFN_vkCmdDrawIndexed _vkCmdDrawIndexed;
    PFN_vkCmdClearAttachments _vkCmdClearAttachments;

    // global descriptor pool
    VkDescriptorPool m_VkDescriptorPool;

    // command pool and buffers
    uint8_t m_CurrentFrameIndex {0};
    VkCommandPool m_CommandPools[k_max_frames_in_flight];
    VkCommandBuffer m_VkCommandBuffers[k_max_frames_in_flight];
    VkSemaphore m_ImageAvailableForRenderSemaphores[k_max_frames_in_flight];
    VkSemaphore m_ImageFinishedForPresentationSemaphores[k_max_frames_in_flight];
    RHISemaphore* m_ImageAvailableForTexturescopySemaphores[k_max_frames_in_flight];
    VkFence m_IsFrameInFlightFences[k_max_frames_in_flight];

    // TODO: set
    VkCommandBuffer m_VkCurrentCommandBuffer;

    uint32_t m_CurrentSwapchainImageIndex;

private:
    const std::vector<char const*> m_ValidationLayers {"VK_LAYER_KHRONOS_validation"};
    uint32_t m_VulkanApiVersion {VK_API_VERSION_1_0};

    // Runtime shader compiler
    std::unique_ptr<ShaderCompiler> m_ShaderCompiler;

    std::vector<char const*> m_DeviceExtensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

    // -----------------------------------------------------------------
    // Bindless / descriptor-indexing runtime capability.
    // Android / HarmonyOS devices may or may not expose
    // VK_EXT_descriptor_indexing. We probe at device-init time and only
    // promote the extension to "enabled" when both the extension and the
    // required feature bits are present. Higher-level code reads these
    // flags via supportsBindlessTextures() and degrades gracefully on
    // legacy hardware.
    // -----------------------------------------------------------------
    bool m_DescriptorIndexingExtensionAvailable = false;
    bool m_BindlessSupported = false;
    uint32_t m_MaxBindlessSampledImages = 0;
    uint32_t m_MaxBindlessStorageBuffers = 0;

    // PR3: owns the global bindless descriptor table. Constructed in
    // Initialize() iff m_BindlessSupported is true; reset in clear().
    std::unique_ptr<VulkanBindlessTextureManager> m_BindlessTextureManager;

    // default sampler cache
    RHISampler* m_LinearSampler = nullptr;
    RHISampler* m_NearestSampler = nullptr;
    std::map<uint32_t, RHISampler*> m_MipmapSamplerMap;

    void CreateInstance();
    void InitializeDebugMessenger();
    void CreateWindowSurface();
    void InitializePhysicalDevice();
    void CreateLogicalDevice();
    void CreateCommandPool() override;
    void CreateCommandBuffers();
    void CreateDescriptorPool();
    void CreateSyncPrimitives();
    void CreateAssetAllocator();

public:
    bool IsPointLightShadowEnabled() override;

private:
    bool m_EnableValidationLayers {true};
    bool m_EnableDebugUtilsLabel {true};
    bool m_EnablePointLightShadow {true};

    // used in descriptor pool creation
    uint32_t m_MaxVertexBlendingMeshCount {256};
    uint32_t m_MaxMaterialCount {256};

    bool CheckValidationLayerSupport();
    std::vector<const char*> GetRequiredExtensions();
    void PopulateDebugMessengerCreateInfo(VkDebugUtilsMessengerCreateInfoEXT& createInfo);

    VkDebugUtilsMessengerEXT m_DebugMessenger = nullptr;
    VkResult CreateDebugUtilsMessengerEXT(VkInstance instance,
                                          const VkDebugUtilsMessengerCreateInfoEXT* pCreateInfo,
                                          const VkAllocationCallbacks* pAllocator,
                                          VkDebugUtilsMessengerEXT* pDebugMessenger);
    void DestroyDebugUtilsMessengerEXT(VkInstance instance,
                                       VkDebugUtilsMessengerEXT debugMessenger,
                                       const VkAllocationCallbacks* pAllocator);

    QueueFamilyIndices FindQueueFamilies(VkPhysicalDevice physical_device);
    bool CheckDeviceExtensionSupport(VkPhysicalDevice physical_device);
    bool IsDeviceSuitable(VkPhysicalDevice physical_device);
    SwapChainSupportDetails QuerySwapChainSupport(VkPhysicalDevice physical_device);

    VkFormat FindDepthFormat();
    VkFormat
    FindSupportedFormat(const std::vector<VkFormat>& candidates, VkImageTiling tiling, VkFormatFeatureFlags features);

    VkSurfaceFormatKHR
    ChooseSwapchainSurfaceFormatFromDetails(const std::vector<VkSurfaceFormatKHR>& available_surface_formats);
    VkPresentModeKHR
    ChooseSwapchainPresentModeFromDetails(const std::vector<VkPresentModeKHR>& available_present_modes);
    VkExtent2D ChooseSwapchainExtentFromDetails(const VkSurfaceCapabilitiesKHR& capabilities);
};