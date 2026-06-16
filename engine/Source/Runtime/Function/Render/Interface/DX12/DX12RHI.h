#pragma once

#include "Runtime/Core/Base/EngineSystem.h"
#include "Runtime/Function/Render/Interface/DX12/DX12BindlessTextureManager.h"
#include "Runtime/Function/Render/Interface/DX12/DX12ShaderCompiler.h"
#include "Runtime/Function/Render/Interface/DX12/Utility/DX12CubemapMipGen.h"
#include "Runtime/Function/Render/Interface/DX12/Utility/DX12GpuProfiler.h"
#include "Runtime/Function/Render/Interface/RHI.h"
#include "Runtime/Function/Render/Interface/RHIStruct.h"

#include <array>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <functional>
#include <memory>
#include <vector>

class DX12Image;
class DX12Pipeline;
class DX12RenderPass;
class DX12Framebuffer;
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

class WindowSystem;

// A floating editor-panel surface: a second DXGI swapchain bound to a child OS
// window's HWND (editor tear-off). It reuses DX12RHI's device + command queue +
// per-frame command list + fence depth, so it needs no extra GPU synchronization
// (the main frame fence gates back-buffer reuse for all swapchains presenting
// once per frame in lockstep). DX12-only; created/destroyed by the editor's
// floating-panel manager via DX12RHI::CreateFloatingSurface.
struct DX12FloatingSurface
{
    HWND hwnd {nullptr};
    ComPtr<IDXGISwapChain3> swapchain;
    ComPtr<ID3D12Resource> render_targets[3];
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    UINT rtv_descriptor_size {0};
    D3D12_RESOURCE_STATES states[3] {
        D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT};
    UINT back_buffer_index {0};
    uint32_t width {0};
    uint32_t height {0};
};

class DX12RHI final : public RHI
{
public:
    std::string GetName() const override { return "DX12RHI"; }
    SystemInitPhase GetInitPhase() const override { return SystemInitPhase::Rendering; }
    std::vector<std::type_index> GetDependencies() const override;
    bool Initialize() override;
    void Shutdown() override;

    // Get the graphics API
    GraphicsAPI getGraphicsAPI() const override { return GraphicsAPI::DirectX12; }

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
                                                  std::vector<uint8_t>& output_binary,
                                                  const std::string& entry_point = "main",
                                                  bool embed_debug = false) override;
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
    virtual void CopyBufferImmediate(RHIBuffer* srcBuffer,
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

    // Re-emit IBL cubemap upload diagnostics after Editor UI (Console) is live.
    void LogDeferredIblCubemapDiagnostics() const;

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
    virtual void DestroyRenderPass(RHIRenderPass* renderPass) override;
    virtual bool CreateSampler(const RHISamplerCreateInfo* pCreateInfo, RHISampler*& pSampler) override;
    virtual bool CreateSemaphore(const RHISemaphoreCreateInfo* pCreateInfo, RHISemaphore*& pSemaphore) override;

    // Command operations
    virtual bool
    WaitForFencesPFN(uint32_t fenceCount, RHIFence* const* pFence, RHIBool32 waitAll, uint64_t timeout) override;
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
    void CmdSetBindlessIndexPFN(RHICommandBuffer* commandBuffer,
                                RHIPipelineBindPoint pipelineBindPoint,
                                RHIPipelineLayout* layout,
                                uint32_t packed_index) override;
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
    virtual bool
    QueueSubmit(RHIQueue* queue, uint32_t submitCount, const RHISubmitInfo* pSubmits, RHIFence* fence) override;
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
    virtual void BeginGpuTimingScope(const char* name) override;
    virtual void EndGpuTimingScope() override;

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
    virtual void
    FlushMappedMemoryRanges(void* pNext, RHIDeviceMemory* memory, RHIDeviceSize offset, RHIDeviceSize size) override;

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

    ID3D12Device* getDevice() const { return m_Device.Get(); }
    ID3D12CommandQueue* getCommandQueue() const { return m_CommandQueue.Get(); }
    ID3D12GraphicsCommandList* getCurrentCommandList() const { return m_CommandLists[m_CurrentFrameIndex].Get(); }

    UINT getCurrentBackBufferIndex() const { return m_CurrentBackBufferIndex; }
    ID3D12DescriptorHeap* GetCbvSrvUavDescriptorHeap();
    ID3D12DescriptorHeap* GetSamplerDescriptorHeap();
    DXGI_FORMAT GetSwapchainDXGIFormat() const;
    // RP2 UI subpass: backup_even is R16G16B16A16_SFLOAT (matches MainCameraFramebufferResources).
    static DXGI_FORMAT GetUiLayerRtvFormat();
    void RestoreSwapchainRenderState();
    // After RP2 ends the swapchain image may be in PRESENT; rebind swapchain RTV for
    // fullscreen overlay draws (scene grid / skybox) that run outside the RP2 pass.
    void BeginSwapchainOverlayDraw();
    // DEBUG: Clear swapchain to a solid color to verify present pipeline works.
    void ClearSwapchainToColor(const float color[4]);
    // Bind backup_even (or any color RTV) for ImGui during RP2 UI subpass. No-op if view is null
    // or lacks an RTV handle.
    void BindUiLayerRenderTarget(RHIImageView* color_view);

    // ---- Editor floating-panel windows (tear-off) -- DX12-only ----------------
    // CreateFloatingSurface binds a second DXGI swapchain to a child OS window's
    // HWND (cast to void* by the editor to avoid leaking <windows.h>). The pair
    // Begin/EndFloatingSurfaceDraw records into the CURRENT frame command list --
    // call them around the overlay DrawBatch for that panel (Begin transitions the
    // floater's back buffer to RENDER_TARGET, binds its RTV + viewport and clears;
    // End transitions back to PRESENT and rebinds the main swapchain RTV).
    // SubmitRendering presents every surface drawn this frame right after the main
    // swapchain present, so the caller never presents explicitly.
    DX12FloatingSurface* CreateFloatingSurface(void* hwnd, uint32_t width, uint32_t height);
    void DestroyFloatingSurface(DX12FloatingSurface* surface);
    void ResizeFloatingSurface(DX12FloatingSurface* surface, uint32_t width, uint32_t height);
    void BeginFloatingSurfaceDraw(DX12FloatingSurface* surface, const float clear_color[4]);
    void EndFloatingSurfaceDraw(DX12FloatingSurface* surface);
    // Re-apply RP2/full-frame viewport+scissor while inside an active emulated render pass.
    void EnsureActiveRenderPassViewport();
    bool AllocateImGuiSrvDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE& cpu_handle,
                                    D3D12_GPU_DESCRIPTOR_HANDLE& gpu_handle);

    // Editor ZSlate native overlay: UiGpuResources textures must live in the
    // bindless heap when SetBindlessDescriptorHeaps is active.
    void EnsureShaderVisibleImageView(RHIImageView* image_view) override;

    // While true, CmdBindDescriptorSetsPFN binds the bindless (+ sampler) heap
    // instead of the legacy m_CbvSrvUavHeap (swapchain overlay draw path).
    void SetOverlayDescriptorBindActive(bool active) { m_OverlayDescriptorBindActive = active; }
    bool IsOverlayDescriptorBindActive() const { return m_OverlayDescriptorBindActive; }

    // PR-DX1: bind the bindless heap (and optional sampler heap) as
    // the active descriptor heaps on the current command list. When
    // bindless is supported, this replaces the old
    // SetDescriptorHeaps({m_CbvSrvUavHeap, m_SamplerHeap}) call.
    // Returns false if bindless is not supported.
    bool SetBindlessDescriptorHeaps();

    // PR-DX1: bind a constant buffer as a root CBV at the given root
    // parameter index. Used by bindless production passes that bind
    // UBOs via root descriptors instead of descriptor tables (avoiding
    // the legacy m_CbvSrvUavHeap).
    void CmdSetRootConstantBufferView(RHIPipelineBindPoint bind_point,
                                      uint32_t root_param_index,
                                      D3D12_GPU_VIRTUAL_ADDRESS gpu_va);

    // ---- PR4: bindless ----------------------------------------------
    // Capability queries override RHI defaults so higher-level code can
    // pick the bindless path uniformly across Vulkan / DX12. Values are
    // populated by Initialize() after the SM 6.6 + Resource-Binding-
    // Tier probe; they stay at false / 0 on hardware/drivers that fail
    // the probe (very old GPUs, WARP at low feature levels, etc.).
    bool supportsBindlessTextures() const override { return m_BindlessSupported; }
    uint32_t maxBindlessSampledImages() const override { return m_MaxBindlessSampledImages; }
    uint32_t maxBindlessStorageBuffers() const override { return m_MaxBindlessStorageBuffers; }

    RHIBindlessTextureManager* getBindlessTextureManager() override { return m_BindlessTextureManager.get(); }

    // ---- PR7: bindless static-sampler bank size (s0..s3 at b0/space0).
    // Whenever a pipeline layout contains at least one bindless descriptor
    // set, createPipelineLayout attaches exactly this many static samplers
    // (LinearWrap, LinearClamp, PointWrap, PointClamp -- index order MUST
    // match `BindlessBlitSampler` in dx12/utility/bindless_texture_blit_pipeline.h
    // AND the `g_*_*` declarations in dx12/utility/shaders/bindless_blit_ps.hlsl).
    //
    // Exposed as a public constexpr so dx12_bindless_smoke_test.cpp and
    // DX12MainCameraPass can pin it with a `static_assert` / use it in
    // root-signature construction -- if a future PR widens the sampler
    // bank without updating the HLSL or BindlessBlitSampler enum, the
    // smoke-test build breaks first instead of failing as a silent
    // runtime sampler swap. See AGENTS.md 2.9 PR7.
    static constexpr uint32_t kBindlessStaticSamplerCount = 4u;
    // ------------------------------------------------------------------

    // ------------------------------------------------------------------
    // DX12-only state-transition helpers.
    //
    // These wrap raw ResourceBarrier calls and -- crucially -- keep
    // DX12Image::m_States in sync, so subsequent backend calls (the
    // RP begin/end pair, blit copies, etc.) emit the correct
    // transition deltas. Editor-side code that already
    // static_cast<DX12RHI*>s and is intentionally DX12-specific
    // (e.g. inspector_window's bindless blit smoke / texture
    // preview) is the legitimate caller; using these instead of
    // issuing raw ResourceBarrier calls is the difference between a
    // working pipeline and a hard-to-debug "image is in the wrong
    // state" hang. Promoted from private to public in PR #8c so
    // smoke / preview TUs compile without friending; the underlying
    // semantics (and all internal RHI call sites such as
    // createCommandBuffers / cmdBegin/EndRenderPass / blit / copy)
    // are unchanged -- a public method is just as callable from
    // inside the class as a private one.
    void TransitionImage(DX12Image* image,
                         uint32_t subresource,
                         D3D12_RESOURCE_STATES new_state);
    void TransitionImage(DX12Image* image,
                         D3D12_RESOURCE_STATES new_state);
    void TransitionSwapchainBuffer(uint32_t back_buffer_index, D3D12_RESOURCE_STATES new_state);
    // ------------------------------------------------------------------

    bool IsDeviceRemoved(const char* context) const;

    /// True while CmdBeginRenderPassPFN has not been paired with CmdEndRenderPassPFN.
    bool IsInsideActiveRenderPass() const { return m_ActiveRenderPass != nullptr; }

    // Records GPU work onto a one-shot, fence-synchronized command list with its
    // own allocator (NOT the per-frame allocator / command list). This is
    // independent of the frame's render-pass state, so callers can run an
    // offscreen draw / copy at any point (e.g. the editor shader preview RTT)
    // without colliding with an active (emulated) render pass. Must be invoked on
    // the RHI worker thread (or with in-flight frames flushed) so the shared
    // command queue is not concurrently submitted to -- see
    // RenderSystem::RunSynchronizedGpuReadback. Blocks until the GPU finishes.
    bool ExecuteDedicatedUploadCommands(
        const std::function<void(ID3D12GraphicsCommandList*)>& record_commands);

    // Per-frame CBV/SRV/UAV heap allocation — public so that render passes
    // (e.g. DX12MainCameraPass::DrawSkyboxWithCamera) can allocate fresh
    // per-frame SRV slots to avoid stale GPU handles after WaitForFences
    // resets the per-frame partition counter.
    bool AllocateCbvSrvUavDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE& cpu_handle,
                                     D3D12_GPU_DESCRIPTOR_HANDLE& gpu_handle);
private:
    // Block the CPU until the GPU has finished ALL in-flight frame slots (not just
    // the current one). This is the equivalent of UE's FD3D12Adapter::BlockUntilIdle
    // and is mandatory before ResizeBuffers: DXGI requires that no GPU work still
    // references any swapchain backbuffer when it is resized. WaitForFences() alone
    // only drains m_CurrentFrameIndex, so a maximize (large resize) could fault the
    // device while another frame slot's work is still reading the old backbuffers.
    void WaitForGpuIdle();
    // Floating-surface helpers (editor tear-off). See CreateFloatingSurface.
    bool CreateFloatingSurfaceRtvs(DX12FloatingSurface* surface);
    void ReleaseFloatingSurfaceRtvs(DX12FloatingSurface* surface);
    void PresentPendingFloatingSurfaces();
    bool EnsureRtvDescriptorHeap();
    bool EnsureDsvDescriptorHeap();
    bool EnsureCbvSrvUavDescriptorHeap();
    bool EnsureSamplerDescriptorHeap();
    bool AllocateRtvDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE& cpu_handle);
    bool TryGetSwapchainBackBufferRtv(uint8_t back_buffer_index, D3D12_CPU_DESCRIPTOR_HANDLE& out_rtv) const;
    bool AllocateDsvDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE& cpu_handle);
    bool AllocateSamplerDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE& cpu_handle,
                                   D3D12_GPU_DESCRIPTOR_HANDLE& gpu_handle);
    bool CreateDynamicBufferGpuHandle(const DX12DescriptorBufferBinding& binding,
                                      uint32_t dynamic_offset,
                                      D3D12_GPU_DESCRIPTOR_HANDLE& out_gpu_handle);
    bool EnsureDispatchIndirectSignature();

    // ---- PR6: root-signature flag set OR'd in when a pipeline layout
    // contains at least one bindless descriptor set. Today this is just
    // CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED. SAMPLER_HEAP_DIRECTLY_INDEXED is
    // intentionally NOT included -- static-sampler arrays remain the
    // current sampler model; a future sampler-bindless PR can add it
    // here in one line. Returning the flags by value (instead of OR-ing
    // them inline at every call site) keeps the smoke-test's
    // independent mirror in `dx12_bindless_smoke_test.cpp` honest:
    // the test re-declares the same expression and a hard divergence
    // would surface as a PSO-creation failure under either path.
    static D3D12_ROOT_SIGNATURE_FLAGS GetBindlessRootSignatureFlags();

    // transitionImage was promoted to public above (search for
    // "DX12-only state-transition helpers"); see the comment block
    // there for the rationale.
    bool UploadTextureData(DX12Image* image,
                           const void* pixels,
                           uint32_t width,
                           uint32_t height,
                           uint32_t array_layer,
                           uint32_t mip_level,
                           uint32_t bytes_per_pixel);

    bool UploadCubeMapMip0(DX12Image* image,
                           const std::array<const void*, 6>& face_pixels,
                           uint32_t width,
                           uint32_t height,
                           uint32_t bytes_per_pixel);

    bool UploadCubeMapAllMips(DX12Image* image,
                              const std::array<const void*, 6>& face_pixels,
                              uint32_t width,
                              uint32_t height,
                              uint32_t mip_levels,
                              uint32_t bytes_per_pixel);

    DX12CubemapMipGenerator m_CubemapMipGenerator;
    uint32_t m_IblGpuCubemapUploadCount {0};
    uint32_t m_IblGpuCubemapLastMips {0};
    uint32_t m_IblGpuCubemapLastWidth {0};
    bool m_IblMipGenReady {false};
    DXGI_FORMAT m_IblMipGenFormat {DXGI_FORMAT_UNKNOWN};

    // DX12 specific members
    ComPtr<IDXGIFactory7> m_DxgiFactory;
    ComPtr<IDXGIAdapter4> m_Adapter;
    ComPtr<ID3D12Device> m_Device;
    ComPtr<IDXGISwapChain3> m_Swapchain;
    // Floating surfaces that recorded a draw this frame; presented (once each)
    // by SubmitRendering right after the main swapchain present, then cleared.
    std::vector<DX12FloatingSurface*> m_PendingFloatingPresents;
    ComPtr<ID3D12CommandQueue> m_CommandQueue;
    ComPtr<ID3D12CommandAllocator> m_CommandAllocators[3];  // k_max_frames_in_flight
    ComPtr<ID3D12GraphicsCommandList> m_CommandLists[3];
    ComPtr<ID3D12DescriptorHeap> m_RtvHeap;
    ComPtr<ID3D12DescriptorHeap> m_DsvHeap;
    ComPtr<ID3D12DescriptorHeap> m_CbvSrvUavHeap;
    ComPtr<ID3D12DescriptorHeap> m_SamplerHeap;
    ComPtr<ID3D12CommandSignature> m_DispatchIndirectSignature;
    ComPtr<ID3D12Resource> m_RenderTargets[3];
    ComPtr<ID3D12Resource> m_DepthStencil;
    ComPtr<ID3D12Fence> m_Fences[3];
    HANDLE m_FenceEvent {nullptr};
    static constexpr uint8_t k_max_frames_in_flight {3};
    uint64_t m_FenceValues[3] {};

    // ZEngine Insights GPU timing (lazily created on first frame; dormant unless
    // the Insights window has capture active). m_GpuFrameOpen guards the matched
    // whole-frame scope so a skipped PrepareBeforePass doesn't desync EndScope.
    DX12GpuProfiler m_GpuProfiler;
    bool m_GpuFrameOpen {false};
    UINT m_RtvDescriptorSize {0};
    UINT m_DsvDescriptorSize {0};
    UINT m_CbvSrvUavDescriptorSize {0};
    UINT m_SamplerDescriptorSize {0};
    UINT m_NextRtvDescriptor {0};
    UINT m_NextDsvDescriptor {0};
    UINT m_NextCbvSrvUavDescriptorPerFrame[k_max_frames_in_flight] {};
    UINT m_NextSamplerDescriptor {0};
    UINT m_RtvDescriptorCapacity {4096};
    UINT m_DsvDescriptorCapacity {1024};
    UINT m_CbvSrvUavDescriptorCapacity {4096};
    UINT m_SamplerDescriptorCapacity {1024};
    UINT m_CurrentBackBufferIndex {0};
    uint8_t m_CurrentFrameIndex {0};
    bool m_OverlayDescriptorBindActive {false};

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
    RHIImage* m_DepthImage {nullptr};
    RHIDeviceMemory* m_DepthImageMemory {nullptr};
    RHIImageView* m_DepthImageView {nullptr};
    RHIFence* m_RhiIsFrameInFlightFences[k_max_frames_in_flight] {};
    RHISemaphore* m_ImageAvailableForTexturescopySemaphores[k_max_frames_in_flight] {};
    RHIDescriptorPool* m_DescriptorPool {nullptr};
    RHICommandPool* m_RhiCommandPool {nullptr};
    RHICommandBuffer* m_CommandBuffers[k_max_frames_in_flight] {};
    RHICommandBuffer* m_CurrentCommandBuffer {nullptr};
    DX12Pipeline* m_CurrentGraphicsPipeline {nullptr};
    DX12Pipeline* m_CurrentComputePipeline {nullptr};
    DX12RenderPass* m_ActiveRenderPass {nullptr};
    DX12Framebuffer* m_ActiveFramebuffer {nullptr};
    uint32_t m_ActiveSubpassIndex {0};
    // Per-Vulkan spec: loadOp only applies on the FIRST use of an attachment.
    // This mask tracks which attachments have already been "first-used" (and cleared)
    // within the current render pass, so that CmdNextSubpassPFN does not
    // clear them again for subsequent subpasses.
    uint64_t m_ActiveSubpassAttachmentsUsed {0};
    static constexpr uint32_t k_max_stored_render_pass_clear_values = 16;
    RHIClearValue m_ActiveRenderPassClearValues[k_max_stored_render_pass_clear_values] {};
    uint32_t m_ActiveRenderPassClearValueCount {0};
    enum class SwapchainSurfaceState : uint8_t
    {
        Present,
        RenderTarget,
    };
    SwapchainSurfaceState m_SwapchainSurfaceState {SwapchainSurfaceState::Present};
    std::array<D3D12_RESOURCE_STATES, 3> m_SwapchainResourceStates {};
    // RTV handles saved before RecreateSwapchain tears down image views so the
    // replacement backbuffers reuse the same heap slots (m_NextRtvDescriptor keeps
    // advancing otherwise, leaving PrepareBeforePass pointing at stale descriptors).
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, k_max_frames_in_flight> m_SwapchainRtvReuse {};
    bool m_HasSwapchainRtvReuse {false};

    void BindSubpassRenderTargets(const RHISubpassDescription& subpass,
                                  const RHIRenderPassBeginInfo* begin_info,
                                  bool clear_on_load);
    void TransitionSubpassAttachments(const RHISubpassDescription& subpass, bool for_color_output);
    void ApplySubpassDependencies(uint32_t src_subpass, uint32_t dst_subpass);
    QueueFamilyIndices m_QueueIndices;

    // Runtime shader compiler
    std::unique_ptr<DX12ShaderCompiler> m_ShaderCompiler;

    // ---- PR4: bindless ----------------------------------------------
    // Cached caps probed at Initialize() time (D3D12_FEATURE_SHADER_MODEL
    // for SM 6.6, D3D12_FEATURE_D3D12_OPTIONS for the Resource Binding
    // Tier). Demoted to false / 0 if any probe fails or if the manager
    // itself fails to come up.
    bool m_BindlessSupported = false;
    uint32_t m_MaxBindlessSampledImages = 0;
    uint32_t m_MaxBindlessStorageBuffers = 0;
    D3D_SHADER_MODEL m_MaxSupportedShaderModel {D3D_SHADER_MODEL_6_0};
    D3D12_RESOURCE_BINDING_TIER m_ResourceBindingTier {D3D12_RESOURCE_BINDING_TIER_1};

    // Owns the dedicated SHADER_VISIBLE CBV/SRV/UAV heap that backs the
    // bindless table. Kept separate from m_CbvSrvUavHeap so the
    // legacy linear-bump Allocator (used by ImGui SRV slot) is
    // untouched. Built only when m_BindlessSupported is true.
    std::unique_ptr<DX12BindlessTextureManager> m_BindlessTextureManager;
    // ------------------------------------------------------------------

    // Cached samplers (Vulkan parity). GetOrCreateDefaultSampler used to call
    // CreateSampler every time and exhausted the 1024-slot shader-visible heap.
    RHISampler* m_LinearSampler {nullptr};
    RHISampler* m_NearestSampler {nullptr};
    std::map<uint32_t, RHISampler*> m_MipmapSamplerMap;
};
