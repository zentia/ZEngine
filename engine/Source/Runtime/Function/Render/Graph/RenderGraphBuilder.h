#pragma once

#include "Runtime/Function/Render/Graph/RenderGraphTypes.h"

#include <vector>

class RenderGraph;

// =====================================================================================
// RenderGraphBuilder -- the Setup-phase API a pass body talks to.
// -------------------------------------------------------------------------------------
// One builder is bound to ONE pass; RenderGraph::AddPass runs the setup callback with it
// and then throws the builder away. Everything the callback does is recorded into the
// graph:
//
//     graph.AddPass("Tonemap", RGPassType::Render,
//         [](RenderGraphBuilder& builder) {                 // Setup -- cheap, no RHI
//             auto hdr = builder.ReadTexture(g_hdr, RGResourceState::ShaderResource);
//             auto ldr = builder.CreateTexture("LDR", ldr_desc);
//             ldr = builder.WriteTexture(ldr, RGResourceState::RenderTarget);
//         },
//         [](RenderGraphExecuteContext& ctx) {              // Execute -- records commands
//             rhi->CmdBeginRenderPassPFN(ctx.GetCommandBuffer(), ...);
//         });
//
// Rules that the compiler enforces (they become validation errors, not UB):
//   * A handle you read/write must have come from THIS graph (Create* or Import*).
//   * Declaring the same resource twice in one pass with different states is a hazard.
//   * Reading a resource you also write in the same pass is a hazard (no RW textures).
// =====================================================================================

/// One declared use of one resource by one pass.
struct RGResourceUse
{
    uint32_t resource_index {kRGInvalidIndex};
    RGResourceState state {RGResourceState::Undefined};
    bool is_write {false};
};

class RenderGraphBuilder
{
public:
    /// Called by RenderGraph only.
    RenderGraphBuilder(RenderGraph& graph, uint32_t pass_index);

    // ---- resource creation ----------------------------------------------------------
    RGTextureHandle CreateTexture(const char* name, const RGTextureDesc& desc);
    RGBufferHandle CreateBuffer(const char* name, const RGBufferDesc& desc);

    /// Bring an existing texture into the graph (swapchain, a pass-owned RT, scene
    /// depth). Never allocated, never destroyed, never memory-aliased.
    RGTextureHandle ImportTexture(const char* name, const RGExternalTexture& external);
    RGBufferHandle ImportBuffer(const char* name, const RGExternalBuffer& external);

    // ---- dependency declaration -----------------------------------------------------
    /// Texture must be in `state` when this pass runs. Returns the handle for chaining.
    RGTextureHandle ReadTexture(RGTextureHandle handle, RGResourceState state = RGResourceState::ShaderResource);
    RGTextureHandle WriteTexture(RGTextureHandle handle, RGResourceState state = RGResourceState::RenderTarget);

    RGBufferHandle ReadBuffer(RGBufferHandle handle, RGResourceState state = RGResourceState::ShaderResource);
    RGBufferHandle WriteBuffer(RGBufferHandle handle, RGResourceState state = RGResourceState::UnorderedAccess);

    /// Keep a transient resource alive after the graph finished (UE's Extract). Its
    /// producer pass is therefore never culled.
    void KeepAlive(RGTextureHandle handle);
    void KeepAlive(RGBufferHandle handle);

    // ---- queries ---------------------------------------------------------------------
    bool IsDeclared(RGTextureHandle handle) const;
    bool IsDeclared(RGBufferHandle handle) const;

    size_t GetDeclaredReadCount() const { return m_Reads.size(); }
    size_t GetDeclaredWriteCount() const { return m_Writes.size(); }

private:
    /// Records a use; returns false (and logs) when the handle is not from this graph or
    /// clashes with an earlier declaration in the same pass.
    bool Declare(uint32_t resource_index, bool is_texture, RGResourceState state, bool is_write);

    RenderGraph& m_Graph;
    uint32_t m_PassIndex;
    std::vector<RGResourceUse> m_Reads;
    std::vector<RGResourceUse> m_Writes;

    friend class RenderGraph;
};
