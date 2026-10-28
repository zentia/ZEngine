// =============================================================================
// VirtualTexture **片元阶段**页表消费者 A/B（Vulkan headless，离屏，不碰主相机 pass）。
//
// vt_resolve.comp 在 compute 里逐整数 texel 查页表，验证了查找契约本身。但真实材质是
// 在 fragment shader 里用**光栅化插值出来的 UV**去采样。本测试用一个全屏三角形把虚拟纹理
// 画到自己的离屏 R8G8B8A8 目标上（vt_blit.vert / vt_blit.frag，后者 #include vt_common.h），
// 读回逐像素比对：
//
//   A 路 = GPU 光栅化的结果（每像素中心 UV → 页表 → 槽位 → 页池）
//   B 路 = CPU 按**同一套 UV→texel 公式**从源纹理 / fallback 算出的期望值
//
// 它验证 compute 版验证不了的几件事：
//   ① vt_common.h 在 fragment 阶段能用（sampler2D/texelFetch 在两个阶段的约束不同）；
//   ② 1:1 输出下逐像素 == 源（光栅化器给的像素中心 UV 恰好落回同一个 texel）；
//   ③ 输出尺寸 != 虚拟纹理尺寸（放大 / 缩小）时，插值 UV → texel 的换算与 CPU 一致；
//   ④ 图像方向：输出第 0 行 == 虚拟纹理第 0 行（顶点着色器与 Vulkan y 向下约定一致，
//      任一侧翻 y 整张图上下颠倒）；
//   ⑤ 池满驱逐后，被驱逐页的区域在光栅化输出里是 fallback，不是别人的像素。
//
// fallback 用不透明洋红 (1,0,1,1)：目标清屏值是 (0,0,0,0)，alpha=0 的 fallback 会与
// "三角形根本没画到"无法区分。不透明 fallback 让"没画到"（0,0,0,0）与"画了但页不驻留"
// 两种情况各自可辨。
//
//   cmake -B build -DZENGINE_BUILD_VT_BLIT_AB_SMOKE_TEST=ON
//   cmake --build build --target VirtualTextureBlitABSmokeTest
// =============================================================================

#include "CommonPCH/pch.h"

#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Function/Render/Interface/RHI.h"
#include "Runtime/Function/Render/Interface/Vulkan/VulkanRHI.h"
#if defined(_WIN32)
    #include "Runtime/Function/Render/Interface/DX12/DX12RHI.h"
#endif
#include "Runtime/Function/Render/RenderType.h"
#include "Runtime/Function/Render/Texture/VirtualTextureBlitter.h"
#include "Runtime/Function/Render/Texture/VirtualTextureManager.h"
#include "Runtime/Function/Render/Texture/VirtualTextureTypes.h"
#include "Runtime/Function/Render/WindowSystem.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <memory>
#include <string>
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

    constexpr uint32_t kPageSize = 64;
    constexpr float kFallback[4] = {1.0f, 0.0f, 1.0f, 1.0f};
    constexpr uint32_t kFallbackPacked = 0xFFFF00FFu;  // R 在低字节：(255,0,255,255)
    constexpr uint32_t kClearPacked = 0u;              // 清屏值 (0,0,0,0)：三角形没画到

    uint32_t SourcePacked(const TextureData& s, uint32_t x, uint32_t y)
    {
        const uint8_t* t = s.m_Pixels + (static_cast<size_t>(y) * s.m_Width + x) * 4u;
        return static_cast<uint32_t>(t[0]) | (static_cast<uint32_t>(t[1]) << 8) | (static_cast<uint32_t>(t[2]) << 16) |
               (static_cast<uint32_t>(t[3]) << 24);
    }

    // 每个 texel 都不同（R=x*2+seed, G=y*3+1, B=种子^页号）：任何寻址错位 / 翻转 / 串页必然失配。
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

    // CPU 侧期望值：与 vt_common.h 的 VTSampleUV 逐步对应。
    //   uv = (像素中心) / 输出尺寸；texel_f = clamp(uv,0,1) * 虚拟尺寸；texel = min(uint(texel_f), 虚拟尺寸-1)
    // 页是否驻留看 CPU 侧权威状态（PageTableEntry），不看 GPU。
    // 注意用 float 做同样的运算（不是 double）：GPU 是 fp32，CPU 若用 double 会在恰好落在
    // texel 边界的像素上与 GPU 的最后一位舍入不同。
    uint32_t ExpectedPixel(uint32_t px,
                           uint32_t py,
                           uint32_t out_w,
                           uint32_t out_h,
                           const std::shared_ptr<VirtualTextureResource>& resource,
                           const TextureData& source)
    {
        const float u = (static_cast<float>(px) + 0.5f) / static_cast<float>(out_w);
        const float v = (static_cast<float>(py) + 0.5f) / static_cast<float>(out_h);
        const float tx_f = std::fmin(std::fmax(u, 0.0f), 1.0f) * static_cast<float>(source.m_Width);
        const float ty_f = std::fmin(std::fmax(v, 0.0f), 1.0f) * static_cast<float>(source.m_Height);
        const uint32_t tx = std::min(static_cast<uint32_t>(tx_f), source.m_Width - 1u);
        const uint32_t ty = std::min(static_cast<uint32_t>(ty_f), source.m_Height - 1u);

        const PageTableEntry cpu = resource->GetPageTableEntry(0, tx / kPageSize, ty / kPageSize);
        const bool resident = cpu.is_valid && cpu.physical_slot != UINT32_MAX;
        return resident ? SourcePacked(source, tx, ty) : kFallbackPacked;
    }

    // 采样点恰好落在 texel 边界（texel_f 是整数）时，"该取哪一侧"本身就是歧义的：
    // 光栅化器逐像素插值出的 UV 与 CPU 精确除法的最后一位舍入不同，GPU 可能取到边界另一侧的
    // texel（真实硬件采样在边界上同样如此，两侧都合法）。例：37 宽输出、100 宽虚拟纹理时，
    // 像素 18 的 UV = 18.5/37 = 0.5，texel_f = 50.0 恰好是边界，GPU 取了 49、CPU 取了 50。
    //
    // 只对**确实落在边界上**的像素接受相邻 texel（texel_f 距整数 < kBoundaryEps）；其余
    // 像素仍严格逐字节比较。被容忍的像素数单独统计并断言有界，免得容忍度悄悄吞掉真 bug。
    constexpr float kBoundaryEps = 1.0e-3f;  // 单位：texel。fp32 插值误差的数量级是 1e-5~1e-4

    bool IsBoundary(float texel_f)
    {
        const float nearest = std::nearbyint(texel_f);
        return std::fabs(texel_f - nearest) < kBoundaryEps && nearest > 0.0f;
    }

    struct Candidates
    {
        uint32_t txs[2];
        uint32_t tys[2];
        uint32_t tx_count = 1;
        uint32_t ty_count = 1;
        bool boundary = false;
    };

    Candidates MakeCandidates(uint32_t px, uint32_t py, uint32_t out_w, uint32_t out_h, const TextureData& source)
    {
        const float u = (static_cast<float>(px) + 0.5f) / static_cast<float>(out_w);
        const float v = (static_cast<float>(py) + 0.5f) / static_cast<float>(out_h);
        const float tx_f = std::fmin(std::fmax(u, 0.0f), 1.0f) * static_cast<float>(source.m_Width);
        const float ty_f = std::fmin(std::fmax(v, 0.0f), 1.0f) * static_cast<float>(source.m_Height);

        Candidates c;
        c.txs[0] = std::min(static_cast<uint32_t>(tx_f), source.m_Width - 1u);
        c.tys[0] = std::min(static_cast<uint32_t>(ty_f), source.m_Height - 1u);
        if (IsBoundary(tx_f))
        {
            // 边界两侧：floor(nearest) - 1 与 floor(nearest)
            const uint32_t edge = static_cast<uint32_t>(std::nearbyint(tx_f));
            c.txs[0] = std::min(edge - 1u, source.m_Width - 1u);
            c.txs[1] = std::min(edge, source.m_Width - 1u);
            c.tx_count = 2;
            c.boundary = true;
        }
        if (IsBoundary(ty_f))
        {
            const uint32_t edge = static_cast<uint32_t>(std::nearbyint(ty_f));
            c.tys[0] = std::min(edge - 1u, source.m_Height - 1u);
            c.tys[1] = std::min(edge, source.m_Height - 1u);
            c.ty_count = 2;
            c.boundary = true;
        }
        return c;
    }

    struct Tally
    {
        uint32_t match = 0;               // 严格相等（含边界像素恰好取到 CPU 那一侧）
        uint32_t boundary_tolerated = 0;  // 严格不等，但落在边界上且等于边界另一侧的合法 texel
        uint32_t boundary_pixels = 0;     // 落在边界上的像素总数（上界）
        uint32_t mismatch = 0;
        uint32_t cleared = 0;  // 读回 == 清屏值：三角形没盖到的像素（必须为 0）
        uint32_t first_bad_x = 0xFFFFFFFFu;
        uint32_t first_bad_y = 0;
    };

    Tally Compare(const std::vector<uint32_t>& gpu,
                  uint32_t out_w,
                  uint32_t out_h,
                  const std::shared_ptr<VirtualTextureResource>& resource,
                  const TextureData& source)
    {
        Tally t;
        if (gpu.size() != static_cast<size_t>(out_w) * out_h)
        {
            t.mismatch = 0xFFFFFFFFu;
            return t;
        }
        for (uint32_t y = 0; y < out_h; ++y)
        {
            for (uint32_t x = 0; x < out_w; ++x)
            {
                const uint32_t g = gpu[static_cast<size_t>(y) * out_w + x];
                if (g == kClearPacked)
                {
                    ++t.cleared;
                }
                const Candidates c = MakeCandidates(x, y, out_w, out_h, source);
                if (c.boundary)
                {
                    ++t.boundary_pixels;
                }
                if (g == ExpectedPixel(x, y, out_w, out_h, resource, source))
                {
                    ++t.match;
                    continue;
                }
                // 严格不等：只有边界像素才允许落到"边界另一侧的合法 texel"。
                bool tolerated = false;
                if (c.boundary)
                {
                    for (uint32_t i = 0; i < c.tx_count && !tolerated; ++i)
                    {
                        for (uint32_t j = 0; j < c.ty_count && !tolerated; ++j)
                        {
                            const PageTableEntry cpu = resource->GetPageTableEntry(0, c.txs[i] / kPageSize, c.tys[j] / kPageSize);
                            const bool resident = cpu.is_valid && cpu.physical_slot != UINT32_MAX;
                            const uint32_t want = resident ? SourcePacked(source, c.txs[i], c.tys[j]) : kFallbackPacked;
                            tolerated = (g == want);
                        }
                    }
                }
                if (tolerated)
                {
                    ++t.boundary_tolerated;
                }
                else
                {
                    if (t.mismatch == 0)
                    {
                        t.first_bad_x = x;
                        t.first_bad_y = y;
                    }
                    ++t.mismatch;
                }
            }
        }
        return t;
    }

    void Report(const char* label, const Tally& t, const std::vector<uint32_t>& gpu, uint32_t out_w,
                const std::shared_ptr<VirtualTextureResource>& resource, const TextureData& source)
    {
        std::printf("       [info] %s: match=%u boundary-tolerated=%u (of %u boundary px) mismatch=%u cleared=%u\n",
                    label, t.match, t.boundary_tolerated, t.boundary_pixels, t.mismatch, t.cleared);
        if (t.first_bad_x != 0xFFFFFFFFu && t.mismatch != 0xFFFFFFFFu)
        {
            const uint32_t out_h = static_cast<uint32_t>(gpu.size() / out_w);
            std::printf("       [diag] first mismatch at (%u,%u): gpu=0x%08X expected=0x%08X\n",
                        t.first_bad_x,
                        t.first_bad_y,
                        gpu[static_cast<size_t>(t.first_bad_y) * out_w + t.first_bad_x],
                        ExpectedPixel(t.first_bad_x, t.first_bad_y, out_w, out_h, resource, source));
        }
    }
}  // namespace

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // 后端选择：默认 Vulkan；`--rhi dx12`（仅 Windows）跑同一套 22 项检查在 DX12 无头设备上。
    // 同一份源数据、同一个 CPU 参考，所以两个后端的绿是同一个判据下的绿。
    bool use_dx12 = false;
    for (int i = 1; i < argc; ++i)
    {
        if (std::string(argv[i]) == "--rhi" && i + 1 < argc)
        {
            use_dx12 = (std::string(argv[i + 1]) == "dx12");
        }
    }
#if !defined(_WIN32)
    if (use_dx12)
    {
        std::printf("--rhi dx12 is only available on Windows\n");
        return 2;
    }
#endif
    std::printf("=== VirtualTexture fragment-stage page-table consumer A/B smoke test (%s headless) ===\n",
                use_dx12 ? "DX12" : "Vulkan");

    SystemRegistry::GetInstance().RegisterSystem(new WindowSystem());
    // RHI::Initialize/Shutdown 在基类里是 protected（IEngineSystem），只能经具体类型调用。
    std::unique_ptr<VulkanRHI> vulkan_owner;
#if defined(_WIN32)
    std::unique_ptr<DX12RHI> dx12_owner;
#endif
    RHI* rhi_ptr = nullptr;
    bool vulkan_ready = false;
    if (use_dx12)
    {
#if defined(_WIN32)
        dx12_owner = std::make_unique<DX12RHI>();
        rhi_ptr = dx12_owner.get();
        vulkan_ready = dx12_owner->Initialize();
#endif
    }
    else
    {
        vulkan_owner = std::make_unique<VulkanRHI>();
        rhi_ptr = vulkan_owner.get();
        vulkan_ready = vulkan_owner->Initialize();
    }
    RHI& vulkan_rhi = *rhi_ptr;  // 名字沿用：下面 40 多处引用不动
    Check(vulkan_ready, use_dx12 ? "DX12RHI::Initialize succeeds (headless)" : "VulkanRHI::Initialize succeeds (headless)");
    if (!vulkan_ready)
    {
        std::printf("\n%d checks, %d failures\n", g_Checks, g_Failures);
        return 1;
    }

    auto manager = std::make_unique<VirtualTextureManager>();
    VirtualTextureConfig config;
    config.page_size = kPageSize;
    config.physical_pool_width = 2;
    config.physical_pool_height = 2;  // 4 槽位：故意小，逼出驱逐
    config.memory_budget_mb = 64;
    config.enable_async_loading = false;
    manager->SetConfig(config);
    manager->Initialize(&vulkan_rhi, nullptr);

    VirtualTextureBlitter blitter;
    const bool blitter_ready = blitter.Initialize(&vulkan_rhi);
    Check(blitter_ready, "vt_blit.vert/frag compiled and the graphics pipeline was created");
    if (!blitter_ready)
    {
        std::printf("       [hint] vt_blit_*.h missing? rebuild the ZShaderCompile target\n");
        std::printf("\n%d checks, %d failures\n", g_Checks, g_Failures);
        return 1;
    }

    // A: 100x90（非整页倍数，右/下边缘页残缺），2x2 页。B: 128x64，2x1 页。A+B = 6 页 > 4 槽位。
    auto source_a = MakeSource(100, 90, 0x21);
    auto source_b = MakeSource(128, 64, 0x84);
    const VirtualTextureHandle handle_a = manager->RegisterVirtualTexture(source_a, kPageSize);
    const VirtualTextureHandle handle_b = manager->RegisterVirtualTexture(source_b, kPageSize);
    auto vt_a = manager->GetVirtualTexture(handle_a);
    auto vt_b = manager->GetVirtualTexture(handle_b);

    std::vector<uint32_t> gpu;

    // ---------------------------------------------------------------------
    std::printf("--- stage 0: nothing uploaded yet ---\n");
    Check(!blitter.Render(*manager, handle_a, 100, 90, kFallback, gpu),
          "rendering before any page was uploaded fails cleanly (pool image still UNDEFINED)");

    // ---------------------------------------------------------------------
    std::printf("--- stage 1: A fully resident, 1:1 output ---\n");
    RunWithWatchdog("ForceLoadPages(A)", [&] { manager->ForceLoadPages(handle_a, 0); });
    {
        const bool ok = blitter.Render(*manager, handle_a, 100, 90, kFallback, gpu);
        Check(ok && gpu.size() == 100u * 90u, "A: 100x90 render returned 9000 pixels");
        const Tally t = Compare(gpu, 100, 90, vt_a, *source_a);
        Report("A 1:1", t, gpu, 100, vt_a, *source_a);
        Check(t.cleared == 0, "A: the triangle covers every pixel (no pixel left at the clear value)");
        Check(t.mismatch == 0 && t.match == 100u * 90u && t.boundary_pixels == 0 && t.boundary_tolerated == 0,
              "A: every rasterised pixel == the source texel, STRICTLY (1:1 has no boundary ambiguity, nothing is tolerated)");

        // 方向：输出第 0 行 == 虚拟纹理第 0 行。比较角点，翻 y 或翻 x 当场不等。
        Check(ok && gpu[0] == SourcePacked(*source_a, 0, 0), "A: top-left output pixel == source (0,0)");
        Check(ok && gpu[99] == SourcePacked(*source_a, 99, 0), "A: top-right output pixel == source (99,0)");
        Check(ok && gpu[89 * 100] == SourcePacked(*source_a, 0, 89), "A: bottom-left output pixel == source (0,89)");
        Check(ok && gpu[89 * 100 + 99] == SourcePacked(*source_a, 99, 89),
              "A: bottom-right output pixel == source (99,89) -- image is not flipped");
    }

    // ---------------------------------------------------------------------
    std::printf("--- stage 2: A resident, output SIZE != virtual size (magnify and minify) ---\n");
    {
        const uint32_t sizes[][2] = {{300, 270}, {37, 29}, {512, 64}};
        for (const auto& s : sizes)
        {
            char label[96];
            std::snprintf(label, sizeof(label), "A %ux%u", s[0], s[1]);
            const bool ok = blitter.Render(*manager, handle_a, s[0], s[1], kFallback, gpu);
            const Tally t = Compare(gpu, s[0], s[1], vt_a, *source_a);
            Report(label, t, gpu, s[0], vt_a, *source_a);
            char name[200];
            std::snprintf(name, sizeof(name),
                          "A: %ux%u output matches the CPU's UV->texel computation (only true texel-boundary samples may differ)",
                          s[0], s[1]);
            Check(ok && t.mismatch == 0 && t.cleared == 0, name);
            // 容忍度必须有界：被容忍的像素只可能是边界像素，且绝不是大头。
            std::snprintf(name, sizeof(name), "A: %ux%u tolerance is bounded (<= boundary pixels, < 10%% of the image)", s[0], s[1]);
            Check(t.boundary_tolerated <= t.boundary_pixels &&
                      static_cast<uint64_t>(t.boundary_tolerated) * 10u < static_cast<uint64_t>(s[0]) * s[1],
                  name);
        }
    }

    // ---------------------------------------------------------------------
    std::printf("--- stage 3: load B into the full pool (evicts 2 of A's pages) ---\n");
    RunWithWatchdog("ForceLoadPages(B)", [&] { manager->ForceLoadPages(handle_b, 0); });
    {
        const bool ok = blitter.Render(*manager, handle_a, 100, 90, kFallback, gpu);
        const Tally t = Compare(gpu, 100, 90, vt_a, *source_a);
        Report("A after eviction", t, gpu, 100, vt_a, *source_a);
        Check(ok && t.mismatch == 0 && t.cleared == 0,
              "A: after eviction every pixel still matches the CPU's expectation");

        // 数一下真的有 fallback 区域（场景有意义）以及没有"别人的像素"：Compare 已经保证了后者，
        // 因为 ExpectedPixel 对不驻留的页期望 fallback，别人的像素 != fallback。
        uint32_t fallback_pixels = 0;
        uint32_t source_pixels = 0;
        for (uint32_t p : gpu)
        {
            fallback_pixels += (p == kFallbackPacked) ? 1u : 0u;
        }
        for (uint32_t y = 0; y < 90; ++y)
        {
            for (uint32_t x = 0; x < 100; ++x)
            {
                source_pixels += (gpu[static_cast<size_t>(y) * 100 + x] == SourcePacked(*source_a, x, y)) ? 1u : 0u;
            }
        }
        std::printf("       [info] A: fallback pixels=%u source pixels=%u\n", fallback_pixels, source_pixels);
        Check(fallback_pixels > 0 && source_pixels > 0,
              "A: some regions are evicted (fallback) and some survive (source) -- the scenario is meaningful");
        Check(fallback_pixels + source_pixels == 100u * 90u,
              "A: every pixel is exactly one of {own source texel, fallback} -- never another page's pixels");

        Check(blitter.Render(*manager, handle_b, 128, 64, kFallback, gpu), "B: render");
        const Tally tb = Compare(gpu, 128, 64, vt_b, *source_b);
        Report("B 1:1", tb, gpu, 128, vt_b, *source_b);
        Check(tb.mismatch == 0 && tb.match == 128u * 64u && tb.boundary_tolerated == 0,
              "B: every pixel == its source texel, strictly");
    }

    // ---------------------------------------------------------------------
    std::printf("--- stage 4: unload A entirely ---\n");
    RunWithWatchdog("UnloadPages(A)", [&] { manager->UnloadPages(handle_a); });
    {
        const bool ok = blitter.Render(*manager, handle_a, 100, 90, kFallback, gpu);
        uint32_t fallback_pixels = 0;
        for (uint32_t p : gpu)
        {
            fallback_pixels += (p == kFallbackPacked) ? 1u : 0u;
        }
        Check(ok && fallback_pixels == 100u * 90u, "A: with nothing resident every pixel is the fallback colour");
    }

    vt_a.reset();
    vt_b.reset();
    blitter.Shutdown();
    RunWithWatchdog("VirtualTextureManager::Shutdown", [&] { manager->Shutdown(); });
    manager.reset();
#if defined(_WIN32)
    if (use_dx12)
    {
        // DX12 的"校验层一声不吭"判据：Vulkan 版靠捕获 std::cerr 里的 validation 输出；DX12 的
        // debug layer 把消息存进 ID3D12InfoQueue，直接数 CORRUPTION / ERROR / WARNING 条数。
        // 没装 Graphics Tools 时 InfoQueue 不存在——如实标注"未校验"，不假装通过。
        Microsoft::WRL::ComPtr<ID3D12InfoQueue> info_queue;
        if (dx12_owner->getDevice() != nullptr &&
            SUCCEEDED(dx12_owner->getDevice()->QueryInterface(IID_PPV_ARGS(&info_queue))) && info_queue)
        {
            uint32_t bad = 0;
            const UINT64 count = info_queue->GetNumStoredMessages();
            for (UINT64 i = 0; i < count; ++i)
            {
                SIZE_T size = 0;
                if (FAILED(info_queue->GetMessage(i, nullptr, &size)) || size == 0)
                {
                    continue;
                }
                std::vector<uint8_t> buffer(size);
                auto* message = reinterpret_cast<D3D12_MESSAGE*>(buffer.data());
                if (FAILED(info_queue->GetMessage(i, message, &size)))
                {
                    continue;
                }
                // 白名单（两条，都是 DX12RHI 对**所有**渲染目标 / 采样器的固有行为，与 VT 的正确性无关，
                // 且都只是 WARNING；除它们以外任何 WARNING / ERROR / CORRUPTION 都算失败）：
                //   #820  CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE —— CreateImage 不给 RT 带最优清屏值（性能提示）
                //   #1361 CREATE_SAMPLER_COMPARISON_FUNC_IGNORED —— CreateSampler 对非比较采样器写 ComparisonFunc=ALWAYS
                // 本轮已修掉的真问题（不在白名单里）：#649 CBV 越过资源末尾（参数缓冲只有 48 字节，CBV 按 256 对齐）。
                auto is_known_rhi_wide_warning = [](const D3D12_MESSAGE& m) {
                    return m.Severity == D3D12_MESSAGE_SEVERITY_WARNING &&
                           (static_cast<int>(m.ID) == 820 || static_cast<int>(m.ID) == 1361);
                };
                if (message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING && !is_known_rhi_wide_warning(*message))
                {
                    ++bad;
                    std::printf("       [d3d12 debug layer] severity=%d id=%d: %s\n",
                                static_cast<int>(message->Severity),
                                static_cast<int>(message->ID),
                                message->pDescription);
                }
            }
            Check(bad == 0, "D3D12 debug layer recorded no corruption / error / warning for the whole run");
        }
        else
        {
            std::printf("       [note] D3D12 debug layer unavailable (Graphics Tools not installed): API validation NOT checked\n");
        }
    }
#endif

    if (use_dx12)
    {
#if defined(_WIN32)
        dx12_owner->Shutdown();
#endif
    }
    else
    {
        vulkan_owner->Shutdown();
    }

    std::printf("\n%d checks, %d failures\n", g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
