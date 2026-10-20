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
//     RenderGraphResourcePool pool(rhi);
//     RenderGraphExecutor executor(rhi, &pool);
//     ... AddPass(...) ...
//     executor.Execute(graph, command_buffer);
// =====================================================================================

#include "Runtime/Function/Render/Graph/RenderGraph.h"
#include "Runtime/Function/Render/Interface/RHI.h"
#include "Runtime/Function/Render/Interface/RHIStruct.h"

#include <string>
#include <unordered_map>
#include <vector>

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
    explicit RenderGraphResourcePool(RHI* rhi);
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

    RHI* m_Rhi {nullptr};
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
    RenderGraphExecutor(RHI* rhi, RenderGraphResourcePool* pool);

    /// Compile (if needed), resolve every handle to a real resource, then record all
    /// passes into `command_buffer` in the order the compiler chose, emitting the derived
    /// barriers before each pass. Returns false when the graph failed validation.
    bool Execute(RenderGraph& graph, RHICommandBuffer* command_buffer);

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
