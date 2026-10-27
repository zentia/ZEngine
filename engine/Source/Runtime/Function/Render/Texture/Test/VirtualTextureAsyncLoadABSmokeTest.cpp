// =============================================================================
// VirtualTexture **异步加载线程安全** A/B（Vulkan headless）。
//
// 前面几个 VT 测试全是同步路径（ForceLoadPages）。但 enable_async_loading 默认 true，
// Initialize 会拉起后台加载线程 —— 而那条路径此前：
//   * 从未真正跑起来：全文件唯一一处 notify 是关停，RequestPages 入队后从不唤醒 worker；
//   * 一旦跑起来就是错的：worker 直接调 LoadPage，LoadPage 会分配槽位、改页状态、提交
//     GPU 一次性命令缓冲，与渲染线程并发 = 数据竞争 + 并发 queue submit；
//   * cv 等的是 m_PageLoadingMutex，队列却由 m_PageLoadQueueMutex 保护 —— 谓词无同步地读队列。
//
// 修复后的契约：worker 只做 CPU 活（ExtractPagePixels），GPU 上传 / 槽位 / 页表只在
// owner 线程（调 Initialize 的线程）经 PumpLoads()/Update() 提交。本测试把它钉死：
//
//   ① worker 真的跑了（getAsyncPreparedCount > 0）—— 否则后面的断言全是在测同步路径；
//   ② GPU 活零次发生在 owner 线程之外（getGpuThreadViolationCount == 0）；
//   ③ 多线程同时 RequestPages 同一批页，最终每页都驻留且像素正确（真 shader 读回逐 texel
//      比对，不是 C++ 模拟）；
//   ④ 池比页少（驱逐压力）+ 并发请求下，**被驱逐页读到 fallback、不读别人的像素**；
//   ⑤ 背压：预备队列峰值有界（<= 2*max_concurrent_loads）；
//   ⑥ 运行中关停不死锁（看门狗）、之后可再次初始化。
//
//   cmake -B build -DZENGINE_BUILD_VT_ASYNC_LOAD_AB_SMOKE_TEST=ON
//   cmake --build build --target VirtualTextureAsyncLoadABSmokeTest
// =============================================================================

#include "CommonPCH/pch.h"

#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Core/Math/AxisAligned.h"
#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Function/Render/Interface/RHI.h"
#include "Runtime/Function/Render/Interface/Vulkan/VulkanRHI.h"
#include "Runtime/Function/Render/RenderType.h"
#include "Runtime/Function/Render/Texture/VirtualTextureManager.h"
#include "Runtime/Function/Render/Texture/VirtualTextureResolver.h"
#include "Runtime/Function/Render/Texture/VirtualTextureTypes.h"
#include "Runtime/Function/Render/WindowSystem.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <memory>
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
            std::printf("[FAIL] %s did not return within %d s -- deadlock / lost-wakeup regression\n", what, seconds);
            std::printf("\n%d checks, %d failures (aborted)\n", g_Checks, g_Failures + 1);
            std::fflush(stdout);
            std::_Exit(2);
        }
        future.get();
    }

    constexpr uint32_t kPageSize = 64;
    constexpr float kFallback[4] = {1.0f, 0.0f, 1.0f, 1.0f};
    constexpr uint32_t kFallbackPacked = 0xFFFF00FFu;

    uint32_t SourcePacked(const TextureData& s, uint32_t x, uint32_t y)
    {
        const uint8_t* t = s.m_Pixels + (static_cast<size_t>(y) * s.m_Width + x) * 4u;
        return static_cast<uint32_t>(t[0]) | (static_cast<uint32_t>(t[1]) << 8) | (static_cast<uint32_t>(t[2]) << 16) |
               (static_cast<uint32_t>(t[3]) << 24);
    }

    std::shared_ptr<TextureData> MakeSource(uint32_t width, uint32_t height, uint8_t seed)
    {
        auto data = std::make_shared<TextureData>();
        data->m_Width = width;
        data->m_Height = height;
        data->m_Depth = 1;
        data->m_MipLevels = 1;
        data->m_ArrayLayers = 1;
        data->m_Format = RHI_FORMAT_R8G8B8A8_UNORM;
        data->m_PixelsLength = width * height * 4u;
        data->m_Pixels = static_cast<uint8_t*>(std::malloc(data->m_PixelsLength));
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
                t[0] = static_cast<uint8_t>((x * 2u + seed) & 0xFF);
                t[1] = static_cast<uint8_t>((y * 3u + 1u) & 0xFF);
                t[2] = static_cast<uint8_t>(seed ^ (page_index * 17u));
                t[3] = 255;
            }
        }
        return data;
    }

    struct Tally
    {
        uint32_t resident_ok = 0;
        uint32_t resident_wrong = 0;     // 该驻留，颜色错 / 读成不驻留
        uint32_t absent_ok = 0;
        uint32_t absent_bad = 0;         // 该不驻留，却读到了别的像素（陈旧页表 / 槽位别名）
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
                    if (g.resident != 0u && g.rgba == SourcePacked(source, x, y))
                    {
                        ++t.resident_ok;
                    }
                    else
                    {
                        ++t.resident_wrong;
                    }
                }
                else
                {
                    if (g.resident == 0u && g.rgba == kFallbackPacked)
                    {
                        ++t.absent_ok;
                    }
                    else
                    {
                        ++t.absent_bad;
                    }
                }
            }
        }
        return t;
    }

    // 反复 Pump 直到队列排空且连续两次没有新进展；超时返回 false。
    bool PumpUntilIdle(VirtualTextureManager& manager, int timeout_ms)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        int idle_rounds = 0;
        while (std::chrono::steady_clock::now() < deadline)
        {
            manager.PumpLoads();
            if (manager.getPendingLoadCount() == 0)
            {
                if (++idle_rounds >= 3)
                {
                    return true;
                }
            }
            else
            {
                idle_rounds = 0;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    }

    void RequestAll(VirtualTextureManager& manager, VirtualTextureHandle handle)
    {
        // screen_size > 1 => mip 0。相机/物体位置无所谓，只要 distance 有限。
        manager.RequestPages(handle, Vector3(0, 0, 0), Vector3(0, 0, 10), AxisAlignedBox(), 2.0f);
    }
}  // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("=== VirtualTexture async-load thread-safety A/B smoke test (Vulkan headless) ===\n");

    SystemRegistry::GetInstance().RegisterSystem(new WindowSystem());
    VulkanRHI vulkan_rhi;
    const bool vulkan_ready = vulkan_rhi.Initialize();
    Check(vulkan_ready, "VulkanRHI::Initialize succeeds (headless)");
    if (!vulkan_ready)
    {
        std::printf("\n%d checks, %d failures\n", g_Checks, g_Failures);
        return 1;
    }

    // 6x6 = 36 页的大纹理；池只有 4x4 = 16 槽位 —— 逼出驱逐压力。
    constexpr uint32_t kPoolSide = 4;
    constexpr uint32_t kPagesPerSide = 6;
    constexpr uint32_t kDim = kPagesPerSide * kPageSize;
    constexpr uint32_t kMaxLoads = 4;

    auto manager = std::make_unique<VirtualTextureManager>();
    VirtualTextureConfig config;
    config.page_size = kPageSize;
    config.physical_pool_width = kPoolSide;
    config.physical_pool_height = kPoolSide;
    config.memory_budget_mb = 64;
    config.max_concurrent_loads = kMaxLoads;
    config.enable_async_loading = true;  // 本测试的全部意义
    manager->SetConfig(config);
    manager->Initialize(&vulkan_rhi, nullptr);

    VirtualTextureResolver resolver;
    Check(resolver.Initialize(&vulkan_rhi), "vt_resolve.comp pipeline ready");

    auto source = MakeSource(kDim, kDim, 0x37);
    const VirtualTextureHandle handle = manager->RegisterVirtualTexture(source, kPageSize);
    auto vt = manager->GetVirtualTexture(handle);
    Check(vt && vt->getPagesX() == kPagesPerSide && vt->getPagesY() == kPagesPerSide, "36-page virtual texture registered");

    std::vector<VirtualTextureResolvedTexel> gpu;

    // ---------------------------------------------------------------------
    std::printf("--- stage 1: one request, async worker, owner-thread commit ---\n");
    RequestAll(*manager, handle);
    {
        // 关键：此刻**不**调用 PumpLoads。worker 必须仅凭 RequestPages 的 notify 自己醒来，
        // 把预备队列备到背压上限（2*max_concurrent_loads = 8）。owner 的 Pump 在腾出预备队列后
        // 也会 notify，会掩盖"RequestPages 忘了 notify"这个旧缺陷，所以这里要隔离验证。
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
        while (manager->getAsyncPreparedCount() < kMaxLoads * 2u && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        Check(manager->getAsyncPreparedCount() == kMaxLoads * 2u,
              "the worker wakes on RequestPages alone (no pump) and fills the prepared queue to its back-pressure cap");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        Check(manager->getAsyncPreparedCount() == kMaxLoads * 2u && manager->getPreparedBacklogPeak() == kMaxLoads * 2u,
              "...and then stops there: nothing commits until the owner pumps (worker never commits by itself)");
    }
    Check(PumpUntilIdle(*manager, 20000), "the request queue drains (worker woke up and the owner committed)");
    Check(manager->getAsyncPreparedCount() > 0,
          "the worker thread really prepared pages (async path actually ran -- the old code never notified it)");
    {
        const VirtualTextureStats stats = manager->GetStats();
        std::printf("       [info] loaded=%u pool_used=%u/%u prepared=%u backlog_peak=%u\n",
                    stats.loaded_pages, stats.physical_pool_used_slots, stats.physical_pool_total_slots,
                    manager->getAsyncPreparedCount(), manager->getPreparedBacklogPeak());
        Check(stats.loaded_pages > 0 && stats.loaded_pages <= 16, "pages became resident, never more than the 16 slots");
    }
    Check(manager->getGpuThreadViolationCount() == 0, "no GPU work happened off the owner thread");

    // ---------------------------------------------------------------------
    std::printf("--- stage 2: 4 threads hammer RequestPages concurrently ---\n");
    {
        std::atomic<bool> go {false};
        std::vector<std::thread> threads;
        for (int i = 0; i < 4; ++i)
        {
            threads.emplace_back([&] {
                while (!go.load())
                {
                    std::this_thread::yield();
                }
                for (int k = 0; k < 25; ++k)
                {
                    RequestAll(*manager, handle);
                }
            });
        }
        go.store(true);
        // owner 线程同时在泵（真实情况：渲染线程每帧 Update，其它线程在请求）。
        const auto stop_at = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
        while (std::chrono::steady_clock::now() < stop_at)
        {
            manager->PumpLoads();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        for (auto& t : threads)
        {
            t.join();
        }
    }
    Check(PumpUntilIdle(*manager, 30000), "all concurrent requests drain");
    Check(manager->getGpuThreadViolationCount() == 0, "still no GPU work off the owner thread under concurrent requests");
    {
        const uint32_t cap = kMaxLoads * 2u;
        std::printf("       [info] backlog peak=%u (cap %u)\n", manager->getPreparedBacklogPeak(), cap);
        Check(manager->getPreparedBacklogPeak() <= cap,
              "back-pressure: prepared-but-uncommitted pages never exceed 2*max_concurrent_loads");
    }

    // ---------------------------------------------------------------------
    std::printf("--- stage 3: what the shader actually sees ---\n");
    {
        const bool ok = resolver.Resolve(*manager, handle, VirtualTextureResolver::Mode::TexelFetch, kFallback, gpu);
        Check(ok && gpu.size() == static_cast<size_t>(kDim) * kDim, "resolve returned every texel");
        const Tally t = Compare(gpu, vt, *source);
        std::printf("       [info] resident ok=%u wrong=%u | absent ok=%u bad=%u\n",
                    t.resident_ok, t.resident_wrong, t.absent_ok, t.absent_bad);
        Check(t.resident_wrong == 0, "every resident page reads its own pixels through the GPU page table");
        Check(t.absent_bad == 0,
              "every evicted/never-loaded page reads fallback -- not another page's pixels (no stale table, no slot aliasing)");
        Check(t.resident_ok > 0 && t.absent_ok > 0,
              "the scenario is meaningful: some pages resident, some evicted (36 pages > 16 slots)");
        Check(t.resident_ok == manager->GetStats().loaded_pages * kPageSize * kPageSize,
              "resident texel count == loaded pages x page area (no duplicate / lost residency)");
    }

    // ---------------------------------------------------------------------
    std::printf("--- stage 4: shut down while requests are still queued ---\n");
    for (int i = 0; i < 8; ++i)
    {
        RequestAll(*manager, handle);
    }
    vt.reset();
    resolver.Shutdown();
    // worker 可能正睡在 cv 上，也可能正卡在背压上 —— 关停都必须唤醒它并 join。
    RunWithWatchdog("VirtualTextureManager::Shutdown with queued work", [&] { manager->Shutdown(); });
    Check(true, "Shutdown with a live worker and queued requests returned (no lost wakeup)");
    Check(manager->getGpuThreadViolationCount() == 0, "no GPU thread violations over the whole run");

    // 再来一遍：同一个对象能重新初始化并重新工作（stop 标志被正确复位）。
    manager->SetConfig(config);
    manager->Initialize(&vulkan_rhi, nullptr);
    auto source2 = MakeSource(128, 128, 0x5A);
    const VirtualTextureHandle handle2 = manager->RegisterVirtualTexture(source2, kPageSize);
    auto vt2 = manager->GetVirtualTexture(handle2);
    RequestAll(*manager, handle2);
    Check(PumpUntilIdle(*manager, 20000), "after re-Initialize the async path works again");
    {
        VirtualTextureResolver resolver2;
        Check(resolver2.Initialize(&vulkan_rhi), "resolver re-init");
        const bool ok = resolver2.Resolve(*manager, handle2, VirtualTextureResolver::Mode::TexelFetch, kFallback, gpu);
        const Tally t = Compare(gpu, vt2, *source2);
        Check(ok && t.resident_wrong == 0 && t.absent_bad == 0 && t.resident_ok == 128u * 128u,
              "re-initialised: all 4 pages of the second texture resident and correct");
        resolver2.Shutdown();
    }
    vt2.reset();
    RunWithWatchdog("second Shutdown", [&] { manager->Shutdown(); });
    manager.reset();
    vulkan_rhi.Shutdown();

    std::printf("\n%d checks, %d failures\n", g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
