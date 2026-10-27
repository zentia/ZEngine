// =============================================================================
// VirtualTexture **最小生产者** A/B（Vulkan headless，真异步 worker，真 shader 读回）。
//
// 此前 r.VirtualTexture.Enable 1 是个空操作：引擎里没有任何东西初始化 VirtualTextureManager
// 单例、注册虚拟纹理或请求页。VirtualTextureProducer 补上这段生命周期。本测试用它**生产**
// 一张纹理，再用 vt_resolve.comp 从 GPU 读回，逐 texel 与源比对——证明"开关 + 路径"这条线
// 真的把像素送进了页池，而不只是 CPU 簿记。
//
// 同时钉住一批在写生产者时挖出来的、此前被"没人调用"掩盖的缺陷：
//   ① 默认 VirtualTextureConfig 要 2048x2048 页 = 262144x262144 px = 256 GiB 的池图。
//      Initialize 过去是 void 且吞掉失败，留下一个"活着但一页也装不下"的管理器。现在
//      Initialize 返回 bool，池创建前量设备上限；
//   ② RequestPages 每帧调用会把整个 mip 的所有页重新入队（36 页纹理 600 帧 = 19200 个任务）。
//      现在幂等；
//   ③ enable_mip_streaming 是死字段。没有 mip 链之前只有 mip 0 能上传，生产者必须把请求钉在
//      mip 0；
//   ④ LoadPage 在没有池时先把页标成 Loading 再返回 false，页永远卡在 Loading。
//
//   cmake -B build -DZENGINE_BUILD_VT_PRODUCER_AB_SMOKE_TEST=ON
//   cmake --build build --target VirtualTextureProducerABSmokeTest
// =============================================================================

#include "CommonPCH/pch.h"

#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Core/Math/AxisAligned.h"
#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Function/Render/Interface/RHI.h"
#include "Runtime/Function/Render/Interface/Vulkan/VulkanRHI.h"
#include "Runtime/Function/Render/RenderType.h"
#include "Runtime/Function/Render/Texture/VirtualTextureManager.h"
#include "Runtime/Function/Render/Texture/VirtualTextureProducer.h"
#include "Runtime/Function/Render/Texture/VirtualTextureResolver.h"
#include "Runtime/Function/Render/Texture/VirtualTextureTypes.h"
#include "Runtime/Function/Render/WindowSystem.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace
{
    int g_Checks = 0;
    int g_Failures = 0;

    void Check(bool condition, const char* name)
    {
        ++g_Checks;
        if (!condition)
        {
            ++g_Failures;
        }
        std::printf("%s %s\n", condition ? "[ ok ]" : "[FAIL]", name);
    }

    template <class Fn>
    void RunWithWatchdog(const char* what, Fn&& fn, int seconds = 30)
    {
        auto future = std::async(std::launch::async, std::forward<Fn>(fn));
        if (future.wait_for(std::chrono::seconds(seconds)) == std::future_status::timeout)
        {
            std::printf("[FAIL] %s did not return within %d s\n", what, seconds);
            std::printf("\n%d checks, %d failures (aborted)\n", g_Checks, g_Failures + 1);
            std::fflush(stdout);
            std::_Exit(2);
        }
        future.get();
    }

    constexpr uint32_t kPageSize = 128;
    constexpr float kFallback[4] = {1.0f, 0.0f, 1.0f, 1.0f};
    constexpr uint32_t kFallbackPacked = 0xFFFF00FFu;

    uint32_t SourcePacked(const TextureData& s, uint32_t x, uint32_t y)
    {
        const uint8_t* t = s.m_Pixels + (static_cast<size_t>(y) * s.m_Width + x) * 4u;
        return static_cast<uint32_t>(t[0]) | (static_cast<uint32_t>(t[1]) << 8) | (static_cast<uint32_t>(t[2]) << 16) |
               (static_cast<uint32_t>(t[3]) << 24);
    }

    std::shared_ptr<TextureData> MakeSource(uint32_t width,
                                            uint32_t height,
                                            uint8_t seed,
                                            RHIFormat format = RHI_FORMAT_R8G8B8A8_UNORM)
    {
        auto data = std::make_shared<TextureData>();
        data->m_Width = width;
        data->m_Height = height;
        data->m_Depth = 1;
        data->m_MipLevels = 1;
        data->m_ArrayLayers = 1;
        data->m_Format = format;
        data->m_PixelsLength = width * height * 4u;
        data->m_Pixels = static_cast<uint8_t*>(std::malloc(data->m_PixelsLength));  // TextureData 用 free()
        if (data->m_Pixels == nullptr)
        {
            return nullptr;
        }
        const uint32_t pages_x = (width + kPageSize - 1) / kPageSize;
        for (uint32_t y = 0; y < height; ++y)
        {
            for (uint32_t x = 0; x < width; ++x)
            {
                const uint32_t page_index = (y / kPageSize) * pages_x + (x / kPageSize);
                uint8_t* t = data->m_Pixels + (static_cast<size_t>(y) * width + x) * 4u;
                t[0] = static_cast<uint8_t>((x + seed) & 0xFF);
                t[1] = static_cast<uint8_t>((y * 3u + seed) & 0xFF);
                t[2] = static_cast<uint8_t>(seed ^ (page_index * 29u));
                t[3] = 255;
            }
        }
        return data;
    }

    // 生产者的 loader 回调环境：路径 -> 纹理，并数每个路径被加载了几次。
    struct LoaderEnv
    {
        std::map<std::string, std::shared_ptr<TextureData>> textures;
        std::map<std::string, int> loads;
    };

    std::shared_ptr<TextureData> TestLoader(void* user, const std::string& path)
    {
        auto* env = static_cast<LoaderEnv*>(user);
        ++env->loads[path];
        auto it = env->textures.find(path);
        return it == env->textures.end() ? nullptr : it->second;
    }

    // 校验层的消息走 std::cerr（VulkanRHI.cpp 的 debugCallback）。进程内临时接管 cerr 的缓冲区，
    // 让"这段代码是否向驱动发了非法请求"成为可断言的事实，而不是要靠外部脚本去 grep 输出——
    // 后者在测试自己被单独运行时根本不存在。
    class CerrCapture
    {
    public:
        CerrCapture() : m_Old(std::cerr.rdbuf(m_Buffer.rdbuf())) {}
        ~CerrCapture() { std::cerr.rdbuf(m_Old); }
        std::string text() const { return m_Buffer.str(); }
        bool sawValidationError() const { return text().find("validation layer") != std::string::npos; }

    private:
        std::ostringstream m_Buffer;
        std::streambuf* m_Old;
    };

    uint32_t VmaAllocationCount(VulkanRHI& rhi)
    {
        VmaTotalStatistics stats {};
        vmaCalculateStatistics(rhi.getVmaAllocator(), &stats);
        return stats.total.statistics.allocationCount;
    }

    struct Tally
    {
        uint32_t resident_ok = 0;
        uint32_t resident_wrong = 0;
        uint32_t absent_ok = 0;
        uint32_t absent_bad = 0;
    };

    Tally Compare(const std::vector<VirtualTextureResolvedTexel>& gpu,
                  const std::shared_ptr<VirtualTextureResource>& resource,
                  const TextureData& source)
    {
        Tally t;
        if (gpu.size() != static_cast<size_t>(source.m_Width) * source.m_Height)
        {
            t.resident_wrong = 0xFFFFFFFFu;
            return t;
        }
        for (uint32_t y = 0; y < source.m_Height; ++y)
        {
            for (uint32_t x = 0; x < source.m_Width; ++x)
            {
                const PageTableEntry cpu = resource->GetPageTableEntry(0, x / kPageSize, y / kPageSize);
                const bool resident = cpu.is_valid && cpu.physical_slot != UINT32_MAX;
                const auto& g = gpu[static_cast<size_t>(y) * source.m_Width + x];
                if (resident)
                {
                    (g.resident != 0u && g.rgba == SourcePacked(source, x, y)) ? ++t.resident_ok : ++t.resident_wrong;
                }
                else
                {
                    (g.resident == 0u && g.rgba == kFallbackPacked) ? ++t.absent_ok : ++t.absent_bad;
                }
            }
        }
        return t;
    }

    // 模拟渲染系统每帧做的事：先 Producer.Tick，再 Manager.Update。
    struct Frame
    {
        VulkanRHI* rhi;
        VirtualTextureProducer* producer;
        VirtualTextureManager* manager;
        bool enabled = true;
        std::string path;
        Vector3 camera_pos {0.0f, 0.0f, 0.0f};
        Vector3 camera_fwd {0.0f, 0.0f, -1.0f};

        void Run()
        {
            producer->Tick(rhi, nullptr, enabled, path, camera_pos, camera_fwd);
            if (manager->getPhysicalTexturePool() != nullptr)
            {
                manager->Update(camera_pos, camera_fwd, 1.047f, 16.0f / 9.0f, 1280, 720, 0.016f);
            }
        }
    };

    bool RunUntilResident(Frame& frame, int timeout_ms)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline)
        {
            frame.Run();
            uint32_t resident = 0;
            uint32_t total = 0;
            frame.producer->GetResidency(resident, total);
            if (total > 0 && resident == total && frame.manager->getPendingLoadCount() == 0)
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    }
}  // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("=== VirtualTexture minimal-producer A/B smoke test (Vulkan headless) ===\n");

    SystemRegistry::GetInstance().RegisterSystem(new WindowSystem());
    VulkanRHI vulkan_rhi;
    const bool vulkan_ready = vulkan_rhi.Initialize();
    Check(vulkan_ready, "VulkanRHI::Initialize succeeds (headless)");
    if (!vulkan_ready)
    {
        std::printf("\n%d checks, %d failures\n", g_Checks, g_Failures);
        return 1;
    }
    VirtualTextureResolver resolver;
    Check(resolver.Initialize(&vulkan_rhi), "vt_resolve.comp pipeline ready");
    // 基线取在 resolver 初始化**之后**：resolver 自己也可能占 VMA 分配，我们要比的是"生产者及其
    // 纹理/池/页表"净增了多少，不是 resolver。
    const uint32_t vma_with_resolver = VmaAllocationCount(vulkan_rhi);

    // =========================================================================
    std::printf("--- A: a manager with the DEFAULT config must fail cleanly, not limp ---\n");
    {
        VirtualTextureManager bare;
        // 默认配置：2048x2048 页 x 128 px = 262144 px 边长（RGBA8 = 256 GiB）。
        bool ok = true;
        bool sent_illegal_request = false;
        {
            CerrCapture capture;
            ok = bare.Initialize(&vulkan_rhi, nullptr);
            sent_illegal_request = capture.sawValidationError();
        }
        Check(!ok, "Initialize with the default 2048x2048-page config returns false (pool exceeds the device)");
        // 光"返回 false"不够：vmaCreateImage 对超限图像本来就会失败，所以去掉设备上限检查后上面这条
        // 照样为真。检查的价值是**根本不把违反 Vulkan 规范的请求（262144 > maxImageDimension2D）发给
        // 驱动**——这才是校验层能看见的区别。
        Check(!sent_illegal_request,
              "...and it never sent the over-limit vkCreateImage to the driver (checked against the device limit first)");
        Check(bare.getPhysicalTexturePool() == nullptr, "no half-dead pool object is left behind");

        // 没有池时 LoadPage 不能把页卡在 Loading。
        auto source = MakeSource(256, 256, 0x11);
        const VirtualTextureHandle handle = bare.RegisterVirtualTexture(source, kPageSize);
        auto resource = bare.GetVirtualTexture(handle);
        RunWithWatchdog("ForceLoadPages without a pool", [&] { bare.ForceLoadPages(handle, 0); });
        bool stuck_loading = false;
        for (const auto& pair : resource->getAllPages())
        {
            if (pair.second.state == VirtualTexturePageState::Loading)
            {
                stuck_loading = true;
            }
        }
        Check(!stuck_loading, "with no pool, pages are NOT left stuck in the Loading state");
        Check(bare.GetStats().loaded_pages == 0, "nothing claims to be loaded");
        resource.reset();
        bare.Shutdown();
    }
    Check(VmaAllocationCount(vulkan_rhi) == vma_with_resolver, "VMA: the failed Initialize leaked nothing");

    // =========================================================================
    std::printf("--- B: mip knob. enable_mip_streaming=false pins requests to mip 0 ---\n");
    {
        auto run = [&](bool mip_streaming, uint32_t& mip0_loaded, uint32_t& higher_loaded) {
            VirtualTextureManager manager;
            VirtualTextureConfig config;
            config.page_size = kPageSize;
            config.physical_pool_width = 8;
            config.physical_pool_height = 8;
            config.memory_budget_mb = 64;
            config.enable_async_loading = false;
            config.enable_mip_streaming = mip_streaming;
            manager.SetConfig(config);
            manager.Initialize(&vulkan_rhi, nullptr);
            auto source = MakeSource(512, 512, 0x22);
            const VirtualTextureHandle handle = manager.RegisterVirtualTexture(source, kPageSize);
            auto resource = manager.GetVirtualTexture(handle);
            // screen_size 很小 ⇒ CalculateRequiredMipLevel 选最粗的 mip（若 mip 流送开着）。
            manager.RequestPages(handle, Vector3(0, 0, 0), Vector3(0, 0, 50), AxisAlignedBox(), 0.001f);
            RunWithWatchdog("pump", [&] {
                for (int i = 0; i < 40; ++i)
                {
                    manager.PumpLoads();
                }
            });
            mip0_loaded = 0;
            higher_loaded = 0;
            for (const auto& pair : resource->getAllPages())
            {
                if (pair.second.state == VirtualTexturePageState::Loaded)
                {
                    (pair.second.id.mip_level == 0 ? mip0_loaded : higher_loaded)++;
                }
            }
            resource.reset();
            manager.Shutdown();
        };
        uint32_t m0 = 0;
        uint32_t hi = 0;
        run(false, m0, hi);
        Check(m0 == 16 && hi == 0, "mip streaming OFF: all 16 mip-0 pages loaded, nothing at a coarser mip");
        run(true, m0, hi);
        Check(m0 == 0 && hi > 0, "mip streaming ON : the coarse mip is requested instead (the flag is no longer dead)");
    }

    // =========================================================================
    std::printf("--- C: the producer, driven like the render system drives it ---\n");
    LoaderEnv env;
    env.textures["a.png"] = MakeSource(700, 500, 0x31);                              // 6x4 页，右/下残缺
    env.textures["b.png"] = MakeSource(384, 256, 0x7C);                              // 3x2 页
    env.textures["float.exr"] = MakeSource(256, 256, 0x01, RHI_FORMAT_R32G32B32A32_SFLOAT);  // 不是 RGBA8
    // "missing.png" 故意不放进 env：loader 返回 null。

    VirtualTextureManager manager;
    VirtualTextureProducer producer;
    VirtualTextureProducer::Config producer_config;
    producer_config.log_every_frames = 0;
    producer.Bind(&manager, &TestLoader, &env, producer_config);
    Frame frame {&vulkan_rhi, &producer, &manager};

    // ---- C1: 关着 / 路径空 ⇒ 什么都不发生 ----
    frame.enabled = false;
    frame.path = "a.png";
    frame.Run();
    Check(!producer.isStreaming() && manager.getPhysicalTexturePool() == nullptr && env.loads["a.png"] == 0,
          "disabled: nothing registered, the manager is not brought up, the loader is not called");
    frame.enabled = true;
    frame.path = "";
    frame.Run();
    Check(!producer.isStreaming() && manager.getPhysicalTexturePool() == nullptr,
          "enabled but r.VirtualTexture.Source empty: still nothing (the old 'advance an empty manager' behaviour)");

    // ---- C2: 打开 + 路径 ⇒ 流送 ----
    frame.path = "a.png";
    bool settled = false;
    RunWithWatchdog("stream a.png", [&] { settled = RunUntilResident(frame, 30000); }, 40);
    Check(settled, "enabled + path: every page of a.png becomes resident");
    Check(producer.isStreaming() && producer.getActivePath() == "a.png" && env.loads["a.png"] == 1,
          "registered once, loaded exactly once");
    Check(manager.getPhysicalTexturePool() != nullptr && manager.getPhysicalTexturePool()->getPoolWidthPages() == 16 &&
              manager.getPhysicalTexturePool()->getPoolHeightPages() == 16,
          "the producer sized the pool to 16x16 pages (2048 px), not the 2048x2048-page default");
    Check(!manager.getConfig().enable_mip_streaming, "the producer turned mip streaming off (only mip 0 can upload)");
    Check(manager.getAsyncPreparedCount() > 0, "the async worker thread really prepared the pages");
    Check(manager.getGpuThreadViolationCount() == 0, "no GPU work happened off the owner thread");
    {
        uint32_t resident = 0;
        uint32_t total = 0;
        producer.GetResidency(resident, total);
        std::printf("       [info] a.png residency %u/%u, pool used %u/%u\n", resident, total,
                    manager.GetStats().physical_pool_used_slots, manager.GetStats().physical_pool_total_slots);
        Check(total == 24 && resident == 24, "a.png is 6x4 = 24 pages, all resident");
        Check(manager.GetStats().loaded_pages == 24, "no coarse-mip page was loaded behind the producer's back");
    }

    // ---- C3: 真 shader 读回 ----
    std::vector<VirtualTextureResolvedTexel> gpu;
    {
        auto resource = manager.GetVirtualTexture(producer.getHandle());
        const bool ok = resolver.Resolve(manager, producer.getHandle(), VirtualTextureResolver::Mode::TexelFetch, kFallback, gpu);
        const Tally t = Compare(gpu, resource, *env.textures["a.png"]);
        std::printf("       [info] GPU readback: resident ok=%u wrong=%u | absent ok=%u bad=%u\n",
                    t.resident_ok, t.resident_wrong, t.absent_ok, t.absent_bad);
        Check(ok && t.resident_wrong == 0 && t.absent_bad == 0 && t.resident_ok == 700u * 500u,
              "GPU readback: every texel of a.png (incl. the partial edge pages) equals the source");
    }

    // ---- C4: 请求幂等 ----
    {
        // 不泵管理器，只反复 Tick（= 反复 RequestPages）。旧实现每次都把整张 mip 重新入队。
        // 先卸掉再 Tick，让页重新变成"待请求"。
        RunWithWatchdog("unload", [&] { manager.UnloadPages(producer.getHandle()); });
        uint32_t peak = 0;
        for (int i = 0; i < 200; ++i)
        {
            producer.Tick(&vulkan_rhi, nullptr, true, "a.png", frame.camera_pos, frame.camera_fwd);
            peak = std::max(peak, manager.getPendingLoadCount());
        }
        std::printf("       [info] pending loads after 200 requests with no pumping: peak=%u (24 pages)\n", peak);
        Check(peak <= 24u, "RequestPages is idempotent: 200 requests of a 24-page texture never queue more than 24 tasks");
        settled = false;
        RunWithWatchdog("re-stream", [&] { settled = RunUntilResident(frame, 30000); }, 40);
        Check(settled && manager.getPendingLoadCount() == 0, "it still streams back fully, and the queue drains to 0");
        // 稳态：全驻留后继续每帧请求，队列保持为空。
        uint32_t steady_peak = 0;
        for (int i = 0; i < 100; ++i)
        {
            frame.Run();
            steady_peak = std::max(steady_peak, manager.getPendingLoadCount());
        }
        Check(steady_peak == 0, "steady state: 100 more frames of requests queue nothing (everything is already resident)");
        Check(env.loads["a.png"] == 1, "the source was never reloaded from disk by all of that");
    }

    // ---- C5: 换路径 ----
    {
        const uint64_t old_handle = producer.getHandle();
        frame.path = "b.png";
        settled = false;
        RunWithWatchdog("stream b.png", [&] { settled = RunUntilResident(frame, 30000); }, 40);
        Check(settled && producer.getActivePath() == "b.png", "changing the path streams the new texture");
        Check(producer.getHandle() != old_handle && manager.GetVirtualTexture(old_handle) == nullptr,
              "the old texture was unregistered");
        auto resource = manager.GetVirtualTexture(producer.getHandle());
        const bool ok = resolver.Resolve(manager, producer.getHandle(), VirtualTextureResolver::Mode::TexelFetch, kFallback, gpu);
        const Tally t = Compare(gpu, resource, *env.textures["b.png"]);
        Check(ok && t.resident_wrong == 0 && t.absent_bad == 0 && t.resident_ok == 384u * 256u,
              "GPU readback: every texel of b.png equals the source");
        Check(manager.GetStats().total_virtual_textures == 1, "exactly one virtual texture is registered");
    }

    // ---- C6: 失败不每帧重试 ----
    {
        frame.path = "missing.png";
        for (int i = 0; i < 50; ++i)
        {
            frame.Run();
        }
        Check(!producer.isStreaming() && producer.getFailureCount() == 1 && env.loads["missing.png"] == 1,
              "a path that fails to load is attempted ONCE, not every frame (50 frames -> 1 load, 1 failure)");

        frame.path = "float.exr";
        for (int i = 0; i < 50; ++i)
        {
            frame.Run();
        }
        Check(!producer.isStreaming() && producer.getFailureCount() == 2 && env.loads["float.exr"] == 1,
              "a non-RGBA8 source is refused (never silently sliced as RGBA8), also only once");

        frame.path = "a.png";
        settled = false;
        RunWithWatchdog("recover", [&] { settled = RunUntilResident(frame, 30000); }, 40);
        Check(settled && producer.isStreaming() && producer.getActivePath() == "a.png",
              "after failures, switching to a valid path recovers");
        Check(env.loads["a.png"] == 2, "recovery loaded a.png again (the earlier one was released when the path changed)");
    }

    // ---- C7: 运行时关掉再打开 ----
    {
        const uint32_t vma_streaming = VmaAllocationCount(vulkan_rhi);
        frame.enabled = false;
        frame.Run();
        Check(!producer.isStreaming(), "disabling releases the texture");
        Check(manager.GetStats().total_virtual_textures == 0 && manager.GetStats().loaded_pages == 0,
              "...and all its pages");
        Check(manager.getPhysicalTexturePool() != nullptr, "...but the pool stays up, so re-enabling does not rebuild it");
        Check(VmaAllocationCount(vulkan_rhi) < vma_streaming, "...and the texture's GPU page table was released");

        const int loads_before = env.loads["a.png"];
        frame.enabled = true;
        settled = false;
        RunWithWatchdog("re-enable", [&] { settled = RunUntilResident(frame, 30000); }, 40);
        Check(settled && env.loads["a.png"] == loads_before + 1, "re-enabling streams it again (the failure memo was cleared)");
        auto resource = manager.GetVirtualTexture(producer.getHandle());
        const bool ok = resolver.Resolve(manager, producer.getHandle(), VirtualTextureResolver::Mode::TexelFetch, kFallback, gpu);
        const Tally t = Compare(gpu, resource, *env.textures["a.png"]);
        Check(ok && t.resident_wrong == 0 && t.absent_bad == 0 && t.resident_ok == 700u * 500u,
              "GPU readback after disable/enable: still correct");
    }

    // ---- C8: 关停拆自己拉起来的管理器 ----
    {
        RunWithWatchdog("producer Shutdown", [&] { producer.Shutdown(); });
        Check(!producer.isStreaming(), "Shutdown releases the texture");
        Check(manager.getPhysicalTexturePool() == nullptr,
              "Shutdown tore down the manager the producer brought up (pool released while the RHI is alive)");
        Check(VmaAllocationCount(vulkan_rhi) == vma_with_resolver, "VMA: back to baseline -- nothing leaked end to end");
        RunWithWatchdog("producer Shutdown again", [&] { producer.Shutdown(); });
        Check(true, "a second Shutdown is harmless");
    }

    // =========================================================================
    std::printf("--- D: a manager someone ELSE brought up is not the producer's to shut down ---\n");
    {
        VirtualTextureManager external;
        VirtualTextureConfig config;
        config.page_size = kPageSize;
        config.physical_pool_width = 8;
        config.physical_pool_height = 8;
        config.memory_budget_mb = 64;
        external.SetConfig(config);
        Check(external.Initialize(&vulkan_rhi, nullptr), "external manager initialised by its owner");

        VirtualTextureProducer guest;
        guest.Bind(&external, &TestLoader, &env, producer_config);
        Frame guest_frame {&vulkan_rhi, &guest, &external};
        guest_frame.path = "b.png";
        settled = false;
        RunWithWatchdog("guest streams", [&] { settled = RunUntilResident(guest_frame, 30000); }, 40);
        Check(settled, "the producer streams through a manager it did not create");
        Check(external.getPhysicalTexturePool()->getPoolWidthPages() == 8,
              "...without resizing the owner's 8x8 pool");
        RunWithWatchdog("guest Shutdown", [&] { guest.Shutdown(); });
        Check(external.getPhysicalTexturePool() != nullptr,
              "guest Shutdown leaves the owner's manager (and pool) alone");
        RunWithWatchdog("external Shutdown", [&] { external.Shutdown(); });
    }
    Check(VmaAllocationCount(vulkan_rhi) == vma_with_resolver, "VMA: baseline again");

    resolver.Shutdown();
    vulkan_rhi.Shutdown();
    std::printf("\n%d checks, %d failures\n", g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
