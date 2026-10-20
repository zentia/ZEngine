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

    std::printf("\n%d checks, %d failures\n", g_Checks, g_Failures);
    if (g_Failures != 0)
    {
        std::printf("RenderGraphSmokeTest: FAIL\n");
        return 1;
    }
    std::printf("RenderGraphSmokeTest: PASS\n");
    return 0;
}
