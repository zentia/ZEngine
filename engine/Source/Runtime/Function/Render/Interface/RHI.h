#pragma once

#include "RHIStruct.h"
#include "Runtime/Core/Base/EngineSystem.h"
#include "Runtime/Function/Render/RenderType.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

// Forward declarations for platform-specific types
// These will be conditionally included in implementation files
#if defined(Z_HAS_VULKAN)
    #define GLFW_INCLUDE_VULKAN
    #include <GLFW/glfw3.h>
    #include <vma/vk_mem_alloc.h>
#endif

class WindowSystem;

// =====================================================================
// Bindless texture manager (PR3)
// ---------------------------------------------------------------------
// Abstract interface that owns a single, large, GPU-visible descriptor
// table of sampled-image+sampler slots. Higher-level material / texture
// systems request a slot via allocate(), receive a stable uint32_t
// "bindless index", and embed that index in shader constants. The
// shader then samples through:
//
//   Vulkan GLSL : texture(g_BindlessTextures[nonuniformEXT(idx)], uv)
//   DX12   HLSL : ResourceDescriptorHeap[NonUniformResourceIndex(idx)].Sample(...)
//
// Backend mapping:
//   - Vulkan : single VkDescriptorSet with one VK_DESCRIPTOR_TYPE_
//              COMBINED_IMAGE_SAMPLER binding sized to capacity(),
//              created with UPDATE_AFTER_BIND + PARTIALLY_BOUND +
//              VARIABLE_DESCRIPTOR_COUNT. Requires
//              VK_EXT_descriptor_indexing (probed in PR2).
//   - DX12   : reserved range inside the global CBV/SRV/UAV heap,
//              accessed via SM 6.6 ResourceDescriptorHeap (no root-
//              signature table entry). Implementation lands in PR4.
//
// Lifecycle:
//   - Owned by the RHI; its lifetime equals the RHI's. Never assume
//     non-null on backends that do not support bindless: callers must
//     guard on RHI::supportsBindlessTextures() before dereferencing
//     RHI::getBindlessTextureManager().
//   - Slot 0 is reserved for a "default white" / "missing texture"
//     placeholder so that uninitialized indices never sample garbage.
// =====================================================================
class RHIBindlessTextureManager
{
public:
    static constexpr uint32_t kInvalidBindlessIndex = 0xFFFFFFFFu;

    virtual ~RHIBindlessTextureManager() = default;

    // Number of descriptor slots provisioned in the global table.
    // Equals the cap chosen at RHI init time, clamped against the
    // driver-reported maxPerStageDescriptorUpdateAfterBindSampledImages.
    virtual uint32_t capacity() const = 0;

    // Reserve a slot and write (image_view, sampler) into it. Returns
    // a stable index in [0, capacity()) on success, or
    // kInvalidBindlessIndex if the table is full or the manager is
    // not in a usable state. Thread-safety: implementations MUST be
    // safe to call from any thread that is not currently recording
    // GPU commands that read the table; the underlying Vulkan write
    // is UPDATE_AFTER_BIND so concurrent read by an in-flight command
    // buffer is permitted, but shaders that already sampled a stale
    // slot are not retroactively patched.
    virtual uint32_t allocate(RHIImageView* image_view, RHISampler* sampler) = 0;

    // Release a previously-allocated slot back to the free list. The
    // descriptor entry is left as "stale" until the slot is re-used
    // (PARTIALLY_BOUND lets us skip rewriting). Passing
    // kInvalidBindlessIndex or an out-of-range index is a no-op.
    virtual void free(uint32_t index) = 0;

    // Update the (image_view, sampler) pair at an existing slot
    // without changing the index. Used by streaming systems that
    // swap mip pyramids in place.
    virtual void Update(uint32_t index, RHIImageView* image_view, RHISampler* sampler) = 0;
};

// =====================================================================
// Bindless index packing helper (PR6 follow-up)
// ---------------------------------------------------------------------
// Engine-wide convention for delivering a bindless draw's
// (texture-table-index, sampler-array-slot) pair through the single
// 32-bit root constant that DX12RHI::CreatePipelineLayout reserves at
// b0/space0 (and that future Vulkan/Metal backends will deliver via
// push-constants). Layout:
//
//     bits  [ 0..15] : texture_index   -- slot in the global bindless
//                                         CBV/SRV/UAV heap (the table
//                                         owned by RHIBindlessTextureManager).
//     bits  [16..31] : sampler_index   -- index into the per-pipeline
//                                         static-sampler array baked
//                                         into the root signature.
//
// Why two uint16s and not a wider split:
//   - 65 535 simultaneously live texture slots is several orders of
//     magnitude above any ZEngine demo's working set (the default cap
//     today is 16 384, see DX12RHI::supportsBindlessTextures), and
//     stays comfortably below every D3D12 Resource Binding Tier 2
//     hardware limit and every Vulkan driver's reported
//     maxPerStageDescriptorUpdateAfterBindSampledImages.
//   - 65 536 distinct sampler permutations is likewise far past any
//     realistic shader's needs; current shaders use 4 (linear/point ×
//     wrap/clamp) and the ceiling is set by D3D12's hard cap of
//     2 032 static samplers per root signature anyway.
//   - Keeping the split symmetric (uint16:uint16) makes the HLSL
//     unpack code a single AND/SHIFT pair on every DXC target and
//     spares us from per-platform pack format negotiation when the
//     Vulkan path comes online.
//
// This helper is the SINGLE SOURCE OF TRUTH for the format. Both the
// engine (RHI::CmdSetBindlessIndexPFN call sites, material systems)
// and the shader-side unpack code (see e.g.
// engine/source/Runtime/Function/Render/Interface/dx12/test/
// bindless_smoke.hlsl, lines `texture_index = g_packed_indices &
// 0xFFFFu;`) MUST use these constants -- the smoke-test asserts
// round-trip equivalence to catch any drift.
//
// Sampler-bindless (SAMPLER_HEAP_DIRECTLY_INDEXED + a sampler
// descriptor heap) is intentionally NOT modeled here -- the high half
// is a static-sampler-array slot, not a heap index. AGENTS.md §2.9
// item 3 explains the deferral.
// =====================================================================
namespace BindlessIndex
{
    // Width of each half. Changing these is a breaking change; bump the
    // constants and update bindless_smoke.hlsl + every consumer.
    inline constexpr uint32_t kTextureIndexBits = 16;
    inline constexpr uint32_t kSamplerIndexBits = 16;
    static_assert(kTextureIndexBits + kSamplerIndexBits == 32,
                  "BindlessIndex packed layout must fully consume the 32-bit root constant.");

    inline constexpr uint32_t kTextureIndexMask = (1u << kTextureIndexBits) - 1u;
    inline constexpr uint32_t kSamplerIndexMask = (1u << kSamplerIndexBits) - 1u;

    // Largest representable index in each half (NOT capacities -- those
    // are queried per-RHI via supportsBindlessTextures /
    // maxBindlessSampledImages and may be smaller). Useful as a debug
    // upper bound when validating allocator returns.
    inline constexpr uint32_t kMaxTextureIndex = kTextureIndexMask;
    inline constexpr uint32_t kMaxSamplerIndex = kSamplerIndexMask;

    // Pack (texture, sampler) into the 32-bit payload pushed through
    // RHI::cmdSetBindlessIndexPFN. Out-of-range halves are silently
    // truncated to their N-bit window; callers that care must bounds-
    // check upstream (the validation cost on the draw hot path is not
    // worth paying every frame, and the smoke-test plus debug builds'
    // asserts elsewhere catch drift early).
    [[nodiscard]] inline constexpr uint32_t Pack(uint32_t texture_index, uint32_t sampler_index) noexcept
    {
        return (texture_index & kTextureIndexMask) |
               ((sampler_index & kSamplerIndexMask) << kTextureIndexBits);
    }

    [[nodiscard]] inline constexpr uint32_t UnpackTexture(uint32_t packed) noexcept
    {
        return packed & kTextureIndexMask;
    }

    [[nodiscard]] inline constexpr uint32_t UnpackSampler(uint32_t packed) noexcept
    {
        return (packed >> kTextureIndexBits) & kSamplerIndexMask;
    }
}  // namespace BindlessIndex

struct RHIInitInfo
{
    WindowSystem* window_system;
    GraphicsAPI api = GraphicsAPI::Vulkan;  // Default to Vulkan for backward compatibility
};

class RHI : public IEngineSystem
{
public:
    virtual ~RHI() = 0;
    virtual void PrepareContext() = 0;

    // Get the graphics API this RHI implementation uses
    virtual GraphicsAPI getGraphicsAPI() const = 0;

    virtual bool IsPointLightShadowEnabled() = 0;
    // allocate and create
    virtual bool AllocateCommandBuffers(const RHICommandBufferAllocateInfo* pAllocateInfo,
                                        RHICommandBuffer*& pCommandBuffers) = 0;
    virtual bool AllocateDescriptorSets(const RHIDescriptorSetAllocateInfo* pAllocateInfo,
                                        RHIDescriptorSet*& pDescriptorSets) = 0;
    virtual void CreateSwapchain() = 0;
    virtual void RecreateSwapchain() = 0;
    // Called by the editor when the OS window regains input focus (Alt-Tab back,
    // etc.). For DX12 this flags the swapchain as stale so PrepareBeforePass
    // forces a full recreate — recovering from DXGI occlusion / TDR / driver
    // internal reset that can leave a FLIP-model swapchain presenting blank.
    // Vulkan already recovers reactively (VK_ERROR_OUT_OF_DATE_KHR), so its
    // implementation is typically a no-op.
    virtual void NotifyWindowFocusGained() = 0;
    virtual void CreateSwapchainImageViews() = 0;
    virtual void CreateFramebufferImageAndView() = 0;
    virtual RHISampler* GetOrCreateDefaultSampler(RHIDefaultSamplerType type) = 0;
    virtual RHISampler* GetOrCreateMipmapSampler(uint32_t width, uint32_t height) = 0;
    virtual RHIShader* CreateShaderModule(const std::vector<unsigned char>& shader_code) = 0;

    // Runtime shader compilation (like Unity/Unreal)
    // Shader macros for variants (e.g., {"ENABLE_SHADOWS", "1"}, {"MAX_LIGHTS", "4"})
    using ShaderMacros = std::map<std::string, std::string>;

    // Compile shader from file path and create shader module.
    // `embed_debug`: if true, emit debug info (-Zi -Qembed_debug for DX12,
    // -g for Vulkan) for tools like PIX / RenderDoc shader debugging.
    virtual RHIShader* CreateShaderModuleFromFile(const std::string& file_path,
                                                  ShaderStage shader_stage,
                                                  const std::vector<std::string>& include_paths,
                                                  const ShaderMacros& macros,
                                                  std::vector<uint8_t>& output_binary,
                                                  const std::string& entry_point = "main",
                                                  bool embed_debug = false) = 0;

    // Compile GLSL shader from source code string and create shader module
    virtual RHIShader* CreateShaderModuleFromSource(const std::string& source_code,
                                                    ShaderStage shader_stage,
                                                    const std::string& shader_name = "",
                                                    const std::vector<std::string>& include_paths = {},
                                                    const ShaderMacros& macros = {}) = 0;
    virtual void CreateBuffer(RHIDeviceSize size,
                              RHIBufferUsageFlags usage,
                              RHIMemoryPropertyFlags properties,
                              RHIBuffer*& buffer,
                              RHIDeviceMemory*& buffer_memory) = 0;
    virtual void CreateBufferAndInitialize(RHIBufferUsageFlags usage,
                                           RHIMemoryPropertyFlags properties,
                                           RHIBuffer*& buffer,
                                           RHIDeviceMemory*& buffer_memory,
                                           RHIDeviceSize size,
                                           void* data = nullptr,
                                           int datasize = 0) = 0;
    // VMA-specific functions (Vulkan only, optional for other APIs)
    // These use void* to avoid including VMA headers in base class
    virtual bool CreateBufferVMA(void* allocator,
                                 const RHIBufferCreateInfo* pBufferCreateInfo,
                                 void* pAllocationCreateInfo,
                                 RHIBuffer*& pBuffer,
                                 void** pAllocation,
                                 void* pAllocationInfo)
    {
        // Default implementation returns false for non-Vulkan APIs
        return false;
    }
    virtual bool CreateBufferWithAlignmentVMA(void* allocator,
                                              const RHIBufferCreateInfo* pBufferCreateInfo,
                                              void* pAllocationCreateInfo,
                                              RHIDeviceSize minAlignment,
                                              RHIBuffer*& pBuffer,
                                              void** pAllocation,
                                              void* pAllocationInfo)
    {
        // Default implementation returns false for non-Vulkan APIs
        return false;
    }
    virtual void CopyBuffer(RHIBuffer* srcBuffer,
                            RHIBuffer* dstBuffer,
                            RHIDeviceSize srcOffset,
                            RHIDeviceSize dstOffset,
                            RHIDeviceSize size) = 0;

    // Force immediate upload via a dedicated command list (synchronous).
    // Default implementation just calls CopyBuffer(); backends that batch
    // CopyBuffer into the frame command list must override this to guarantee
    // the copy is visible to the GPU before the call returns.
    virtual void CopyBufferImmediate(RHIBuffer* srcBuffer,
                                    RHIBuffer* dstBuffer,
                                    RHIDeviceSize srcOffset,
                                    RHIDeviceSize dstOffset,
                                    RHIDeviceSize size)
    {
        CopyBuffer(srcBuffer, dstBuffer, srcOffset, dstOffset, size);
    }
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
                             uint32_t miplevels) = 0;
    virtual void CreateImageView(RHIImage* image,
                                 RHIFormat format,
                                 RHIImageAspectFlags image_aspect_flags,
                                 RHIImageViewType view_type,
                                 uint32_t layout_count,
                                 uint32_t miplevels,
                                 RHIImageView*& image_view) = 0;

    // DX12 editor overlay: sampled-image views created in the legacy
    // CBV/SRV/UAV heap are invisible once the bindless heap is bound.
    // Backends that need this relocate the SRV; default is a no-op.
    virtual void EnsureShaderVisibleImageView(RHIImageView* image_view) {}
    virtual void CreateGlobalImage(RHIImage*& image,
                                   RHIImageView*& image_view,
                                   void* image_allocation,  // Changed from VmaAllocation& to void*&
                                   uint32_t texture_image_width,
                                   uint32_t texture_image_height,
                                   void* texture_image_pixels,
                                   RHIFormat texture_image_format,
                                   uint32_t miplevels = 0) = 0;
    virtual void CreateCubeMap(RHIImage*& image,
                               RHIImageView*& image_view,
                               void* image_allocation,  // Changed from VmaAllocation& to void*&
                               uint32_t texture_image_width,
                               uint32_t texture_image_height,
                               std::array<void*, 6> texture_image_pixels,
                               RHIFormat texture_image_format,
                               uint32_t miplevels) = 0;
    virtual void CreateCommandPool() = 0;
    virtual bool CreateCommandPool(const RHICommandPoolCreateInfo* pCreateInfo, RHICommandPool*& pCommandPool) = 0;
    virtual bool CreateDescriptorPool(const RHIDescriptorPoolCreateInfo* pCreateInfo,
                                      RHIDescriptorPool*& pDescriptorPool) = 0;
    virtual bool CreateDescriptorSetLayout(const RHIDescriptorSetLayoutCreateInfo* pCreateInfo,
                                           RHIDescriptorSetLayout*& pSetLayout) = 0;
    virtual bool CreateFence(const RHIFenceCreateInfo* pCreateInfo, RHIFence*& pFence) = 0;
    virtual bool CreateFramebuffer(const RHIFramebufferCreateInfo* pCreateInfo, RHIFramebuffer*& pFramebuffer) = 0;
    virtual bool CreateGraphicsPipelines(RHIPipelineCache* pipelineCache,
                                         uint32_t createInfoCount,
                                         const RHIGraphicsPipelineCreateInfo* pCreateInfos,
                                         RHIPipeline*& pPipelines) = 0;
    virtual bool CreateComputePipelines(RHIPipelineCache* pipelineCache,
                                        uint32_t createInfoCount,
                                        const RHIComputePipelineCreateInfo* pCreateInfos,
                                        RHIPipeline*& pPipelines) = 0;
    virtual bool CreatePipelineLayout(const RHIPipelineLayoutCreateInfo* pCreateInfo,
                                      RHIPipelineLayout*& pPipelineLayout) = 0;
    virtual bool CreateRenderPass(const RHIRenderPassCreateInfo* pCreateInfo, RHIRenderPass*& pRenderPass) = 0;
    virtual void DestroyRenderPass(RHIRenderPass* renderPass) = 0;
    virtual bool CreateSampler(const RHISamplerCreateInfo* pCreateInfo, RHISampler*& pSampler) = 0;
    virtual bool CreateSemaphore(const RHISemaphoreCreateInfo* pCreateInfo, RHISemaphore*& pSemaphore) = 0;

    // command and command write
    virtual bool
    WaitForFencesPFN(uint32_t fenceCount, RHIFence* const* pFence, RHIBool32 waitAll, uint64_t timeout) = 0;
    virtual bool ResetFencesPFN(uint32_t fenceCount, RHIFence* const* pFences) = 0;
    virtual bool ResetCommandPoolPFN(RHICommandPool* commandPool, RHICommandPoolResetFlags flags) = 0;
    virtual bool BeginCommandBufferPFN(RHICommandBuffer* commandBuffer,
                                       const RHICommandBufferBeginInfo* pBeginInfo) = 0;
    virtual bool EndCommandBufferPFN(RHICommandBuffer* commandBuffer) = 0;
    virtual void CmdBeginRenderPassPFN(RHICommandBuffer* commandBuffer,
                                       const RHIRenderPassBeginInfo* pRenderPassBegin,
                                       RHISubpassContents contents) = 0;
    virtual void CmdNextSubpassPFN(RHICommandBuffer* commandBuffer, RHISubpassContents contents) = 0;
    virtual void CmdEndRenderPassPFN(RHICommandBuffer* commandBuffer) = 0;
    virtual void CmdBindPipelinePFN(RHICommandBuffer* commandBuffer,
                                    RHIPipelineBindPoint pipelineBindPoint,
                                    RHIPipeline* pipeline) = 0;
    virtual void CmdSetViewportPFN(RHICommandBuffer* commandBuffer,
                                   uint32_t firstViewport,
                                   uint32_t viewportCount,
                                   const RHIViewport* pViewports) = 0;
    virtual void CmdSetScissorPFN(RHICommandBuffer* commandBuffer,
                                  uint32_t firstScissor,
                                  uint32_t scissorCount,
                                  const RHIRect2D* pScissors) = 0;
    virtual void CmdBindVertexBuffersPFN(RHICommandBuffer* commandBuffer,
                                         uint32_t firstBinding,
                                         uint32_t bindingCount,
                                         RHIBuffer* const* pBuffers,
                                         const RHIDeviceSize* pOffsets) = 0;
    virtual void CmdBindIndexBufferPFN(RHICommandBuffer* commandBuffer,
                                       RHIBuffer* buffer,
                                       RHIDeviceSize offset,
                                       RHIIndexType indexType) = 0;
    virtual void CmdBindDescriptorSetsPFN(RHICommandBuffer* commandBuffer,
                                          RHIPipelineBindPoint pipelineBindPoint,
                                          RHIPipelineLayout* layout,
                                          uint32_t firstSet,
                                          uint32_t descriptorSetCount,
                                          const RHIDescriptorSet* const* pDescriptorSets,
                                          uint32_t dynamicOffsetCount,
                                          const uint32_t* pDynamicOffsets) = 0;
    // PR6: push a single 32-bit packed bindless index
    // (low 16 bits = texture-table index, high 16 bits = static-sampler
    // slot) into the bindless-aware root signature created by
    // createPipelineLayout when at least one descriptor set declared
    // RHI_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT.
    //
    // - DX12 backend: translates to SetGraphicsRoot32BitConstant /
    //   SetComputeRoot32BitConstant on the root parameter index that
    //   DX12RHI::CreatePipelineLayout reserved at b0/space0.
    // - Vulkan backend (PR-V1): translates to vkCmdPushConstants on a
    //   VK_SHADER_STAGE_ALL / offset=0 / size=4 push-constant range.
    //   Callers must declare a matching range in the
    //   RHIPipelineLayoutCreateInfo::pPushConstantRanges array; the
    //   VulkanBindlessTextureManager exposes a helper to populate it
    //   with the canonical layout. pipelineBindPoint is ignored on
    //   this backend (vkCmdPushConstants is bind-point agnostic).
    // - Metal backend: no-op for now (the bindless index path will be
    //   wired up when the Metal backend grows a bindless manager
    //   equivalent).
    // - WebGL2 backend: **permanently no-op** -- WebGL2 / GLSL ES 3.00
    //   lacks every prerequisite for bindless textures (no
    //   GL_ARB_bindless_texture, no nonuniformEXT, sampler2D arrays
    //   capped at 16-32 units). This backend will never grow a
    //   BindlessTextureManager; renderers must use per-draw descriptor
    //   sets (legacy bindful) instead.
    // Default-implemented as a no-op so adding the API doesn't break those builds; the contract is
    //   "calling this on a non-bindless layout is a silent no-op
    //   everywhere".
    //
    // Not every PFN follows this pattern, but a default empty
    // implementation is the right call here because the hot path that
    // would consume bindless indices is still being rolled out across
    // backends; promoting this to pure-virtual would force the
    // remaining two backends to ship throwaway stubs.
    virtual void CmdSetBindlessIndexPFN(RHICommandBuffer* /*commandBuffer*/,
                                        RHIPipelineBindPoint /*pipelineBindPoint*/,
                                        RHIPipelineLayout* /*layout*/,
                                        uint32_t /*packed_index*/) {}
    virtual void CmdDrawIndexedPFN(RHICommandBuffer* commandBuffer,
                                   uint32_t indexCount,
                                   uint32_t instanceCount,
                                   uint32_t firstIndex,
                                   int32_t vertexOffset,
                                   uint32_t firstInstance) = 0;
    virtual void CmdClearAttachmentsPFN(RHICommandBuffer* commandBuffer,
                                        uint32_t attachmentCount,
                                        const RHIClearAttachment* pAttachments,
                                        uint32_t rectCount,
                                        const RHIClearRect* pRects) = 0;

    virtual bool BeginCommandBuffer(RHICommandBuffer* commandBuffer, const RHICommandBufferBeginInfo* pBeginInfo) = 0;
    virtual void CmdCopyImageToBuffer(RHICommandBuffer* commandBuffer,
                                      RHIImage* srcImage,
                                      RHIImageLayout srcImageLayout,
                                      RHIBuffer* dstBuffer,
                                      uint32_t regionCount,
                                      const RHIBufferImageCopy* pRegions) = 0;
    virtual void CmdCopyImageToImage(RHICommandBuffer* commandBuffer,
                                     RHIImage* srcImage,
                                     RHIImageAspectFlagBits srcFlag,
                                     RHIImage* dstImage,
                                     RHIImageAspectFlagBits dstFlag,
                                     uint32_t width,
                                     uint32_t height) = 0;
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
                              RHIFilter filter) = 0;
    virtual void CmdCopyBuffer(RHICommandBuffer* commandBuffer,
                               RHIBuffer* srcBuffer,
                               RHIBuffer* dstBuffer,
                               uint32_t regionCount,
                               RHIBufferCopy* pRegions) = 0;
    virtual void CmdDraw(RHICommandBuffer* commandBuffer,
                         uint32_t vertexCount,
                         uint32_t instanceCount,
                         uint32_t firstVertex,
                         uint32_t firstInstance) = 0;
    virtual void
    CmdDispatch(RHICommandBuffer* commandBuffer, uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) = 0;
    virtual void CmdDispatchIndirect(RHICommandBuffer* commandBuffer, RHIBuffer* buffer, RHIDeviceSize offset) = 0;
    virtual void CmdPipelineBarrier(RHICommandBuffer* commandBuffer,
                                    RHIPipelineStageFlags srcStageMask,
                                    RHIPipelineStageFlags dstStageMask,
                                    RHIDependencyFlags dependencyFlags,
                                    uint32_t memoryBarrierCount,
                                    const RHIMemoryBarrier* pMemoryBarriers,
                                    uint32_t bufferMemoryBarrierCount,
                                    const RHIBufferMemoryBarrier* pBufferMemoryBarriers,
                                    uint32_t imageMemoryBarrierCount,
                                    const RHIImageMemoryBarrier* pImageMemoryBarriers) = 0;
    virtual bool EndCommandBuffer(RHICommandBuffer* commandBuffer) = 0;
    virtual void UpdateDescriptorSets(uint32_t descriptorWriteCount,
                                      const RHIWriteDescriptorSet* pDescriptorWrites,
                                      uint32_t descriptorCopyCount,
                                      const RHICopyDescriptorSet* pDescriptorCopies) = 0;
    virtual bool QueueSubmit(RHIQueue* queue, uint32_t submitCount, const RHISubmitInfo* pSubmits, RHIFence* fence) = 0;
    virtual bool QueueWaitIdle(RHIQueue* queue) = 0;
    virtual void ResetCommandPool() = 0;
    virtual void WaitForFences() = 0;

    // query
    virtual void GetPhysicalDeviceProperties(RHIPhysicalDeviceProperties* pProperties) = 0;
    virtual RHICommandBuffer* GetCurrentCommandBuffer() const = 0;
    virtual RHICommandBuffer* const* GetCommandBufferList() const = 0;
    virtual RHICommandPool* GetCommandPoor() const = 0;
    virtual RHIDescriptorPool* GetDescriptorPoor() const = 0;
    virtual RHIFence* const* GetFenceList() const = 0;
    virtual QueueFamilyIndices GetQueueFamilyIndices() const = 0;
    virtual RHIQueue* GetGraphicsQueue() const = 0;
    virtual RHIQueue* GetComputeQueue() const = 0;
    virtual RHISwapChainDesc GetSwapchainInfo() = 0;
    virtual RHIDepthImageDesc GetDepthImageInfo() const = 0;
    virtual uint8_t GetMaxFramesInFlight() const = 0;
    virtual uint8_t GetCurrentFrameIndex() const = 0;
    virtual void SetCurrentFrameIndex(uint8_t index) = 0;

    // ---------------------------------------------------------------------
    // Bindless / descriptor-indexing capability queries.
    // Default: backend has no bindless support. Vulkan / DX12 backends
    // override these once the runtime feature query succeeds.
    // ---------------------------------------------------------------------
    virtual bool supportsBindlessTextures() const { return false; }
    virtual uint32_t maxBindlessSampledImages() const { return 0; }
    virtual uint32_t maxBindlessStorageBuffers() const { return 0; }

    // Returns the per-RHI bindless texture manager, or nullptr if the
    // active backend does not support bindless (in which case the
    // caller must fall back to per-material descriptor sets). The
    // pointer is owned by the RHI and stays valid until RHI::clear().
    virtual RHIBindlessTextureManager* getBindlessTextureManager() { return nullptr; }

    // command write
    virtual RHICommandBuffer* BeginSingleTimeCommands() = 0;
    virtual void EndSingleTimeCommands(RHICommandBuffer* command_buffer) = 0;
    virtual bool PrepareBeforePass(std::function<void()> passUpdateAfterRecreateSwapchain) = 0;
    virtual void SubmitRendering(std::function<void()> passUpdateAfterRecreateSwapchain) = 0;
    virtual void PushEvent(RHICommandBuffer* commond_buffer, const char* name, const float* color) = 0;
    virtual void PopEvent(RHICommandBuffer* commond_buffer) = 0;

    // ZEngine Insights GPU timing. Records a matched begin/end timestamp pair on
    // the current frame's command list; the resolved span is fed into the
    // Insights "GPU" timeline track a few frames later (after the GPU completes
    // and the readback fence signals). Scopes may nest. Default no-op so only the
    // backend that implements GPU timestamps (DX12 today) pays anything, and only
    // while InsightsTrace capture is active.
    virtual void BeginGpuTimingScope(const char* /*name*/) {}
    virtual void EndGpuTimingScope() {}

    // destory
    virtual void clear() = 0;
    virtual void ClearSwapchain() = 0;
    virtual void DestroyDefaultSampler(RHIDefaultSamplerType type) = 0;
    virtual void DestroyMipmappedSampler() = 0;
    virtual void DestroyShaderModule(RHIShader* shader) = 0;
    virtual void DestroySemaphore(RHISemaphore* semaphore) = 0;
    virtual void DestroySampler(RHISampler* sampler) = 0;
    virtual void DestroyInstance(RHIInstance* instance) = 0;
    virtual void DestroyImageView(RHIImageView* imageView) = 0;
    virtual void DestroyImage(RHIImage* image) = 0;
    virtual void DestroyFramebuffer(RHIFramebuffer* framebuffer) = 0;
    virtual void DestroyFence(RHIFence* fence) = 0;
    virtual void DestroyDevice() = 0;
    virtual void DestroyCommandPool(RHICommandPool* commandPool) = 0;
    virtual void DestroyBuffer(RHIBuffer*& buffer) = 0;
    virtual void
    FreeCommandBuffers(RHICommandPool* commandPool, uint32_t commandBufferCount, RHICommandBuffer* pCommandBuffers) = 0;

    // memory
    virtual void FreeMemory(RHIDeviceMemory*& memory) = 0;
    virtual bool MapMemory(RHIDeviceMemory* memory,
                           RHIDeviceSize offset,
                           RHIDeviceSize size,
                           RHIMemoryMapFlags flags,
                           void** ppData) = 0;
    virtual void UnmapMemory(RHIDeviceMemory* memory) = 0;
    virtual void
    InvalidateMappedMemoryRanges(void* pNext, RHIDeviceMemory* memory, RHIDeviceSize offset, RHIDeviceSize size) = 0;
    virtual void
    FlushMappedMemoryRanges(void* pNext, RHIDeviceMemory* memory, RHIDeviceSize offset, RHIDeviceSize size) = 0;

    // semaphores
    virtual RHISemaphore*& GetTextureCopySemaphore(uint32_t index) = 0;

    // Multi-viewport support
    virtual void RegisterViewport(const int viewport_id, const RHIViewport& viewport) = 0;
    virtual void UpdateViewport(ViewportType viewport_id, const RHIViewport& viewport) = 0;
    virtual RHIViewport* GetViewport(ViewportType viewport_id) = 0;

    // Viewport RenderTexture support (for multi-viewport rendering without flickering)
    struct ViewportRenderTexture
    {
        RHIImage* color_image = nullptr;
        RHIImageView* color_image_view = nullptr;
        RHIDeviceMemory* color_memory = nullptr;
        RHIImage* depth_image = nullptr;
        RHIImageView* depth_image_view = nullptr;
        RHIDeviceMemory* depth_memory = nullptr;
        RHIFramebuffer* framebuffer = nullptr;
        uint32_t width = 0;
        uint32_t height = 0;
    };
    virtual void CreateViewportRenderTexture(const std::string& viewport_id, uint32_t width, uint32_t height) = 0;
    virtual void UpdateViewportRenderTexture(const std::string& viewport_id, uint32_t width, uint32_t height) = 0;
    virtual void DestroyViewportRenderTexture(const std::string& viewport_id) = 0;
    virtual ViewportRenderTexture* GetViewportRenderTexture(const std::string& viewport_id) = 0;

    RHIViewport m_Viewports[2];
    int m_ViewportCount {2};

private:
};

inline RHI::~RHI() = default;