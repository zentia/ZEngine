#pragma once

// The WebGL 2.0 RHI is an Emscripten-only backend. On native toolchains
// (MSVC / clang / Apple clang / NDK) the WebGL / GLES3 headers it depends
// on are unavailable, so the entire translation unit is compiled out.
#if defined(__EMSCRIPTEN__)

// -----------------------------------------------------------------------------
// WebGL 2.0 RHI backend.
//
// One-to-one override of the RHI virtual surface declared in
// runtime/function/render/interface/rhi.h. Targets:
//   - Emscripten + WebGL 2.0 contexts inside browsers
//   - 微信 / 抖音 / QQ / 支付宝 小游戏运行时 (canvas-backed WebGL2)
//
// Conventions:
//   - Single in-order GL context => command buffers are recorded into a
//     CPU-side command list and replayed on SubmitRendering().
//   - No descriptor heaps; descriptor sets are resolved into UBO / sampler
//     binding points at draw time using the layout metadata.
//   - No real fences; QueueWaitIdle() flushes via glFinish() and fences
//     translate to glFenceSync / glClientWaitSync.
//   - No compute shaders; createComputePipelines / cmdDispatch are no-ops
//     that log a warning so a unified renderer can still target this backend.
//   - **No bindless textures -- permanently legacy bindful.**
//     WebGL2 / GLSL ES 3.00 lacks every prerequisite for bindless:
//       * No GL_ARB_bindless_texture (desktop-only extension)
//       * No nonuniformEXT / NonUniformResourceIndex (descriptor indexing)
//       * sampler2D arrays require compile-time-constant size and
//         dynamically-uniform indices, capping practical array sizes to
//         MAX_TEXTURE_IMAGE_UNITS (typically 16-32 in browsers)
//     Therefore WebGL2RHI overrides all bindless query methods to return
//     false / 0 / nullptr and will never grow a BindlessTextureManager.
//     Renderers targeting this backend must use per-draw descriptor sets
//     (the traditional bindful model).
// -----------------------------------------------------------------------------

    #include "Runtime/Core/Base/EngineSystem.h"
    #include "Runtime/Function/Render/Interface/RHI.h"
    #include "Runtime/Function/Render/Interface/RHIStruct.h"
    #include "Runtime/Function/Render/Interface/WebGL2/WebGL2RHIResource.h"
    #include "Runtime/Function/Render/Interface/WebGL2/WebGL2ShaderCompiler.h"

    #include <GLES3/gl3.h>
    #include <map>
    #include <memory>
    #include <string>
    #include <vector>

class WindowSystem;

namespace ZEngine
{
    namespace WebGL2
    {

        class WebGL2RHI final : public RHI
        {
        public:
            // ---------------- EngineSystem ----------------
            std::string GetName() const override { return "WebGL2RHI"; }
            SystemInitPhase GetInitPhase() const override { return SystemInitPhase::Rendering; }
            std::vector<std::type_index> GetDependencies() const override;
            bool Initialize() override;
            void Shutdown() override;

            // ---------------- API identity ----------------
            GraphicsAPI getGraphicsAPI() const override { return GraphicsAPI::WebGL2; }

            // ---------------- Bindless capability (permanently disabled) ----------------
            // WebGL2 / GLSL ES 3.00 has no bindless texture support whatsoever:
            //   - No GL_ARB_bindless_texture (desktop-only)
            //   - No nonuniformEXT / descriptor indexing
            //   - sampler2D arrays capped at MAX_TEXTURE_IMAGE_UNITS (16-32)
            // This backend is permanently legacy-bindful. Override explicitly so the
            // decision is visible without reading the RHI base class defaults.
            bool supportsBindlessTextures() const override { return false; }
            uint32_t maxBindlessSampledImages() const override { return 0; }
            uint32_t maxBindlessStorageBuffers() const override { return 0; }
            RHIBindlessTextureManager* getBindlessTextureManager() override { return nullptr; }

            virtual void PrepareContext() override final;

            virtual bool IsPointLightShadowEnabled() override;

            // ---------------- Allocation ----------------
            virtual bool AllocateCommandBuffers(const RHICommandBufferAllocateInfo* pAllocateInfo,
                                                RHICommandBuffer*& pCommandBuffers) override;
            virtual bool AllocateDescriptorSets(const RHIDescriptorSetAllocateInfo* pAllocateInfo,
                                                RHIDescriptorSet*& pDescriptorSets) override;

            // ---------------- Swapchain ----------------
            virtual void CreateSwapchain() override;
            virtual void RecreateSwapchain() override;
            virtual void CreateSwapchainImageViews() override;
            virtual void CreateFramebufferImageAndView() override;

            // ---------------- Sampler / shader ----------------
            virtual RHISampler* GetOrCreateDefaultSampler(RHIDefaultSamplerType type) override;
            virtual RHISampler* GetOrCreateMipmapSampler(uint32_t width, uint32_t height) override;
            virtual RHIShader* CreateShaderModule(const std::vector<unsigned char>& shader_code) override;

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

            // ---------------- Buffer ----------------
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

            // ---------------- Image ----------------
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

            // ---------------- Pipeline / pool / layout ----------------
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

            // ---------------- Command recording ----------------
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
            // WebGL2 has no compute. Recorded as a no-op + warning.
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

            // ---------------- Query ----------------
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

            // ---------------- Frame loop ----------------
            virtual RHICommandBuffer* BeginSingleTimeCommands() override;
            virtual void EndSingleTimeCommands(RHICommandBuffer* command_buffer) override;
            virtual bool PrepareBeforePass(std::function<void()> passUpdateAfterRecreateSwapchain) override;
            virtual void SubmitRendering(std::function<void()> passUpdateAfterRecreateSwapchain) override;
            virtual void PushEvent(RHICommandBuffer* commond_buffer, const char* name, const float* color) override;
            virtual void PopEvent(RHICommandBuffer* commond_buffer) override;

            // ---------------- Destroy ----------------
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

            // ---------------- Memory ----------------
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

            // ---------------- Semaphore ----------------
            virtual RHISemaphore*& GetTextureCopySemaphore(uint32_t index) override;

            // ---------------- Multi-viewport ----------------
            virtual void RegisterViewport(const int viewport_id, const RHIViewport& viewport) override;
            virtual void UpdateViewport(ViewportType viewport_id, const RHIViewport& viewport) override;
            virtual RHIViewport* GetViewport(ViewportType viewport_id) override;

            virtual void CreateViewportRenderTexture(const std::string& viewport_id, uint32_t width, uint32_t height) override;
            virtual void UpdateViewportRenderTexture(const std::string& viewport_id, uint32_t width, uint32_t height) override;
            virtual void DestroyViewportRenderTexture(const std::string& viewport_id) override;
            virtual ViewportRenderTexture* GetViewportRenderTexture(const std::string& viewport_id) override;

            // ---------------- WebGL2-specific accessors ----------------
            GLuint getDefaultFramebuffer() const { return m_DefaultFramebuffer; }
            int getDrawableWidth() const { return m_DrawableWidth; }
            int getDrawableHeight() const { return m_DrawableHeight; }

        private:
            // -- platform context (Emscripten / WebGL2) --
            bool CreateGLContext(WindowSystem* window_system);
            void DestroyGLContext();

            // -- command list replay --
            void ExecuteCommandList(WebGL2CommandBuffer* cb);
            void FlushPipelineState(WebGL2Pipeline* pipeline);
            void BindDescriptorSets(WebGL2PipelineLayout* layout,
                                    uint32_t first_set,
                                    uint32_t count,
                                    const RHIDescriptorSet* const* sets);

            // -- format / topology helpers --
            static GLenum ToGLInternalFormat(RHIFormat fmt);
            static GLenum ToGLFormat(RHIFormat fmt);
            static GLenum ToGLType(RHIFormat fmt);
            static GLenum ToGLTopology(RHIPipelineBindPoint, /*topology*/ int topo);
            static GLenum ToGLIndexType(RHIIndexType t);

            // -- platform handles --
            void* m_GlContextHandle {nullptr};  // EMSCRIPTEN_WEBGL_CONTEXT_HANDLE
            int m_DrawableWidth {0};
            int m_DrawableHeight {0};
            GLuint m_DefaultFramebuffer {0};  // 0 in WebGL2 = canvas

            // -- queues / pool / cmd buffers (single-queue) --
            WebGL2Queue m_GraphicsQueue;
            WebGL2CommandPool m_CommandPool;
            static constexpr uint8_t k_max_frames_in_flight {2};
            WebGL2CommandBuffer m_CommandBuffers[k_max_frames_in_flight];
            RHICommandBuffer* m_CommandBufferPtrs[k_max_frames_in_flight] {};
            WebGL2Fence m_FrameFences[k_max_frames_in_flight];
            RHIFence* m_FrameFencePtrs[k_max_frames_in_flight] {};
            uint8_t m_CurrentFrameIndex {0};
            WebGL2CommandBuffer* m_CurrentCommandBuffer {nullptr};

            // -- swapchain (canvas drawable) --
            RHIFormat m_SwapchainImageFormat {RHI_FORMAT_R8G8B8A8_UNORM};
            std::vector<RHIImageView*> m_SwapchainImageviews;
            RHIExtent2D m_SwapchainExtent {};

            // -- depth attachment --
            RHIFormat m_DepthImageFormat {RHI_FORMAT_D24_UNORM_S8_UINT};
            RHIImage* m_DepthImage {nullptr};
            RHIDeviceMemory* m_DepthImageMemory {nullptr};
            RHIImageView* m_DepthImageView {nullptr};

            // -- viewports / rt --
            std::map<std::string, ViewportRenderTexture> m_ViewportRenderTextures;
            std::string m_ActiveViewportId {"default"};

            // -- pipeline state cache (set by cmdBindPipeline / cmdBindDescriptorSets) --
            WebGL2Pipeline* m_CurrentPipeline {nullptr};
            WebGL2RenderPass* m_ActiveRenderPass {nullptr};
            WebGL2Framebuffer* m_ActiveFramebuffer {nullptr};
            GLuint m_GlobalVao {0};

            // -- queue family indices placeholder (single graphics queue) --
            QueueFamilyIndices m_QueueIndices;

            // -- runtime shader compiler (HLSL → GLSL ES via DXC + SPIRV-Cross) --
            std::unique_ptr<WebGL2ShaderCompiler> m_ShaderCompiler;
        };

    }  // namespace WebGL2
}  // namespace ZEngine

#endif  // __EMSCRIPTEN__
