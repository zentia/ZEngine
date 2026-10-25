// =====================================================================================
// RenderGraphSmokeTest -- headless checks for the graph compiler.
// -------------------------------------------------------------------------------------
// Links ONLY RenderGraphTypes.cpp + RenderGraphBuilder.cpp + RenderGraph.cpp: no RHI, no
// window, no GPU. Run it after touching anything in Function/Render/Graph/.
//
//   cmake -S . -B build -DZENGINE_BUILD_RENDERGRAPH_SMOKE_TEST=ON
//   cmake --build build --target RenderGraphSmokeTest
// =====================================================================================

#include "Runtime/Function/Render/Graph/RenderGraph.h"
#include "Runtime/Function/Render/Passes/MainCameraRp1GraphDecl.h"

#include <cstdio>
#include <string>
#include <vector>

namespace
{
    int g_Checks = 0;
    int g_Failures = 0;
    int g_Group = 0;

    void BeginGroup(const char* title)
    {
        ++g_Group;
        std::printf("[%d] %s\n", g_Group, title);
    }

    void Check(bool condition, const char* what)
    {
        ++g_Checks;
        if (!condition)
        {
            ++g_Failures;
            std::printf("    FAIL: %s\n", what);
        }
    }

#define CHECK(expr) Check((expr), #expr)

    // A 1920x1080 RGBA8 texture -- the size a post-process pass would ask for.
    RGTextureDesc FullscreenLDR(const char* name)
    {
        RGTextureDesc desc;
        desc.name = name;
        desc.width = 1920;
        desc.height = 1080;
        desc.format = RGFormat::R8G8B8A8_UNORM;
        desc.usage = RGTextureUsage::RenderTarget | RGTextureUsage::ShaderRead;
        return desc;
    }

    RGTextureDesc HalfSize(const char* name)
    {
        RGTextureDesc desc = FullscreenLDR(name);
        desc.width = 960;
        desc.height = 540;
        return desc;
    }

    // A fake swapchain target; the graph never dereferences the pointers.
    RHIImage* FakeImage() { return reinterpret_cast<RHIImage*>(0x1234); }
    RHIImageView* FakeView() { return reinterpret_cast<RHIImageView*>(0x5678); }

    /// Every test graph ends with a pass that writes the imported backbuffer, because a
    /// graph whose last write is transient has nothing worth doing and is culled away.
    RGTextureHandle ImportBackbuffer(RenderGraph& graph, RGResourceState state = RGResourceState::Undefined)
    {
        RGExternalTexture external;
        external.image = FakeImage();
        external.view = FakeView();
        external.state = state;
        return graph.ImportTexture("Backbuffer", external);
    }

    /// pass_order resolved to names, for readable assertions.
    std::vector<std::string> OrderNames(const RenderGraph& graph, const RGCompiledGraph& compiled)
    {
        std::vector<std::string> names;
        for (const uint32_t pass_index : compiled.pass_order)
        {
            names.push_back(graph.GetPasses()[pass_index].name);
        }
        return names;
    }

    void DumpErrors(const RGCompiledGraph& compiled)
    {
        for (const std::string& error : compiled.errors)
        {
            std::printf("    error: %s\n", error.c_str());
        }
        for (const std::string& warning : compiled.warnings)
        {
            std::printf("    warn : %s\n", warning.c_str());
        }
    }

    bool Contains(const std::vector<std::string>& values, const std::string& value)
    {
        for (const std::string& candidate : values)
        {
            if (candidate == value)
            {
                return true;
            }
        }
        return false;
    }

    /// Import a texture the caller owns -- a MainCameraFramebufferResources attachment
    /// or the swapchain. The graph never allocates, destroys or aliases these; it only
    /// derives the barriers to reach the state each pass asks for.
    RGTextureHandle ImportExternal(RenderGraph& graph, const char* name, RGResourceState state)
    {
        RGExternalTexture external;
        external.image = FakeImage();
        external.view = FakeView();
        external.state = state;
        return graph.ImportTexture(name, external);
    }

    /// The barrier a given pass slot emits for a resource, or nullptr when it emits
    /// none -- which is the interesting case, because "already in the target state"
    /// must produce no barrier at all.
    const RGBarrier* FindBarrier(const RGCompiledGraph& compiled, uint32_t pass_slot, uint32_t resource_index)
    {
        const uint32_t begin = compiled.barrier_offsets[pass_slot];
        const uint32_t end = compiled.barrier_offsets[pass_slot + 1];
        for (uint32_t i = begin; i < end; ++i)
        {
            if (compiled.barriers[i].resource_index == resource_index)
            {
                return &compiled.barriers[i];
            }
        }
        return nullptr;
    }

    // The handles of the RP2 chain, so the assertions can name resources instead of
    // indexing into them.
    struct Rp2Handles
    {
        RGTextureHandle backup_odd;
        RGTextureHandle backup_even;
        RGTextureHandle post_odd;
        RGTextureHandle swapchain;
    };

    /// Declare the DX12 desktop frame's RP2 post-process chain in a graph.
    ///
    /// This is MainCameraRp2Pass::DrawRP2 (MainCameraRp2Pass.cpp:611) expressed as
    /// declarations: ColorGrading -> (FXAA) -> CombineUI. Every texture is imported
    /// because MainCameraFramebufferResources owns them and they survive the frame, so
    /// the graph allocates nothing here -- what it contributes is the pass order and
    /// the barriers, which DrawRP2() currently writes by hand with two
    /// CmdPipelineBarrier calls (MainCameraRp2Pass.cpp:689 and :746).
    Rp2Handles BuildRp2(RenderGraph& graph, bool enable_fxaa)
    {
        Rp2Handles handles;

        // backup_odd arrives as a render target: RP1's forward-lighting pass just wrote
        // it. backup_even and post_odd are scratch left over from earlier frames, and
        // the swapchain comes back from Present.
        handles.backup_odd = ImportExternal(graph, "backup_odd", RGResourceState::RenderTarget);
        handles.backup_even = ImportExternal(graph, "backup_even", RGResourceState::Undefined);
        handles.post_odd = ImportExternal(graph, "post_odd", RGResourceState::Undefined);
        handles.swapchain = ImportExternal(graph, "Swapchain", RGResourceState::Undefined);

        // With FXAA on, ColorGrading renders into post_odd and FXAA resolves it into
        // backup_even; without it, ColorGrading goes straight to backup_even.
        const RGTextureHandle grading_output = enable_fxaa ? handles.post_odd : handles.backup_even;

        graph.AddPass(
            "ColorGrading",
            RGPassType::Render,
            [&](RenderGraphBuilder& builder) {
                builder.ReadTexture(handles.backup_odd, RGResourceState::ShaderResource);
                builder.WriteTexture(grading_output, RGResourceState::RenderTarget);
            },
            nullptr);

        if (enable_fxaa)
        {
            graph.AddPass(
                "FXAA",
                RGPassType::Render,
                [&](RenderGraphBuilder& builder) {
                    builder.ReadTexture(handles.post_odd, RGResourceState::ShaderResource);
                    builder.WriteTexture(handles.backup_even, RGResourceState::RenderTarget);
                },
                nullptr);
        }

        // Writes the swapchain, so it is a root: nothing in this chain can be culled.
        graph.AddPass(
            "CombineUI",
            RGPassType::Render,
            [&](RenderGraphBuilder& builder) {
                builder.ReadTexture(handles.backup_odd, RGResourceState::ShaderResource);
                builder.ReadTexture(handles.backup_even, RGResourceState::ShaderResource);
                builder.WriteTexture(handles.swapchain, RGResourceState::RenderTarget);
            },
            nullptr);

        return handles;
    }

    // The handles of the wiring's RP2 chain.
    struct Rp2WiredHandles
    {
        RGTextureHandle backup_odd;
        RGTextureHandle backup_even;
        RGTextureHandle post_odd;
        RGTextureHandle backbuffer;
    };

    /// The declaration DrawRP2WithGraph() builds, with the import states it uses.
    ///
    /// This differs from BuildRp2() above in exactly one way that matters: the import
    /// states are the ones the textures are ACTUALLY in when RP2 starts, not convenient
    /// guesses. DeriveBarriers() walks forward from the import state, so the two produce
    /// different barrier sets.
    Rp2WiredHandles BuildRp2Wiring(RenderGraph& graph, bool enable_fxaa)
    {
        Rp2WiredHandles handles;
        handles.backup_odd = ImportExternal(graph, "backup_odd", RGResourceState::ShaderResource);
        handles.backup_even = ImportExternal(graph, "backup_even", RGResourceState::ShaderResource);
        handles.backbuffer = ImportExternal(graph, "Backbuffer", RGResourceState::RenderTarget);

        RGTextureHandle grading_output = handles.backup_even;
        if (enable_fxaa)
        {
            handles.post_odd = ImportExternal(graph, "post_odd", RGResourceState::ShaderResource);
            grading_output = handles.post_odd;
        }

        graph.AddPass(
            "ColorGrading",
            RGPassType::Render,
            [&](RenderGraphBuilder& builder) {
                builder.ReadTexture(handles.backup_odd, RGResourceState::ShaderResource);
                builder.WriteTexture(grading_output, RGResourceState::RenderTarget);
            },
            nullptr);

        if (enable_fxaa)
        {
            graph.AddPass(
                "FXAA",
                RGPassType::Render,
                [&](RenderGraphBuilder& builder) {
                    builder.ReadTexture(handles.post_odd, RGResourceState::ShaderResource);
                    builder.WriteTexture(handles.backup_even, RGResourceState::RenderTarget);
                },
                nullptr);
        }

        // A side effect the graph cannot see: the UI overlay records its own render passes.
        // It declares nothing, so nothing pulls it out of position -- the topological sort
        // breaks ties by declaration index, which is what keeps it between the post chain
        // and CombineUI.
        graph.AddPass("UI", RGPassType::Render, [](RenderGraphBuilder&) {}, nullptr, RGPassFlags::NeverCull);

        graph.AddPass(
            "CombineUI",
            RGPassType::Render,
            [&](RenderGraphBuilder& builder) {
                builder.ReadTexture(handles.backup_odd, RGResourceState::ShaderResource);
                builder.ReadTexture(handles.backup_even, RGResourceState::ShaderResource);
                builder.WriteTexture(handles.backbuffer, RGResourceState::RenderTarget);
            },
            nullptr);

        return handles;
    }

    // -------------------------------------------------------------------------------------
    // RP1 (MainCameraRp1Pass) wiring -- the three-way-split deferred chain that step 2.3
    // already landed in code (DrawGBufferPass / DrawDeferredLightingPass / DrawForwardLightingPass,
    // each its own CmdBeginRenderPassPFN with subpass=0) but which has NO RenderGraph wiring
    // yet (only RP2 has DrawRP2WithGraph). This mirrors BuildRp2Wiring above: it declares the
    // REAL resource topology the split passes exchange, so the compiler derives the same order
    // and barriers the hand-written chain relies on -- proving the topology RENDER_GRAPH.md:117
    // claimed was "whole-segment only" IS expressible at the compiler level.
    //
    // Resource topology (from MainCameraFramebufferResources::Setup{GBuffer,DeferredLighting,
    // ForwardLighting}Pass + deferred_lighting.frag.hlsl / mesh_forward.frag.hlsl):
    //
    //   GBufferPass        writes GBufferA/B/C (transient RT), writes scene depth (imported).
    //   DeferredLighting   reads GBufferA/B/C + depth, writes BackupOdd (imported HDR output).
    //   ForwardLighting    writes BackupOdd (imported, blended), reads depth (imported).
    //
    // Where the graph's model and the real passes differ (re-verified 2026-10-25, see
    // PHASE2_PLAN.md 落地记录·2.4 勘误). This test asserts the compiler-level contract only:
    //   1. Descriptor type is NOT a gap on the backends that run this pass. MainCameraRp1Pass
    //      is built only for DX12 (inside DX12MainCameraPass) and Apple/Metal; Vulkan uses the
    //      older multi-subpass MainCameraPass. DX12 maps RHI_DESCRIPTOR_TYPE_INPUT_ATTACHMENT to
    //      a plain SRV (DX12RHI.cpp ToDX12DescriptorRangeType) and the HLSL shader reads
    //      Texture2D.Load(); Metal gets [[texture(n)]] via SPIRV-Cross. So "ShaderResource" is
    //      exactly what happens. (Running THIS pass on Vulkan would be illegal -- input
    //      attachments across independent render passes -- but no code path does that.)
    //   2. ForwardLighting's blend is fixed-function (loadOp=LOAD + blend state on a colour
    //      attachment), i.e. RenderTarget state. Declaring Forward as WriteTexture(RenderTarget)
    //      is therefore faithful, not a simplification; group 20 only rejects SAMPLING the
    //      same texture you render into (a real feedback-loop hazard).
    //   The real wiring gap is elsewhere: the render passes' own initialLayout/finalLayout
    //   already transition these attachments, so a graph-driven RP1 must import in the state
    //   the previous render pass LEFT the texture in (as DrawRP2WithGraph does).
    // -------------------------------------------------------------------------------------
    struct Rp1WiredHandles
    {
        RGTextureHandle gbuffer_a;
        RGTextureHandle gbuffer_b;
        RGTextureHandle gbuffer_c;
        RGTextureHandle depth;
        RGTextureHandle backup_odd;
    };

    /// A fullscreen depth texture -- the scene depth the GBuffer pass writes and the two
    /// lighting passes read. Imported as an external the GBuffer pass writes into.
    RGTextureDesc FullscreenDepth(const char* name)
    {
        RGTextureDesc desc;
        desc.name = name;
        desc.width = 1920;
        desc.height = 1080;
        desc.format = RGFormat::D32_SFLOAT;
        desc.usage = RGTextureUsage::DepthStencil | RGTextureUsage::ShaderRead;
        return desc;
    }

    Rp1WiredHandles BuildRp1Wiring(RenderGraph& graph)
    {
        Rp1WiredHandles handles;

        // Scene depth: an external the GBuffer pass writes (finalLayout DEPTH_STENCIL_READ_ONLY_OPTIMAL
        // on the real render pass) and both lighting passes read. Imported in the state it arrives in
        // (Undefined -- the GBuffer pass is its first writer), matching how the real RHI depth target
        // is cleared at the top of the frame before RP1 runs.
        handles.depth = ImportExternal(graph, "scene_depth", RGResourceState::Undefined);

        // BackupOdd: the HDR output the Deferred pass writes and the Forward pass writes again
        // (blended). Imported in the state it arrives in from the previous frame (ShaderResource --
        // RP1's own previous Forward/Deferred render pass ended it in SHADER_READ_ONLY_OPTIMAL).
        handles.backup_odd = ImportExternal(graph, "backup_odd", RGResourceState::ShaderResource);

        // GBufferPass: clears + writes GBufferA/B/C (transient) and the scene depth.
        graph.AddPass(
            "GBufferPass",
            RGPassType::Render,
            [&](RenderGraphBuilder& builder) {
                handles.gbuffer_a = builder.CreateTexture("GBufferA", FullscreenLDR("GBufferA"));
                handles.gbuffer_b = builder.CreateTexture("GBufferB", FullscreenLDR("GBufferB"));
                handles.gbuffer_c = builder.CreateTexture("GBufferC", FullscreenLDR("GBufferC"));
                builder.WriteTexture(handles.gbuffer_a, RGResourceState::RenderTarget);
                builder.WriteTexture(handles.gbuffer_b, RGResourceState::RenderTarget);
                builder.WriteTexture(handles.gbuffer_c, RGResourceState::RenderTarget);
                builder.WriteTexture(handles.depth, RGResourceState::DepthStencilWrite);
            },
            nullptr);

        // DeferredLightingPass: samples GBufferA/B/C + depth, writes BackupOdd.
        graph.AddPass(
            "DeferredLightingPass",
            RGPassType::Render,
            [&](RenderGraphBuilder& builder) {
                builder.ReadTexture(handles.gbuffer_a, RGResourceState::ShaderResource);
                builder.ReadTexture(handles.gbuffer_b, RGResourceState::ShaderResource);
                builder.ReadTexture(handles.gbuffer_c, RGResourceState::ShaderResource);
                builder.ReadTexture(handles.depth, RGResourceState::DepthStencilRead);
                builder.WriteTexture(handles.backup_odd, RGResourceState::RenderTarget);
            },
            nullptr);

        // ForwardLightingPass: blends transparent objects into BackupOdd + depth-tests against depth.
        // The blend is fixed-function on a colour attachment (loadOp=LOAD + blend state), which IS
        // the RenderTarget state -- so a plain WriteTexture(RenderTarget) is the faithful declaration.
        graph.AddPass(
            "ForwardLightingPass",
            RGPassType::Render,
            [&](RenderGraphBuilder& builder) {
                builder.ReadTexture(handles.depth, RGResourceState::DepthStencilRead);
                builder.WriteTexture(handles.backup_odd, RGResourceState::RenderTarget);
            },
            nullptr);

        return handles;
    }
}  // namespace

int main()
{
    // Unbuffered: if something aborts mid-test we still see how far it got.
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // ---------------------------------------------------------------------------------
    BeginGroup("a chain of passes compiles in declaration order");
    {
        RenderGraph graph("chain");
        const RGTextureHandle backbuffer = ImportBackbuffer(graph);

        RGTextureHandle gbuffer;
        RGTextureHandle lighting;

        graph.AddPass("Gbuffer",
                      RGPassType::Render,
                      [&](RenderGraphBuilder& builder) {
                          gbuffer = builder.CreateTexture("GBuffer", FullscreenLDR("GBuffer"));
                          gbuffer = builder.WriteTexture(gbuffer, RGResourceState::RenderTarget);
                      },
                      nullptr);

        graph.AddPass("Lighting",
                      RGPassType::Render,
                      [&](RenderGraphBuilder& builder) {
                          gbuffer = builder.ReadTexture(gbuffer, RGResourceState::ShaderResource);
                          lighting = builder.CreateTexture("Lighting", FullscreenLDR("Lighting"));
                          lighting = builder.WriteTexture(lighting, RGResourceState::RenderTarget);
                      },
                      nullptr);

        graph.AddPass("Present",
                      RGPassType::Render,
                      [&](RenderGraphBuilder& builder) {
                          lighting = builder.ReadTexture(lighting, RGResourceState::ShaderResource);
                          builder.WriteTexture(backbuffer, RGResourceState::RenderTarget);
                      },
                      nullptr);

        const RGCompiledGraph& compiled = graph.Compile();
        DumpErrors(compiled);
        CHECK(compiled.valid);
        CHECK(compiled.errors.empty());
        CHECK(compiled.pass_order.size() == 3);
        const std::vector<std::string> names = OrderNames(graph, compiled);
        CHECK(names.size() == 3 && names[0] == "Gbuffer" && names[1] == "Lighting" && names[2] == "Present");
        CHECK(compiled.barrier_offsets.size() == compiled.pass_order.size() + 1);
    }

    // ---------------------------------------------------------------------------------
    BeginGroup("a pass whose result nobody consumes is culled, and so is its producer");
    {
        RenderGraph graph("cull");
        const RGTextureHandle backbuffer = ImportBackbuffer(graph);

        RGTextureHandle dead;
        RGTextureHandle live;

        graph.AddPass("Dead",
                      RGPassType::Render,
                      [&](RenderGraphBuilder& builder) {
                          dead = builder.CreateTexture("Dead", FullscreenLDR("Dead"));
                          dead = builder.WriteTexture(dead, RGResourceState::RenderTarget);
                      },
                      nullptr);

        graph.AddPass("AlsoDead",
                      RGPassType::Render,
                      [&](RenderGraphBuilder& builder) {
                          // Consumes `dead` and produces something nobody reads either,
                          // so the whole chain is dead.
                          dead = builder.ReadTexture(dead, RGResourceState::ShaderResource);
                          const RGTextureHandle dead2 =
                              builder.CreateTexture("Dead2", FullscreenLDR("Dead2"));
                          builder.WriteTexture(dead2, RGResourceState::RenderTarget);
                      },
                      nullptr);

        graph.AddPass("Live",
                      RGPassType::Render,
                      [&](RenderGraphBuilder& builder) {
                          live = builder.CreateTexture("Live", FullscreenLDR("Live"));
                          live = builder.WriteTexture(live, RGResourceState::RenderTarget);
                          builder.WriteTexture(backbuffer, RGResourceState::RenderTarget);
                      },
                      nullptr);

        const RGCompiledGraph& compiled = graph.Compile();
        CHECK(compiled.valid);
        CHECK(compiled.pass_order.size() == 1);
        CHECK(compiled.culled_passes.size() == 2);
        const std::vector<std::string> names = OrderNames(graph, compiled);
        CHECK(!names.empty() && names[0] == "Live");
        // The culled texture has no lifetime in the executed order.
        CHECK(compiled.lifetimes[dead.index].first_pass == kRGInvalidIndex);
    }

    // ---------------------------------------------------------------------------------
    BeginGroup("NeverCull and Extract keep a pass alive");
    {
        RenderGraph graph("keep");
        RGTextureHandle extracted;

        graph.AddPass("SideEffect",
                      RGPassType::Compute,
                      [&](RenderGraphBuilder& builder) {
                          extracted = builder.CreateTexture("Extracted", FullscreenLDR("Extracted"));
                          extracted = builder.WriteTexture(extracted, RGResourceState::UnorderedAccess);
                          builder.KeepAlive(extracted);
                      },
                      nullptr,
                      RGPassFlags::NeverCull);

        const RGCompiledGraph& compiled = graph.Compile();
        CHECK(compiled.valid);
        CHECK(compiled.pass_order.size() == 1);
        CHECK(compiled.culled_passes.empty());
        CHECK(compiled.lifetimes[extracted.index].first_pass == 0);
        // Extracted resources must not share memory with anything.
        CHECK(compiled.lifetimes[extracted.index].memory_slot != kRGInvalidIndex);
        CHECK(!compiled.lifetimes[extracted.index].aliased);
    }

    // ---------------------------------------------------------------------------------
    BeginGroup("transient resources with disjoint lifetimes share one memory slot");
    {
        RenderGraph graph("alias");
        const RGTextureHandle backbuffer = ImportBackbuffer(graph);

        RGTextureHandle first;
        RGTextureHandle second;
        RGTextureHandle third;

        // Chain A: first -> backbuffer.  Chain B: second -> third -> backbuffer.
        // `first` is dead after pass 1, `second` only starts at pass 2, so they can
        // share one allocation.
        graph.AddPass("A0",
                      RGPassType::Render,
                      [&](RenderGraphBuilder& builder) {
                          first = builder.CreateTexture("First", FullscreenLDR("First"));
                          first = builder.WriteTexture(first, RGResourceState::RenderTarget);
                      },
                      nullptr);

        graph.AddPass("A1",
                      RGPassType::Render,
                      [&](RenderGraphBuilder& builder) {
                          first = builder.ReadTexture(first, RGResourceState::ShaderResource);
                          builder.WriteTexture(backbuffer, RGResourceState::RenderTarget);
                      },
                      nullptr);

        graph.AddPass("B0",
                      RGPassType::Render,
                      [&](RenderGraphBuilder& builder) {
                          second = builder.CreateTexture("Second", HalfSize("Second"));
                          second = builder.WriteTexture(second, RGResourceState::RenderTarget);
                      },
                      nullptr);

        graph.AddPass("B1",
                      RGPassType::Render,
                      [&](RenderGraphBuilder& builder) {
                          second = builder.ReadTexture(second, RGResourceState::ShaderResource);
                          builder.WriteTexture(backbuffer, RGResourceState::RenderTarget);
                      },
                      nullptr);

        const RGCompiledGraph& compiled = graph.Compile();
        CHECK(compiled.valid);
        CHECK(compiled.pass_order.size() == 4);
        CHECK(compiled.lifetimes[first.index].first_pass == 0);
        CHECK(compiled.lifetimes[first.index].last_pass == 1);
        CHECK(compiled.lifetimes[second.index].first_pass == 2);
        // Smaller than First, allocated later, no overlap -> reuses First's slot.
        CHECK(compiled.lifetimes[second.index].memory_slot == compiled.lifetimes[first.index].memory_slot);
        CHECK(compiled.lifetimes[second.index].aliased);
        CHECK(!compiled.lifetimes[first.index].aliased);
        CHECK(compiled.total_allocated_bytes == compiled.lifetimes[first.index].byte_size);
        CHECK(compiled.peak_live_bytes == compiled.lifetimes[first.index].byte_size);
        (void)third;
    }

    // ---------------------------------------------------------------------------------
    BeginGroup("overlapping lifetimes never share a slot");
    {
        RenderGraph graph("overlap");
        const RGTextureHandle backbuffer = ImportBackbuffer(graph);

        RGTextureHandle gbuffer;
        RGTextureHandle lighting;

        graph.AddPass("Gbuffer",
                      RGPassType::Render,
                      [&](RenderGraphBuilder& builder) {
                          gbuffer = builder.CreateTexture("GBuffer", FullscreenLDR("GBuffer"));
                          gbuffer = builder.WriteTexture(gbuffer, RGResourceState::RenderTarget);
                      },
                      nullptr);

        graph.AddPass("Lighting",
                      RGPassType::Render,
                      [&](RenderGraphBuilder& builder) {
                          gbuffer = builder.ReadTexture(gbuffer, RGResourceState::ShaderResource);
                          lighting = builder.CreateTexture("Lighting", FullscreenLDR("Lighting"));
                          lighting = builder.WriteTexture(lighting, RGResourceState::RenderTarget);
                      },
                      nullptr);

        graph.AddPass("Tonemap",
                      RGPassType::Render,
                      [&](RenderGraphBuilder& builder) {
                          lighting = builder.ReadTexture(lighting, RGResourceState::ShaderResource);
                          builder.WriteTexture(backbuffer, RGResourceState::RenderTarget);
                      },
                      nullptr);

        const RGCompiledGraph& compiled = graph.Compile();
        CHECK(compiled.valid);
        // GBuffer [0,1] and Lighting [1,2] overlap at pass 1.
        CHECK(compiled.lifetimes[gbuffer.index].memory_slot != compiled.lifetimes[lighting.index].memory_slot);
        CHECK(!compiled.lifetimes[lighting.index].aliased);
        CHECK(compiled.peak_live_bytes == compiled.lifetimes[gbuffer.index].byte_size * 2);
    }

    // ---------------------------------------------------------------------------------
    BeginGroup("barriers are derived from the state each pass asks for");
    {
        RenderGraph graph("barriers");
        // The backbuffer arrives already readable: the first pass that reads it must not
        // pay for a barrier.
        const RGTextureHandle backbuffer = ImportBackbuffer(graph, RGResourceState::ShaderResource);

        RGTextureHandle hdr;

        // Reading the backbuffer costs nothing: it was imported already readable.
        graph.AddPass("Scene",
                      RGPassType::Render,
                      [&](RenderGraphBuilder& builder) {
                          builder.ReadTexture(backbuffer, RGResourceState::ShaderResource);
                          hdr = builder.CreateTexture("HDR", FullscreenLDR("HDR"));
                          hdr = builder.WriteTexture(hdr, RGResourceState::RenderTarget);
                      },
                      nullptr);

        graph.AddPass("Composite",
                      RGPassType::Render,
                      [&](RenderGraphBuilder& builder) {
                          hdr = builder.ReadTexture(hdr, RGResourceState::ShaderResource);
                          builder.WriteTexture(backbuffer, RGResourceState::RenderTarget);
                      },
                      nullptr);

        const RGCompiledGraph& compiled = graph.Compile();
        CHECK(compiled.valid);
        CHECK(compiled.pass_order.size() == 2);

        // Scene only pays for the fresh HDR target; the backbuffer read is free.
        CHECK(compiled.barrier_offsets.size() == 3);
        CHECK(compiled.barrier_offsets[0] == 0);
        CHECK(compiled.barrier_offsets[1] == 1);
        CHECK(compiled.barriers[0].resource_index == hdr.index);
        CHECK(compiled.barriers[0].before == RGResourceState::Undefined);
        CHECK(compiled.barriers[0].after == RGResourceState::RenderTarget);

        // Composite: HDR RenderTarget -> ShaderResource, backbuffer ShaderResource ->
        // RenderTarget.
        CHECK(compiled.barrier_offsets[2] == 3);
        CHECK(compiled.barriers[1].resource_index == hdr.index);
        CHECK(compiled.barriers[1].before == RGResourceState::RenderTarget);
        CHECK(compiled.barriers[1].after == RGResourceState::ShaderResource);
        CHECK(compiled.barriers[2].resource_index == backbuffer.index);
        CHECK(compiled.barriers[2].before == RGResourceState::ShaderResource);
        CHECK(compiled.barriers[2].after == RGResourceState::RenderTarget);
        CHECK(compiled.barriers.size() == 3);
    }

    // ---------------------------------------------------------------------------------
    BeginGroup("declaring the same resource twice in one pass is rejected");
    {
        RenderGraph graph("hazard");
        const RGTextureHandle backbuffer = ImportBackbuffer(graph);
        RGTextureHandle target;
        bool write_twice_returned_valid = true;
        bool read_write_returned_valid = true;

        graph.AddPass("TwoWrites",
                      RGPassType::Render,
                      [&](RenderGraphBuilder& builder) {
                          target = builder.CreateTexture("Target", FullscreenLDR("Target"));
                          builder.WriteTexture(target, RGResourceState::RenderTarget);
                          const RGTextureHandle again =
                              builder.WriteTexture(target, RGResourceState::RenderTarget);
                          write_twice_returned_valid = again.IsValid();
                      },
                      nullptr);

        graph.AddPass("ReadAndWrite",
                      RGPassType::Render,
                      [&](RenderGraphBuilder& builder) {
                          builder.ReadTexture(target, RGResourceState::ShaderResource);
                          const RGTextureHandle again =
                              builder.WriteTexture(target, RGResourceState::RenderTarget);
                          read_write_returned_valid = again.IsValid();
                      },
                      nullptr);

        graph.AddPass("Present",
                      RGPassType::Render,
                      [&](RenderGraphBuilder& builder) {
                          builder.ReadTexture(target, RGResourceState::ShaderResource);
                          builder.WriteTexture(backbuffer, RGResourceState::RenderTarget);
                      },
                      nullptr);

        CHECK(!write_twice_returned_valid);
        CHECK(!read_write_returned_valid);
        const RGCompiledGraph& compiled = graph.Compile();
        CHECK(!compiled.valid);
        CHECK(!compiled.errors.empty());
    }

    // ---------------------------------------------------------------------------------
    BeginGroup("a handle from a previous frame is rejected");
    {
        RenderGraph graph("stale");
        RGTextureHandle stale;
        graph.AddPass("Make",
                      RGPassType::Render,
                      [&](RenderGraphBuilder& builder) {
                          stale = builder.CreateTexture("Old", FullscreenLDR("Old"));
                          stale = builder.WriteTexture(stale, RGResourceState::RenderTarget);
                      },
                      nullptr,
                      RGPassFlags::NeverCull);

        graph.Reset();

        bool stale_accepted = true;
        graph.AddPass("Reuse",
                      RGPassType::Render,
                      [&](RenderGraphBuilder& builder) {
                          const RGTextureHandle again =
                              builder.ReadTexture(stale, RGResourceState::ShaderResource);
                          stale_accepted = again.IsValid();
                      },
                      nullptr,
                      RGPassFlags::NeverCull);

        CHECK(!stale_accepted);
        const RGCompiledGraph& compiled = graph.Compile();
        CHECK(!compiled.valid);
    }

    // ---------------------------------------------------------------------------------
    BeginGroup("validation catches malformed declarations");
    {
        {
            RenderGraph graph("no-size");
            graph.AddPass("Bad",
                          RGPassType::Render,
                          [](RenderGraphBuilder& builder) {
                              RGTextureDesc desc;
                              desc.name = "Bad";
                              desc.format = RGFormat::R8G8B8A8_UNORM;
                              const RGTextureHandle handle = builder.CreateTexture("Bad", desc);
                              builder.WriteTexture(handle, RGResourceState::RenderTarget);
                          },
                          nullptr,
                          RGPassFlags::NeverCull);
            CHECK(!graph.Compile().valid);
        }
        {
            RenderGraph graph("no-format");
            graph.AddPass("Bad",
                          RGPassType::Render,
                          [](RenderGraphBuilder& builder) {
                              RGTextureDesc desc = FullscreenLDR("Bad");
                              desc.format = RGFormat::Unknown;
                              const RGTextureHandle handle = builder.CreateTexture("Bad", desc);
                              builder.WriteTexture(handle, RGResourceState::RenderTarget);
                          },
                          nullptr,
                          RGPassFlags::NeverCull);
            CHECK(!graph.Compile().valid);
        }
        {
            RenderGraph graph("read-with-write-state");
            graph.AddPass("Bad",
                          RGPassType::Render,
                          [](RenderGraphBuilder& builder) {
                              const RGTextureHandle handle =
                                  builder.CreateTexture("T", FullscreenLDR("T"));
                              builder.WriteTexture(handle, RGResourceState::RenderTarget);
                              builder.ReadTexture(handle, RGResourceState::RenderTarget);
                          },
                          nullptr,
                          RGPassFlags::NeverCull);
            CHECK(!graph.Compile().valid);
        }
        {
            // A texture that is read but never written cannot be produced.
            RenderGraph graph("never-written");
            const RGTextureHandle backbuffer = ImportBackbuffer(graph);
            graph.AddPass("Bad",
                          RGPassType::Render,
                          [&](RenderGraphBuilder& builder) {
                              const RGTextureHandle handle =
                                  builder.CreateTexture("T", FullscreenLDR("T"));
                              builder.ReadTexture(handle, RGResourceState::ShaderResource);
                              builder.WriteTexture(backbuffer, RGResourceState::RenderTarget);
                          },
                          nullptr);
            CHECK(!graph.Compile().valid);
        }
    }

    // ---------------------------------------------------------------------------------
    BeginGroup("write-after-read and write-after-write stay in declaration order");
    {
        RenderGraph graph("ordering");
        // Two ping-pong targets: every pass reads one and writes the other, so the only
        // thing keeping them in order is the read/write dependency the graph derives.
        const RGTextureHandle ping = ImportBackbuffer(graph);

        RGExternalTexture external;
        external.image = FakeImage();
        external.view = FakeView();
        external.state = RGResourceState::Undefined;
        const RGTextureHandle pong = graph.ImportTexture("Pong", external);

        graph.AddPass("First",
                      RGPassType::Render,
                      [&](RenderGraphBuilder& builder) {
                          builder.ReadTexture(ping, RGResourceState::ShaderResource);
                          builder.WriteTexture(pong, RGResourceState::RenderTarget);
                      },
                      nullptr);
        graph.AddPass("Second",
                      RGPassType::Render,
                      [&](RenderGraphBuilder& builder) {
                          builder.ReadTexture(pong, RGResourceState::ShaderResource);
                          builder.WriteTexture(ping, RGResourceState::RenderTarget);
                      },
                      nullptr);
        graph.AddPass("Third",
                      RGPassType::Render,
                      [&](RenderGraphBuilder& builder) {
                          builder.ReadTexture(ping, RGResourceState::ShaderResource);
                          builder.WriteTexture(pong, RGResourceState::RenderTarget);
                      },
                      nullptr);

        const RGCompiledGraph& compiled = graph.Compile();
        CHECK(compiled.valid);
        const std::vector<std::string> names = OrderNames(graph, compiled);
        CHECK(names.size() == 3 && names[0] == "First" && names[1] == "Second" && names[2] == "Third");
    }

    // ---------------------------------------------------------------------------------
    BeginGroup("buffers take part in the graph like textures do");
    {
        RenderGraph graph("buffers");
        RGBufferHandle counter;
        RHIBuffer* fake_buffer = reinterpret_cast<RHIBuffer*>(0x9abc);

        graph.AddPass("Build",
                      RGPassType::Compute,
                      [&](RenderGraphBuilder& builder) {
                          RGBufferDesc desc;
                          desc.name = "Counter";
                          desc.size_bytes = 1024;
                          desc.usage = RGBufferUsage::UnorderedAccess | RGBufferUsage::Indirect;
                          counter = builder.CreateBuffer("Counter", desc);
                          counter = builder.WriteBuffer(counter, RGResourceState::UnorderedAccess);
                      },
                      nullptr);

        graph.AddPass("Consume",
                      RGPassType::Compute,
                      [&](RenderGraphBuilder& builder) {
                          counter = builder.ReadBuffer(counter, RGResourceState::ShaderResource);
                      },
                      nullptr,
                      RGPassFlags::NeverCull);

        const RGCompiledGraph& compiled = graph.Compile();
        CHECK(compiled.valid);
        CHECK(compiled.pass_order.size() == 2);
        CHECK(compiled.barriers.size() == 2);
        CHECK(!compiled.barriers[0].is_texture);
        CHECK(compiled.barriers[0].after == RGResourceState::UnorderedAccess);
        CHECK(compiled.barriers[1].before == RGResourceState::UnorderedAccess);
        CHECK(compiled.barriers[1].after == RGResourceState::ShaderResource);
        CHECK(compiled.lifetimes[counter.index].byte_size == 1024);

        // A texture handle used as a buffer (and vice versa) is rejected.
        RenderGraph mixed("mixed");
        RGTextureHandle texture;
        mixed.AddPass("Make",
                      RGPassType::Render,
                      [&](RenderGraphBuilder& builder) {
                          texture = builder.CreateTexture("T", FullscreenLDR("T"));
                          texture = builder.WriteTexture(texture, RGResourceState::RenderTarget);
                      },
                      nullptr,
                      RGPassFlags::NeverCull);
        mixed.AddPass("Misuse",
                      RGPassType::Compute,
                      [&](RenderGraphBuilder& builder) {
                          builder.ReadBuffer(RGBufferHandle {texture.index, texture.generation},
                                             RGResourceState::ShaderResource);
                      },
                      nullptr,
                      RGPassFlags::NeverCull);
        (void)fake_buffer;
        CHECK(!mixed.Compile().valid);
    }

    // ---------------------------------------------------------------------------------
    BeginGroup("Compile is deterministic and idempotent");
    {
        RenderGraph graph("deterministic");
        const RGTextureHandle backbuffer = ImportBackbuffer(graph);
        RGTextureHandle a;

        graph.AddPass("A",
                      RGPassType::Render,
                      [&](RenderGraphBuilder& builder) {
                          a = builder.CreateTexture("A", FullscreenLDR("A"));
                          a = builder.WriteTexture(a, RGResourceState::RenderTarget);
                      },
                      nullptr);
        graph.AddPass("B",
                      RGPassType::Render,
                      [&](RenderGraphBuilder& builder) {
                          a = builder.ReadTexture(a, RGResourceState::ShaderResource);
                          builder.WriteTexture(backbuffer, RGResourceState::RenderTarget);
                      },
                      nullptr);

        const RGCompiledGraph& first = graph.Compile();
        const size_t barrier_count = first.barriers.size();
        const uint32_t memory_slot = first.lifetimes[a.index].memory_slot;
        const RGCompiledGraph& second = graph.Compile();
        CHECK(&first == &second);
        CHECK(second.barriers.size() == barrier_count);
        CHECK(second.lifetimes[a.index].memory_slot == memory_slot);
        CHECK(second.pass_order == first.pass_order);
    }

    // ---------------------------------------------------------------------------------
    BeginGroup("descriptor sizing and imported resources");
    {
        RGTextureDesc rgba8 = FullscreenLDR("RGBA8");
        CHECK(rgba8.GetByteSize() == 1920u * 1080u * 4u);
        RGTextureDesc hdr = FullscreenLDR("HDR");
        hdr.format = RGFormat::R16G16B16A16_SFLOAT;
        CHECK(hdr.GetByteSize() == 1920u * 1080u * 8u);
        RGTextureDesc mipped = FullscreenLDR("Mipped");
        mipped.mip_levels = 2;
        CHECK(mipped.GetByteSize() > rgba8.GetByteSize());

        RenderGraph graph("imported");
        const RGTextureHandle backbuffer = ImportBackbuffer(graph);
        graph.AddPass("Write",
                      RGPassType::Render,
                      [&](RenderGraphBuilder& builder) {
                          builder.WriteTexture(backbuffer, RGResourceState::RenderTarget);
                      },
                      nullptr);
        const RGCompiledGraph& compiled = graph.Compile();
        CHECK(compiled.valid);
        // Imported resources are never aliased and never counted as allocated.
        CHECK(compiled.lifetimes[backbuffer.index].memory_slot == kRGInvalidIndex);
        CHECK(compiled.total_allocated_bytes == 0);
        CHECK(graph.GetResources()[backbuffer.index].external);
    }

    // ---------------------------------------------------------------------------------
    // The declaration the pipeline integration will actually use. Getting this right
    // headless is what has to happen before any of it is allowed to touch a pixel:
    // DrawRP2() cannot be rewritten against a graph whose order and barriers have
    // never been checked against what the hand-written code does today.
    // ---------------------------------------------------------------------------------
    BeginGroup("RP2 post-process chain (no FXAA) matches DrawRP2");
    {
        RenderGraph graph("RP2");
        const Rp2Handles h = BuildRp2(graph, false);
        const RGCompiledGraph& compiled = graph.Compile();
        DumpErrors(compiled);
        CHECK(compiled.valid);
        CHECK(compiled.errors.empty());

        const std::vector<std::string> names = OrderNames(graph, compiled);
        CHECK(names.size() == 2);
        CHECK(names[0] == "ColorGrading");
        CHECK(names[1] == "CombineUI");
        CHECK(compiled.culled_passes.empty());

        // Every texture is imported, so the graph owns nothing and aliases nothing:
        // the honest statement about today's RP2 is that it has no transient to reuse.
        CHECK(compiled.total_allocated_bytes == 0);
        CHECK(compiled.lifetimes[h.backup_odd.index].memory_slot == kRGInvalidIndex);
        CHECK(compiled.lifetimes[h.backup_even.index].memory_slot == kRGInvalidIndex);
        CHECK(compiled.lifetimes[h.swapchain.index].memory_slot == kRGInvalidIndex);

        // ColorGrading: backup_odd RenderTarget -> ShaderResource, and backup_even
        // Undefined -> RenderTarget.
        const RGBarrier* odd_to_srv = FindBarrier(compiled, 0, h.backup_odd.index);
        CHECK(odd_to_srv != nullptr);
        CHECK(odd_to_srv->before == RGResourceState::RenderTarget);
        CHECK(odd_to_srv->after == RGResourceState::ShaderResource);
        const RGBarrier* even_to_rtv = FindBarrier(compiled, 0, h.backup_even.index);
        CHECK(even_to_rtv != nullptr);
        CHECK(even_to_rtv->before == RGResourceState::Undefined);
        CHECK(even_to_rtv->after == RGResourceState::RenderTarget);

        // CombineUI: backup_odd is already in ShaderResource, so it gets no second
        // barrier. This "already in the target state" rule is what stands in for the
        // hand-written visibility-only barrier at MainCameraRp2Pass.cpp:689, whose
        // oldLayout and newLayout are the same.
        CHECK(FindBarrier(compiled, 1, h.backup_odd.index) == nullptr);
        const RGBarrier* even_to_srv = FindBarrier(compiled, 1, h.backup_even.index);
        CHECK(even_to_srv != nullptr);
        CHECK(even_to_srv->before == RGResourceState::RenderTarget);
        CHECK(even_to_srv->after == RGResourceState::ShaderResource);
        const RGBarrier* swap_to_rtv = FindBarrier(compiled, 1, h.swapchain.index);
        CHECK(swap_to_rtv != nullptr);
        CHECK(swap_to_rtv->before == RGResourceState::Undefined);
        CHECK(swap_to_rtv->after == RGResourceState::RenderTarget);

        CHECK(compiled.barriers.size() == 4);
    }

    // ---------------------------------------------------------------------------------
    BeginGroup("RP2 post-process chain (FXAA) inserts the resolve pass");
    {
        RenderGraph graph("RP2-FXAA");
        const Rp2Handles h = BuildRp2(graph, true);
        const RGCompiledGraph& compiled = graph.Compile();
        DumpErrors(compiled);
        CHECK(compiled.valid);
        CHECK(compiled.errors.empty());

        const std::vector<std::string> names = OrderNames(graph, compiled);
        CHECK(names.size() == 3);
        CHECK(names[0] == "ColorGrading");
        CHECK(names[1] == "FXAA");
        CHECK(names[2] == "CombineUI");
        CHECK(compiled.culled_passes.empty());

        // With FXAA on, ColorGrading targets post_odd and leaves backup_even alone
        // until FXAA resolves into it. That is the whole point of the extra texture,
        // and the graph has to agree.
        CHECK(FindBarrier(compiled, 0, h.post_odd.index) != nullptr);
        CHECK(FindBarrier(compiled, 0, h.backup_even.index) == nullptr);

        const RGBarrier* post_to_srv = FindBarrier(compiled, 1, h.post_odd.index);
        CHECK(post_to_srv != nullptr);
        CHECK(post_to_srv->before == RGResourceState::RenderTarget);
        CHECK(post_to_srv->after == RGResourceState::ShaderResource);
        CHECK(FindBarrier(compiled, 1, h.backup_even.index) != nullptr);

        // CombineUI still reads both, and backup_odd still needs only one barrier.
        CHECK(FindBarrier(compiled, 2, h.backup_odd.index) == nullptr);
        const RGBarrier* swap_to_rtv = FindBarrier(compiled, 2, h.swapchain.index);
        CHECK(swap_to_rtv != nullptr);
        CHECK(swap_to_rtv->after == RGResourceState::RenderTarget);

        CHECK(compiled.barriers.size() == 6);
    }

    // ---------------------------------------------------------------------------------
    BeginGroup("RP2's documented SRV/RTV hazard is an error, not a comment");
    {
        // DrawRP2() carries a comment warning that the color-grading source and target
        // must be different textures, because loadOp=CLEAR would zero the target before
        // the fragment shader samples it (MainCameraRp2Pass.cpp:640). In the graph that
        // stops being something a reader has to notice: reading and writing one resource
        // in the same pass is rejected outright.
        RenderGraph graph("RP2-InPlace");
        const RGTextureHandle scene = ImportExternal(graph, "scene", RGResourceState::RenderTarget);
        graph.AddPass(
            "GradeInPlace",
            RGPassType::Render,
            [&](RenderGraphBuilder& builder) {
                builder.ReadTexture(scene, RGResourceState::ShaderResource);
                builder.WriteTexture(scene, RGResourceState::RenderTarget);
            },
            nullptr,
            RGPassFlags::NeverCull);

        const RGCompiledGraph& compiled = graph.Compile();
        CHECK(!compiled.valid);
        CHECK(!compiled.errors.empty());
    }

    // ---------------------------------------------------------------------------------
    // What unblocking the swapchain actually buys. Until RHISwapChainDesc carried images,
    // the back-buffer could not be imported at all: a barrier transitions an *image*,
    // and the only thing the swapchain exposed was views (whose getImage() is nullptr on
    // DX12). The import state is Present because that is where SubmitRendering leaves
    // every back-buffer (DX12RHI.cpp:5945); getting it wrong is the difference between
    // one correct barrier and a StateBefore the D3D12 debug layer rejects.
    // ---------------------------------------------------------------------------------
    BeginGroup("the swapchain round-trips Present -> RenderTarget -> CopySource");
    {
        RenderGraph graph("Swapchain");
        const RGTextureHandle swapchain = ImportExternal(graph, "Swapchain", RGResourceState::Present);

        graph.AddPass(
            "CombineUI",
            RGPassType::Render,
            [&](RenderGraphBuilder& builder) { builder.WriteTexture(swapchain, RGResourceState::RenderTarget); },
            nullptr);

        // A second writer in a row -- the editor's UI overlay draws into the same
        // back-buffer without an intervening present.
        graph.AddPass(
            "DebugOverlay",
            RGPassType::Render,
            [&](RenderGraphBuilder& builder) { builder.WriteTexture(swapchain, RGResourceState::RenderTarget); },
            nullptr);

        graph.AddPass(
            "Screenshot",
            RGPassType::Copy,
            [&](RenderGraphBuilder& builder) { builder.ReadTexture(swapchain, RGResourceState::CopySource); },
            nullptr,
            RGPassFlags::NeverCull);

        const RGCompiledGraph& compiled = graph.Compile();
        DumpErrors(compiled);
        CHECK(compiled.valid);
        CHECK(compiled.errors.empty());

        const std::vector<std::string> names = OrderNames(graph, compiled);
        CHECK(names.size() == 3);
        CHECK(names[0] == "CombineUI" && names[1] == "DebugOverlay" && names[2] == "Screenshot");

        // Imported, so never aliased and never counted against the frame budget --
        // writing the swapchain is a root all by itself, which is why nothing here is
        // culled even though no pass reads the swapchain inside the graph.
        CHECK(compiled.lifetimes[swapchain.index].memory_slot == kRGInvalidIndex);
        CHECK(compiled.total_allocated_bytes == 0);
        CHECK(compiled.culled_passes.empty());

        const RGBarrier* present_to_rtv = FindBarrier(compiled, 0, swapchain.index);
        CHECK(present_to_rtv != nullptr);
        CHECK(present_to_rtv->before == RGResourceState::Present);
        CHECK(present_to_rtv->after == RGResourceState::RenderTarget);

        // Already a render target, so the overlay adds no barrier. This matters more on
        // Vulkan than on DX12: a redundant RT -> RT transition is a tolerated no-op
        // there, but a vkCmdPipelineBarrier whose oldLayout misstates the current layout
        // is undefined behaviour, and VulkanRHI::CmdPipelineBarrier passes the layout
        // through verbatim.
        CHECK(FindBarrier(compiled, 1, swapchain.index) == nullptr);

        const RGBarrier* rtv_to_copy = FindBarrier(compiled, 2, swapchain.index);
        CHECK(rtv_to_copy != nullptr);
        CHECK(rtv_to_copy->before == RGResourceState::RenderTarget);
        CHECK(rtv_to_copy->after == RGResourceState::CopySource);

        CHECK(compiled.barriers.size() == 2);
    }

    // ---------------------------------------------------------------------------------
    // What the wiring actually asks the compiler for. The import states are the true ones:
    //   * backup_odd / backup_even -- ShaderResource, because RP1's forward-lighting pass
    //     and RP2's own HDR pass both end in finalLayout SHADER_READ_ONLY_OPTIMAL.
    //   * Backbuffer -- RenderTarget, NOT Present, because PrepareBeforePass transitions
    //     the swapchain to RENDER_TARGET and clears it at the top of every frame.
    // ---------------------------------------------------------------------------------
    BeginGroup("the wiring's real import states give RP2 exactly the transitions it needs");
    {
        {
            RenderGraph graph("RP2-Wired");
            const Rp2WiredHandles h = BuildRp2Wiring(graph, false);

            const RGCompiledGraph& compiled = graph.Compile();
            DumpErrors(compiled);
            CHECK(compiled.valid);
            CHECK(compiled.pass_order.size() == 3);
            const std::vector<std::string> names = OrderNames(graph, compiled);
            CHECK(names.size() == 3 && names[0] == "ColorGrading" && names[1] == "UI" &&
                  names[2] == "CombineUI");

            // backup_odd arrives in the state ColorGrading reads it, so it gets no barrier.
            // DrawRP2() records one by hand anyway (MainCameraRp2Pass.cpp:689); on DX12 that
            // barrier has never done anything, because TransitionImage() early-returns when
            // the tracked state already matches.
            CHECK(FindBarrier(compiled, 0, h.backup_odd.index) == nullptr);

            // backup_even round-trips: shader-readable -> render target for ColorGrading,
            // then back for CombineUI. The Backbuffer gets nothing at all.
            const RGBarrier* even_to_rt = FindBarrier(compiled, 0, h.backup_even.index);
            CHECK(even_to_rt != nullptr);
            CHECK(even_to_rt->before == RGResourceState::ShaderResource);
            CHECK(even_to_rt->after == RGResourceState::RenderTarget);

            const RGBarrier* even_to_srv = FindBarrier(compiled, 2, h.backup_even.index);
            CHECK(even_to_srv != nullptr);
            CHECK(even_to_srv->before == RGResourceState::RenderTarget);
            CHECK(even_to_srv->after == RGResourceState::ShaderResource);

            CHECK(FindBarrier(compiled, 2, h.backbuffer.index) == nullptr);
            CHECK(compiled.barriers.size() == 2);
        }

        {
            RenderGraph graph("RP2-Wired-Fxaa");
            const Rp2WiredHandles h = BuildRp2Wiring(graph, true);

            const RGCompiledGraph& compiled = graph.Compile();
            DumpErrors(compiled);
            CHECK(compiled.valid);
            CHECK(compiled.pass_order.size() == 4);
            const std::vector<std::string> names = OrderNames(graph, compiled);
            CHECK(names.size() == 4 && names[0] == "ColorGrading" && names[1] == "FXAA" &&
                  names[2] == "UI" && names[3] == "CombineUI");

            const RGBarrier* grading_out = FindBarrier(compiled, 0, h.post_odd.index);
            CHECK(grading_out != nullptr);
            CHECK(grading_out->before == RGResourceState::ShaderResource);
            CHECK(grading_out->after == RGResourceState::RenderTarget);

            const RGBarrier* fxaa_in = FindBarrier(compiled, 1, h.post_odd.index);
            CHECK(fxaa_in != nullptr);
            CHECK(fxaa_in->before == RGResourceState::RenderTarget);
            CHECK(fxaa_in->after == RGResourceState::ShaderResource);

            const RGBarrier* fxaa_out = FindBarrier(compiled, 1, h.backup_even.index);
            CHECK(fxaa_out != nullptr);
            CHECK(fxaa_out->before == RGResourceState::ShaderResource);
            CHECK(fxaa_out->after == RGResourceState::RenderTarget);

            const RGBarrier* combine_in = FindBarrier(compiled, 3, h.backup_even.index);
            CHECK(combine_in != nullptr);
            CHECK(combine_in->before == RGResourceState::RenderTarget);
            CHECK(combine_in->after == RGResourceState::ShaderResource);

            CHECK(FindBarrier(compiled, 3, h.backbuffer.index) == nullptr);
            CHECK(compiled.barriers.size() == 4);
        }
    }

    // ---------------------------------------------------------------------------------
    // RP1 three-way-split chain (step 2.4). The graph derives the same execution order
    // and the same inter-pass barriers the hand-written split passes rely on. This is
    // the compiler-level proof that the topology RENDER_GRAPH.md:117 flagged as
    // "whole-segment only" IS expressible. Note the import states here are the convenient
    // Undefined/ShaderResource ones; a real wiring imports in whatever state the previous
    // render pass's finalLayout left each texture in (see PHASE2_PLAN.md 落地记录·2.4).
    // ---------------------------------------------------------------------------------
    BeginGroup("RP1 three-way split: GBuffer -> DeferredLighting -> ForwardLighting");
    {
        RenderGraph graph("RP1-Wired");
        const Rp1WiredHandles h = BuildRp1Wiring(graph);

        const RGCompiledGraph& compiled = graph.Compile();
        DumpErrors(compiled);
        CHECK(compiled.valid);
        CHECK(compiled.errors.empty());
        CHECK(compiled.pass_order.size() == 3);
        const std::vector<std::string> names = OrderNames(graph, compiled);
        CHECK(names.size() == 3 && names[0] == "GBufferPass" && names[1] == "DeferredLightingPass" &&
              names[2] == "ForwardLightingPass");
        CHECK(compiled.culled_passes.empty());
        CHECK(compiled.barrier_offsets.size() == compiled.pass_order.size() + 1);

        // GBufferPass (slot 0): the three GBuffer textures and the depth start Undefined
        // and are written for the first time -- one barrier each.
        const RGBarrier* gbuffer_a_out = FindBarrier(compiled, 0, h.gbuffer_a.index);
        CHECK(gbuffer_a_out != nullptr);
        CHECK(gbuffer_a_out->before == RGResourceState::Undefined);
        CHECK(gbuffer_a_out->after == RGResourceState::RenderTarget);

        const RGBarrier* depth_out = FindBarrier(compiled, 0, h.depth.index);
        CHECK(depth_out != nullptr);
        CHECK(depth_out->before == RGResourceState::Undefined);
        CHECK(depth_out->after == RGResourceState::DepthStencilWrite);

        // DeferredLightingPass (slot 1): flips each GBuffer to a shader-readable state,
        // lifts depth from the write-layout to the read-layout, and writes BackupOdd.
        const RGBarrier* gbuffer_a_srv = FindBarrier(compiled, 1, h.gbuffer_a.index);
        CHECK(gbuffer_a_srv != nullptr);
        CHECK(gbuffer_a_srv->before == RGResourceState::RenderTarget);
        CHECK(gbuffer_a_srv->after == RGResourceState::ShaderResource);

        const RGBarrier* depth_to_read = FindBarrier(compiled, 1, h.depth.index);
        CHECK(depth_to_read != nullptr);
        CHECK(depth_to_read->before == RGResourceState::DepthStencilWrite);
        CHECK(depth_to_read->after == RGResourceState::DepthStencilRead);

        const RGBarrier* backup_to_rt = FindBarrier(compiled, 1, h.backup_odd.index);
        CHECK(backup_to_rt != nullptr);
        CHECK(backup_to_rt->before == RGResourceState::ShaderResource);
        CHECK(backup_to_rt->after == RGResourceState::RenderTarget);

        // ForwardLightingPass (slot 2): depth is already DepthStencilRead and BackupOdd is
        // already RenderTarget -- same-state writes must produce NO barrier (exactly the
        // "already in target state" case DeriveBarriers skips).
        CHECK(FindBarrier(compiled, 2, h.depth.index) == nullptr);
        CHECK(FindBarrier(compiled, 2, h.backup_odd.index) == nullptr);

        // Total barrier count: 4 (GBuffer) + 5 (Deferred) + 0 (Forward) = 9.
        CHECK(compiled.barriers.size() == 9);

        // The GBuffer textures are transient: produced by GBufferPass, last read by
        // DeferredLightingPass -- so they can be memory-aliased and freed after slot 1.
        CHECK(compiled.lifetimes[h.gbuffer_a.index].first_pass == 0);
        CHECK(compiled.lifetimes[h.gbuffer_a.index].last_pass == 1);
        CHECK(compiled.lifetimes[h.gbuffer_b.index].first_pass == 0);
        CHECK(compiled.lifetimes[h.gbuffer_b.index].last_pass == 1);
        CHECK(compiled.lifetimes[h.gbuffer_c.index].first_pass == 0);
        CHECK(compiled.lifetimes[h.gbuffer_c.index].last_pass == 1);
        // BackupOdd is external (imported): it lives across the whole chain and is never
        // aliased -- the Deferred pass is its producer, Forward its last writer.
        CHECK(compiled.lifetimes[h.backup_odd.index].first_pass == 1);
        CHECK(compiled.lifetimes[h.backup_odd.index].last_pass == 2);
        CHECK(!compiled.lifetimes[h.backup_odd.index].aliased);
    }

    // ---------------------------------------------------------------------------------
    // What the graph DOES reject: sampling a texture (ShaderResource) in the same pass that
    // renders into it (RenderTarget) -- a genuine feedback-loop hazard, and a validation
    // error rather than UB (RenderGraphBuilder::Declare). This is NOT a limit for RP1:
    // Forward's alpha blend is fixed-function on the attachment and needs no shader read
    // (see BuildRp1Wiring). The group guards against someone "modelling" the blend as a
    // sampled read + write, which would describe a different (illegal) pass.
    // ---------------------------------------------------------------------------------
    BeginGroup("RP1 guard: sampling BackupOdd while rendering into it is a hard error");
    {
        RenderGraph graph("RP1-Blend-Hazard");
        Rp1WiredHandles h;
        h.depth = ImportExternal(graph, "scene_depth", RGResourceState::Undefined);
        h.backup_odd = ImportExternal(graph, "backup_odd", RGResourceState::ShaderResource);
        h.gbuffer_a = ImportExternal(graph, "GBufferA", RGResourceState::ShaderResource);
        h.gbuffer_b = ImportExternal(graph, "GBufferB", RGResourceState::ShaderResource);
        h.gbuffer_c = ImportExternal(graph, "GBufferC", RGResourceState::ShaderResource);

        graph.AddPass(
            "ForwardLightingWithBlend",
            RGPassType::Render,
            [&](RenderGraphBuilder& builder) {
                builder.ReadTexture(h.gbuffer_a, RGResourceState::ShaderResource);
                builder.ReadTexture(h.gbuffer_b, RGResourceState::ShaderResource);
                builder.ReadTexture(h.gbuffer_c, RGResourceState::ShaderResource);
                builder.ReadTexture(h.depth, RGResourceState::DepthStencilRead);
                // Illegal on purpose: sample BackupOdd AND render into it in the same pass.
                builder.ReadTexture(h.backup_odd, RGResourceState::ShaderResource);
                builder.WriteTexture(h.backup_odd, RGResourceState::RenderTarget);
            },
            nullptr);

        const RGCompiledGraph& compiled = graph.Compile();
        DumpErrors(compiled);
        CHECK(!compiled.valid);
        CHECK(!compiled.errors.empty());
    }

    // ---------------------------------------------------------------------------------
    // The PRODUCTION RP1 declaration (Passes/MainCameraRp1GraphDecl.h, the exact code
    // MainCameraRp1Pass::DrawRP1WithGraph runs) with its REAL import states -- groups 19/20
    // above use convenient Undefined imports. Two topologies: with scene depth tracked
    // (a backend that publishes the depth RHIImage) and without (DX12 today:
    // GetDepthImageInfo().depth_image == nullptr).
    // ---------------------------------------------------------------------------------
    BeginGroup("production RP1 declaration (DeclareRp1Graph) with real import states");
    {
        for (const bool track_depth : {true, false})
        {
            RenderGraph graph(track_depth ? "RP1-Prod-Depth" : "RP1-Prod-NoDepth");
            Rp1GraphTextures textures {};
            const auto fake = [](RGFormat format) {
                RGExternalTexture t;
                t.image = FakeImage();
                t.view = FakeView();
                t.format = format;
                return t;
            };
            textures.gbuffer_a = fake(RGFormat::R8G8B8A8_UNORM);
            textures.gbuffer_b = fake(RGFormat::R8G8B8A8_UNORM);
            textures.gbuffer_c = fake(RGFormat::R8G8B8A8_SRGB);
            textures.backup_odd = fake(RGFormat::R16G16B16A16_SFLOAT);
            if (track_depth)
            {
                textures.depth = fake(RGFormat::D32_SFLOAT);
            }
            const Rp1GraphHandles h =
                DeclareRp1Graph(graph, MakeRp1GraphImportStates(textures), Rp1GraphCallbacks {});

            const RGCompiledGraph& compiled = graph.Compile();
            DumpErrors(compiled);
            CHECK(compiled.valid);
            CHECK(compiled.culled_passes.empty());
            const std::vector<std::string> names = OrderNames(graph, compiled);
            CHECK(names.size() == 3 && names[0] == "GBufferPass" && names[1] == "DeferredLightingPass" &&
                  names[2] == "ForwardLightingPass");
            CHECK(h.depth.IsValid() == track_depth);
            // Every RP1 texture is imported: nothing transient, nothing for the pool.
            CHECK(compiled.peak_live_bytes == 0);

            // GBufferPass: GBuffer ShaderResource (last frame's finalLayout) -> RenderTarget.
            for (const RGTextureHandle g : {h.gbuffer_a, h.gbuffer_b, h.gbuffer_c})
            {
                const RGBarrier* to_rt = FindBarrier(compiled, 0, g.index);
                CHECK(to_rt != nullptr && to_rt->before == RGResourceState::ShaderResource &&
                      to_rt->after == RGResourceState::RenderTarget);
                const RGBarrier* to_srv = FindBarrier(compiled, 1, g.index);
                CHECK(to_srv != nullptr && to_srv->before == RGResourceState::RenderTarget &&
                      to_srv->after == RGResourceState::ShaderResource);
                CHECK(FindBarrier(compiled, 2, g.index) == nullptr);
            }
            // BackupOdd: ShaderResource (RP2 sampled it last frame) -> RenderTarget once,
            // before Deferred; Forward keeps rendering into it with no barrier.
            CHECK(FindBarrier(compiled, 0, h.backup_odd.index) == nullptr);
            const RGBarrier* backup_rt = FindBarrier(compiled, 1, h.backup_odd.index);
            CHECK(backup_rt != nullptr && backup_rt->before == RGResourceState::ShaderResource &&
                  backup_rt->after == RGResourceState::RenderTarget);
            CHECK(FindBarrier(compiled, 2, h.backup_odd.index) == nullptr);

            if (track_depth)
            {
                // Imported as DepthStencilWrite (Forward's finalLayout): GBuffer needs none.
                CHECK(FindBarrier(compiled, 0, h.depth.index) == nullptr);
                const RGBarrier* depth_read = FindBarrier(compiled, 1, h.depth.index);
                CHECK(depth_read != nullptr && depth_read->before == RGResourceState::DepthStencilWrite &&
                      depth_read->after == RGResourceState::DepthStencilRead);
                CHECK(FindBarrier(compiled, 2, h.depth.index) == nullptr);
                CHECK(compiled.barriers.size() == 8);  // 3 + 5 + 0
            }
            else
            {
                CHECK(compiled.barriers.size() == 7);  // 3 + 4 + 0
            }
        }
    }

    std::printf("\n%d checks, %d failures\n", g_Checks, g_Failures);
    if (g_Failures != 0)
    {
        std::printf("RenderGraphSmokeTest: FAIL\n");
        return 1;
    }
    std::printf("RenderGraphSmokeTest: PASS\n");
    return 0;
}
