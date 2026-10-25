// =====================================================================================
// RenderGraphExecuteSmokeTest -- headless checks for the Execute half of the graph.
// -------------------------------------------------------------------------------------
// RenderGraphSmokeTest covers Setup/Compile. This one covers what happens after: the
// transient resource pool and the executor -- handle resolution, pass ordering, barrier
// batching, transient recycling, culled-resource skipping and extracted-resource
// lifetime. No GPU, no window, no real RHI: the executor only talks to
// IRGResourceDevice, so a ten-method stub is enough to observe everything it does.
//
//   cmake -S . -B build -DZENGINE_BUILD_RENDERGRAPH_EXECUTE_SMOKE_TEST=ON
//   cmake --build build --target RenderGraphExecuteSmokeTest
//
// Run it after touching anything in Function/Render/Graph/.
// =====================================================================================

#include "Runtime/Function/Render/Graph/RenderGraphResources.h"

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

    // ---------------------------------------------------------------------------------
    // Sentinel RHI objects. The RHI types are opaque to the graph -- it only ever
    // stores and compares the pointers -- so real addresses of empty objects are enough
    // to tell "allocated" from "not allocated" and to identify which object a barrier
    // was recorded for.
    // ---------------------------------------------------------------------------------
    constexpr int kSentinelCount = 256;
    int s_NextImage = 0;
    int s_NextView = 0;
    int s_NextBuffer = 0;
    int s_NextMemory = 0;
    RHIImage s_Images[kSentinelCount];
    RHIImageView s_Views[kSentinelCount];
    RHIBuffer s_Buffers[kSentinelCount];
    RHIDeviceMemory s_Memory[kSentinelCount];
    RHICommandBuffer s_CommandBuffer;

    /// A ten-method stand-in for the RHI. Counts everything the executor does so the
    /// test can assert on allocation churn and barrier batching without a GPU.
    class FakeDevice final : public IRGResourceDevice
    {
    public:
        int images_created {0};
        int views_created {0};
        int buffers_created {0};
        int images_destroyed {0};
        int views_destroyed {0};
        int buffers_destroyed {0};
        int memory_freed {0};
        int barrier_calls {0};
        int events_pushed {0};
        int events_popped {0};

        struct BarrierRecord
        {
            RHIImage* image {nullptr};
            RHIImageLayout old_layout {RHI_IMAGE_LAYOUT_UNDEFINED};
            RHIImageLayout new_layout {RHI_IMAGE_LAYOUT_UNDEFINED};
            RHIImageAspectFlags aspect {0};
            int call_index {0};  // which CmdPipelineBarrier call (0-based) recorded it
        };
        std::vector<BarrierRecord> barriers;

        void CreateImage(uint32_t,
                         uint32_t,
                         RHIFormat,
                         RHIImageTiling,
                         RHIImageUsageFlags,
                         RHIMemoryPropertyFlags,
                         RHIImage*& image,
                         RHIDeviceMemory*& memory,
                         RHIImageCreateFlags,
                         uint32_t,
                         uint32_t) override
        {
            image = &s_Images[s_NextImage++ % kSentinelCount];
            memory = &s_Memory[s_NextMemory++ % kSentinelCount];
            ++images_created;
        }

        void CreateImageView(RHIImage*,
                             RHIFormat,
                             RHIImageAspectFlags,
                             RHIImageViewType,
                             uint32_t,
                             uint32_t,
                             RHIImageView*& image_view) override
        {
            image_view = &s_Views[s_NextView++ % kSentinelCount];
            ++views_created;
        }

        void CreateBuffer(RHIDeviceSize,
                          RHIBufferUsageFlags,
                          RHIMemoryPropertyFlags,
                          RHIBuffer*& buffer,
                          RHIDeviceMemory*& buffer_memory) override
        {
            buffer = &s_Buffers[s_NextBuffer++ % kSentinelCount];
            buffer_memory = &s_Memory[s_NextMemory++ % kSentinelCount];
            ++buffers_created;
        }

        void DestroyImageView(RHIImageView*) override { ++views_destroyed; }
        void DestroyImage(RHIImage*) override { ++images_destroyed; }

        void DestroyBuffer(RHIBuffer*& buffer) override
        {
            ++buffers_destroyed;
            buffer = nullptr;
        }

        void FreeMemory(RHIDeviceMemory*& memory) override
        {
            ++memory_freed;
            memory = nullptr;
        }

        void CmdPipelineBarrier(RHICommandBuffer*,
                                RHIPipelineStageFlags,
                                RHIPipelineStageFlags,
                                RHIDependencyFlags,
                                uint32_t,
                                const RHIMemoryBarrier*,
                                uint32_t,
                                const RHIBufferMemoryBarrier*,
                                uint32_t image_memory_barrier_count,
                                const RHIImageMemoryBarrier* p_image_memory_barriers) override
        {
            ++barrier_calls;
            for (uint32_t i = 0; i < image_memory_barrier_count; ++i)
            {
                BarrierRecord record;
                record.image = p_image_memory_barriers[i].image;
                record.old_layout = p_image_memory_barriers[i].oldLayout;
                record.new_layout = p_image_memory_barriers[i].newLayout;
                record.aspect = p_image_memory_barriers[i].subresourceRange.aspectMask;
                record.call_index = barrier_calls - 1;
                barriers.push_back(record);
            }
        }

        void PushEvent(RHICommandBuffer*, const char*, const float*) override { ++events_pushed; }
        void PopEvent(RHICommandBuffer*) override { ++events_popped; }
    };

    // ---- graph fixtures --------------------------------------------------------------

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

    RGBufferDesc SmallBuffer(const char* name)
    {
        RGBufferDesc desc;
        desc.name = name;
        desc.size_bytes = 4096;
        desc.usage = RGBufferUsage::UnorderedAccess | RGBufferUsage::ShaderRead;
        return desc;
    }

    /// Every test graph ends with a pass that writes the imported backbuffer, because a
    /// graph whose last write is transient has nothing worth doing and is culled away.
    RGTextureHandle ImportBackbuffer(RenderGraph& graph)
    {
        RGExternalTexture external;
        external.image = &s_Images[0];
        external.view = &s_Views[0];
        external.state = RGResourceState::Undefined;
        return graph.ImportTexture("Backbuffer", external);
    }
}  // namespace

int main()
{
    // Unbuffered: if something aborts mid-test we still see how far it got.
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // ---------------------------------------------------------------------------------
    BeginGroup("the pool hands a released texture back instead of reallocating");
    {
        FakeDevice device;
        RenderGraphResourcePool pool(&device);
        const RGTextureDesc desc = FullscreenLDR("A");

        const RGPhysicalTexture first = pool.AcquireTexture(desc);
        CHECK(first.image != nullptr);
        CHECK(first.view != nullptr);
        CHECK(first.memory != nullptr);
        CHECK(pool.GetLiveObjectCount() == 1);
        CHECK(pool.GetIdleTextureCount() == 0);

        // Releasing only parks it -- nothing is destroyed until the pool is full or
        // Flush() runs.
        pool.ReleaseTexture(desc, first);
        CHECK(pool.GetIdleTextureCount() == 1);
        CHECK(pool.GetLiveObjectCount() == 1);
        CHECK(device.images_destroyed == 0);

        const RGPhysicalTexture second = pool.AcquireTexture(desc);
        CHECK(second.image == first.image);
        CHECK(device.images_created == 1);
        CHECK(pool.GetLiveObjectCount() == 1);
    }

    // ---------------------------------------------------------------------------------
    BeginGroup("overflowing MaxIdlePerKey destroys instead of parking");
    {
        FakeDevice device;
        RenderGraphResourcePool pool(&device);
        pool.SetMaxIdlePerKey(1);
        const RGTextureDesc desc = FullscreenLDR("A");

        // Two live textures, room for only one idle: the second release has to destroy.
        const RGPhysicalTexture first = pool.AcquireTexture(desc);
        const RGPhysicalTexture second = pool.AcquireTexture(desc);
        CHECK(pool.GetLiveObjectCount() == 2);

        pool.ReleaseTexture(desc, first);
        CHECK(pool.GetIdleTextureCount() == 1);
        CHECK(pool.GetLiveObjectCount() == 2);
        CHECK(device.images_destroyed == 0);

        pool.ReleaseTexture(desc, second);
        // Still one parked -- the overflowing one was destroyed, not silently dropped.
        CHECK(pool.GetIdleTextureCount() == 1);
        CHECK(pool.GetLiveObjectCount() == 1);
        CHECK(device.images_destroyed == 1);
        CHECK(device.views_destroyed == 1);
        CHECK(device.memory_freed == 1);
    }

    // ---------------------------------------------------------------------------------
    // Regression: Flush() used to decrement the live-object counter twice per resource.
    // Clamping at zero hid it whenever every object was idle at once, so the test keeps
    // one acquired texture alive -- that is what makes the over-decrement visible.
    BeginGroup("Flush() destroys only idle resources and counts each one once");
    {
        FakeDevice device;
        RenderGraphResourcePool pool(&device);
        pool.SetMaxIdlePerKey(8);

        const RGTextureDesc half = HalfSize("B");
        std::vector<RGPhysicalTexture> parked;
        for (int i = 0; i < 3; ++i)
        {
            parked.push_back(pool.AcquireTexture(half));
        }
        const RGPhysicalTexture held = pool.AcquireTexture(FullscreenLDR("A"));
        for (const RGPhysicalTexture& physical : parked)
        {
            pool.ReleaseTexture(half, physical);
        }
        CHECK(pool.GetLiveObjectCount() == 4);
        CHECK(pool.GetIdleTextureCount() == 3);

        pool.Flush();
        // Live was 4 and exactly 3 objects were destroyed: one must remain.
        CHECK(pool.GetLiveObjectCount() == 1);
        CHECK(pool.GetIdleTextureCount() == 0);
        CHECK(device.images_destroyed == 3);
        CHECK(device.views_destroyed == 3);

        // The still-acquired texture was not touched.
        CHECK(held.image != nullptr);
        pool.ReleaseTexture(FullscreenLDR("A"), held);
    }

    // ---------------------------------------------------------------------------------
    BeginGroup("the executor runs passes in compiled order and resolves handles");
    {
        FakeDevice device;
        RenderGraphResourcePool pool(&device);
        RenderGraph graph("post");
        const RGTextureHandle backbuffer = ImportBackbuffer(graph);

        RGTextureHandle hdr;
        std::vector<std::string> ran;
        RHIImage* hdr_image = nullptr;
        RHIImageView* hdr_view = nullptr;

        graph.AddPass("Gbuffer",
                      RGPassType::Render,
                      [&](RenderGraphBuilder& builder) {
                          hdr = builder.CreateTexture("HDR", FullscreenLDR("HDR"));
                          hdr = builder.WriteTexture(hdr, RGResourceState::RenderTarget);
                      },
                      [&](RenderGraphExecuteContext& context) {
                          ran.push_back(context.GetPassName());
                          CHECK(context.GetPassIndex() == 0);
                          hdr_image = context.GetImage(hdr);
                          hdr_view = context.GetImageView(hdr);
                      });

        graph.AddPass("Tonemap",
                      RGPassType::Render,
                      [&](RenderGraphBuilder& builder) {
                          (void)builder.ReadTexture(hdr, RGResourceState::ShaderResource);
                          (void)builder.WriteTexture(backbuffer, RGResourceState::RenderTarget);
                      },
                      [&](RenderGraphExecuteContext& context) {
                          ran.push_back(context.GetPassName());
                          CHECK(context.IsResolved(hdr));
                          // A handle from an older generation must not silently resolve.
                          RGTextureHandle stale {hdr.index, hdr.generation + 7};
                          CHECK(!context.IsResolved(stale));
                      });

        RenderGraphExecutor executor(nullptr, &device, &pool);
        CHECK(executor.Execute(graph, &s_CommandBuffer));
        CHECK(executor.GetLastError().empty());

        CHECK(ran.size() == 2);
        if (ran.size() == 2)
        {
            CHECK(ran[0] == "Gbuffer");
            CHECK(ran[1] == "Tonemap");
        }
        CHECK(executor.GetLastPassCount() == 2);
        CHECK(hdr_image != nullptr);
        CHECK(hdr_view != nullptr);

        // Only HDR was allocated; the backbuffer was imported.
        CHECK(executor.GetLastAllocatedTextureCount() == 1);
        CHECK(device.images_created == 1);
        CHECK(device.views_created == 1);

        // Debug events bracket every pass and stay balanced.
        CHECK(device.events_pushed == 2);
        CHECK(device.events_popped == 2);
    }

    // ---------------------------------------------------------------------------------
    BeginGroup("one CmdPipelineBarrier per pass, transitioning the right image");
    {
        FakeDevice device;
        RenderGraphResourcePool pool(&device);
        RenderGraph graph("barriers");
        const RGTextureHandle backbuffer = ImportBackbuffer(graph);

        RGTextureHandle hdr;
        RHIImage* hdr_image = nullptr;

        graph.AddPass("Gbuffer",
                      RGPassType::Render,
                      [&](RenderGraphBuilder& builder) {
                          hdr = builder.CreateTexture("HDR", FullscreenLDR("HDR"));
                          hdr = builder.WriteTexture(hdr, RGResourceState::RenderTarget);
                      },
                      [&](RenderGraphExecuteContext& context) { hdr_image = context.GetImage(hdr); });

        graph.AddPass("Tonemap",
                      RGPassType::Render,
                      [&](RenderGraphBuilder& builder) {
                          (void)builder.ReadTexture(hdr, RGResourceState::ShaderResource);
                          (void)builder.WriteTexture(backbuffer, RGResourceState::RenderTarget);
                      },
                      [](RenderGraphExecuteContext&) {});

        RenderGraphExecutor executor(nullptr, &device, &pool);
        CHECK(executor.Execute(graph, &s_CommandBuffer));

        CHECK(executor.GetLastBarrierCount() >= 1);
        CHECK(device.barrier_calls >= 1);
        // Batched: never one call per resource.
        CHECK(device.barrier_calls <= static_cast<int>(executor.GetLastPassCount()));

        bool saw_hdr_transition = false;
        for (const FakeDevice::BarrierRecord& record : device.barriers)
        {
            if (record.image == hdr_image && record.old_layout != record.new_layout)
            {
                saw_hdr_transition = true;
            }
        }
        CHECK(saw_hdr_transition);

        // SetDebugEventsEnabled(false) skips the markers but not the passes.
        device = FakeDevice {};
        executor.SetDebugEventsEnabled(false);
        CHECK(executor.Execute(graph, &s_CommandBuffer));
        CHECK(device.events_pushed == 0);
        CHECK(device.events_popped == 0);
        CHECK(executor.GetLastPassCount() == 2);
    }

    // ---------------------------------------------------------------------------------
    BeginGroup("transient resources go back to the pool the moment they die");
    {
        FakeDevice device;
        RenderGraphResourcePool pool(&device);
        RenderGraph graph("recycle");
        const RGTextureHandle backbuffer = ImportBackbuffer(graph);

        RGTextureHandle hdr;
        graph.AddPass("Gbuffer",
                      RGPassType::Render,
                      [&](RenderGraphBuilder& builder) {
                          hdr = builder.CreateTexture("HDR", FullscreenLDR("HDR"));
                          hdr = builder.WriteTexture(hdr, RGResourceState::RenderTarget);
                      },
                      [](RenderGraphExecuteContext&) {});
        graph.AddPass("Tonemap",
                      RGPassType::Render,
                      [&](RenderGraphBuilder& builder) {
                          (void)builder.ReadTexture(hdr, RGResourceState::ShaderResource);
                          (void)builder.WriteTexture(backbuffer, RGResourceState::RenderTarget);
                      },
                      [](RenderGraphExecuteContext&) {});

        RenderGraphExecutor executor(nullptr, &device, &pool);
        CHECK(executor.Execute(graph, &s_CommandBuffer));

        // HDR's last reader is Tonemap, the final pass, so it is already back in the pool.
        CHECK(pool.GetIdleTextureCount() == 1);
        CHECK(pool.GetLiveObjectCount() == 1);

        // Running the same graph again reuses it: no new allocation. (No Reset() here --
        // that would drop the passes too, and this test is about the pool.)
        CHECK(executor.Execute(graph, &s_CommandBuffer));
        CHECK(device.images_created == 1);
        CHECK(pool.GetIdleTextureCount() == 1);
    }

    // ---------------------------------------------------------------------------------
    BeginGroup("buffers take the same path as textures");
    {
        FakeDevice device;
        RenderGraphResourcePool pool(&device);
        RenderGraph graph("buffer");
        const RGTextureHandle backbuffer = ImportBackbuffer(graph);

        RGBufferHandle args;
        RHIBuffer* args_buffer = nullptr;

        graph.AddPass("BuildArgs",
                      RGPassType::Compute,
                      [&](RenderGraphBuilder& builder) {
                          args = builder.CreateBuffer("Args", SmallBuffer("Args"));
                          args = builder.WriteBuffer(args, RGResourceState::UnorderedAccess);
                      },
                      [&](RenderGraphExecuteContext& context) {
                          args_buffer = context.GetBuffer(args);
                          CHECK(context.IsResolved(args));
                      });

        graph.AddPass("Draw",
                      RGPassType::Render,
                      [&](RenderGraphBuilder& builder) {
                          (void)builder.ReadBuffer(args, RGResourceState::ShaderResource);
                          (void)builder.WriteTexture(backbuffer, RGResourceState::RenderTarget);
                      },
                      [](RenderGraphExecuteContext&) {});

        RenderGraphExecutor executor(nullptr, &device, &pool);
        CHECK(executor.Execute(graph, &s_CommandBuffer));
        CHECK(args_buffer != nullptr);
        CHECK(device.buffers_created == 1);
        CHECK(pool.GetIdleBufferCount() == 1);
    }

    // ---------------------------------------------------------------------------------
    // Regression: the executor used to allocate every declared resource, including ones
    // whose producer pass was culled -- a culled debug pass still reserved its
    // full-size render target.
    BeginGroup("a culled pass neither runs nor allocates");
    {
        FakeDevice device;
        RenderGraphResourcePool pool(&device);
        RenderGraph graph("cull");
        const RGTextureHandle backbuffer = ImportBackbuffer(graph);

        bool dead_ran = false;
        graph.AddPass("Dead",
                      RGPassType::Render,
                      [](RenderGraphBuilder& builder) {
                          RGTextureHandle dead = builder.CreateTexture("Dead", FullscreenLDR("Dead"));
                          (void)builder.WriteTexture(dead, RGResourceState::RenderTarget);
                      },
                      [&](RenderGraphExecuteContext&) { dead_ran = true; });

        bool live_ran = false;
        graph.AddPass("Live",
                      RGPassType::Render,
                      [&](RenderGraphBuilder& builder) {
                          (void)builder.WriteTexture(backbuffer, RGResourceState::RenderTarget);
                      },
                      [&](RenderGraphExecuteContext&) { live_ran = true; });

        RenderGraphExecutor executor(nullptr, &device, &pool);
        CHECK(executor.Execute(graph, &s_CommandBuffer));
        CHECK(live_ran);
        CHECK(!dead_ran);
        CHECK(executor.GetLastPassCount() == 1);
        CHECK(executor.GetLastAllocatedTextureCount() == 0);
        CHECK(device.images_created == 0);
    }

    // ---------------------------------------------------------------------------------
    // Regression: extracted resources used to be released into the pool at the end of
    // Execute(), so the caller's readback could silently land on a texture the next
    // frame had already reused.
    BeginGroup("an extracted resource outlives the graph");
    {
        FakeDevice device;
        RenderGraphResourcePool pool(&device);
        RenderGraph graph("extract");
        const RGTextureHandle backbuffer = ImportBackbuffer(graph);

        RGTextureHandle keep;
        graph.AddPass("Make",
                      RGPassType::Render,
                      [&](RenderGraphBuilder& builder) {
                          keep = builder.CreateTexture("Keep", FullscreenLDR("Keep"));
                          keep = builder.WriteTexture(keep, RGResourceState::RenderTarget);
                          builder.KeepAlive(keep);
                      },
                      [](RenderGraphExecuteContext&) {});

        graph.AddPass("Use",
                      RGPassType::Render,
                      [&](RenderGraphBuilder& builder) {
                          (void)builder.ReadTexture(keep, RGResourceState::ShaderResource);
                          (void)builder.WriteTexture(backbuffer, RGResourceState::RenderTarget);
                      },
                      [](RenderGraphExecuteContext&) {});

        RenderGraphExecutor executor(nullptr, &device, &pool);
        CHECK(executor.Execute(graph, &s_CommandBuffer));

        // Still held by the executor -- not parked where the next frame can grab it.
        CHECK(pool.GetIdleTextureCount() == 0);
        CHECK(pool.GetLiveObjectCount() == 1);

        executor.ReleaseExtracted();
        CHECK(pool.GetIdleTextureCount() == 1);
        CHECK(pool.GetLiveObjectCount() == 1);
        CHECK(device.images_destroyed == 0);
    }

    // ---------------------------------------------------------------------------------
    BeginGroup("the next Execute() reclaims the previous frame's extracted resources");
    {
        FakeDevice device;
        RenderGraphResourcePool pool(&device);

        RenderGraph graph("extract2");
        const RGTextureHandle backbuffer = ImportBackbuffer(graph);
        RGTextureHandle keep;
        graph.AddPass("Make",
                      RGPassType::Render,
                      [&](RenderGraphBuilder& builder) {
                          keep = builder.CreateTexture("Keep", FullscreenLDR("Keep"));
                          keep = builder.WriteTexture(keep, RGResourceState::RenderTarget);
                          builder.KeepAlive(keep);
                      },
                      [](RenderGraphExecuteContext&) {});
        graph.AddPass("Use",
                      RGPassType::Render,
                      [&](RenderGraphBuilder& builder) {
                          (void)builder.ReadTexture(keep, RGResourceState::ShaderResource);
                          (void)builder.WriteTexture(backbuffer, RGResourceState::RenderTarget);
                      },
                      [](RenderGraphExecuteContext&) {});

        RenderGraphExecutor executor(nullptr, &device, &pool);
        CHECK(executor.Execute(graph, &s_CommandBuffer));
        CHECK(pool.GetIdleTextureCount() == 0);

        // Second frame: Execute() reclaims frame 1's texture first, so frame 2 reuses
        // it and the whole run costs exactly one allocation.
        CHECK(executor.Execute(graph, &s_CommandBuffer));
        CHECK(device.images_created == 1);
        CHECK(pool.GetIdleTextureCount() == 0);
        CHECK(pool.GetLiveObjectCount() == 1);
    }

    // ---------------------------------------------------------------------------------
    BeginGroup("the executor refuses to run without a device or command buffer");
    {
        FakeDevice device;
        RenderGraphResourcePool pool(&device);
        RenderGraph empty("empty");

        RenderGraphExecutor no_device(nullptr, nullptr, &pool);
        CHECK(!no_device.Execute(empty, &s_CommandBuffer));
        CHECK(no_device.GetLastError() == "no resource device");

        RenderGraphExecutor no_buffer(nullptr, &device, &pool);
        CHECK(!no_buffer.Execute(empty, nullptr));
        CHECK(no_buffer.GetLastError() == "no command buffer");

        // A graph that fails validation is reported, not silently recorded. (Reading a
        // resource nobody writes is the loudest validation error the compiler has.)
        RenderGraph broken("broken");
        broken.AddPass("Reader",
                       RGPassType::Render,
                       [](RenderGraphBuilder& builder) {
                           RGTextureHandle unwritten = builder.CreateTexture("Unwritten", FullscreenLDR("Unwritten"));
                           (void)builder.ReadTexture(unwritten, RGResourceState::ShaderResource);
                       },
                       [](RenderGraphExecuteContext&) {});

        RenderGraphExecutor executor(nullptr, &device, &pool);
        CHECK(!executor.Execute(broken, &s_CommandBuffer));
        CHECK(!executor.GetLastError().empty());
        CHECK(executor.GetLastPassCount() == 0);
    }

    // ---------------------------------------------------------------------------------
    BeginGroup("a pool with no device is inert instead of crashing");
    {
        RenderGraphResourcePool pool(nullptr);
        const RGTextureDesc desc = FullscreenLDR("A");
        const RGPhysicalTexture physical = pool.AcquireTexture(desc);
        CHECK(physical.image == nullptr);
        CHECK(physical.view == nullptr);
        pool.ReleaseTexture(desc, physical);
        pool.Flush();
        CHECK(pool.GetLiveObjectCount() == 0);
        CHECK(pool.GetIdleTextureCount() == 0);
    }

    // ---------------------------------------------------------------------------------
    // RP1 (step 2.4) through the EXECUTOR: the compiler test (RenderGraphSmokeTest
    // group 19) proves the order / abstract barriers; this proves what actually reaches
    // the RHI. It is also the regression test for the depth-aspect bug: before
    // RGExternalTexture::format existed an imported depth target had format Unknown, so
    // the executor emitted a COLOR-aspect barrier for it (Vulkan validation error), and
    // a D32_SFLOAT transient got DEPTH|STENCIL (also invalid -- D32 has no stencil).
    // ---------------------------------------------------------------------------------
    BeginGroup("RP1 three-way split executes with correct RHI barriers (depth aspect)");
    {
        FakeDevice device;
        RenderGraphResourcePool pool(&device);
        RenderGraph graph("RP1-Execute");

        RHIImage* const depth_image = &s_Images[kSentinelCount - 1];
        RHIImage* const backup_image = &s_Images[kSentinelCount - 2];
        RGExternalTexture depth_ext;
        depth_ext.image = depth_image;
        depth_ext.view = &s_Views[kSentinelCount - 1];
        depth_ext.state = RGResourceState::Undefined;
        depth_ext.format = RGFormat::D32_SFLOAT;  // Vulkan FindDepthFormat's first choice
        const RGTextureHandle depth = graph.ImportTexture("scene_depth", depth_ext);

        RGExternalTexture backup_ext;
        backup_ext.image = backup_image;
        backup_ext.view = &s_Views[kSentinelCount - 2];
        backup_ext.state = RGResourceState::ShaderResource;
        backup_ext.format = RGFormat::R16G16B16A16_SFLOAT;
        const RGTextureHandle backup_odd = graph.ImportTexture("backup_odd", backup_ext);

        RGTextureHandle gbuffer_a;
        RHIImage* gbuffer_a_image = nullptr;
        std::vector<std::string> ran;

        graph.AddPass(
            "GBufferPass",
            RGPassType::Render,
            [&](RenderGraphBuilder& builder) {
                gbuffer_a = builder.CreateTexture("GBufferA", FullscreenLDR("GBufferA"));
                builder.WriteTexture(gbuffer_a, RGResourceState::RenderTarget);
                builder.WriteTexture(depth, RGResourceState::DepthStencilWrite);
            },
            [&](RenderGraphExecuteContext& context) {
                ran.push_back("GBufferPass");
                gbuffer_a_image = context.GetImage(gbuffer_a);
            });

        graph.AddPass(
            "DeferredLightingPass",
            RGPassType::Render,
            [&](RenderGraphBuilder& builder) {
                builder.ReadTexture(gbuffer_a, RGResourceState::ShaderResource);
                builder.ReadTexture(depth, RGResourceState::DepthStencilRead);
                builder.WriteTexture(backup_odd, RGResourceState::RenderTarget);
            },
            [&](RenderGraphExecuteContext& context) {
                ran.push_back("DeferredLightingPass");
                // Imports resolve to the caller's own objects, never pool allocations.
                CHECK(context.GetImage(depth) == depth_image);
                CHECK(context.GetImage(backup_odd) == backup_image);
            });

        graph.AddPass(
            "ForwardLightingPass",
            RGPassType::Render,
            [&](RenderGraphBuilder& builder) {
                builder.ReadTexture(depth, RGResourceState::DepthStencilRead);
                builder.WriteTexture(backup_odd, RGResourceState::RenderTarget);
            },
            [&](RenderGraphExecuteContext&) { ran.push_back("ForwardLightingPass"); });

        RenderGraphExecutor executor(nullptr, &device, &pool);
        CHECK(executor.Execute(graph, &s_CommandBuffer));
        CHECK(ran.size() == 3 && ran[0] == "GBufferPass" && ran[1] == "DeferredLightingPass" &&
              ran[2] == "ForwardLightingPass");

        // 2 + 3 + 0 abstract barriers -> 2 batched RHI calls (Forward needs none).
        CHECK(executor.GetLastBarrierCount() == 5);
        CHECK(device.barrier_calls == 2);
        // Only GBufferA is pool-allocated; depth and backup_odd are imports.
        CHECK(device.images_created == 1);

        const FakeDevice::BarrierRecord* depth_first = nullptr;
        const FakeDevice::BarrierRecord* depth_second = nullptr;
        const FakeDevice::BarrierRecord* backup_rt = nullptr;
        const FakeDevice::BarrierRecord* gbuffer_srv = nullptr;
        for (const FakeDevice::BarrierRecord& record : device.barriers)
        {
            if (record.image == depth_image)
            {
                (depth_first == nullptr ? depth_first : depth_second) = &record;
            }
            else if (record.image == backup_image)
            {
                backup_rt = &record;
            }
            else if (record.image == gbuffer_a_image && record.call_index == 1)
            {
                gbuffer_srv = &record;
            }
        }

        // Depth: DEPTH aspect only (no COLOR -- the old bug; no STENCIL -- D32 has none).
        CHECK(depth_first != nullptr);
        CHECK(depth_first != nullptr && depth_first->aspect == RHI_IMAGE_ASPECT_DEPTH_BIT);
        CHECK(depth_first != nullptr && depth_first->old_layout == RHI_IMAGE_LAYOUT_UNDEFINED);
        CHECK(depth_first != nullptr &&
              depth_first->new_layout == RHI_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        CHECK(depth_first != nullptr && depth_first->call_index == 0);
        CHECK(depth_second != nullptr);
        CHECK(depth_second != nullptr && depth_second->aspect == RHI_IMAGE_ASPECT_DEPTH_BIT);
        CHECK(depth_second != nullptr &&
              depth_second->old_layout == RHI_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        CHECK(depth_second != nullptr &&
              depth_second->new_layout == RHI_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
        CHECK(depth_second != nullptr && depth_second->call_index == 1);

        // GBuffer: COLOR aspect, RT -> SHADER_READ_ONLY right before Deferred.
        CHECK(gbuffer_srv != nullptr);
        CHECK(gbuffer_srv != nullptr && gbuffer_srv->aspect == RHI_IMAGE_ASPECT_COLOR_BIT);
        CHECK(gbuffer_srv != nullptr && gbuffer_srv->old_layout == RHI_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        CHECK(gbuffer_srv != nullptr && gbuffer_srv->new_layout == RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        // BackupOdd: imported ShaderResource -> RenderTarget, once, before Deferred.
        CHECK(backup_rt != nullptr);
        CHECK(backup_rt != nullptr && backup_rt->aspect == RHI_IMAGE_ASPECT_COLOR_BIT);
        CHECK(backup_rt != nullptr && backup_rt->old_layout == RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        CHECK(backup_rt != nullptr && backup_rt->new_layout == RHI_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        CHECK(backup_rt != nullptr && backup_rt->call_index == 1);
    }

    // ---------------------------------------------------------------------------------
    BeginGroup("depth aspect follows the format: D24S8 transient gets DEPTH|STENCIL");
    {
        FakeDevice device;
        RenderGraphResourcePool pool(&device);
        RenderGraph graph("DepthStencilAspect");
        const RGTextureHandle backbuffer = ImportBackbuffer(graph);

        RGTextureDesc ds_desc = FullscreenLDR("DS");
        ds_desc.format = RGFormat::D24_UNORM_S8_UINT;
        ds_desc.usage = RGTextureUsage::DepthStencil | RGTextureUsage::ShaderRead;
        RGTextureHandle ds;
        RHIImage* ds_image = nullptr;

        graph.AddPass(
            "Prepass",
            RGPassType::Render,
            [&](RenderGraphBuilder& builder) {
                ds = builder.CreateTexture("DS", ds_desc);
                builder.WriteTexture(ds, RGResourceState::DepthStencilWrite);
            },
            [&](RenderGraphExecuteContext& context) { ds_image = context.GetImage(ds); });
        graph.AddPass(
            "Consume",
            RGPassType::Render,
            [&](RenderGraphBuilder& builder) {
                builder.ReadTexture(ds, RGResourceState::DepthStencilRead);
                builder.WriteTexture(backbuffer, RGResourceState::RenderTarget);
            },
            [](RenderGraphExecuteContext&) {});

        RenderGraphExecutor executor(nullptr, &device, &pool);
        CHECK(executor.Execute(graph, &s_CommandBuffer));
        int ds_barriers = 0;
        bool all_depth_stencil = true;
        for (const FakeDevice::BarrierRecord& record : device.barriers)
        {
            if (record.image == ds_image)
            {
                ++ds_barriers;
                all_depth_stencil = all_depth_stencil &&
                                    record.aspect == (RHI_IMAGE_ASPECT_DEPTH_BIT | RHI_IMAGE_ASPECT_STENCIL_BIT);
            }
        }
        CHECK(ds_barriers == 2);
        CHECK(all_depth_stencil);
    }

    // ---------------------------------------------------------------------------------
    std::printf("\n%d checks, %d failures\n", g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
