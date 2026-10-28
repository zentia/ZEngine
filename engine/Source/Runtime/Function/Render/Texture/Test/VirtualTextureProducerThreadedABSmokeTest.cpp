// =============================================================================
// VirtualTexture **渲染线程 / RHI 线程拆分** A/B（Vulkan headless，真两线程并发）。
//
// 真实引擎里 Vulkan 编辑器走并行渲染：渲染线程与 RHI 线程是两个线程，GPU 工作只能在 RHI 线程
// （Vulkan 的"当前命令缓冲"和图形队列归它所有——RenderSystemFrameCommands.cpp 记录过把 GPU
// 工作放到渲染线程会崩溃 / 双线程同时录同一命令缓冲）。VirtualTextureProducer 因此拆成
//   TickCpu (渲染线程)：加载源图、RegisterVirtualTexture、RequestPages —— 只有 CPU 活；
//   TickGpu (RHI 线程)：建池、销毁被丢弃的纹理、PumpLoads/Update —— 全部 GPU 活。
//
// 之前的生产者测试在**同一线程**上背靠背调 Tick，测不到任何跨线程问题。本测试让两个线程真并发
// 跑，并钉住：
//   ① GPU 活零次发生在 RHI 线程之外（以 getGpuThreadViolationCount 为准——管理器的 owner 线程
//      在 TickGpu 首次建池时被记为 RHI 线程，之后任何别的线程碰 GPU 都会计数）；
//   ② 并发运行中纹理最终完整驻留，真 shader 读回逐 texel 与源一致；
//   ③ 路径切换、关掉再打开（渲染线程只"退役"句柄，由 RHI 线程销毁）期间无崩溃、无校验错误、
//      无泄漏（VMA 分配数回到基线）——特别是"渲染线程丢掉纹理时 RHI 线程可能正在 Update 它"
//      这个 use-after-free 窗口；
//   ④ 池建不起来时失败回报给渲染线程，且不每帧重试。
//
//   cmake -B build -DZENGINE_BUILD_VT_PRODUCER_THREADED_AB_SMOKE_TEST=ON
//   cmake --build build --target VirtualTextureProducerThreadedABSmokeTest
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

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
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
    void RunWithWatchdog(const char* what, Fn&& fn, int seconds = 60)
    {
        auto future = std::async(std::launch::async, std::forward<Fn>(fn));
        if (future.wait_for(std::chrono::seconds(seconds)) == std::future_status::timeout)
        {
            std::printf("[FAIL] %s did not return within %d s -- deadlock regression\n", what, seconds);
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
                t[0] = static_cast<uint8_t>((x + seed) & 0xFF);
                t[1] = static_cast<uint8_t>((y * 3u + seed) & 0xFF);
                t[2] = static_cast<uint8_t>(seed ^ (page_index * 29u));
                t[3] = 255;
            }
        }
        return data;
    }

    struct LoaderEnv
    {
        std::mutex mutex;
        std::map<std::string, std::shared_ptr<TextureData>> textures;
        std::map<std::string, int> loads;
        std::thread::id loader_thread;  // 最近一次调用 loader 的线程（应当是"渲染线程"）
    };

    std::shared_ptr<TextureData> TestLoader(void* user, const std::string& path)
    {
        auto* env = static_cast<LoaderEnv*>(user);
        std::lock_guard<std::mutex> lock(env->mutex);
        ++env->loads[path];
        env->loader_thread = std::this_thread::get_id();
        auto it = env->textures.find(path);
        return it == env->textures.end() ? nullptr : it->second;
    }

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

    // 两个真线程：render 线程循环 TickCpu，rhi 线程循环 TickGpu。主线程通过原子变量改"期望状态"
    // （相当于 CVar），并在需要时暂停两个线程做读回。
    class TwoThreadHarness
    {
    public:
        TwoThreadHarness(VulkanRHI& rhi, VirtualTextureProducer& producer, VirtualTextureManager& manager)
            : m_Rhi(rhi), m_Producer(producer), m_Manager(manager)
        {
        }

        void Start()
        {
            m_Stop = false;
            m_RenderThread = std::thread([this] {
                while (!m_Stop)
                {
                    if (!m_Paused)
                    {
                        std::string path;
                        {
                            std::lock_guard<std::mutex> lock(m_StateMutex);
                            path = m_Path;
                        }
                        m_Producer.TickCpu(m_Enabled.load(), path, Vector3(0, 0, 0), Vector3(0, 0, -1));
                        m_RenderFrames.fetch_add(1);
                    }
                    else
                    {
                        m_RenderParked = true;
                    }
                    if (m_Paused)
                    {
                        m_RenderParked = true;
                    }
                    else
                    {
                        m_RenderParked = false;
                    }
                    std::this_thread::sleep_for(std::chrono::microseconds(500));
                }
            });
            m_RhiThread = std::thread([this] {
                while (!m_Stop)
                {
                    if (!m_Paused)
                    {
                        m_Producer.TickGpu(&m_Rhi, nullptr, Vector3(0, 0, 0), Vector3(0, 0, -1), 1.047f, 16.0f / 9.0f,
                                           1280, 720, 0.016f);
                        m_RhiFrames.fetch_add(1);
                        m_RhiParked = false;
                    }
                    else
                    {
                        m_RhiParked = true;
                    }
                    std::this_thread::sleep_for(std::chrono::microseconds(500));
                }
            });
        }

        void Stop()
        {
            m_Stop = true;
            if (m_RenderThread.joinable())
            {
                m_RenderThread.join();
            }
            if (m_RhiThread.joinable())
            {
                m_RhiThread.join();
            }
        }

        void SetState(bool enabled, const std::string& path)
        {
            std::lock_guard<std::mutex> lock(m_StateMutex);
            m_Enabled = enabled;
            m_Path = path;
        }

        // 暂停两个线程并等它们都停在循环顶部（读回 / 断言要在静止状态下做）。
        void Pause()
        {
            m_Paused = true;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (!(m_RenderParked.load() && m_RhiParked.load()) && std::chrono::steady_clock::now() < deadline)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        void Resume() { m_Paused = false; }

        bool WaitResident(int timeout_ms)
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
            while (std::chrono::steady_clock::now() < deadline)
            {
                uint32_t resident = 0;
                uint32_t total = 0;
                m_Producer.GetResidency(resident, total);
                if (total > 0 && resident == total && m_Manager.getPendingLoadCount() == 0)
                {
                    return true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            return false;
        }

        uint64_t renderFrames() const { return m_RenderFrames.load(); }
        uint64_t rhiFrames() const { return m_RhiFrames.load(); }
        std::thread::id renderThreadId() const { return m_RenderThread.get_id(); }
        std::thread::id rhiThreadId() const { return m_RhiThread.get_id(); }

    private:
        VulkanRHI& m_Rhi;
        VirtualTextureProducer& m_Producer;
        VirtualTextureManager& m_Manager;

        std::thread m_RenderThread;
        std::thread m_RhiThread;
        std::atomic<bool> m_Stop {false};
        std::atomic<bool> m_Paused {false};
        std::atomic<bool> m_RenderParked {false};
        std::atomic<bool> m_RhiParked {false};
        std::atomic<bool> m_Enabled {false};
        std::mutex m_StateMutex;
        std::string m_Path;
        std::atomic<uint64_t> m_RenderFrames {0};
        std::atomic<uint64_t> m_RhiFrames {0};
    };
}  // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("=== VirtualTexture producer render-thread / RHI-thread split A/B (Vulkan headless) ===\n");

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
    const uint32_t vma_baseline = VmaAllocationCount(vulkan_rhi);

    LoaderEnv env;
    env.textures["a.png"] = MakeSource(700, 500, 0x31);
    env.textures["b.png"] = MakeSource(384, 256, 0x7C);

    CerrCapture capture;  // 整个并发阶段的校验层输出都收在这里，结尾统一断言

    VirtualTextureManager manager;
    VirtualTextureProducer producer;
    VirtualTextureProducer::Config producer_config;
    producer_config.log_every_frames = 0;
    producer.Bind(&manager, &TestLoader, &env, producer_config);

    TwoThreadHarness harness(vulkan_rhi, producer, manager);
    harness.Start();

    std::vector<VirtualTextureResolvedTexel> gpu;

    // =========================================================================
    std::printf("--- A: stream a.png with the two threads running concurrently ---\n");
    harness.SetState(true, "a.png");
    bool settled = false;
    RunWithWatchdog("stream a.png", [&] { settled = harness.WaitResident(30000); });
    Check(settled, "a.png becomes fully resident with TickCpu and TickGpu on different threads");
    Check(env.loads["a.png"] == 1, "the source was loaded exactly once");
    Check(env.loader_thread == harness.renderThreadId(), "the (slow, IO-bound) loader ran on the RENDER thread, not the RHI thread");
    Check(manager.getPhysicalTexturePool() != nullptr, "the RHI thread built the pool on demand");
    Check(manager.getGpuThreadViolationCount() == 0,
          "no GPU work happened off the thread that built the pool (the RHI thread)");
    Check(manager.getAsyncPreparedCount() > 0, "the async worker also contributed (three threads in play)");
    {
        harness.Pause();
        auto resource = manager.GetVirtualTexture(producer.getHandle());
        const bool ok =
            resolver.Resolve(manager, producer.getHandle(), VirtualTextureResolver::Mode::TexelFetch, kFallback, gpu);
        const Tally t = Compare(gpu, resource, *env.textures["a.png"]);
        std::printf("       [info] resident ok=%u wrong=%u | absent ok=%u bad=%u\n", t.resident_ok, t.resident_wrong,
                    t.absent_ok, t.absent_bad);
        Check(ok && t.resident_wrong == 0 && t.absent_bad == 0 && t.resident_ok == 700u * 500u,
              "GPU readback: every texel of a.png equals the source");
        harness.Resume();
    }

    // =========================================================================
    std::printf("--- B: hammer path switches while the RHI thread keeps updating ---\n");
    // 这是 use-after-free 窗口：渲染线程丢掉纹理（退役句柄）的同时，RHI 线程可能正在对它 Update。
    // 若渲染线程直接 Unregister，页表纹理会在 RHI 线程用它的时候被销毁。
    {
        const uint64_t frames_before = harness.rhiFrames();
        for (int i = 0; i < 60; ++i)
        {
            harness.SetState(true, (i % 2 == 0) ? "b.png" : "a.png");
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }
        harness.SetState(true, "a.png");
        Check(harness.rhiFrames() > frames_before + 5, "the RHI thread kept ticking throughout the switching (it was not blocked)");
        bool resettled = false;
        RunWithWatchdog("settle after switching", [&] { resettled = harness.WaitResident(30000); });
        Check(resettled, "after 60 rapid path switches the final texture still streams in fully");
        Check(manager.GetStats().total_virtual_textures == 1,
              "exactly one virtual texture is registered (every retired one was destroyed on the RHI thread)");
        Check(manager.getGpuThreadViolationCount() == 0, "still no GPU work off the RHI thread after the switching storm");
    }

    // =========================================================================
    std::printf("--- C: disable / re-enable under concurrency ---\n");
    {
        harness.SetState(false, "a.png");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (manager.GetStats().total_virtual_textures != 0 && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        Check(manager.GetStats().total_virtual_textures == 0, "disabling: the RHI thread destroyed the retired texture");
        Check(!producer.isStreaming(), "the producer no longer reports streaming");
        Check(manager.getPhysicalTexturePool() != nullptr, "the pool stays up for a cheap re-enable");

        harness.SetState(true, "a.png");
        bool again = false;
        RunWithWatchdog("re-enable", [&] { again = harness.WaitResident(30000); });
        Check(again, "re-enabling streams it back in fully");
        harness.Pause();
        auto resource = manager.GetVirtualTexture(producer.getHandle());
        const bool ok =
            resolver.Resolve(manager, producer.getHandle(), VirtualTextureResolver::Mode::TexelFetch, kFallback, gpu);
        const Tally t = Compare(gpu, resource, *env.textures["a.png"]);
        Check(ok && t.resident_wrong == 0 && t.absent_bad == 0 && t.resident_ok == 700u * 500u,
              "GPU readback after disable/enable: still correct");
        harness.Resume();
    }

    harness.Stop();

    // =========================================================================
    std::printf("--- D: teardown and leak accounting ---\n");
    RunWithWatchdog("producer Shutdown", [&] { producer.Shutdown(); });
    Check(manager.getPhysicalTexturePool() == nullptr, "Shutdown tore down the manager the producer brought up");
    Check(VmaAllocationCount(vulkan_rhi) == vma_baseline,
          "VMA: back to baseline after all that concurrency -- nothing leaked (every retired handle was destroyed)");
    Check(!capture.sawValidationError(),
          "the Vulkan validation layer said NOTHING during the whole two-thread run (no use-after-free, no bad state)");
    if (capture.sawValidationError())
    {
        std::printf("%s\n", capture.text().substr(0, 1500).c_str());
    }

    resolver.Shutdown();
    vulkan_rhi.Shutdown();
    std::printf("\n%d checks, %d failures\n", g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
