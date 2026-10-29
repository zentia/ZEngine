// =============================================================================
// VirtualTexture sampling contract v2 A/B：mip 链 + 页 gutter + 导数选 mip + 双线性
// （Vulkan / DX12 headless，离屏，不碰主相机 pass；`--rhi dx12` 跑 DX12）。
//
// 前面的 blit 测试验的是 v1（mip 0、点采样）。v2 在同一套"页表 → 槽位 → 页池"之上加了三件事，
// 每一件都有一个"错了就必然失配"的判据：
//
//   gutter + 双线性   页池槽位是 (page+2)^2，页周围一圈 1 texel 是相邻 texel 的拷贝。放大 3 倍后
//                     很多采样点的 2x2 足迹跨页边——gutter 不对，这些像素就读到别的页的内容。
//   mip 链 + 导数选 mip  输出尺寸决定 uv 导数，导数决定 lod，lod 决定 mip：1:1 → mip0，2:1 → mip1，
//                     4:1 → mip2，8:1 以上 → 钳到最后一个 mip。每个 mip 的图像都不同（盒式滤波），
//                     选错 mip 必然失配。
//   缺页回退          把 mip0（或 mip0+1）卸掉，原本该采 mip0 的像素必须落到更粗的、驻留的 mip；
//                     全卸掉则是 fallback 颜色。
//
// 判据：GPU 光栅化读回 vs CPU 参考（Texture/VirtualTextureMath.h 的 VTRef::SampleWithFallback，对
// 源纹理自己建的 mip 链做 clamp 双线性）。容差 ±2/通道：硬件双线性权重是定点（约 8 位），CPU 是 fp32。
// 1:1 且像素中心恰好落在 texel 中心时，权重是 0/1，应当逐字节一致（容差 ≤1）。
//
// 同时检查：生产代码建的 mip 链 == 测试自己建的 mip 链（逐字节）；ExtractPagePixels 的 gutter 内容；
// 页上传全部成功（getPageUploadCount / FailureCount）；DX12 下 debug layer 零错误。
//
//   cmake -B build -DZENGINE_BUILD_VT_MIP_BILINEAR_AB_SMOKE_TEST=ON
//   cmake --build build --target VirtualTextureMipBilinearABSmokeTest
//   VirtualTextureMipBilinearABSmokeTest [--rhi dx12]
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
#include "Runtime/Function/Render/Texture/VirtualTextureMath.h"
#include "Runtime/Function/Render/Texture/VirtualTextureResource.h"
#include "Runtime/Function/Render/Texture/VirtualTextureTypes.h"
#include "Runtime/Function/Render/WindowSystem.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

    constexpr uint32_t kPageSize = 32;
    constexpr uint32_t kBorder = 1;
    constexpr uint32_t kWidth = 200;   // 故意不是页的整数倍，mip 尺寸 200,100,50,25 —— 到 25 是奇数
    constexpr uint32_t kHeight = 120;  // 120,60,30,15
    constexpr float kFallback[4] = {1.0f, 0.0f, 1.0f, 1.0f};
    constexpr uint32_t kFallbackPacked = 0xFFFF00FFu;  // R 在低字节
    constexpr float kTolerance = 2.0f;                 // 每通道，0..255 刻度

    // 每个 texel 都带哈希噪声：任何寻址错位、串页、mip 选错都必然失配。
    std::shared_ptr<TextureData> MakeSource(uint32_t width, uint32_t height)
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
        for (uint32_t y = 0; y < height; ++y)
        {
            for (uint32_t x = 0; x < width; ++x)
            {
                uint32_t h = x * 73856093u ^ y * 19349663u ^ 0x9E3779B9u;
                h ^= h >> 13;
                h *= 0x5bd1e995u;
                h ^= h >> 15;
                uint8_t* t = data->m_Pixels + (static_cast<size_t>(y) * width + x) * 4u;
                t[0] = static_cast<uint8_t>(h & 0xFF);
                t[1] = static_cast<uint8_t>((h >> 8) & 0xFF);
                t[2] = static_cast<uint8_t>((h >> 16) & 0xFF);
                t[3] = 255;
            }
        }
        return data;
    }

    struct Compare
    {
        uint32_t pixels {0};
        uint32_t exact_fallback {0};  // 期望是 fallback 且 GPU 也是 fallback
        uint32_t within {0};          // 在容差内
        uint32_t bad {0};
        float max_error {0.0f};
        uint32_t first_bad_x {0}, first_bad_y {0};
        uint32_t gpu_at_bad {0};
        float exp_r {0}, exp_g {0}, exp_b {0};
        uint32_t mip_histogram[16] {};
    };

    // CPU 参考：对 out_w x out_h 的每个像素中心，按 GPU 同样的规则（uv = (x+.5)/W，导数 = 1/W,1/H）算期望。
    Compare CompareToReference(const std::vector<uint32_t>& gpu,
                               uint32_t out_w,
                               uint32_t out_h,
                               const std::vector<VTRef::Image>& chain,
                               const std::shared_ptr<VirtualTextureResource>& resource,
                               float tolerance)
    {
        Compare c;
        c.pixels = out_w * out_h;
        if (gpu.size() != static_cast<size_t>(out_w) * out_h)
        {
            c.bad = 0xFFFFFFFFu;
            return c;
        }
        const auto resident = [&](uint32_t mip, uint32_t px, uint32_t py) {
            const PageTableEntry e = resource->GetPageTableEntry(mip, px, py);
            return e.is_valid && e.physical_slot != UINT32_MAX;
        };
        const float lod = VTRef::Lod(kWidth, kHeight, 1.0f / out_w, 0.0f, 0.0f, 1.0f / out_h);
        for (uint32_t y = 0; y < out_h; ++y)
        {
            for (uint32_t x = 0; x < out_w; ++x)
            {
                const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(out_w);
                const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(out_h);
                const VTRef::SampleResult ref = VTRef::SampleWithFallback(chain, kWidth, kHeight, kPageSize, resident, u, v, lod);
                const uint32_t g = gpu[static_cast<size_t>(y) * out_w + x];
                if (!ref.resident)
                {
                    if (g == kFallbackPacked)
                    {
                        ++c.exact_fallback;
                    }
                    else
                    {
                        if (c.bad == 0)
                        {
                            c.first_bad_x = x;
                            c.first_bad_y = y;
                            c.gpu_at_bad = g;
                        }
                        ++c.bad;
                    }
                    continue;
                }
                ++c.mip_histogram[std::min(ref.mip, 15u)];
                const float gr = static_cast<float>(g & 0xFF), gg = static_cast<float>((g >> 8) & 0xFF),
                            gb = static_cast<float>((g >> 16) & 0xFF), ga = static_cast<float>((g >> 24) & 0xFF);
                const float err = std::max({std::fabs(gr - ref.color.r), std::fabs(gg - ref.color.g),
                                            std::fabs(gb - ref.color.b), std::fabs(ga - ref.color.a)});
                c.max_error = std::max(c.max_error, err);
                if (err <= tolerance)
                {
                    ++c.within;
                }
                else
                {
                    if (c.bad == 0)
                    {
                        c.first_bad_x = x;
                        c.first_bad_y = y;
                        c.gpu_at_bad = g;
                        c.exp_r = ref.color.r;
                        c.exp_g = ref.color.g;
                        c.exp_b = ref.color.b;
                    }
                    ++c.bad;
                }
            }
        }
        return c;
    }

    void Report(const char* label, const Compare& c)
    {
        std::printf("       [info] %s: pixels=%u within=%u fallback-exact=%u BAD=%u max-error=%.2f  mips(0..3)=%u/%u/%u/%u\n",
                    label, c.pixels, c.within, c.exact_fallback, c.bad, c.max_error, c.mip_histogram[0],
                    c.mip_histogram[1], c.mip_histogram[2], c.mip_histogram[3]);
        if (c.bad != 0 && c.bad != 0xFFFFFFFFu)
        {
            std::printf("       [diag] first bad at (%u,%u): gpu=0x%08X expected rgb=(%.1f,%.1f,%.1f)\n", c.first_bad_x,
                        c.first_bad_y, c.gpu_at_bad, c.exp_r, c.exp_g, c.exp_b);
        }
    }

    bool RenderBilinear(VirtualTextureBlitter& blitter, VirtualTextureManager& manager, uint64_t handle,
                        uint32_t w, uint32_t h, std::vector<uint32_t>& out)
    {
        bool ok = false;
        RunWithWatchdog("Blitter::Render", [&] { ok = blitter.Render(manager, handle, w, h, kFallback, out, VirtualTextureBlitter::Filter::MipBilinear); });
        return ok;
    }
}  // namespace

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);

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
    std::printf("=== VirtualTexture sampling contract v2 A/B (mip chain + gutter + bilinear) (%s headless) ===\n",
                use_dx12 ? "DX12" : "Vulkan");

    // ------------------------------------------------------------------ stage 0: pure CPU
    std::printf("--- stage 0: CPU math (no GPU) ---\n");
    {
        Check(VTRef::SampledMipCount(kWidth, kHeight, kPageSize) == 4, "sampled mips of 200x120 @ page 32 == 4 (pages 7x4, 4x2, 2x1, 1x1)");
        Check(VTRef::MipRowOffset(kWidth, kHeight, kPageSize, 0) == 0 && VTRef::MipRowOffset(kWidth, kHeight, kPageSize, 1) == 4 &&
                  VTRef::MipRowOffset(kWidth, kHeight, kPageSize, 2) == 6 && VTRef::MipRowOffset(kWidth, kHeight, kPageSize, 3) == 7 &&
                  VTRef::AtlasHeight(kWidth, kHeight, kPageSize, 4) == 8,
              "page-table atlas row offsets are 0,4,6,7 and the atlas is 8 rows high");
        // 手算的盒式滤波：[[10,20],[30,40]] -> (100+2)/4 = 25
        VTRef::Image tiny;
        tiny.w = 2;
        tiny.h = 2;
        tiny.rgba = {10, 0, 0, 255, 20, 0, 0, 255, 30, 0, 0, 255, 40, 0, 0, 255};
        const VTRef::Image d = VTRef::Downsample(tiny);
        Check(d.w == 1 && d.h == 1 && d.rgba[0] == 25 && d.rgba[3] == 255, "2x2 box filter of {10,20,30,40} == 25 (round-to-nearest)");
        // 奇数边：3x1 -> 1x1，取前两列平均（最后一列被丢弃，规则在 VirtualTextureMath.h 里写明）
        VTRef::Image odd;
        odd.w = 3;
        odd.h = 1;
        odd.rgba = {100, 0, 0, 255, 200, 0, 0, 255, 50, 0, 0, 255};
        const VTRef::Image od = VTRef::Downsample(odd);
        Check(od.w == 1 && od.h == 1 && od.rgba[0] == 150, "3x1 -> 1x1 averages the first two columns (odd edge drops its last column)");
    }

    SystemRegistry::GetInstance().RegisterSystem(new WindowSystem());
    std::unique_ptr<VulkanRHI> vulkan_owner;
#if defined(_WIN32)
    std::unique_ptr<DX12RHI> dx12_owner;
#endif
    RHI* rhi_ptr = nullptr;
    bool rhi_ready = false;
    if (use_dx12)
    {
#if defined(_WIN32)
        dx12_owner = std::make_unique<DX12RHI>();
        rhi_ptr = dx12_owner.get();
        rhi_ready = dx12_owner->Initialize();
#endif
    }
    else
    {
        vulkan_owner = std::make_unique<VulkanRHI>();
        rhi_ptr = vulkan_owner.get();
        rhi_ready = vulkan_owner->Initialize();
    }
    Check(rhi_ready, use_dx12 ? "DX12RHI::Initialize succeeds (headless)" : "VulkanRHI::Initialize succeeds (headless)");
    if (!rhi_ready)
    {
        std::printf("\n%d checks, %d failures\n", g_Checks, g_Failures);
        return 1;
    }
    RHI& rhi = *rhi_ptr;

    auto manager = std::make_unique<VirtualTextureManager>();
    VirtualTextureConfig config;
    config.page_size = kPageSize;
    config.physical_pool_width = 8;
    config.physical_pool_height = 8;  // 64 槽位 >= 39 页：这个测试不逼驱逐，驱逐已有别的测试覆盖
    config.memory_budget_mb = 64;
    config.enable_async_loading = false;
    config.page_border = kBorder;
    config.resident_mip_chain = true;
    manager->SetConfig(config);
    manager->Initialize(&rhi, nullptr);

    VirtualTextureBlitter blitter;
    const bool blitter_ready = blitter.Initialize(&rhi);
    Check(blitter_ready, "vt_blit shaders compiled and the graphics pipeline was created");
    if (!blitter_ready)
    {
        std::printf("\n%d checks, %d failures\n", g_Checks, g_Failures);
        return 1;
    }

    auto source = MakeSource(kWidth, kHeight);
    const VirtualTextureHandle handle = manager->RegisterVirtualTexture(source, kPageSize);
    auto resource = manager->GetVirtualTexture(handle);
    Check(handle != 0 && resource != nullptr, "RegisterVirtualTexture(200x120) with border 1 + mip chain");
    if (!resource)
    {
        return 1;
    }
    const std::vector<VTRef::Image> chain = VTRef::BuildMipChain(source->m_Pixels, kWidth, kHeight, 4);

    // ------------------------------------------------------------------ stage 1: resource-level contract
    std::printf("--- stage 1: resource contract (mip chain, gutter, atlas geometry) ---\n");
    {
        Check(resource->getSampledMipCount() == 4 && resource->getPageBorder() == 1 && resource->getPageTableHeight() == 8 &&
                  resource->getPageTableWidth() == 7,
              "resource: 4 sampled mips, border 1, page-table atlas 7 x 8");
        bool chain_equal = true;
        for (uint32_t m = 0; m < 4 && chain_equal; ++m)
        {
            uint32_t mw = 0, mh = 0;
            const uint8_t* pixels = resource->getMipPixels(m, mw, mh);
            chain_equal = pixels != nullptr && mw == chain[m].w && mh == chain[m].h &&
                          std::memcmp(pixels, chain[m].rgba.data(), chain[m].rgba.size()) == 0;
        }
        Check(chain_equal, "the production mip chain == the test's independently built chain, byte for byte, for all 4 mips");

        // 页 (1,1) @ mip0：内部 texel (0,0) 的左上邻居在 gutter 里，必须是源图 (31,31)
        std::vector<uint8_t> block;
        uint32_t real_w = 0, real_h = 0;
        const bool ok = resource->ExtractPagePixels(0, 1, 1, block, real_w, real_h);
        bool gutter_ok = ok && block.size() == 34u * 34u * 4u && real_w == 32 && real_h == 32;
        auto src_at = [&](uint32_t x, uint32_t y, uint32_t c) { return source->m_Pixels[(static_cast<size_t>(y) * kWidth + x) * 4u + c]; };
        auto blk_at = [&](uint32_t x, uint32_t y, uint32_t c) { return block[(static_cast<size_t>(y) * 34u + x) * 4u + c]; };
        for (uint32_t c = 0; c < 4 && gutter_ok; ++c)
        {
            gutter_ok = blk_at(0, 0, c) == src_at(31, 31, c)      // 左上角 gutter
                        && blk_at(33, 0, c) == src_at(64, 31, c)  // 右上角
                        && blk_at(0, 33, c) == src_at(31, 64, c)  // 左下角
                        && blk_at(1, 1, c) == src_at(32, 32, c)   // 页内第一个 texel
                        && blk_at(17, 0, c) == src_at(48, 31, c)  // 上边 gutter 中段
                        && blk_at(0, 17, c) == src_at(31, 48, c); // 左边 gutter 中段
        }
        Check(gutter_ok, "ExtractPagePixels(page 1,1): the 1-texel gutter holds the NEIGHBOURING source texels (corners and edges)");

        // 右下角残缺页 (6,3)：真实 8x24，块的其余部分是 clamp-to-edge 复制（200 = 6*32+8, 120 = 3*32+24）
        const bool ok2 = resource->ExtractPagePixels(0, 6, 3, block, real_w, real_h);
        bool tail_ok = ok2 && real_w == 8 && real_h == 24;
        for (uint32_t c = 0; c < 4 && tail_ok; ++c)
        {
            tail_ok = blk_at(1 + 8 + 5, 1 + 3, c) == src_at(199, 96 + 3, c)    // 页内越过图像右边：复制最后一列
                      && blk_at(1 + 2, 1 + 24 + 4, c) == src_at(192 + 2, 119, c)  // 越过图像下边：复制最后一行
                      && blk_at(33, 33, c) == src_at(199, 119, c);                // 右下角
        }
        Check(tail_ok, "ExtractPagePixels(partial page 6,3): texels past the mip image edge replicate the edge texel (clamp)");
    }

    // ------------------------------------------------------------------ stage 2: load every mip
    std::printf("--- stage 2: load all 4 mips (39 pages) ---\n");
    for (uint32_t mip = 0; mip < 4; ++mip)
    {
        RunWithWatchdog("ForceLoadPages", [&] { manager->ForceLoadPages(handle, mip); });
    }
    Check(manager->getPageUploadCount() == 39 && manager->getPageUploadFailureCount() == 0,
          "39 page uploads (28 + 8 + 2 + 1), none failed");
    {
        auto pool = manager->getPhysicalTexturePool();
        Check(pool && pool->getPageBorder() == 1 && pool->getSlotSize() == 34, "physical pool slot is 34x34 (32 + 2*1)");
    }

    std::vector<uint32_t> gpu;
    {
        // v1 点采样走 border=1 的槽位：证明 v1 的 "slot*slot_size + border + in_page" 对
        bool ok = false;
        RunWithWatchdog("Render point", [&] { ok = blitter.Render(*manager, handle, kWidth, kHeight, kFallback, gpu, VirtualTextureBlitter::Filter::Point); });
        bool all_equal = ok && gpu.size() == static_cast<size_t>(kWidth) * kHeight;
        uint32_t wrong = 0;
        for (uint32_t y = 0; y < kHeight && all_equal; ++y)
        {
            for (uint32_t x = 0; x < kWidth; ++x)
            {
                const uint32_t want = source->m_Pixels[(y * kWidth + x) * 4 + 0] | (source->m_Pixels[(y * kWidth + x) * 4 + 1] << 8) |
                                      (source->m_Pixels[(y * kWidth + x) * 4 + 2] << 16) | (255u << 24);
                if (gpu[static_cast<size_t>(y) * kWidth + x] != want)
                {
                    ++wrong;
                }
            }
        }
        std::printf("       [info] v1 point 1:1 through a bordered pool: wrong=%u\n", wrong);
        Check(ok && wrong == 0, "v1 point sampling through a border-1 pool is still byte-exact at 1:1 (slot offset includes the gutter)");
    }

    // ------------------------------------------------------------------ stage 3: bilinear at several scales
    std::printf("--- stage 3: mip selection + bilinear vs CPU reference ---\n");
    struct Case
    {
        uint32_t w, h;
        const char* name;
        float tol;
        uint32_t expect_mip;  // 期望几乎全部像素落在哪个 mip（0xFFFFFFFF = 不检查）
    };
    const Case cases[] = {
        {200, 120, "1:1            (lod 0.00 -> mip 0, texel centres -> exact)", 1.0f, 0},
        {600, 360, "3x magnify     (lod<0 -> mip 0, page-edge footprints cross the gutter)", kTolerance, 0},
        {150, 90, "1.33x minify   (lod 0.41 -> mip 0, fractional bilinear)", kTolerance, 0},
        {130, 78, "1.54x minify   (lod 0.62 -> mip 1)", kTolerance, 1},
        {100, 60, "2:1            (lod 1.00 -> mip 1, texel centres of mip 1 -> exact)", 1.0f, 1},
        {80, 48, "2.5x minify    (lod 1.32 -> mip 1)", kTolerance, 1},
        {60, 36, "3.33x minify   (lod 1.74 -> mip 2)", kTolerance, 2},
        {50, 30, "4:1            (lod 2.00 -> mip 2, texel centres of mip 2 -> exact)", 1.0f, 2},
        {30, 18, "6.67x minify   (lod 2.74 -> mip 3)", kTolerance, 3},
        {17, 11, "11.8x minify   (lod 3.56 -> clamped to the last mip 3)", kTolerance, 3},
    };
    for (const Case& c : cases)
    {
        const bool ok = RenderBilinear(blitter, *manager, handle, c.w, c.h, gpu);
        const Compare cmp = CompareToReference(gpu, c.w, c.h, chain, resource, c.tol);
        Report(c.name, cmp);
        char label[200];
        std::snprintf(label, sizeof(label), "%ux%u: every pixel within +-%.0f of the CPU reference", c.w, c.h, c.tol);
        Check(ok && cmp.bad == 0 && cmp.pixels == cmp.within, label);
        if (c.expect_mip != 0xFFFFFFFFu)
        {
            // 边界像素可能因 lod 舍入落到相邻 mip，所以只要求"绝大多数"在期望的 mip：这条检查证明
            // 参考实现本身选中了设计意图中的 mip（否则上面的"一致"可能是两边一起错）。
            uint32_t in_expected = cmp.mip_histogram[c.expect_mip];
            std::snprintf(label, sizeof(label), "%ux%u: the reference itself selects mip %u for all pixels", c.w, c.h, c.expect_mip);
            Check(in_expected == c.w * c.h, label);
        }
    }

    // ------------------------------------------------------------------ stage 4: residency fallback
    std::printf("--- stage 4: missing pages fall back to coarser resident mips ---\n");
    RunWithWatchdog("UnloadPages", [&] { manager->UnloadPages(handle); });
    for (uint32_t mip = 1; mip < 4; ++mip)
    {
        RunWithWatchdog("ForceLoadPages", [&] { manager->ForceLoadPages(handle, mip); });  // mip 0 故意不载
    }
    {
        const bool ok = RenderBilinear(blitter, *manager, handle, 200, 120, gpu);
        const Compare cmp = CompareToReference(gpu, 200, 120, chain, resource, kTolerance);
        Report("mip 0 evicted, 1:1 (lod 0)", cmp);
        Check(ok && cmp.bad == 0 && cmp.mip_histogram[0] == 0 && cmp.mip_histogram[1] == 200 * 120,
              "with mip 0 missing, every pixel that wanted mip 0 is served by resident mip 1 (matches the reference)");
    }
    RunWithWatchdog("UnloadPages", [&] { manager->UnloadPages(handle); });
    for (uint32_t mip = 2; mip < 4; ++mip)
    {
        RunWithWatchdog("ForceLoadPages", [&] { manager->ForceLoadPages(handle, mip); });  // mip 0 和 1 都不载
    }
    {
        const bool ok = RenderBilinear(blitter, *manager, handle, 200, 120, gpu);
        const Compare cmp = CompareToReference(gpu, 200, 120, chain, resource, kTolerance);
        Report("mips 0+1 evicted, 1:1", cmp);
        Check(ok && cmp.bad == 0 && cmp.mip_histogram[0] == 0 && cmp.mip_histogram[1] == 0 && cmp.mip_histogram[2] == 200 * 120,
              "with mips 0 and 1 missing, every pixel falls through to resident mip 2");
    }
    RunWithWatchdog("UnloadPages", [&] { manager->UnloadPages(handle); });
    for (uint32_t mip = 0; mip < 3; ++mip)
    {
        RunWithWatchdog("ForceLoadPages", [&] { manager->ForceLoadPages(handle, mip); });  // 只缺最粗的 mip 3
    }
    {
        // 缺最粗的 mip 不影响需要更细 mip 的像素；但要采 mip 3 的像素（lod>=2.5）找不到更粗的 -> fallback 颜色
        const bool ok = RenderBilinear(blitter, *manager, handle, 30, 18, gpu);
        const Compare cmp = CompareToReference(gpu, 30, 18, chain, resource, kTolerance);
        Report("coarsest mip evicted, 30x18 (wants mip 3)", cmp);
        Check(ok && cmp.bad == 0 && cmp.exact_fallback == 30 * 18, "wanting mip 3 with nothing coarser resident gives exactly the fallback colour");
    }
    RunWithWatchdog("UnloadPages", [&] { manager->UnloadPages(handle); });
    {
        const bool ok = RenderBilinear(blitter, *manager, handle, 200, 120, gpu);
        Check(ok == false || std::all_of(gpu.begin(), gpu.end(), [](uint32_t p) { return p == kFallbackPacked; }),
              "nothing resident: the render either refuses or returns only the fallback colour");
    }

    // ------------------------------------------------------------------ stage 5: reload restores
    std::printf("--- stage 5: reload restores the exact image ---\n");
    for (uint32_t mip = 0; mip < 4; ++mip)
    {
        RunWithWatchdog("ForceLoadPages", [&] { manager->ForceLoadPages(handle, mip); });
    }
    {
        const bool ok = RenderBilinear(blitter, *manager, handle, 600, 360, gpu);
        const Compare cmp = CompareToReference(gpu, 600, 360, chain, resource, kTolerance);
        Report("reloaded, 3x magnify", cmp);
        Check(ok && cmp.bad == 0, "after unload + reload the 3x magnified image matches the reference again");
    }

    // ------------------------------------------------------------------ stage 6: guard rails
    std::printf("--- stage 6: guard rails ---\n");
    {
        auto plain = std::make_unique<VirtualTextureManager>();
        VirtualTextureConfig plain_config;
        plain_config.page_size = kPageSize;
        plain_config.physical_pool_width = 4;
        plain_config.physical_pool_height = 4;
        plain_config.memory_budget_mb = 64;
        plain_config.enable_async_loading = false;  // border 0, no chain: the original layout
        plain->SetConfig(plain_config);
        plain->Initialize(&rhi, nullptr);
        auto tiny_source = MakeSource(64, 64);
        const VirtualTextureHandle tiny_handle = plain->RegisterVirtualTexture(tiny_source, kPageSize);
        plain->ForceLoadPages(tiny_handle, 0);
        std::vector<uint32_t> out;
        const bool refused = !blitter.Render(*plain, tiny_handle, 64, 64, kFallback, out, VirtualTextureBlitter::Filter::MipBilinear);
        Check(refused, "MipBilinear on a border-0 pool is refused (a 2x2 footprint would read the neighbouring slot)");
        const bool point_ok = blitter.Render(*plain, tiny_handle, 64, 64, kFallback, out, VirtualTextureBlitter::Filter::Point);
        Check(point_ok && out.size() == 64u * 64u, "Point on the same border-0 pool still works (the v1 layout is untouched)");
        plain->Shutdown();
    }

#if defined(_WIN32)
    if (use_dx12)
    {
        dx12_owner->DrainDebugLayerMessagesForTest();
        uint32_t bad = 0;
        for (const DebugLayerRecord& record : dx12_owner->CopyDebugLayerHistory())
        {
            const bool known = record.severity == D3D12_MESSAGE_SEVERITY_WARNING && (record.id == 820 || record.id == 1361);
            if (record.severity <= D3D12_MESSAGE_SEVERITY_WARNING && !known)
            {
                ++bad;
                std::printf("       [d3d12 debug layer] severity=%d id=%d: %s\n", record.severity, record.id, record.text.c_str());
            }
        }
        Check(bad == 0, "D3D12 debug layer recorded no corruption / error / warning for the whole run");
    }
#endif

    blitter.Shutdown();
    manager->Shutdown();
    resource.reset();
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
