// Shadow VulkanRHI.h for the headless WASM VT harness.
//
// WHY: the three VirtualTexture *.cpp files hard-#include
// "Runtime/Function/Render/Interface/Vulkan/VulkanRHI.h" at the top. That real
// header unconditionally pulls <vulkan/vulkan.h> + <vma/vk_mem_alloc.h> (and the
// sibling VulkanBindlessTextureManager.h / VulkanRHIResource.h which do the
// same) at file scope, and those are not available under Emscripten. This
// shadowed copy provides EXACTLY the Vulkan/vma surface those headers name --
// nothing more -- and a concrete VulkanRHI subclass, so the streaming
// bookkeeping compiles and runs under node with no device. It is NEVER linked
// into the real engine; it lives only in scripts/vt_wasm_shadow/ and is placed
// on the include path BEFORE engine/Source.
//
// The real PhysicalTexturePool::Initialize() Vulkan body is compiled OUT on
// Emscripten (Z_HAS_VULKAN undefined -> VirtualTextureTypes.h takes the void*
// VmaAllocation branch; the harness uses InitializeLogical() and never enters
// the Vulkan body). BUT the pool's #else branch still does
// `static_cast<VulkanRHI*>(rhi)` and calls getVmaAllocator(), so this shadow's
// VulkanRHI must be a COMPLETE RHI subclass -- every pure virtual overridden
// with a trivial stub. The harness never calls any of them.

#pragma once

#include "Runtime/Function/Render/Interface/RHI.h"

#include <cstdint>

// --- Minimal Vulkan scalar/type stubs (only what the VT pool + RHI base name) ---
using VkResult = int32_t;
constexpr VkResult VK_SUCCESS = 0;

enum VkImageType : int32_t { VK_IMAGE_TYPE_2D = 0 };
enum VkImageTiling : int32_t { VK_IMAGE_TILING_OPTIMAL = 0 };
enum VkImageLayout : uint32_t { VK_IMAGE_LAYOUT_UNDEFINED = 0 };
// The pool's #else body ORs two constants and assigns the result to
// vk_image_info.usage, so the type must be uint32_t-assignable (a scoped enum
// would reject the raw OR). Expose the two bits as constexpr and alias the type.
constexpr uint32_t VK_IMAGE_USAGE_SAMPLED_BIT = 0x00000004;
constexpr uint32_t VK_IMAGE_USAGE_TRANSFER_DST_BIT = 0x00000040;
using VkImageUsageFlagBits = uint32_t;
enum VkSampleCountFlagBits : uint32_t { VK_SAMPLE_COUNT_1_BIT = 0x00000001 };
using VkFormat = int32_t;

struct VkExtent2D { uint32_t width = 0; uint32_t height = 0; };
struct VkExtent3D { uint32_t width = 0; uint32_t height = 0; uint32_t depth = 0; };

struct VkImageCreateInfo {
    int32_t sType = 0;
    const void* pNext = nullptr;
    VkImageType imageType = VK_IMAGE_TYPE_2D;
    VkFormat format = 0;
    VkExtent3D extent;
    uint32_t mipLevels = 0;
    uint32_t arrayLayers = 0;
    VkImageTiling tiling = VK_IMAGE_TILING_OPTIMAL;
    VkImageLayout initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageUsageFlagBits usage = static_cast<VkImageUsageFlagBits>(0);
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    uint32_t flags = 0;
};

// --- Minimal VMA stubs ---
using VmaAllocator = void*;
// On Emscripten VirtualTextureTypes.h defines `using VmaAllocation = void*;`
// (the __APPLE__/__EMSCRIPTEN__ branch). Match it exactly so there is no
// conflicting redefinition when both headers are in the same TU.
using VmaAllocation = void*;

struct VmaAllocationCreateInfo {
    uint32_t flags = 0;
    VmaAllocator owner = nullptr;
    void* pNext = nullptr;
    // The pool's #else body sets alloc_info.usage / alloc_info.flags and reads
    // nothing back, so these two fields (plus the constants below) are all the
    // VMA surface the harness actually needs to compile.
    uint32_t usage = 0;
};
struct VmaAllocationInfo {
    VmaAllocator dedicatedMemory = nullptr;
    void* pNext = nullptr;
};

// VMA constants used by the pool's #else body (values irrelevant to the harness).
constexpr uint32_t VMA_MEMORY_USAGE_AUTO = 0;
constexpr uint32_t VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT = 0x00000002;

// VkStructureType is only ever assigned (never read) in the pool's #else body.
enum VkStructureType : int32_t { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO = 0 };

// --- Opaque Vulkan handles (only named; never dereferenced by the harness) ---
using VkImage = void*;
using VkBuffer = void*;
using VkBufferView = void*;
using VkCommandBuffer = void*;
using VkCommandPool = void*;
using VkDescriptorPool = void*;
using VkDescriptorSet = void*;
using VkDescriptorSetLayout = void*;
using VkDevice = void*;
using VkDeviceMemory = void*;
using VkEvent = void*;
using VkFence = void*;
using VkFramebuffer = void*;
using VkImageView = void*;
using VkInstance = void*;
using VkPhysicalDevice = void*;
using VkPipeline = void*;
using VkPipelineCache = void*;
using VkPipelineLayout = void*;
using VkQueue = void*;
using VkRenderPass = void*;
using VkSampler = void*;
using VkSemaphore = void*;
using VkShaderModule = void*;
using VkSurfaceKHR = void*;
using VkSwapchainKHR = void*;
using VkAllocationCallbacks = void;

// Defined out of line in the (shadowed) pool TU; the harness never calls these.
// (Distinct param names: the base RHI never declares these, they only need to
// satisfy the pool's #else body which references vmaCreateImage/vmaDestroyImage.)
VkResult vmaCreateImage(VmaAllocator allocator, const VkImageCreateInfo* pCreateInfo,
                        const VmaAllocationCreateInfo* pAllocCreateInfo, VkImage* pImage,
                        VmaAllocation* pAllocation, VmaAllocationInfo* pAllocInfo);
void vmaDestroyImage(VmaAllocator allocator, VkImage image, VmaAllocation allocation);

// --- Concrete VulkanRHI stub: a complete RHI subclass ---------------------
// Every pure virtual from RHI/IEngineSystem is overridden with a trivial body
// (false / nullptr / no-op). The harness uses InitializeLogical(), so none of
// these are ever invoked; they only exist so `static_cast<VulkanRHI*>(rhi)` in
// the pool's #else branch is well-formed.
class VulkanRHI final : public RHI
{
public:
    std::string GetName() const override { return "ShadowVulkanRHI"; }
    SystemInitPhase GetInitPhase() const override { return SystemInitPhase::Rendering; }
    bool Initialize() override { return true; }
    void Shutdown() override {}
    ~VulkanRHI() override = default;

    VmaAllocator getVmaAllocator() const { return m_Allocator; }

    void PrepareContext() override {}
    GraphicsAPI getGraphicsAPI() const override { return GraphicsAPI::Vulkan; }
    bool IsPointLightShadowEnabled() override { return false; }
    bool AllocateCommandBuffers(const RHICommandBufferAllocateInfo*, RHICommandBuffer*&) override { return false; }
    bool AllocateDescriptorSets(const RHIDescriptorSetAllocateInfo*, RHIDescriptorSet*&) override { return false; }
    void CreateSwapchain() override {}
    void RecreateSwapchain() override {}
    void NotifyWindowFocusGained() override {}
    void CreateSwapchainImageViews() override {}
    void CreateFramebufferImageAndView() override {}
    RHISampler* GetOrCreateDefaultSampler(RHIDefaultSamplerType) override { return nullptr; }
    RHISampler* GetOrCreateMipmapSampler(uint32_t, uint32_t) override { return nullptr; }
    RHIShader* CreateShaderModule(const std::vector<unsigned char>&) override { return nullptr; }
    RHIShader* CreateShaderModuleFromFile(const std::string&, ShaderStage, const std::vector<std::string>&,
                                          const ShaderMacros&, std::vector<uint8_t>&, const std::string&,
                                          bool) override { return nullptr; }
    RHIShader* CreateShaderModuleFromSource(const std::string&, ShaderStage, const std::string&,
                                            const std::vector<std::string>&, const ShaderMacros&) override {
        return nullptr;
    }
    void CreateBuffer(RHIDeviceSize, RHIBufferUsageFlags, RHIMemoryPropertyFlags, RHIBuffer*&,
                      RHIDeviceMemory*&) override {}
    void CreateBufferAndInitialize(RHIBufferUsageFlags, RHIMemoryPropertyFlags, RHIBuffer*&, RHIDeviceMemory*&,
                                   RHIDeviceSize, void*, int) override {}
    void CopyBuffer(RHIBuffer*, RHIBuffer*, RHIDeviceSize, RHIDeviceSize, RHIDeviceSize) override {}
    void CreateImage(uint32_t, uint32_t, RHIFormat, RHIImageTiling, RHIImageUsageFlags, RHIMemoryPropertyFlags,
                     RHIImage*&, RHIDeviceMemory*&, RHIImageCreateFlags, uint32_t, uint32_t) override {}
    void CreateImageView(RHIImage*, RHIFormat, RHIImageAspectFlags, RHIImageViewType, uint32_t, uint32_t,
                         RHIImageView*&) override {}
    void CreateGlobalImage(RHIImage*&, RHIImageView*&, void*, uint32_t, uint32_t, void*, RHIFormat,
                           uint32_t) override {}
    void CreateCubeMap(RHIImage*&, RHIImageView*&, void*, uint32_t, uint32_t, std::array<void*, 6>, RHIFormat,
                       uint32_t) override {}
    void CreateCommandPool() override {}
    bool CreateCommandPool(const RHICommandPoolCreateInfo*, RHICommandPool*&) override { return false; }
    bool CreateDescriptorPool(const RHIDescriptorPoolCreateInfo*, RHIDescriptorPool*&) override { return false; }
    bool CreateDescriptorSetLayout(const RHIDescriptorSetLayoutCreateInfo*, RHIDescriptorSetLayout*&) override {
        return false;
    }
    bool CreateFence(const RHIFenceCreateInfo*, RHIFence*&) override { return false; }
    bool CreateFramebuffer(const RHIFramebufferCreateInfo*, RHIFramebuffer*&) override { return false; }
    bool CreateGraphicsPipelines(RHIPipelineCache*, uint32_t, const RHIGraphicsPipelineCreateInfo*,
                                 RHIPipeline*&) override { return false; }
    bool CreateComputePipelines(RHIPipelineCache*, uint32_t, const RHIComputePipelineCreateInfo*,
                                RHIPipeline*&) override { return false; }
    bool CreatePipelineLayout(const RHIPipelineLayoutCreateInfo*, RHIPipelineLayout*&) override { return false; }
    bool CreateRenderPass(const RHIRenderPassCreateInfo*, RHIRenderPass*&) override { return false; }
    void DestroyRenderPass(RHIRenderPass*) override {}
    bool CreateSampler(const RHISamplerCreateInfo*, RHISampler*&) override { return false; }
    bool CreateSemaphore(const RHISemaphoreCreateInfo*, RHISemaphore*&) override { return false; }
    bool WaitForFencesPFN(uint32_t, RHIFence* const*, RHIBool32, uint64_t) override { return false; }
    bool ResetFencesPFN(uint32_t, RHIFence* const*) override { return false; }
    bool ResetCommandPoolPFN(RHICommandPool*, RHICommandPoolResetFlags) override { return false; }
    bool BeginCommandBufferPFN(RHICommandBuffer*, const RHICommandBufferBeginInfo*) override { return false; }
    bool EndCommandBufferPFN(RHICommandBuffer*) override { return false; }
    void CmdBeginRenderPassPFN(RHICommandBuffer*, const RHIRenderPassBeginInfo*, RHISubpassContents) override {}
    void CmdNextSubpassPFN(RHICommandBuffer*, RHISubpassContents) override {}
    void CmdEndRenderPassPFN(RHICommandBuffer*) override {}
    void CmdBindPipelinePFN(RHICommandBuffer*, RHIPipelineBindPoint, RHIPipeline*) override {}
    void CmdSetViewportPFN(RHICommandBuffer*, uint32_t, uint32_t, const RHIViewport*) override {}
    void CmdSetScissorPFN(RHICommandBuffer*, uint32_t, uint32_t, const RHIRect2D*) override {}
    void CmdBindVertexBuffersPFN(RHICommandBuffer*, uint32_t, uint32_t, RHIBuffer* const*,
                                 const RHIDeviceSize*) override {}
    void CmdBindIndexBufferPFN(RHICommandBuffer*, RHIBuffer*, RHIDeviceSize, RHIIndexType) override {}
    void CmdBindDescriptorSetsPFN(RHICommandBuffer*, RHIPipelineBindPoint, RHIPipelineLayout*, uint32_t,
                                  uint32_t, const RHIDescriptorSet* const*, uint32_t, const uint32_t*) override {}
    void CmdDrawIndexedPFN(RHICommandBuffer*, uint32_t, uint32_t, uint32_t, int32_t, uint32_t) override {}
    void CmdDrawIndexedIndirect(RHICommandBuffer*, RHIBuffer*, RHIDeviceSize, uint32_t, uint32_t) override {}
    void CmdDrawIndirect(RHICommandBuffer*, RHIBuffer*, RHIDeviceSize, uint32_t, uint32_t) override {}
    void CmdClearAttachmentsPFN(RHICommandBuffer*, uint32_t, const RHIClearAttachment*, uint32_t,
                                const RHIClearRect*) override {}
    bool BeginCommandBuffer(RHICommandBuffer*, const RHICommandBufferBeginInfo*) override { return false; }
    void CmdCopyImageToBuffer(RHICommandBuffer*, RHIImage*, RHIImageLayout, RHIBuffer*, uint32_t,
                              const RHIBufferImageCopy*) override {}
    void CmdCopyImageToImage(RHICommandBuffer*, RHIImage*, RHIImageAspectFlagBits, RHIImage*,
                             RHIImageAspectFlagBits, uint32_t, uint32_t) override {}
    void CmdBlitImage(RHICommandBuffer*, RHIImage*, RHIImageLayout, RHIImage*, RHIImageLayout, uint32_t,
                      uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, RHIFilter) override {}
    void CmdCopyBuffer(RHICommandBuffer*, RHIBuffer*, RHIBuffer*, uint32_t, RHIBufferCopy*) override {}
    void CmdDraw(RHICommandBuffer*, uint32_t, uint32_t, uint32_t, uint32_t) override {}
    void CmdDispatch(RHICommandBuffer*, uint32_t, uint32_t, uint32_t) override {}
    void CmdDispatchIndirect(RHICommandBuffer*, RHIBuffer*, RHIDeviceSize) override {}
    void CmdPipelineBarrier(RHICommandBuffer*, RHIPipelineStageFlags, RHIPipelineStageFlags, RHIDependencyFlags,
                            uint32_t, const RHIMemoryBarrier*, uint32_t, const RHIBufferMemoryBarrier*,
                            uint32_t, const RHIImageMemoryBarrier*) override {}
    bool EndCommandBuffer(RHICommandBuffer*) override { return false; }
    void UpdateDescriptorSets(uint32_t, const RHIWriteDescriptorSet*, uint32_t, const RHICopyDescriptorSet*)
        override {}
    bool QueueSubmit(RHIQueue*, uint32_t, const RHISubmitInfo*, RHIFence*) override { return false; }
    bool QueueWaitIdle(RHIQueue*) override { return false; }
    void ResetCommandPool() override {}
    void WaitForFences() override {}
    void GetPhysicalDeviceProperties(RHIPhysicalDeviceProperties*) override {}
    RHICommandBuffer* GetCurrentCommandBuffer() const override { return nullptr; }
    RHICommandBuffer* const* GetCommandBufferList() const override { return nullptr; }
    RHICommandPool* GetCommandPoor() const override { return nullptr; }
    RHIDescriptorPool* GetDescriptorPoor() const override { return nullptr; }
    RHIFence* const* GetFenceList() const override { return nullptr; }
    QueueFamilyIndices GetQueueFamilyIndices() const override { return {}; }
    RHIQueue* GetGraphicsQueue() const override { return nullptr; }
    RHIQueue* GetComputeQueue() const override { return nullptr; }
    RHISwapChainDesc GetSwapchainInfo() override { return {}; }
    RHIDepthImageDesc GetDepthImageInfo() const override { return {}; }
    uint8_t GetMaxFramesInFlight() const override { return 0; }
    uint8_t GetCurrentFrameIndex() const override { return 0; }
    void SetCurrentFrameIndex(uint8_t) override {}
    RHICommandBuffer* BeginSingleTimeCommands() override { return nullptr; }
    void EndSingleTimeCommands(RHICommandBuffer*) override {}
    bool PrepareBeforePass(std::function<void()>) override { return false; }
    void SubmitRendering(std::function<void()>) override {}
    void PushEvent(RHICommandBuffer*, const char*, const float*) override {}
    void PopEvent(RHICommandBuffer*) override {}
    void clear() override {}
    void ClearSwapchain() override {}
    void DestroyDefaultSampler(RHIDefaultSamplerType) override {}
    void DestroyMipmappedSampler() override {}
    void DestroyShaderModule(RHIShader*) override {}
    void DestroySemaphore(RHISemaphore*) override {}
    void DestroySampler(RHISampler*) override {}
    void DestroyInstance(RHIInstance*) override {}
    void DestroyImageView(RHIImageView*) override {}
    void DestroyImage(RHIImage*) override {}
    void DestroyFramebuffer(RHIFramebuffer*) override {}
    void DestroyFence(RHIFence*) override {}
    void DestroyDevice() override {}
    void DestroyCommandPool(RHICommandPool*) override {}
    void DestroyBuffer(RHIBuffer*&) override {}
    void FreeCommandBuffers(RHICommandPool*, uint32_t, RHICommandBuffer*) override {}
    void FreeMemory(RHIDeviceMemory*&) override {}
    bool MapMemory(RHIDeviceMemory*, RHIDeviceSize, RHIDeviceSize, RHIMemoryMapFlags, void** ppData) override {
        if (ppData) *ppData = nullptr;
        return false;
    }
    void UnmapMemory(RHIDeviceMemory*) override {}
    void InvalidateMappedMemoryRanges(void*, RHIDeviceMemory*, RHIDeviceSize, RHIDeviceSize) override {}
    void FlushMappedMemoryRanges(void*, RHIDeviceMemory*, RHIDeviceSize, RHIDeviceSize) override {}
    RHISemaphore*& GetTextureCopySemaphore(uint32_t) override { return m_NullSemaphoreRef; }
    void RegisterViewport(int, const RHIViewport&) override {}
    void UpdateViewport(ViewportType, const RHIViewport&) override {}
    RHIViewport* GetViewport(ViewportType) override { return nullptr; }
    void CreateViewportRenderTexture(const std::string&, uint32_t, uint32_t) override {}
    void UpdateViewportRenderTexture(const std::string&, uint32_t, uint32_t) override {}
    void DestroyViewportRenderTexture(const std::string&) override {}
    ViewportRenderTexture* GetViewportRenderTexture(const std::string&) override { return nullptr; }

private:
    VmaAllocator m_Allocator = nullptr;
    RHISemaphore* m_NullSemaphoreRef = nullptr;
};
