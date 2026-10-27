// =============================================================================
// VirtualTexture 页表纹理**生命周期** A/B（Vulkan headless）。
//
// VirtualTextureGpuUploadABSmokeTest 只验了"加载"这一个方向：页传进池子、页表写对。
// 页表纹理的另外两半生命周期在那之前根本没实现：
//
//   (1) 失效同步。UnloadPage 清掉 CPU 页表项，却从不重传 GPU 页表 —— GPU 继续认为
//       "该页驻留在槽位 N"，而槽位 N 早已分给了别的页 ⇒ shader 采到别人的像素，
//       没有任何报错。池满时的 LRU 驱逐更糟：AllocateSlot 直接把槽位偷给新页，
//       连 CPU 侧都不通知原主人（两页别名同一槽位）。
//   (2) 释放。VirtualTextureResource::Shutdown 只把指针置空，注释说"由 manager 销毁"，
//       manager 从没销毁过 ⇒ 每个注册过的 VT 泄漏一张页表纹理；页池的 view 同样泄漏。
//
// 顺带三处自死锁（原生 std::mutex 非递归；WASM 无 pthreads 时 mutex 是空操作，所以
// WASM 冒烟测试一直是绿的）：Unregister 持锁调 UnloadPages→GetVirtualTexture；池满时
// AllocateSlot 持锁调 FindLRUSlot。这些调用全部包在看门狗里 —— 回归会表现为 FAIL，
// 而不是一个挂死的 CI 任务。
//
// A/B 判据（模拟 shader 的页表间接寻址）：
//   A 路 = 读回 GPU 页表 → 取该页槽位 → 从读回的 GPU 页池取像素；
//   B 路 = 直接取源纹理对应区域。
//   GPU 页表声称驻留的每一页，A 必须与 B 逐字节相同；GPU 页表与 CPU PageTableEntry
//   必须逐项一致；没有两页（跨 VT）共享一个槽位。
//
// 另外两条量化断言：
//   * 批量：一批操作（ForceLoadPages / UnloadPages）每个脏页表只上传一次，干净页表
//     零次 —— 由 getPageTableUploadCount() 精确计数；
//   * 无泄漏：VMA 分配数在每个阶段严格等于"池 1 + 每个已建页表 1"，全部注销 +
//     Shutdown 后回到基线。
//
//   cmake -B build -DZENGINE_BUILD_VT_PAGETABLE_LIFECYCLE_AB_SMOKE_TEST=ON
//   cmake --build build --target VirtualTexturePageTableLifecycleABSmokeTest
// =============================================================================

#include "CommonPCH/pch.h"

#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Function/Render/Interface/RHI.h"
#include "Runtime/Function/Render/Interface/Vulkan/VulkanRHI.h"
#include "Runtime/Function/Render/RenderType.h"
#include "Runtime/Function/Render/Texture/VirtualTextureManager.h"
#include "Runtime/Function/Render/Texture/VirtualTextureTypes.h"
#include "Runtime/Function/Render/WindowSystem.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <memory>
#include <set>
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

    constexpr uint32_t kPageSize = 64;
    constexpr uint32_t kPoolPagesPerSide = 2;  // 2x2 = 4 个槽位：故意小，逼出池满驱逐
    constexpr uint32_t kPoolPixels = kPoolPagesPerSide * kPageSize;
    constexpr uint32_t kBytesPerPixel = 4;      // 页池 R8G8B8A8
    constexpr uint32_t kTableBytesPerTexel = 8;  // 页表 R32G32_SFLOAT

    // 会死锁的调用跑在看门狗里：超时直接判 FAIL 并 _Exit（future 析构会阻塞等那个
    // 永远不返回的线程，所以不能正常返回）。
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

    uint32_t VmaAllocationCount(VulkanRHI& rhi)
    {
        VmaTotalStatistics stats {};
        vmaCalculateStatistics(rhi.getVmaAllocator(), &stats);
        return stats.total.statistics.allocationCount;
    }

    // 读回 2D COLOR 图。读完把布局**还原成 SHADER_READ_ONLY**：之后的增量上传
    // （discard_prior_content=false）按 SHADER_READ_ONLY→TRANSFER_DST 走，不还原会
    // 踩 layout 不符的校验错。
    std::vector<uint8_t> ReadBackImage(RHI* rhi, RHIImage* image, uint32_t width, uint32_t height, uint32_t bpp)
    {
        std::vector<uint8_t> pixels;
        if (rhi == nullptr || image == nullptr || width == 0 || height == 0)
        {
            return pixels;
        }
        const RHIDeviceSize bytes = static_cast<RHIDeviceSize>(width) * height * bpp;

        RHIBuffer* buffer = nullptr;
        RHIDeviceMemory* memory = nullptr;
        rhi->CreateBuffer(bytes,
                          RHI_BUFFER_USAGE_TRANSFER_DST_BIT,
                          RHI_MEMORY_PROPERTY_HOST_VISIBLE_BIT | RHI_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          buffer,
                          memory);
        if (buffer == nullptr || memory == nullptr)
        {
            return pixels;
        }

        RHICommandBuffer* command_buffer = rhi->BeginSingleTimeCommands();
        if (command_buffer != nullptr)
        {
            RHIImageMemoryBarrier barrier {};
            barrier.sType = RHI_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.srcAccessMask = RHI_ACCESS_SHADER_READ_BIT;
            barrier.dstAccessMask = RHI_ACCESS_TRANSFER_READ_BIT;
            barrier.oldLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barrier.newLayout = RHI_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barrier.srcQueueFamilyIndex = ~0u;
            barrier.dstQueueFamilyIndex = ~0u;
            barrier.image = image;
            barrier.subresourceRange.aspectMask = RHI_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.levelCount = 1;
            barrier.subresourceRange.layerCount = 1;
            rhi->CmdPipelineBarrier(command_buffer,
                                    RHI_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                    RHI_PIPELINE_STAGE_TRANSFER_BIT,
                                    0, 0, nullptr, 0, nullptr, 1, &barrier);

            RHIBufferImageCopy region {};
            region.bufferRowLength = 0;  // 紧凑打包；Vulkan 单位是 texel
            region.bufferImageHeight = 0;
            region.imageSubresource.aspectMask = RHI_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.layerCount = 1;
            region.imageExtent.width = width;
            region.imageExtent.height = height;
            region.imageExtent.depth = 1;
            rhi->CmdCopyImageToBuffer(command_buffer, image, RHI_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);

            RHIImageMemoryBarrier restore = barrier;
            restore.srcAccessMask = RHI_ACCESS_TRANSFER_READ_BIT;
            restore.dstAccessMask = RHI_ACCESS_SHADER_READ_BIT;
            restore.oldLayout = RHI_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            restore.newLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            rhi->CmdPipelineBarrier(command_buffer,
                                    RHI_PIPELINE_STAGE_TRANSFER_BIT,
                                    RHI_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                    0, 0, nullptr, 0, nullptr, 1, &restore);
            rhi->EndSingleTimeCommands(command_buffer);
        }

        void* mapped = nullptr;
        if (rhi->MapMemory(memory, 0, bytes, 0, &mapped) && mapped != nullptr)
        {
            const uint8_t* src = static_cast<const uint8_t*>(mapped);
            pixels.assign(src, src + bytes);
            rhi->UnmapMemory(memory);
        }
        rhi->FreeMemory(memory);
        rhi->DestroyBuffer(buffer);
        return pixels;
    }

    // 两张源纹理用不同的种子，逐 texel 都不一样 —— 槽位串到另一张 VT 的页上一定失配。
    std::shared_ptr<TextureData> MakeSource(uint32_t width, uint32_t height, uint8_t seed)
    {
        auto data = std::make_shared<TextureData>();
        data->m_Width = width;
        data->m_Height = height;
        data->m_Depth = 1;
        data->m_MipLevels = 1;
        data->m_ArrayLayers = 1;
        data->m_Format = RHI_FORMAT_R8G8B8A8_UNORM;
        data->m_PixelsLength = width * height * kBytesPerPixel;
        data->m_Pixels = static_cast<uint8_t*>(std::malloc(data->m_PixelsLength));  // TextureData 用 free()
        if (data->m_Pixels == nullptr)
        {
            return nullptr;
        }
        const uint32_t pages_x = width / kPageSize;
        for (uint32_t y = 0; y < height; ++y)
        {
            for (uint32_t x = 0; x < width; ++x)
            {
                const uint32_t page_index = (y / kPageSize) * pages_x + (x / kPageSize);
                uint8_t* t = data->m_Pixels + (static_cast<size_t>(y) * width + x) * kBytesPerPixel;
                t[0] = static_cast<uint8_t>((x + seed) & 0xFF);
                t[1] = static_cast<uint8_t>((y * 3u + seed) & 0xFF);
                t[2] = static_cast<uint8_t>(seed | page_index);
                t[3] = 255;
            }
        }
        return data;
    }

    struct Residency
    {
        uint32_t gpu_resident = 0;      // GPU 页表说驻留的页数
        uint32_t gpu_cpu_mismatch = 0;  // GPU 页表项 != CPU PageTableEntry
        uint32_t ab_mismatch = 0;       // A（页表→池）!= B（源）的页数
        bool table_read_ok = false;
        std::vector<int32_t> slots;     // 每页 GPU 槽位（-1 = 不驻留），行主序
    };

    // 核心 A/B：对一张 VT 的每一页做一次"shader 间接寻址"。
    Residency VerifyResidency(VulkanRHI& rhi,
                              const std::shared_ptr<VirtualTextureResource>& resource,
                              const std::shared_ptr<TextureData>& source,
                              const std::vector<uint8_t>& pool_pixels,
                              PhysicalTexturePool& pool)
    {
        Residency r;
        if (!resource)
        {
            return r;
        }
        const uint32_t pages_x = resource->getPagesX();
        const uint32_t pages_y = resource->getPagesY();
        std::vector<uint8_t> table =
            ReadBackImage(&rhi, resource->GetPageTableTexture(), pages_x, pages_y, kTableBytesPerTexel);
        r.table_read_ok = table.size() == static_cast<size_t>(pages_x) * pages_y * kTableBytesPerTexel;
        if (!r.table_read_ok)
        {
            return r;
        }
        const float* texels = reinterpret_cast<const float*>(table.data());

        for (uint32_t py = 0; py < pages_y; ++py)
        {
            for (uint32_t px = 0; px < pages_x; ++px)
            {
                const size_t i = (static_cast<size_t>(py) * pages_x + px) * 2;
                const float slot_f = texels[i + 0];
                const float mip_f = texels[i + 1];
                const bool gpu_resident = slot_f >= 0.0f;
                const PageTableEntry cpu = resource->GetPageTableEntry(0, px, py);
                const bool cpu_resident = cpu.is_valid && cpu.physical_slot != UINT32_MAX;

                const bool agree = gpu_resident == cpu_resident && mip_f == 0.0f &&
                                   (gpu_resident ? static_cast<uint32_t>(slot_f) == cpu.physical_slot
                                                 : slot_f == VirtualTextureResource::kPageTableNotResident);
                if (!agree)
                {
                    ++r.gpu_cpu_mismatch;
                    std::printf("       [diag] page (%u,%u): GPU (%.1f,%.1f) vs CPU valid=%d slot=%u\n",
                                px, py, static_cast<double>(slot_f), static_cast<double>(mip_f),
                                cpu.is_valid ? 1 : 0, cpu.physical_slot);
                }

                r.slots.push_back(gpu_resident ? static_cast<int32_t>(slot_f) : -1);
                if (!gpu_resident)
                {
                    continue;
                }
                ++r.gpu_resident;

                // A 路：GPU 页表给的槽位 → GPU 池里那一块。B 路：源纹理那一块。
                uint32_t sx = 0;
                uint32_t sy = 0;
                pool.GetSlotCoordinates(static_cast<uint32_t>(slot_f), sx, sy);
                bool page_ok = !pool_pixels.empty();
                for (uint32_t row = 0; row < kPageSize && page_ok; ++row)
                {
                    const uint8_t* a = pool_pixels.data() +
                                       (static_cast<size_t>(sy * kPageSize + row) * kPoolPixels + sx * kPageSize) *
                                           kBytesPerPixel;
                    const uint8_t* b = source->m_Pixels +
                                       (static_cast<size_t>(py * kPageSize + row) * source->m_Width + px * kPageSize) *
                                           kBytesPerPixel;
                    page_ok = std::memcmp(a, b, kPageSize * kBytesPerPixel) == 0;
                }
                if (!page_ok)
                {
                    ++r.ab_mismatch;
                    std::printf("       [diag] page (%u,%u) -> slot %u: pool pixels != source (stale/aliased)\n",
                                px, py, static_cast<uint32_t>(slot_f));
                }
            }
        }
        return r;
    }

    std::vector<uint8_t> ReadPool(VulkanRHI& rhi, PhysicalTexturePool& pool)
    {
        return ReadBackImage(&rhi, pool.getPhysicalTexture(), kPoolPixels, kPoolPixels, kBytesPerPixel);
    }
}  // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // 崩溃/超时时至少保住已打印的行
    std::printf("=== VirtualTexture page-table lifecycle A/B smoke test (Vulkan headless) ===\n");

    SystemRegistry::GetInstance().RegisterSystem(new WindowSystem());
    VulkanRHI vulkan_rhi;
    const bool vulkan_ready = vulkan_rhi.Initialize();
    Check(vulkan_ready, "VulkanRHI::Initialize succeeds (headless)");
    if (!vulkan_ready)
    {
        std::printf("\n%d checks, %d failures\n", g_Checks, g_Failures);
        return 1;
    }

    const uint32_t vma_baseline = VmaAllocationCount(vulkan_rhi);

    auto manager = std::make_unique<VirtualTextureManager>();
    VirtualTextureConfig config;
    config.page_size = kPageSize;
    config.physical_pool_width = kPoolPagesPerSide;
    config.physical_pool_height = kPoolPagesPerSide;
    config.memory_budget_mb = 64;
    config.enable_async_loading = false;
    manager->SetConfig(config);
    manager->Initialize(&vulkan_rhi, nullptr);
    auto pool = manager->getPhysicalTexturePool();
    Check(pool && pool->getPhysicalTexture() && pool->getTotalSlots() == 4, "a 4-slot GPU page pool exists");
    Check(VmaAllocationCount(vulkan_rhi) == vma_baseline + 1, "VMA: pool = +1 allocation");

    // A: 128x128 = 2x2 页；B: 128x64 = 2x1 页。A+B = 6 页 > 4 槽位。
    auto source_a = MakeSource(128, 128, 0x10);
    auto source_b = MakeSource(128, 64, 0x80);
    const VirtualTextureHandle handle_a = manager->RegisterVirtualTexture(source_a, kPageSize);
    const VirtualTextureHandle handle_b = manager->RegisterVirtualTexture(source_b, kPageSize);
    auto vt_a = manager->GetVirtualTexture(handle_a);
    auto vt_b = manager->GetVirtualTexture(handle_b);
    Check(vt_a && vt_a->getPagesX() == 2 && vt_a->getPagesY() == 2, "VT-A registered as 2x2 pages");
    Check(vt_b && vt_b->getPagesX() == 2 && vt_b->getPagesY() == 1, "VT-B registered as 2x1 pages");
    Check(VmaAllocationCount(vulkan_rhi) == vma_baseline + 1, "registration creates no GPU resources yet");

    // ---------------------------------------------------------------------
    std::printf("--- stage 1: load all of A ---\n");
    RunWithWatchdog("ForceLoadPages(A)", [&] { manager->ForceLoadPages(handle_a, 0); });
    {
        auto pool_pixels = ReadPool(vulkan_rhi, *pool);
        Residency ra = VerifyResidency(vulkan_rhi, vt_a, source_a, pool_pixels, *pool);
        Check(ra.table_read_ok && ra.gpu_resident == 4, "A: GPU page table marks all 4 pages resident");
        Check(ra.gpu_cpu_mismatch == 0, "A: GPU page table == CPU PageTableEntry");
        Check(ra.ab_mismatch == 0, "A: indirect lookup (table->pool) == source for every page");
        Check(vt_a->getPageTableUploadCount() == 1, "batching: 4 page loads -> exactly 1 page-table upload");
        Check(!vt_a->isPageTableDirty(), "A: table is clean after the batch flush");
    }
    manager->FlushPageTables();
    Check(vt_a->getPageTableUploadCount() == 1, "flushing a clean table uploads nothing");
    Check(VmaAllocationCount(vulkan_rhi) == vma_baseline + 2, "VMA: pool + A's page table = +2");

    // ---------------------------------------------------------------------
    std::printf("--- stage 2: unload A (invalidation sync) ---\n");
    RunWithWatchdog("UnloadPages(A)", [&] { manager->UnloadPages(handle_a); });
    {
        auto pool_pixels = ReadPool(vulkan_rhi, *pool);
        Residency ra = VerifyResidency(vulkan_rhi, vt_a, source_a, pool_pixels, *pool);
        // 修复前：CPU 项已清，GPU 页表仍说 4 页驻留 —— 指向已释放的槽位。
        Check(ra.table_read_ok && ra.gpu_resident == 0, "A: after unload the GPU page table marks NO page resident");
        Check(ra.gpu_cpu_mismatch == 0, "A: GPU page table == CPU PageTableEntry after unload");
        Check(vt_a->getPageTableUploadCount() == 2, "unload batch -> exactly 1 more upload");
        Check(manager->GetStats().physical_pool_used_slots == 0, "all 4 slots returned to the pool");
    }

    // ---------------------------------------------------------------------
    std::printf("--- stage 3: reload A, then load B into a full pool (cross-VT eviction) ---\n");
    RunWithWatchdog("ForceLoadPages(A) again", [&] { manager->ForceLoadPages(handle_a, 0); });
    Check(vt_a->getPageTableUploadCount() == 3, "reload A -> 1 upload");
    // 池已满（A 占满 4 格）。修复前：AllocateSlot 持 m_SlotMutex 调 FindLRUSlot ⇒ 自死锁。
    RunWithWatchdog("ForceLoadPages(B) with a full pool", [&] { manager->ForceLoadPages(handle_b, 0); });
    {
        auto pool_pixels = ReadPool(vulkan_rhi, *pool);
        Residency ra = VerifyResidency(vulkan_rhi, vt_a, source_a, pool_pixels, *pool);
        Residency rb = VerifyResidency(vulkan_rhi, vt_b, source_b, pool_pixels, *pool);

        Check(rb.table_read_ok && rb.gpu_resident == 2, "B: both pages resident");
        Check(rb.gpu_cpu_mismatch == 0 && rb.ab_mismatch == 0, "B: GPU table == CPU, indirect lookup == source");
        // LRU（单调计数）：A 重载顺序 (0,0)(1,0)(0,1)(1,1)，B 的两页依次驱逐最老的两页。
        Check(ra.table_read_ok && ra.gpu_resident == 2, "A: exactly the 2 evicted pages dropped out");
        Check(ra.slots.size() == 4 && ra.slots[0] < 0 && ra.slots[1] < 0 && ra.slots[2] >= 0 && ra.slots[3] >= 0,
              "A: the two least-recently-loaded pages (row 0) were the ones evicted");
        // 修复前：被偷的槽位不通知 A，A 的 GPU（乃至 CPU）页表仍指向现在装着 B 像素的槽位。
        Check(ra.gpu_cpu_mismatch == 0, "A: GPU page table == CPU after being evicted by ANOTHER texture");
        Check(ra.ab_mismatch == 0, "A: no stale entry -- every page A's table still claims really holds A's pixels");

        std::set<int32_t> used;
        bool aliased = false;
        for (int32_t s : ra.slots)
        {
            if (s >= 0 && !used.insert(s).second)
            {
                aliased = true;
            }
        }
        for (int32_t s : rb.slots)
        {
            if (s >= 0 && !used.insert(s).second)
            {
                aliased = true;
            }
        }
        Check(!aliased && used.size() == 4, "no two resident pages (across A and B) share a physical slot");
        Check(vt_a->getPageTableUploadCount() == 4, "eviction by B dirtied A's table -> A re-uploaded once");
        Check(vt_b->getPageTableUploadCount() == 1, "B: 2 page loads -> 1 upload");
        Check(manager->GetStats().physical_pool_used_slots == 4, "pool fully used (4/4)");
    }
    Check(VmaAllocationCount(vulkan_rhi) == vma_baseline + 3, "VMA: pool + A table + B table = +3");

    // ---------------------------------------------------------------------
    std::printf("--- stage 4: unregister B (release + no deadlock) ---\n");
    vt_b.reset();
    // 修复前：Unregister 持 m_VirtualTexturesMutex 调 UnloadPages → GetVirtualTexture ⇒ 自死锁。
    RunWithWatchdog("UnregisterVirtualTexture(B)", [&] { manager->UnregisterVirtualTexture(handle_b); });
    Check(manager->GetVirtualTexture(handle_b) == nullptr, "B is gone from the manager");
    Check(VmaAllocationCount(vulkan_rhi) == vma_baseline + 2, "VMA: B's page table was released (+3 -> +2)");
    Check(manager->GetStats().physical_pool_used_slots == 2, "B's 2 slots returned to the pool");
    {
        auto pool_pixels = ReadPool(vulkan_rhi, *pool);
        Residency ra = VerifyResidency(vulkan_rhi, vt_a, source_a, pool_pixels, *pool);
        Check(ra.gpu_resident == 2 && ra.gpu_cpu_mismatch == 0 && ra.ab_mismatch == 0,
              "A: untouched by B's unregister (2 resident, table == CPU, lookup == source)");
        Check(vt_a->getPageTableUploadCount() == 4, "unregistering B costs A no upload");
    }

    // ---------------------------------------------------------------------
    std::printf("--- stage 5: unregister A, shut down ---\n");
    vt_a.reset();
    RunWithWatchdog("UnregisterVirtualTexture(A)", [&] { manager->UnregisterVirtualTexture(handle_a); });
    Check(VmaAllocationCount(vulkan_rhi) == vma_baseline + 1, "VMA: A's page table was released (+2 -> +1)");
    pool.reset();
    RunWithWatchdog("VirtualTextureManager::Shutdown", [&] { manager->Shutdown(); });
    Check(VmaAllocationCount(vulkan_rhi) == vma_baseline, "VMA: back to baseline after Shutdown (no leak)");
    manager.reset();  // 析构再调一次 Shutdown：必须幂等

    vulkan_rhi.Shutdown();
    std::printf("\n%d checks, %d failures\n", g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
