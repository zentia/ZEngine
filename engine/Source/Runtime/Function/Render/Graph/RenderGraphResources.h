#pragma once

// =====================================================================================
// RenderGraphResources -- the Execute half of the graph: transient resource pool,
// handle -> physical resource resolution, and the barrier-recording executor.
// -------------------------------------------------------------------------------------
// This is the only place in the graph that knows about the RHI. Everything above
// (RenderGraphTypes / RenderGraphBuilder / RenderGraph) is pure CPU, which is what lets
// the compiler be unit tested headless.
//
// Typical frame:
//
//     RenderGraph graph("Frame");
//     RGRhiDeviceAdapter device(rhi);
//     RenderGraphResourcePool pool(&device);
//     RenderGraphExecutor executor(rhi, &device, &pool);
//     ... AddPass(...) ...
//     executor.Execute(graph, command_buffer);
// =====================================================================================

#include "Runtime/Function/Render/Graph/RenderGraph.h"
#include "Runtime/Function/Render/Interface/RHI.h"
#include "Runtime/Function/Render/Interface/RHIStruct.h"

#include <string>
#include <unordered_map>
#include <vector>

// =====================================================================================
// IRGResourceDevice -- the narrow port the graph's Execute half talks to.
// -------------------------------------------------------------------------------------
// The pool and the executor need exactly ten backend operations. Depending on the whole
// `RHI` (135 virtuals, 122 of them pure) would make both classes impossible to test
// headless -- a stub would have to implement every pure virtual, and any RHI signature
// drift would break the test for reasons that have nothing to do with the graph.
//
// Same idea as the compiler being RHI-free, applied one level down: the graph asks for
// the smallest surface it can, and `RGRhiDeviceAdapter` bridges that surface to the real
// RHI in production.
// =====================================================================================
class IRGResourceDevice
{
public:
    virtual ~IRGResourceDevice() = default;

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

    virtual void CreateBuffer(RHIDeviceSize size,
                              RHIBufferUsageFlags usage,
                              RHIMemoryPropertyFlags properties,
                              RHIBuffer*& buffer,
                              RHIDeviceMemory*& buffer_memory) = 0;

    virtual void DestroyImageView(RHIImageView* image_view) = 0;
    virtual void DestroyImage(RHIImage* image) = 0;
    virtual void DestroyBuffer(RHIBuffer*& buffer) = 0;
    virtual void FreeMemory(RHIDeviceMemory*& memory) = 0;

    virtual void CmdPipelineBarrier(RHICommandBuffer* command_buffer,
                                    RHIPipelineStageFlags src_stage_mask,
                                    RHIPipelineStageFlags dst_stage_mask,
                                    RHIDependencyFlags dependency_flags,
                                    uint32_t memory_barrier_count,
                                    const RHIMemoryBarrier* p_memory_barriers,
                                    uint32_t buffer_memory_barrier_count,
                                    const RHIBufferMemoryBarrier* p_buffer_memory_barriers,
                                    uint32_t image_memory_barrier_count,
                                    const RHIImageMemoryBarrier* p_image_memory_barriers) = 0;

    virtual void PushEvent(RHICommandBuffer* command_buffer, const char* name, const float* color) = 0;
    virtual void PopEvent(RHICommandBuffer* command_buffer) = 0;
};

/// Production bridge: forwards the ten operations above to a real `RHI`.
class RGRhiDeviceAdapter final : public IRGResourceDevice
{
public:
    explicit RGRhiDeviceAdapter(RHI* rhi) : m_Rhi(rhi) {}

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

    void CreateBuffer(RHIDeviceSize size,
                      RHIBufferUsageFlags usage,
                      RHIMemoryPropertyFlags properties,
                      RHIBuffer*& buffer,
                      RHIDeviceMemory*& buffer_memory) override;

    void DestroyImageView(RHIImageView* image_view) override;
    void DestroyImage(RHIImage* image) override;
    void DestroyBuffer(RHIBuffer*& buffer) override;
    void FreeMemory(RHIDeviceMemory*& memory) override;

    void CmdPipelineBarrier(RHICommandBuffer* command_buffer,
                            RHIPipelineStageFlags src_stage_mask,
                            RHIPipelineStageFlags dst_stage_mask,
                            RHIDependencyFlags dependency_flags,
                            uint32_t memory_barrier_count,
                            const RHIMemoryBarrier* p_memory_barriers,
                            uint32_t buffer_memory_barrier_count,
                            const RHIBufferMemoryBarrier* p_buffer_memory_barriers,
                            uint32_t image_memory_barrier_count,
                            const RHIImageMemoryBarrier* p_image_memory_barriers) override;

    void PushEvent(RHICommandBuffer* command_buffer, const char* name, const float* color) override;
    void PopEvent(RHICommandBuffer* command_buffer) override;

private:
    RHI* m_Rhi {nullptr};
};

/// What a texture handle resolves to while a graph is executing.
struct RGPhysicalTexture
{
    RHIImage* image {nullptr};
    RHIImageView* view {nullptr};
    RHIDeviceMemory* memory {nullptr};
};

/// What a buffer handle resolves to while a graph is executing.
struct RGPhysicalBuffer
{
    RHIBuffer* buffer {nullptr};
    RHIDeviceMemory* memory {nullptr};
};

// =====================================================================================
// The object a pass's execute callback receives.
// =====================================================================================

class RenderGraphExecuteContext
{
public:
    RHICommandBuffer* GetCommandBuffer() const { return m_CommandBuffer; }
    RHI* GetRHI() const { return m_Rhi; }
    const char* GetPassName() const { return m_PassName; }
    uint32_t GetPassIndex() const { return m_PassIndex; }

    /// Resolve a handle captured during Setup. Returns nullptr when the handle is stale
    /// (belongs to a previous frame) or when the resource was culled.
    RHIImage* GetImage(RGTextureHandle handle) const;
    RHIImageView* GetImageView(RGTextureHandle handle) const;
    RHIBuffer* GetBuffer(RGBufferHandle handle) const;
    bool IsResolved(RGTextureHandle handle) const;
    bool IsResolved(RGBufferHandle handle) const;

private:
    friend class RenderGraphExecutor;

    RHI* m_Rhi {nullptr};
    RHICommandBuffer* m_CommandBuffer {nullptr};
    const char* m_PassName {""};
    uint32_t m_PassIndex {kRGInvalidIndex};
    const RenderGraph* m_Graph {nullptr};
    const std::vector<RGPhysicalTexture>* m_Textures {nullptr};
    const std::vector<RGPhysicalBuffer>* m_Buffers {nullptr};
};

// =====================================================================================
// Transient resource pool
// =====================================================================================
// Framegraph resources are created and destroyed constantly; allocation is one of the
// most expensive things you can do per frame, so the pool keeps freed resources around
// keyed by their descriptor and hands them back when a matching one is asked for.
// Nothing is destroyed until Flush() -- call it on swapchain resize / shutdown.

class RenderGraphResourcePool
{
public:
    explicit RenderGraphResourcePool(IRGResourceDevice* device);
    ~RenderGraphResourcePool();

    RenderGraphResourcePool(const RenderGraphResourcePool&) = delete;
    RenderGraphResourcePool& operator=(const RenderGraphResourcePool&) = delete;

    RGPhysicalTexture AcquireTexture(const RGTextureDesc& desc);
    void ReleaseTexture(const RGTextureDesc& desc, const RGPhysicalTexture& physical);

    RGPhysicalBuffer AcquireBuffer(const RGBufferDesc& desc);
    void ReleaseBuffer(const RGBufferDesc& desc, const RGPhysicalBuffer& physical);

    /// Destroy everything currently idle. Live (acquired) resources are untouched.
    void Flush();

    void SetMaxIdlePerKey(size_t count) { m_MaxIdlePerKey = count; }
    size_t GetMaxIdlePerKey() const { return m_MaxIdlePerKey; }

    size_t GetIdleTextureCount() const;
    size_t GetIdleBufferCount() const;
    /// How many RHI objects this pool has created and not destroyed.
    size_t GetLiveObjectCount() const { return m_LiveObjectCount; }

private:
    static uint64_t KeyOf(const RGTextureDesc& desc);
    static uint64_t KeyOf(const RGBufferDesc& desc);

    IRGResourceDevice* m_Device {nullptr};
    std::unordered_map<uint64_t, std::vector<RGPhysicalTexture>> m_IdleTextures;
    std::unordered_map<uint64_t, std::vector<RGPhysicalBuffer>> m_IdleBuffers;
    size_t m_MaxIdlePerKey {8};
    size_t m_LiveObjectCount {0};
};

// =====================================================================================
// Executor
// =====================================================================================

class RenderGraphExecutor
{
public:
    /// `rhi` is only handed to the pass callbacks through RenderGraphExecuteContext::GetRHI()
    /// -- the executor itself never touches it, so tests may pass nullptr. `device` is what
    /// actually allocates and records.
    RenderGraphExecutor(RHI* rhi, IRGResourceDevice* device, RenderGraphResourcePool* pool);
    ~RenderGraphExecutor();

    /// Compile (if needed), resolve every handle to a real resource, then record all
    /// passes into `command_buffer` in the order the compiler chose, emitting the derived
    /// barriers before each pass. Returns false when the graph failed validation.
    bool Execute(RenderGraph& graph, RHICommandBuffer* command_buffer);

    /// Hand the previous Execute()'s extracted resources back to the pool. Called
    /// automatically at the start of the next Execute(); exposed so a caller that only
    /// runs a graph once (GPU picking, a screenshot) can reclaim without a second frame.
    void ReleaseExtracted();

    const std::string& GetLastError() const { return m_LastError; }

    // Stats from the last Execute(), handy for HUDs / unit tests.
    size_t GetLastPassCount() const { return m_LastPassCount; }
    size_t GetLastBarrierCount() const { return m_LastBarrierCount; }
    size_t GetLastAllocatedTextureCount() const { return m_LastAllocatedTextures; }

    /// When false the executor still runs every pass but skips PushEvent/PopEvent.
    void SetDebugEventsEnabled(bool enabled) { m_DebugEvents = enabled; }

private:
    void RecordBarriers(RHICommandBuffer* command_buffer,
                        const RenderGraph& graph,
                        size_t barrier_begin,
                        size_t barrier_end);

    RHI* m_Rhi {nullptr};
    IRGResourceDevice* m_Device {nullptr};
    RenderGraphResourcePool* m_Pool {nullptr};
    bool m_DebugEvents {true};

    std::string m_LastError;
    size_t m_LastPassCount {0};
    size_t m_LastBarrierCount {0};
    size_t m_LastAllocatedTextures {0};

    std::vector<RGPhysicalTexture> m_Textures;
    std::vector<RGPhysicalBuffer> m_Buffers;
    std::vector<RGTextureDesc> m_AcquiredTextureDescs;
    std::vector<RGBufferDesc> m_AcquiredBufferDescs;

    // Extracted resources outlive the graph, so they cannot go back into the idle pool
    // while the caller may still be reading them. They are held here and released at the
    // start of the next Execute() (or by ReleaseExtracted()).
    std::vector<RGPhysicalTexture> m_ExtractedTextures;
    std::vector<RGPhysicalBuffer> m_ExtractedBuffers;
    std::vector<RGTextureDesc> m_ExtractedTextureDescs;
    std::vector<RGBufferDesc> m_ExtractedBufferDescs;
};

// =====================================================================================
// RG -> RHI translation (also used by RenderGraphResources.cpp only)
// =====================================================================================

RHIFormat RGToRHIFormat(RGFormat format);
RHIImageUsageFlags RGToRHIImageUsage(RGTextureUsage usage);
RHIBufferUsageFlags RGToRHIBufferUsage(RGBufferUsage usage);

struct RGStateMapping
{
    RHIImageLayout layout {RHI_IMAGE_LAYOUT_UNDEFINED};
    RHIAccessFlags access {0};
    RHIPipelineStageFlags stage {RHI_PIPELINE_STAGE_TOP_OF_PIPE_BIT};
};

RGStateMapping RGMapState(RGResourceState state, RGPassType pass_type);
