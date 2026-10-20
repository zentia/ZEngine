#pragma once

#include "Runtime/Function/Render/Graph/RenderGraphBuilder.h"

#include <string>
#include <vector>

// =====================================================================================
// RenderGraph -- Setup / Compile. The Execute half lives in RenderGraphResources.h.
// -------------------------------------------------------------------------------------
// Usage:
//
//     RenderGraph graph("PostChain");
//     auto backbuffer = graph.ImportTexture("Backbuffer", {swap_image, swap_view,
//                                                          RGResourceState::Undefined});
//     graph.AddPass("Gbuffer", RGPassType::Render, setup_fn, execute_fn);
//     graph.AddPass("Tonemap", RGPassType::Render, setup_fn, execute_fn);
//     if (!graph.Compile().valid) { /* log errors */ }
//
// Compile() is pure CPU and deterministic: same declarations in, same order / barriers /
// aliasing out. That is what Test/RenderGraphSmokeTest.cpp asserts on.
// =====================================================================================

/// One resource in the graph -- a texture or a buffer, transient or imported.
struct RGResource
{
    std::string name;
    bool is_texture {true};

    RGTextureDesc texture_desc;
    RGBufferDesc buffer_desc;

    /// Imported (external) resources are allocated by the caller: the graph never
    /// allocates, destroys or memory-aliases them.
    bool external {false};
    RGExternalTexture external_texture;
    RGExternalBuffer external_buffer;

    /// Stale handles (from a previous frame's setup) are rejected by comparing this.
    uint32_t generation {0};

    uint32_t producer_pass {kRGInvalidIndex};   // first pass that writes it
    std::vector<uint32_t> readers;              // pass indices, declaration order
    std::vector<uint32_t> writers;              // pass indices, declaration order

    /// Kept alive past the end of the graph (UE Extract). Blocks culling of its producer.
    bool extracted {false};

    size_t GetByteSize() const;

    /// The state an imported resource is in when the graph gets it.
    RGResourceState GetExternalState() const
    {
        return is_texture ? external_texture.state : external_buffer.state;
    }
};

/// One pass node.
struct RGPass
{
    std::string name;
    RGPassType type {RGPassType::Render};
    RGPassFlags flags {RGPassFlags::None};

    std::vector<RGResourceUse> reads;
    std::vector<RGResourceUse> writes;

    RGExecuteFn execute;

    // ---- filled in by Compile() ----
    /// Position in the execution order, or kRGInvalidIndex when culled.
    uint32_t order {kRGInvalidIndex};
    bool culled {false};
};

/// One transition the executor has to emit before a pass runs.
struct RGBarrier
{
    uint32_t resource_index {kRGInvalidIndex};
    bool is_texture {true};
    RGResourceState before {RGResourceState::Undefined};
    RGResourceState after {RGResourceState::Undefined};
};

/// Where a resource is live in the execution order, and which memory slot it got.
struct RGResourceLifetime
{
    uint32_t resource_index {kRGInvalidIndex};
    uint32_t first_pass {kRGInvalidIndex};   // index into the execution order
    uint32_t last_pass {kRGInvalidIndex};
    uint32_t memory_slot {kRGInvalidIndex};  // kRGInvalidIndex for imported resources
    size_t byte_size {0};
    bool aliased {false};                    // shares its slot with another resource
};

struct RGCompiledGraph
{
    bool valid {false};

    /// Pass indices in the order they must be recorded.
    std::vector<uint32_t> pass_order;
    std::vector<uint32_t> culled_passes;

    /// barriers[barrier_offsets[p] .. barrier_offsets[p+1]) belong to pass_order[p].
    /// Size is pass_order.size() + 1 so the last range is always well formed.
    std::vector<uint32_t> barrier_offsets;
    std::vector<RGBarrier> barriers;

    /// Indexed by resource index; only meaningful for resources that survived culling.
    std::vector<RGResourceLifetime> lifetimes;

    /// Sum of all memory slots: what the transient allocator actually allocates.
    size_t total_allocated_bytes {0};
    /// Worst instant: the most bytes live at one point in the frame.
    size_t peak_live_bytes {0};

    std::vector<std::string> errors;
    std::vector<std::string> warnings;

    bool HasErrors() const { return !errors.empty(); }
};

class RenderGraph
{
public:
    explicit RenderGraph(std::string name = "RenderGraph");

    const std::string& GetName() const { return m_Name; }

    /// Drop every pass and resource and invalidate all outstanding handles. Call at the
    /// start of each frame; the graph object itself is meant to be reused.
    void Reset();

    // ---- Setup ----------------------------------------------------------------------
    /// Runs `setup` immediately (it only declares reads / writes and may create
    /// resources) and stores `execute` for the Execute phase. Returns the pass index.
    uint32_t AddPass(const char* name,
                     RGPassType type,
                     RGSetupFn setup,
                     RGExecuteFn execute,
                     RGPassFlags flags = RGPassFlags::None);

    /// Convenience for importing before any pass runs (the swapchain, a scene depth
    /// target). The builder exposes the same calls for imports made inside a pass.
    RGTextureHandle ImportTexture(const char* name, const RGExternalTexture& external);
    RGBufferHandle ImportBuffer(const char* name, const RGExternalBuffer& external);

    // ---- Compile --------------------------------------------------------------------
    /// Topological sort -> culling -> lifetimes -> memory aliasing -> barriers.
    /// Idempotent: calling it twice without touching the graph returns the same result.
    const RGCompiledGraph& Compile();
    const RGCompiledGraph& GetCompiled() const { return m_Compiled; }

    // ---- inspection -----------------------------------------------------------------
    const std::vector<RGResource>& GetResources() const { return m_Resources; }
    const std::vector<RGPass>& GetPasses() const { return m_Passes; }
    size_t GetPassCount() const { return m_Passes.size(); }
    size_t GetResourceCount() const { return m_Resources.size(); }

    uint32_t GetGeneration() const { return m_Generation; }

private:
    friend class RenderGraphBuilder;

    RGTextureHandle CreateTexture(const char* name, const RGTextureDesc& desc);
    RGBufferHandle CreateBuffer(const char* name, const RGBufferDesc& desc);

    bool DeclareUse(uint32_t pass_index,
                    uint32_t resource_index,
                    bool is_texture,
                    RGResourceState state,
                    bool is_write);
    void MarkExtracted(uint32_t resource_index);

    // ---- compiler stages ------------------------------------------------------------
    void ValidateResources();
    void BuildEdges(std::vector<std::vector<uint32_t>>& edges, std::vector<uint32_t>& indegree) const;
    bool TopologicalSort(const std::vector<std::vector<uint32_t>>& edges,
                         std::vector<uint32_t>& indegree,
                         std::vector<uint32_t>& order);
    void CullPasses(const std::vector<uint32_t>& order, std::vector<uint32_t>& kept) const;
    void ComputeLifetimes(const std::vector<uint32_t>& kept_order);
    void AliasMemory();
    void DeriveBarriers(const std::vector<uint32_t>& kept_order);
    void ComputeMemoryStats(const std::vector<uint32_t>& kept_order);

    void AddError(std::string message);
    void AddWarning(std::string message);

    /// Errors raised during Setup (before Compile() has a result object to write into).
    /// They are copied into RGCompiledGraph::errors when Compile() runs.
    void AddSetupError(std::string message);
    friend class RenderGraphBuilder;

    std::vector<std::string> m_SetupErrors;
    std::string m_Name;
    std::vector<RGResource> m_Resources;
    std::vector<RGPass> m_Passes;
    uint32_t m_Generation {1};
    bool m_CompiledUpToDate {false};
    RGCompiledGraph m_Compiled;
};
