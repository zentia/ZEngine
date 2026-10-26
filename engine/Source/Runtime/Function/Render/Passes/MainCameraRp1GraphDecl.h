#pragma once

// =====================================================================================
// MainCameraRp1GraphDecl -- the RenderGraph declaration of RP1
// (GBufferPass -> DeferredLightingPass -> ForwardLightingPass), shared by
// MainCameraRp1Pass::DrawRP1WithGraph() and the headless smoke tests.
// -------------------------------------------------------------------------------------
// Header-only and dependent on RenderGraph.h alone (pure stdlib), so the WASM compiler /
// executor tests assert on EXACTLY the graph production builds -- not a hand copy that
// can drift. (It is a header and not a Passes/*.cpp on purpose: the Windows build only
// keeps a whitelist of Passes/*.cpp, see engine/Source/Runtime/CMakeLists.txt.)
//
// Every RP1 texture is IMPORTED, none is transient: the three render passes and their
// framebuffers are created once by MainCameraFramebufferResources and bound to those
// specific image views, so a pool-allocated texture would not be the one the framebuffer
// renders into. The graph contributes ordering, dependency validation and the
// inter-pass barriers; allocation stays with the framebuffer resources.
//
// Import states are the states the textures are ACTUALLY in when RP1 starts (DX12 tracks
// them per subresource; they are what the previous frame's render passes left behind via
// their finalLayout):
//
//   GBufferA/B/C  ShaderResource     GBuffer pass finalLayout = SHADER_READ_ONLY, and the
//                                     Deferred pass keeps them there (initial = final).
//   depth         DepthStencilWrite  Forward pass finalLayout = DEPTH_STENCIL_ATTACHMENT.
//   backup_odd    ShaderResource     Forward pass finalLayout = SHADER_READ_ONLY (RP2
//                                     samples it), same assumption DrawRP2WithGraph makes.
//
// Inside a pass the render pass itself still moves its attachments (DX12:
// TransitionSubpassAttachments at Begin, finalLayout at End). The graph does not see
// that, so on the way OUT of a pass its notion of the state can be one step behind the
// truth (it thinks GBuffer is RenderTarget when End already left it ShaderResource).
// That is harmless on DX12 -- DX12RHI::CmdPipelineBarrier ignores oldLayout and
// transitions from its own tracked state, so the "stale" barriers degenerate to the same
// no-ops the hand-written barrier group already was -- and Metal's CmdPipelineBarrier is
// a no-op. It would NOT be valid on Vulkan (oldLayout must be exact), which is fine
// because Vulkan does not run MainCameraRp1Pass (it uses the multi-subpass
// MainCameraPass); DrawRP1WithGraph refuses every backend but DX12.
// =====================================================================================

#include "Runtime/Function/Render/Graph/RenderGraph.h"

#include <utility>

/// The five textures RP1 touches, as the caller imports them.
struct Rp1GraphTextures
{
    RGExternalTexture gbuffer_a;
    RGExternalTexture gbuffer_b;
    RGExternalTexture gbuffer_c;
    /// Optional. `depth.image == nullptr` means "the graph does not track depth": DX12's
    /// GetDepthImageInfo() publishes no RHIImage for the scene depth today, so there is
    /// nothing to hang a barrier on (the hand-written depth barrier is skipped for the
    /// same reason). The render passes still own every depth transition in that case.
    RGExternalTexture depth;
    RGExternalTexture backup_odd;
};

struct Rp1GraphHandles
{
    RGTextureHandle gbuffer_a;
    RGTextureHandle gbuffer_b;
    RGTextureHandle gbuffer_c;
    RGTextureHandle depth;  // invalid when depth is not tracked
    RGTextureHandle backup_odd;
};

struct Rp1GraphCallbacks
{
    RGExecuteFn gbuffer;
    RGExecuteFn deferred_lighting;
    RGExecuteFn forward_lighting;
};

/// The import states DrawRP1WithGraph uses (see the header comment for why).
inline Rp1GraphTextures MakeRp1GraphImportStates(Rp1GraphTextures textures)
{
    textures.gbuffer_a.state = RGResourceState::ShaderResource;
    textures.gbuffer_b.state = RGResourceState::ShaderResource;
    textures.gbuffer_c.state = RGResourceState::ShaderResource;
    textures.depth.state = RGResourceState::DepthStencilWrite;
    textures.backup_odd.state = RGResourceState::ShaderResource;
    return textures;
}

/// Declares the RP1 chain into `graph` (which the caller has Reset()). Pass names are
/// stable -- the tests and the GPU debug markers rely on them.
inline Rp1GraphHandles DeclareRp1Graph(RenderGraph& graph,
                                       const Rp1GraphTextures& textures,
                                       Rp1GraphCallbacks callbacks)
{
    Rp1GraphHandles h;
    h.gbuffer_a = graph.ImportTexture("GBufferA", textures.gbuffer_a);
    h.gbuffer_b = graph.ImportTexture("GBufferB", textures.gbuffer_b);
    h.gbuffer_c = graph.ImportTexture("GBufferC", textures.gbuffer_c);
    if (textures.depth.image != nullptr)
    {
        h.depth = graph.ImportTexture("SceneDepth", textures.depth);
    }
    h.backup_odd = graph.ImportTexture("BackupOdd", textures.backup_odd);

    // GBufferPass: clears and renders GBufferA/B/C + depth.
    graph.AddPass(
        "GBufferPass",
        RGPassType::Render,
        [h](RenderGraphBuilder& builder) {
            builder.WriteTexture(h.gbuffer_a, RGResourceState::RenderTarget);
            builder.WriteTexture(h.gbuffer_b, RGResourceState::RenderTarget);
            builder.WriteTexture(h.gbuffer_c, RGResourceState::RenderTarget);
            if (h.depth.IsValid())
            {
                builder.WriteTexture(h.depth, RGResourceState::DepthStencilWrite);
            }
        },
        std::move(callbacks.gbuffer));

    // DeferredLightingPass (+ Sky): samples the GBuffer and depth (Texture2D.Load on DX12;
    // the descriptor type says INPUT_ATTACHMENT but DX12 maps that to a plain SRV), clears
    // and renders BackupOdd.
    graph.AddPass(
        "DeferredLightingPass",
        RGPassType::Render,
        [h](RenderGraphBuilder& builder) {
            builder.ReadTexture(h.gbuffer_a, RGResourceState::ShaderResource);
            builder.ReadTexture(h.gbuffer_b, RGResourceState::ShaderResource);
            builder.ReadTexture(h.gbuffer_c, RGResourceState::ShaderResource);
            if (h.depth.IsValid())
            {
                builder.ReadTexture(h.depth, RGResourceState::DepthStencilRead);
            }
            builder.WriteTexture(h.backup_odd, RGResourceState::RenderTarget);
        },
        std::move(callbacks.deferred_lighting));

    // ForwardLightingPass: transparent meshes, editor axis, particle billboards. Alpha
    // blending into BackupOdd is fixed-function on the colour attachment (loadOp=LOAD +
    // blend state; no shader reads BackupOdd), i.e. plain RenderTarget. Depth is tested
    // read-only.
    graph.AddPass(
        "ForwardLightingPass",
        RGPassType::Render,
        [h](RenderGraphBuilder& builder) {
            if (h.depth.IsValid())
            {
                builder.ReadTexture(h.depth, RGResourceState::DepthStencilRead);
            }
            builder.WriteTexture(h.backup_odd, RGResourceState::RenderTarget);
        },
        std::move(callbacks.forward_lighting));

    return h;
}
