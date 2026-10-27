// =============================================================================
// VirtualTexture GPU 页上传 + 页表纹理 真机 A/B（Vulkan headless）。
//
// 2.6 把 VT 的 CPU 侧（屏幕投影 mip / 优先级 / LRU / 统计 / 时间戳 / 世界足迹）填实
// 了，但最硬的两处 GPU TODO 一直空着：
//   * VirtualTextureManager.cpp  UploadPageToPhysicalPool —— 页像素传进物理页池
//   * VirtualTextureResource.cpp UpdatePageTableTexture   —— 页表纹理上传
// 结果是"簿记全对、显存里一个字节都没有"：槽位分配了、页表项写好了，但页池图是空的，
// 页表纹理连对象都没建出来（m_PageTableTexture 恒为 nullptr）。
//
// 这个测试钉的是**只有真的跑起来才能证伪**的三件事：
//
//   ① 源像素真的进了物理页池，且落在**该页槽位对应的那一小块**上。判据：把页池整图
//      拷回 host，逐页与源纹理对应区域逐字节比对。offset 算错（忘了乘 page_size、
//      或按 slot 索引当像素坐标）这里当场红。
//   ② 逐页增量上传不会把先前驻留的页冲掉。这是 discard_prior_content 那条 guard 的
//      回归闸：barrier 若按 oldLayout=UNDEFINED 走，只有最后一页会存活。
//   ③ 页表纹理被建出来且内容正确：texel(px,py) = (physical_slot, mip_level)，与 CPU
//      侧 PageTableEntry 逐项一致。
//
//   cmake -B build -DZENGINE_BUILD_VT_GPU_UPLOAD_AB_SMOKE_TEST=ON
//   cmake --build build --target VirtualTextureGpuUploadABSmokeTest
// =============================================================================

#include "CommonPCH/pch.h"

#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Function/Render/Interface/RHI.h"
#include "Runtime/Function/Render/Interface/Vulkan/VulkanRHI.h"
#include "Runtime/Function/Render/RenderType.h"
#include "Runtime/Function/Render/Texture/VirtualTextureManager.h"
#include "Runtime/Function/Render/Texture/VirtualTextureTypes.h"
#include "Runtime/Function/Render/WindowSystem.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
            std::printf("[FAIL] %s\n", name);
        }
        else
        {
            std::printf("[ ok ] %s\n", name);
        }
    }

    constexpr uint32_t kPageSize = 64;
    constexpr uint32_t kPagesPerSide = 4;  // 源 256x256 / page 64 => 4x4 = 16 页
    constexpr uint32_t kVirtualSize = kPageSize * kPagesPerSide;
    constexpr uint32_t kBytesPerPixel = 4;  // R8G8B8A8_UNORM

    // 把一张 2D 图像（COLOR aspect）拷回 host。
    // 前置：图像当前处于 SHADER_READ_ONLY_OPTIMAL（UploadImageRegion 结束时的布局）。
    // bytes_per_pixel 必须按被测图的**真实格式**给：页池是 R8G8B8A8（4B），页表是
    // R32G32_SFLOAT（8B）。按 4B 去读 8B 的图会踩 VUID-vkCmdCopyImageToBuffer-
    // pRegions-00183（拷贝字节数超出 buffer）。
    std::vector<uint8_t> ReadBackImage(RHI* rhi,
                                       RHIImage* image,
                                       uint32_t width,
                                       uint32_t height,
                                       uint32_t bytes_per_pixel)
    {
        std::vector<uint8_t> pixels;
        if (rhi == nullptr || image == nullptr || width == 0 || height == 0 || bytes_per_pixel == 0)
        {
            return pixels;
        }

        const RHIDeviceSize bytes = static_cast<RHIDeviceSize>(width) * static_cast<RHIDeviceSize>(height) *
                                    static_cast<RHIDeviceSize>(bytes_per_pixel);

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
            RHIImageMemoryBarrier to_transfer_src {};
            to_transfer_src.sType = RHI_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            to_transfer_src.srcAccessMask = RHI_ACCESS_SHADER_READ_BIT;
            to_transfer_src.dstAccessMask = RHI_ACCESS_TRANSFER_READ_BIT;
            to_transfer_src.oldLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            to_transfer_src.newLayout = RHI_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            to_transfer_src.srcQueueFamilyIndex = ~0u;
            to_transfer_src.dstQueueFamilyIndex = ~0u;
            to_transfer_src.image = image;
            to_transfer_src.subresourceRange.aspectMask = RHI_IMAGE_ASPECT_COLOR_BIT;
            to_transfer_src.subresourceRange.baseMipLevel = 0;
            to_transfer_src.subresourceRange.levelCount = 1;
            to_transfer_src.subresourceRange.baseArrayLayer = 0;
            to_transfer_src.subresourceRange.layerCount = 1;
            rhi->CmdPipelineBarrier(command_buffer,
                                    RHI_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                    RHI_PIPELINE_STAGE_TRANSFER_BIT,
                                    0,
                                    0,
                                    nullptr,
                                    0,
                                    nullptr,
                                    1,
                                    &to_transfer_src);

            RHIBufferImageCopy region {};
            region.bufferOffset = 0;
            region.bufferRowLength = 0;  // 0 = 紧凑打包；Vulkan 单位是 texel 不是字节
            region.bufferImageHeight = 0;
            region.imageSubresource.aspectMask = RHI_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.mipLevel = 0;
            region.imageSubresource.baseArrayLayer = 0;
            region.imageSubresource.layerCount = 1;
            region.imageOffset.x = 0;
            region.imageOffset.y = 0;
            region.imageOffset.z = 0;
            region.imageExtent.width = width;
            region.imageExtent.height = height;
            region.imageExtent.depth = 1;
            rhi->CmdCopyImageToBuffer(command_buffer, image, RHI_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);

            rhi->EndSingleTimeCommands(command_buffer);
        }

        void* mapped = nullptr;
        if (rhi->MapMemory(memory, 0, bytes, 0, &mapped) && mapped != nullptr)
        {
            const uint8_t* source = static_cast<const uint8_t*>(mapped);
            pixels.assign(source, source + static_cast<size_t>(bytes));
            rhi->UnmapMemory(memory);
        }

        rhi->FreeMemory(memory);
        rhi->DestroyBuffer(buffer);
        return pixels;
    }

    // 确定性图案：R = x, G = y, B = 页索引, A = 255。
    // 每页颜色都不同 ⇒ 页之间贴错/串位一眼可见（而不是"都是灰的但看不出错位"）。
    std::shared_ptr<TextureData> MakeSourceTexture()
    {
        auto data = std::make_shared<TextureData>();
        data->m_Width = kVirtualSize;
        data->m_Height = kVirtualSize;
        data->m_Depth = 1;
        data->m_MipLevels = 1;
        data->m_ArrayLayers = 1;
        data->m_Format = RHI_FORMAT_R8G8B8A8_UNORM;
        data->m_PixelsLength = kVirtualSize * kVirtualSize * kBytesPerPixel;

        // TextureData 的析构用 free()，所以这里必须 malloc —— 用 new[] 配对会崩。
        data->m_Pixels = static_cast<uint8_t*>(std::malloc(data->m_PixelsLength));
        if (data->m_Pixels == nullptr)
        {
            return nullptr;
        }

        for (uint32_t y = 0; y < kVirtualSize; ++y)
        {
            for (uint32_t x = 0; x < kVirtualSize; ++x)
            {
                const uint32_t page_x = x / kPageSize;
                const uint32_t page_y = y / kPageSize;
                const uint32_t page_index = page_y * kPagesPerSide + page_x;
                uint8_t* texel = data->m_Pixels + (static_cast<size_t>(y) * kVirtualSize + x) * kBytesPerPixel;
                texel[0] = static_cast<uint8_t>(x & 0xFF);
                texel[1] = static_cast<uint8_t>(y & 0xFF);
                texel[2] = static_cast<uint8_t>(page_index & 0xFF);
                texel[3] = 255;
            }
        }
        return data;
    }
}  // namespace

int main()
{
    // 关掉 stdout 缓冲：这类 headless GPU 测试一旦访问违例，缓冲里的输出会全部丢失，
    // 只剩一个 0xC0000005 却看不出死在哪一行。无缓冲至少能定位到最后一条 [ ok ]。
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    std::printf("=== VirtualTexture GPU page-upload A/B smoke test (Vulkan headless) ===\n");

    SystemRegistry::GetInstance().RegisterSystem(new WindowSystem());
    Check(GET_SYSTEM(WindowSystem) != nullptr, "a WindowSystem is registered");

    VulkanRHI vulkan_rhi;
    const bool vulkan_ready = vulkan_rhi.Initialize();
    Check(vulkan_ready, "VulkanRHI::Initialize succeeds (headless)");
    if (!vulkan_ready)
    {
        std::printf("\n%d checks, %d failures\n", g_Checks, g_Failures);
        return g_Failures == 0 ? 0 : 1;
    }

    // ---- 建管理器：池子必须小（默认 2048x2048 页会直接爆显存）----
    // 异步加载关掉：本测试要的是同步、可复现的 LoadPage，不想跟后台线程抢队列。
    VirtualTextureManager manager;
    VirtualTextureConfig config;
    config.page_size = kPageSize;
    config.physical_pool_width = kPagesPerSide;
    config.physical_pool_height = kPagesPerSide;
    config.memory_budget_mb = 64;
    config.enable_async_loading = false;
    manager.SetConfig(config);
    manager.Initialize(&vulkan_rhi, nullptr);

    auto pool = manager.getPhysicalTexturePool();
    Check(pool != nullptr, "a physical texture pool was created");
    Check(pool != nullptr && pool->getPhysicalTexture() != nullptr, "the pool has a real GPU image");

    // ---- 注册源纹理（走 TextureData 重载 ⇒ 源像素被保留，才有东西可传）----
    auto source = MakeSourceTexture();
    Check(source != nullptr && source->IsValid(), "the synthetic source texture was built");

    const VirtualTextureHandle handle = manager.RegisterVirtualTexture(source, kPageSize);
    auto resource = manager.GetVirtualTexture(handle);
    Check(resource != nullptr && resource->isInitialized(), "the virtual texture registered");
    Check(resource != nullptr && resource->getPagesX() == kPagesPerSide && resource->getPagesY() == kPagesPerSide,
          "the virtual texture is 4x4 pages");
    Check(resource != nullptr && resource->getSourceData() != nullptr, "the source pixels were retained");
    Check(resource != nullptr && resource->getBytesPerPixel() == kBytesPerPixel, "bytes-per-pixel is 4");

    // ---- 同步加载全部 16 页 ----
    const bool forced = manager.ForceLoadPages(handle, 0);
    Check(forced, "ForceLoadPages succeeded");

    const VirtualTextureStats stats = manager.GetStats();
    Check(stats.loaded_pages == kPagesPerSide * kPagesPerSide, "all 16 pages report as loaded");
    std::printf("       [info] loaded=%u pool_used=%u/%u\n",
                stats.loaded_pages,
                stats.physical_pool_used_slots,
                stats.physical_pool_total_slots);

    bool all_slots_valid = true;
    for (uint32_t page_y = 0; page_y < kPagesPerSide && all_slots_valid; ++page_y)
    {
        for (uint32_t page_x = 0; page_x < kPagesPerSide; ++page_x)
        {
            const PageTableEntry entry = resource->GetPageTableEntry(0, page_x, page_y);
            if (!entry.is_valid || entry.physical_slot == UINT32_MAX)
            {
                all_slots_valid = false;
                std::printf("       [diag] page (%u,%u) has no physical slot\n", page_x, page_y);
                break;
            }
        }
    }
    Check(all_slots_valid, "every page got a valid physical slot");

    // ---- ①+② 把页池整图拷回来，逐页与源区域比对 ----
    const uint32_t pool_pixels = kPagesPerSide * kPageSize;
    std::vector<uint8_t> pool_readback =
        ReadBackImage(&vulkan_rhi, pool->getPhysicalTexture(), pool_pixels, pool_pixels, kBytesPerPixel);
    Check(pool_readback.size() == static_cast<size_t>(pool_pixels) * pool_pixels * kBytesPerPixel,
          "the pool image read back at full size");
    Check(pool != nullptr && pool->hasContent(), "the pool reports having uploaded content");

    size_t mismatched_pages = 0;
    size_t mismatched_bytes = 0;
    size_t first_bad_page = UINT32_MAX;
    if (pool_readback.size() == static_cast<size_t>(pool_pixels) * pool_pixels * kBytesPerPixel && resource)
    {
        for (uint32_t page_y = 0; page_y < kPagesPerSide; ++page_y)
        {
            for (uint32_t page_x = 0; page_x < kPagesPerSide; ++page_x)
            {
                const PageTableEntry entry = resource->GetPageTableEntry(0, page_x, page_y);
                uint32_t slot_x = 0;
                uint32_t slot_y = 0;
                pool->GetSlotCoordinates(entry.physical_slot, slot_x, slot_y);

                const uint32_t dest_origin_x = slot_x * kPageSize;
                const uint32_t dest_origin_y = slot_y * kPageSize;
                const uint32_t source_origin_x = page_x * kPageSize;
                const uint32_t source_origin_y = page_y * kPageSize;

                size_t page_bad_bytes = 0;
                for (uint32_t row = 0; row < kPageSize; ++row)
                {
                    const uint8_t* dest_row =
                        pool_readback.data() +
                        (static_cast<size_t>(dest_origin_y + row) * pool_pixels + dest_origin_x) * kBytesPerPixel;
                    const uint8_t* source_row =
                        source->m_Pixels +
                        (static_cast<size_t>(source_origin_y + row) * kVirtualSize + source_origin_x) * kBytesPerPixel;
                    if (std::memcmp(dest_row, source_row, kPageSize * kBytesPerPixel) != 0)
                    {
                        for (size_t i = 0; i < static_cast<size_t>(kPageSize) * kBytesPerPixel; ++i)
                        {
                            if (dest_row[i] != source_row[i])
                            {
                                ++page_bad_bytes;
                            }
                        }
                    }
                }

                if (page_bad_bytes != 0)
                {
                    ++mismatched_pages;
                    mismatched_bytes += page_bad_bytes;
                    if (first_bad_page == UINT32_MAX)
                    {
                        first_bad_page = page_y * kPagesPerSide + page_x;
                        std::printf("       [diag] page %zu mismatch: src(%u,%u) -> slot %u at (%u,%u), %zu bad bytes\n",
                                    first_bad_page,
                                    page_x,
                                    page_y,
                                    entry.physical_slot,
                                    dest_origin_x,
                                    dest_origin_y,
                                    page_bad_bytes);
                    }
                }
            }
        }
    }

    Check(mismatched_pages == 0, "every uploaded page in the pool matches its source region byte-for-byte");
    std::printf("       [info] page mismatches: %zu pages, %zu bytes\n", mismatched_pages, mismatched_bytes);

    // ---- ③ 页表纹理 ----
    RHIImage* page_table = resource ? resource->GetPageTableTexture() : nullptr;
    Check(page_table != nullptr, "the page table texture was created");

    // 页表是 R32G32_SFLOAT ⇒ 8 字节/texel，不是页池那 4 字节。
    constexpr uint32_t kTableBytesPerTexel = 8;
    std::vector<uint8_t> table_readback =
        ReadBackImage(&vulkan_rhi, page_table, kPagesPerSide, kPagesPerSide, kTableBytesPerTexel);
    const size_t table_bytes_expected = static_cast<size_t>(kPagesPerSide) * kPagesPerSide * kTableBytesPerTexel;

    bool table_ok = table_readback.size() == table_bytes_expected;
    size_t table_bad_texels = 0;
    if (table_ok && resource)
    {
        const float* texels = reinterpret_cast<const float*>(table_readback.data());
        for (uint32_t page_y = 0; page_y < kPagesPerSide; ++page_y)
        {
            for (uint32_t page_x = 0; page_x < kPagesPerSide; ++page_x)
            {
                const PageTableEntry entry = resource->GetPageTableEntry(0, page_x, page_y);
                const size_t index = (static_cast<size_t>(page_y) * kPagesPerSide + page_x) * 2;
                const float slot_value = texels[index + 0];
                const float mip_value = texels[index + 1];
                if (static_cast<uint32_t>(slot_value) != entry.physical_slot || static_cast<uint32_t>(mip_value) != 0u)
                {
                    ++table_bad_texels;
                    if (table_bad_texels == 1u)
                    {
                        std::printf("       [diag] page table texel (%u,%u) = (%f,%f), CPU says (%u,%u)\n",
                                    page_x,
                                    page_y,
                                    static_cast<double>(slot_value),
                                    static_cast<double>(mip_value),
                                    entry.physical_slot,
                                    entry.mip_level);
                    }
                }
            }
        }
    }
    Check(table_ok, "the page table texture read back at 8 bytes per texel");
    Check(table_ok && table_bad_texels == 0, "every page table texel matches the CPU PageTableEntry");
    std::printf("       [info] page table bad texels: %zu\n", table_bad_texels);

    manager.Shutdown();
    vulkan_rhi.Shutdown();

    std::printf("\n%d checks, %d failures\n", g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
