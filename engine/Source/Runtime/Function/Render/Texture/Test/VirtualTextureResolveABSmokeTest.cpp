// =============================================================================
// VirtualTexture 页表**间接寻址采样** A/B（Vulkan headless）。
//
// 前两个 VT 测试验的是"数据对不对"：页池里的像素 == 源、页表 texel == CPU 项。
// 但它们的 A 路（读回页表 → 取槽位 → 读回页池）是**测试里的 C++ 在模拟 shader**，
// 真正的 shader 从来没读过页表。本测试让真的 GLSL（vt_resolve.comp，经 vt_common.h）
// 去做间接寻址，再读回逐 texel 和源纹理比：
//
//   A 路 = GPU 上 shader 的结果：虚拟 texel → 页表 texelFetch → 槽位 → 页池 texelFetch
//   B 路 = 源纹理对应 texel（驻留页）/ fallback 色（不驻留页）
//
// 判据：
//   ① 驻留页的每个 texel，A == B（槽位→物理坐标的换算、页内偏移、页表格式在 GLSL 与 C++
//      两侧一致；任何一侧改了契约这里当场红）；
//   ② 被驱逐的页，shader 看到的是 fallback 且 resident=0 —— **不是**占着它旧槽位的那个
//      别的页的像素（页表失效同步在 shader 层面的最终证据）；
//   ③ 整数 texel 入口（VTFetch）与 UV 入口（VTSampleUV）逐 texel 给出同一结果；
//   ④ 非整页大小的纹理（100x90，右/下边缘页残缺）也对。
//
//   cmake -B build -DZENGINE_BUILD_VT_RESOLVE_AB_SMOKE_TEST=ON
//   cmake --build build --target VirtualTextureResolveABSmokeTest
// =============================================================================

#include "CommonPCH/pch.h"

#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Function/Render/Interface/RHI.h"
#include "Runtime/Function/Render/Interface/Vulkan/VulkanRHI.h"
#include "Runtime/Function/Render/RenderType.h"
#include "Runtime/Function/Render/Texture/VirtualTextureManager.h"
#include "Runtime/Function/Render/Texture/VirtualTextureResolver.h"
#include "Runtime/Function/Render/Texture/VirtualTextureTypes.h"
#include "Runtime/Function/Render/WindowSystem.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <memory>
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

    // 池满 / 注销路径若回归成死锁，表现为 FAIL 而不是挂死 CI。
    template <class Fn>
    void RunWithWatchdog(const char* what, Fn&& fn, int seconds = 30)
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

    constexpr uint32_t kPageSize = 64;
    constexpr uint32_t kPoolPagesPerSide = 2;  // 4 个槽位：故意小，逼出驱逐
    constexpr float kFallback[4] = {1.0f, 0.0f, 1.0f, 1.0f};
    constexpr uint32_t kFallbackPacked = 0xFFFF00FFu;  // packUnorm4x8(1,0,1,1)：R 在低字节

    // 源纹理 texel 打包成与 packUnorm4x8 相同的布局（R 低字节，A 高字节）。
    uint32_t SourcePacked(const TextureData& source, uint32_t x, uint32_t y)
    {
        const uint8_t* t = source.m_Pixels + (static_cast<size_t>(y) * source.m_Width + x) * 4u;
        return static_cast<uint32_t>(t[0]) | (static_cast<uint32_t>(t[1]) << 8) | (static_cast<uint32_t>(t[2]) << 16) |
               (static_cast<uint32_t>(t[3]) << 24);
    }

    // 每个 texel 都不同（R=x, G=y, B=种子^页号），任何寻址错位 / 串到别的页都必然失配。
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
        uint32_t resident_expected_ok = 0;   // 该驻留且颜色对
        uint32_t resident_wrong_color = 0;   // 该驻留但颜色错（寻址错位 / 串页）
        uint32_t absent_expected_ok = 0;     // 该不驻留且是 fallback
        uint32_t absent_but_resident = 0;    // 该不驻留却被 shader 当成驻留（陈旧页表项）
        uint32_t resident_but_absent = 0;    // 该驻留却被 shader 当成不驻留
        uint32_t absent_wrong_fallback = 0;  // 不驻留但返回的不是 fallback 色
    };

    // 页 (px,py) 该不该驻留：看 CPU 侧权威状态（PageTableEntry），不看 GPU。
    Tally Compare(const std::vector<VirtualTextureResolvedTexel>& gpu,
                  const std::shared_ptr<VirtualTextureResource>& resource,
                  const TextureData& source)
    {
        Tally t;
        const uint32_t width = source.m_Width;
        const uint32_t height = source.m_Height;
        if (gpu.size() != static_cast<size_t>(width) * height)
        {
            t.resident_wrong_color = 0xFFFFFFFFu;  // 尺寸都不对，必红
            return t;
        }
        for (uint32_t y = 0; y < height; ++y)
        {
            for (uint32_t x = 0; x < width; ++x)
            {
                const PageTableEntry cpu = resource->GetPageTableEntry(0, x / kPageSize, y / kPageSize);
                const bool should_be_resident = cpu.is_valid && cpu.physical_slot != UINT32_MAX;
                const VirtualTextureResolvedTexel& g = gpu[static_cast<size_t>(y) * width + x];
                if (should_be_resident)
                {
                    if (g.resident == 0u)
                    {
                        ++t.resident_but_absent;
                    }
                    else if (g.rgba == SourcePacked(source, x, y))
                    {
                        ++t.resident_expected_ok;
                    }
                    else
                    {
                        ++t.resident_wrong_color;
                    }
                }
                else
                {
                    if (g.resident != 0u)
                    {
                        ++t.absent_but_resident;
                    }
                    else if (g.rgba == kFallbackPacked)
                    {
                        ++t.absent_expected_ok;
                    }
                    else
                    {
                        ++t.absent_wrong_fallback;
                    }
                }
            }
        }
        return t;
    }

    void Report(const char* label, const Tally& t)
    {
        std::printf("       [info] %s: resident ok=%u wrong=%u missing=%u | absent ok=%u stale=%u badfallback=%u\n",
                    label,
                    t.resident_expected_ok,
                    t.resident_wrong_color,
                    t.resident_but_absent,
                    t.absent_expected_ok,
                    t.absent_but_resident,
                    t.absent_wrong_fallback);
    }

    bool Clean(const Tally& t)
    {
        return t.resident_wrong_color == 0 && t.resident_but_absent == 0 && t.absent_but_resident == 0 &&
               t.absent_wrong_fallback == 0;
    }
}  // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("=== VirtualTexture page-table indirect sampling A/B smoke test (Vulkan headless) ===\n");

    SystemRegistry::GetInstance().RegisterSystem(new WindowSystem());
    VulkanRHI vulkan_rhi;
    const bool vulkan_ready = vulkan_rhi.Initialize();
    Check(vulkan_ready, "VulkanRHI::Initialize succeeds (headless)");
    if (!vulkan_ready)
    {
        std::printf("\n%d checks, %d failures\n", g_Checks, g_Failures);
        return 1;
    }

    auto manager = std::make_unique<VirtualTextureManager>();
    VirtualTextureConfig config;
    config.page_size = kPageSize;
    config.physical_pool_width = kPoolPagesPerSide;
    config.physical_pool_height = kPoolPagesPerSide;
    config.memory_budget_mb = 64;
    config.enable_async_loading = false;
    manager->SetConfig(config);
    manager->Initialize(&vulkan_rhi, nullptr);

    VirtualTextureResolver resolver;
    const bool resolver_ready = resolver.Initialize(&vulkan_rhi);
    Check(resolver_ready, "vt_resolve.comp compiled and the compute pipeline was created");
    if (!resolver_ready)
    {
        std::printf("       [hint] vt_resolve_comp.h missing? rebuild the ZShaderCompile target\n");
        std::printf("\n%d checks, %d failures\n", g_Checks, g_Failures);
        return 1;
    }

    // A: 100x90 —— 不是页大小的整数倍：2x2 页，右列 36 宽、下行 26 高都是残缺页。
    // B: 128x64 —— 2x1 页。A+B = 6 页 > 4 槽位。
    auto source_a = MakeSource(100, 90, 0x21);
    auto source_b = MakeSource(128, 64, 0x84);
    const VirtualTextureHandle handle_a = manager->RegisterVirtualTexture(source_a, kPageSize);
    const VirtualTextureHandle handle_b = manager->RegisterVirtualTexture(source_b, kPageSize);
    auto vt_a = manager->GetVirtualTexture(handle_a);
    auto vt_b = manager->GetVirtualTexture(handle_b);
    Check(vt_a && vt_a->getPagesX() == 2 && vt_a->getPagesY() == 2, "VT-A (100x90) is 2x2 pages with partial edge pages");
    Check(vt_b && vt_b->getPagesX() == 2 && vt_b->getPagesY() == 1, "VT-B (128x64) is 2x1 pages");

    std::vector<VirtualTextureResolvedTexel> gpu;

    // ---------------------------------------------------------------------
    std::printf("--- stage 0: nothing loaded yet ---\n");
    // 从未驻留 ⇒ 没有页表纹理、池图也没内容；Resolver 必须干净地拒绝/兜底，不能绑空图像。
    Check(!resolver.Resolve(*manager, handle_a, VirtualTextureResolver::Mode::TexelFetch, kFallback, gpu),
          "resolving before any page was ever uploaded fails cleanly (pool image still UNDEFINED)");

    // ---------------------------------------------------------------------
    std::printf("--- stage 1: load all of A, shader indirect lookup ---\n");
    RunWithWatchdog("ForceLoadPages(A)", [&] { manager->ForceLoadPages(handle_a, 0); });
    {
        const bool ok_t = resolver.Resolve(*manager, handle_a, VirtualTextureResolver::Mode::TexelFetch, kFallback, gpu);
        Check(ok_t && gpu.size() == 100u * 90u, "A: texel-fetch resolve returned 100x90 texels");
        const Tally t = Compare(gpu, vt_a, *source_a);
        Report("A texel", t);
        Check(Clean(t) && t.resident_expected_ok == 100u * 90u,
              "A: every texel the shader reads through the page table equals the source");

        std::vector<VirtualTextureResolvedTexel> gpu_uv;
        const bool ok_u = resolver.Resolve(*manager, handle_a, VirtualTextureResolver::Mode::UV, kFallback, gpu_uv);
        Check(ok_u && gpu_uv.size() == gpu.size() &&
                  std::memcmp(gpu_uv.data(), gpu.data(), gpu.size() * sizeof(VirtualTextureResolvedTexel)) == 0,
              "A: UV entry point (VTSampleUV) == integer entry point (VTFetch), texel for texel");
    }

    // ---------------------------------------------------------------------
    std::printf("--- stage 2: load B into the full pool (evicts 2 of A's pages) ---\n");
    RunWithWatchdog("ForceLoadPages(B) with a full pool", [&] { manager->ForceLoadPages(handle_b, 0); });
    {
        Check(resolver.Resolve(*manager, handle_a, VirtualTextureResolver::Mode::TexelFetch, kFallback, gpu),
              "A: resolve after cross-VT eviction");
        const Tally ta = Compare(gpu, vt_a, *source_a);
        Report("A after eviction", ta);
        // 核心：被驱逐的页，shader 看到 fallback，而不是占了它旧槽位的 B 的像素。
        Check(ta.absent_but_resident == 0 && ta.absent_wrong_fallback == 0,
              "A: evicted pages read as fallback in the shader -- NOT as the pixels of the page that took their slot");
        Check(ta.absent_expected_ok > 0, "A: some pages really were evicted (the scenario is meaningful)");
        Check(Clean(ta) && ta.resident_expected_ok > 0, "A: surviving pages still read their own pixels");

        Check(resolver.Resolve(*manager, handle_b, VirtualTextureResolver::Mode::TexelFetch, kFallback, gpu),
              "B: resolve");
        const Tally tb = Compare(gpu, vt_b, *source_b);
        Report("B", tb);
        Check(Clean(tb) && tb.resident_expected_ok == 128u * 64u, "B: every texel equals the source");
    }

    // ---------------------------------------------------------------------
    std::printf("--- stage 3: unload A completely ---\n");
    RunWithWatchdog("UnloadPages(A)", [&] { manager->UnloadPages(handle_a); });
    {
        Check(resolver.Resolve(*manager, handle_a, VirtualTextureResolver::Mode::TexelFetch, kFallback, gpu),
              "A: resolve after unload");
        const Tally t = Compare(gpu, vt_a, *source_a);
        Report("A unloaded", t);
        Check(Clean(t) && t.absent_expected_ok == 100u * 90u && t.resident_expected_ok == 0,
              "A: with nothing resident the shader returns the fallback for every texel");

        Check(resolver.Resolve(*manager, handle_b, VirtualTextureResolver::Mode::TexelFetch, kFallback, gpu),
              "B: resolve (unaffected by A's unload)");
        const Tally tb = Compare(gpu, vt_b, *source_b);
        Check(Clean(tb) && tb.resident_expected_ok == 128u * 64u, "B: still reads its own pixels");
    }

    // ---------------------------------------------------------------------
    std::printf("--- stage 4: reload A on top of B (slot reuse), then shut down ---\n");
    RunWithWatchdog("ForceLoadPages(A) again", [&] { manager->ForceLoadPages(handle_a, 0); });
    {
        Check(resolver.Resolve(*manager, handle_a, VirtualTextureResolver::Mode::TexelFetch, kFallback, gpu),
              "A: resolve after reload");
        const Tally ta = Compare(gpu, vt_a, *source_a);
        Report("A reloaded", ta);
        Check(Clean(ta), "A: reloaded pages read correctly through reused slots");
        Check(resolver.Resolve(*manager, handle_b, VirtualTextureResolver::Mode::TexelFetch, kFallback, gpu),
              "B: resolve after A's reload");
        const Tally tb = Compare(gpu, vt_b, *source_b);
        Report("B after A reload", tb);
        Check(Clean(tb), "B: pages evicted by A's reload read as fallback, survivors read correctly");
    }

    vt_a.reset();
    vt_b.reset();
    resolver.Shutdown();
    RunWithWatchdog("VirtualTextureManager::Shutdown", [&] { manager->Shutdown(); });
    manager.reset();
    vulkan_rhi.Shutdown();

    std::printf("\n%d checks, %d failures\n", g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
