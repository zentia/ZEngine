#include "VirtualTextureTypes.h"

#include "Runtime/Function/Render/Interface/RHI.h"
#if defined(__APPLE__)
    #include <TargetConditionals.h>
#endif
#if ZENGINE_VT_GPU
    #include "Runtime/Core/Base/Macro.h"
    #include "Runtime/Function/Render/Interface/Vulkan/VulkanRHI.h"
    #include "Runtime/Function/Render/Interface/Vulkan/VulkanRHIResource.h"
#endif

#include <algorithm>
#include <cstdint>
#include <mutex>

//////////////////////////////////////////////////////////////////////////
// PhysicalTexturePool Implementation
//////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////
// 后端中立的图像管理 —— 契约见 VirtualTextureTypes.h
//////////////////////////////////////////////////////////////////////////

bool VirtualTextureUsesVulkanPath(RHI* rhi)
{
#if ZENGINE_VT_GPU
    return dynamic_cast<VulkanRHI*>(rhi) != nullptr;
#else
    (void)rhi;
    return false;
#endif
}

bool VirtualTextureBackendSupported(RHI* rhi)
{
#if ZENGINE_VT_GPU
    if (rhi == nullptr)
    {
        return false;
    }
    return VirtualTextureUsesVulkanPath(rhi) || rhi->SupportsVirtualTextureUpload();
#else
    (void)rhi;
    return false;
#endif
}

bool VirtualTextureCreateGenericImage(RHI* rhi,
                                      uint32_t width,
                                      uint32_t height,
                                      RHIFormat format,
                                      RHIImage*& image,
                                      RHIImageView*& view,
                                      void*& backend_alloc)
{
    image = nullptr;
    view = nullptr;
    backend_alloc = nullptr;
    if (rhi == nullptr || width == 0 || height == 0)
    {
        return false;
    }

    RHIDeviceMemory* memory = nullptr;
    // TRANSFER_SRC 只为 A/B 验证（把图拷回 host 逐 texel 比对），同 Vulkan 路径。
    rhi->CreateImage(width,
                     height,
                     format,
                     RHIImageTiling::RHI_IMAGE_TILING_OPTIMAL,
                     RHIImageUsageFlagBits::RHI_IMAGE_USAGE_SAMPLED_BIT |
                         RHIImageUsageFlagBits::RHI_IMAGE_USAGE_TRANSFER_DST_BIT |
                         RHIImageUsageFlagBits::RHI_IMAGE_USAGE_TRANSFER_SRC_BIT,
                     RHIMemoryPropertyFlagBits::RHI_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                     image,
                     memory,
                     0,
                     1,
                     1);
    if (image == nullptr)
    {
        if (memory != nullptr)
        {
            rhi->FreeMemory(memory);
        }
        return false;
    }

    rhi->CreateImageView(image,
                         format,
                         RHIImageAspectFlagBits::RHI_IMAGE_ASPECT_COLOR_BIT,
                         RHIImageViewType::RHI_IMAGE_VIEW_TYPE_2D,
                         1,
                         1,
                         view);
    if (view == nullptr)
    {
        rhi->DestroyImage(image);
        image = nullptr;
        if (memory != nullptr)
        {
            rhi->FreeMemory(memory);
        }
        return false;
    }
    backend_alloc = memory;
    return true;
}

void VirtualTextureDestroyGenericImage(RHI* rhi, RHIImage* image, RHIImageView* view, void* backend_alloc)
{
    if (rhi == nullptr)
    {
        return;
    }
    if (view != nullptr)
    {
        rhi->DestroyImageView(view);
    }
    if (image != nullptr)
    {
        rhi->DestroyImage(image);
    }
    if (backend_alloc != nullptr)
    {
        RHIDeviceMemory* memory = static_cast<RHIDeviceMemory*>(backend_alloc);
        rhi->FreeMemory(memory);
    }
}

PhysicalTexturePool::PhysicalTexturePool()
    : m_Rhi(nullptr), m_PhysicalTexture(nullptr), m_PhysicalTextureView(nullptr), m_Allocation(nullptr),
      m_PoolWidthPages(0), m_PoolHeightPages(0), m_PageSize(0), m_Format(RHIFormat::RHI_FORMAT_R8G8B8A8_UNORM),
      m_LogicalOnly(false), m_HasContent(false)
{
}

PhysicalTexturePool::~PhysicalTexturePool()
{
    Shutdown();
}

bool PhysicalTexturePool::Initialize(RHI* rhi,
                                     uint32_t pool_width_pages,
                                     uint32_t pool_height_pages,
                                     uint32_t page_size,
                                     RHIFormat format)
{
    m_Rhi = rhi;
    m_PoolWidthPages = pool_width_pages;
    m_PoolHeightPages = pool_height_pages;
    m_PageSize = page_size;
    m_Format = format;

#if !ZENGINE_VT_GPU
    return false;  // 无 Vulkan 后端：调用方应走 InitializeLogical
#else
    // Calculate physical texture dimensions. 64 位乘：默认配置 2048 页 * 128 px = 262144，
    // 平方后 2^36 个 texel，32 位乘法在这里不溢出，但再大一点就会，别图省事改回去。
    const uint64_t physical_width = static_cast<uint64_t>(pool_width_pages) * page_size;
    const uint64_t physical_height = static_cast<uint64_t>(pool_height_pages) * page_size;

    // 池图必须塞得进设备：默认 VirtualTextureConfig 要 262144 x 262144（RGBA8 = 256 GiB），
    // 任何 GPU 的 maxImageDimension2D 都远小于它（常见 16384~32768）。不先量一下，
    // vmaCreateImage 只会返回一个错误码，上层又吞了它，最后是一个"活着但一页也装不下"的
    // 管理器。量一下就能在 Initialize 里干净地失败，并说明白为什么。
    RHIPhysicalDeviceProperties device_properties {};
    rhi->GetPhysicalDeviceProperties(&device_properties);
    const uint64_t max_dimension = device_properties.limits.maxImageDimension2D;
    if (page_size == 0 || pool_width_pages == 0 || pool_height_pages == 0 || physical_width > max_dimension ||
        physical_height > max_dimension)
    {
        LOG_ERROR(ZRender,
                  "virtual texture: physical pool {}x{} pages of {} px = {}x{} px exceeds the device's "
                  "maxImageDimension2D ({}); shrink VirtualTextureConfig::physical_pool_width/height",
                  pool_width_pages,
                  pool_height_pages,
                  page_size,
                  physical_width,
                  physical_height,
                  max_dimension);
        return false;
    }

    // 上面的上限检查已保证两个维度都 <= maxImageDimension2D（uint32），这里安全地收窄回 32 位，
    // 供下面 Vulkan 结构体的 uint32_t 字段使用。
    const uint32_t physical_width_px = static_cast<uint32_t>(physical_width);
    const uint32_t physical_height_px = static_cast<uint32_t>(physical_height);

    // 槽位网格：两条后端路径共用。
    auto init_slots = [this, pool_width_pages, pool_height_pages]() {
        uint32_t total_slots = pool_width_pages * pool_height_pages;
        m_Slots.resize(total_slots);
        for (uint32_t i = 0; i < total_slots; ++i)
        {
            uint32_t x, y;
            GetSlotCoordinates(i, x, y);
            m_Slots[i].x = x;
            m_Slots[i].y = y;
            m_Slots[i].is_allocated = false;
            m_Slots[i].last_access_time = 0;
        }
    };

    // 非 Vulkan 后端（DX12）：走通用 RHI 路径，不碰 VMA。
    if (!VirtualTextureUsesVulkanPath(rhi))
    {
        if (!VirtualTextureBackendSupported(rhi))
        {
            return false;
        }
        void* generic_alloc = nullptr;
        if (!VirtualTextureCreateGenericImage(rhi,
                                              physical_width_px,
                                              physical_height_px,
                                              format,
                                              m_PhysicalTexture,
                                              m_PhysicalTextureView,
                                              generic_alloc))
        {
            return false;
        }
        m_Allocation = static_cast<VmaAllocation>(generic_alloc);
        init_slots();
        return true;
    }

    // Create physical texture
    auto vulkan_rhi = static_cast<VulkanRHI*>(rhi);
    if (!vulkan_rhi)
    {
        return false;
    }

    VmaAllocator allocator = vulkan_rhi->getVmaAllocator();
    if (!allocator)
    {
        return false;
    }

    // Create image
    RHIImageCreateInfo image_create_info {};
    image_create_info.imageType = RHIImageType::RHI_IMAGE_TYPE_2D;
    image_create_info.extent.width = physical_width_px;
    image_create_info.extent.height = physical_height_px;
    image_create_info.extent.depth = 1;
    image_create_info.mipLevels = 1;
    image_create_info.arrayLayers = 1;
    image_create_info.format = format;
    image_create_info.tiling = RHIImageTiling::RHI_IMAGE_TILING_OPTIMAL;
    image_create_info.initialLayout = RHIImageLayout::RHI_IMAGE_LAYOUT_UNDEFINED;
    // TRANSFER_SRC_BIT 只为 A/B 验证服务：测试要把页池整图拷回 host，逐像素和源
    // 纹理比对（"传进显存的页 == 源里那一块"是这条链唯一能证伪的判据）。
    // 缺它就报 CmdCopyImageToBuffer 的 usage 校验错（HiZ 切法一踩过同一条）。
    image_create_info.usage = RHIImageUsageFlagBits::RHI_IMAGE_USAGE_SAMPLED_BIT |
                              RHIImageUsageFlagBits::RHI_IMAGE_USAGE_TRANSFER_DST_BIT |
                              RHIImageUsageFlagBits::RHI_IMAGE_USAGE_TRANSFER_SRC_BIT;
    image_create_info.samples = RHISampleCountFlagBits::RHI_SAMPLE_COUNT_1_BIT;
    image_create_info.flags = 0;

    VmaAllocationCreateInfo alloc_info = {};
    alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
    alloc_info.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;

    VkImageCreateInfo vk_image_info = {};
    vk_image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    vk_image_info.imageType = VK_IMAGE_TYPE_2D;
    vk_image_info.extent.width = physical_width_px;
    vk_image_info.extent.height = physical_height_px;
    vk_image_info.extent.depth = 1;
    vk_image_info.mipLevels = 1;
    vk_image_info.arrayLayers = 1;
    vk_image_info.format = (VkFormat)format;
    vk_image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    vk_image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    vk_image_info.usage =
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    vk_image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    vk_image_info.flags = 0;

    VkImage vk_image;
    VmaAllocation vk_allocation;
    VmaAllocationInfo alloc_info_result;

    VkResult result =
        vmaCreateImage(allocator, &vk_image_info, &alloc_info, &vk_image, &vk_allocation, &alloc_info_result);
    if (result != VK_SUCCESS)
    {
        return false;
    }

    // ⚠️ 不能直接把 VkImage 强转成 RHIImage*。RHI 侧所有接口都把 RHIImage* 当成
    // VulkanImage**对象*、用 ((VulkanImage*)image)->getResource() 取句柄
    // （见 VulkanRHI::CreateImageView / CmdCopyImageToBuffer）。VkImage 是个不透明
    // 句柄数值，当对象指针解引用直接访问违例 —— 这条路径此前从没被真正跑过（VT 零
    // 消费者），所以这个坑一直藏着。必须包一层 VulkanImage 对象。
    m_PhysicalTexture = new VulkanImage();
    ((VulkanImage*)m_PhysicalTexture)->setResource(vk_image);
    m_Allocation = vk_allocation;

    // Create image view
    RHIImageViewCreateInfo view_create_info {};
    view_create_info.image = m_PhysicalTexture;
    view_create_info.viewType = RHIImageViewType::RHI_IMAGE_VIEW_TYPE_2D;
    view_create_info.format = format;
    view_create_info.subresourceRange.aspectMask = RHIImageAspectFlagBits::RHI_IMAGE_ASPECT_COLOR_BIT;
    view_create_info.subresourceRange.baseMipLevel = 0;
    view_create_info.subresourceRange.levelCount = 1;
    view_create_info.subresourceRange.baseArrayLayer = 0;
    view_create_info.subresourceRange.layerCount = 1;

    rhi->CreateImageView(m_PhysicalTexture,
                         format,
                         RHIImageAspectFlagBits::RHI_IMAGE_ASPECT_COLOR_BIT,
                         RHIImageViewType::RHI_IMAGE_VIEW_TYPE_2D,
                         1,
                         1,
                         m_PhysicalTextureView);

    init_slots();

    return true;
#endif
}

bool PhysicalTexturePool::InitializeLogical(uint32_t pool_width_pages,
                                            uint32_t pool_height_pages,
                                            uint32_t page_size)
{
    // Headless counterpart of Initialize(): build the slot grid and size the
    // pool metadata, but create NO Vulkan image. Everything that is pure CPU
    // (slot allocation, LRU, access-time tracking) works unchanged, so the
    // streaming policy can be unit-tested under WASM with no RHI/device.
    m_Rhi = nullptr;
    m_PoolWidthPages = pool_width_pages;
    m_PoolHeightPages = pool_height_pages;
    m_PageSize = page_size;
    m_Format = RHIFormat::RHI_FORMAT_R8G8B8A8_UNORM;
    m_LogicalOnly = true;
    m_PhysicalTexture = nullptr;
    m_PhysicalTextureView = nullptr;
    m_Allocation = nullptr;

    uint32_t total_slots = pool_width_pages * pool_height_pages;
    m_Slots.resize(total_slots);
    for (uint32_t i = 0; i < total_slots; ++i)
    {
        uint32_t x, y;
        GetSlotCoordinates(i, x, y);
        m_Slots[i].x = x;
        m_Slots[i].y = y;
        m_Slots[i].is_allocated = false;
        m_Slots[i].last_access_time = 0;
    }

    return true;
}

void PhysicalTexturePool::Shutdown()
{
#if ZENGINE_VT_GPU
    // 通用 RHI 路径（DX12）：图 / view / 内存都是 RHI 自己的对象，一把销毁，不走下面 Vulkan 专用的
    // "RHI 销毁 + 自己 delete 包装" 双步。
    if (m_Rhi && m_PhysicalTexture && !VirtualTextureUsesVulkanPath(m_Rhi))
    {
        VirtualTextureDestroyGenericImage(m_Rhi, m_PhysicalTexture, m_PhysicalTextureView, m_Allocation);
        m_PhysicalTexture = nullptr;
        m_PhysicalTextureView = nullptr;
        m_Allocation = nullptr;
    }
#endif
    if (m_PhysicalTextureView)
    {
        // 旧注释写"Image view will be destroyed by RHI"——RHI 从不替调用方销毁 view，
        // 这里置空就是泄漏（VkImageView + VulkanRHI::CreateImageView new 出来的
        // VulkanImageView 包装对象）。single-time 提交都 vkQueueWaitIdle 过，此刻无
        // GPU 工作在用它，可直接销毁。
#if ZENGINE_VT_GPU
        if (m_Rhi)
        {
            m_Rhi->DestroyImageView(m_PhysicalTextureView);
        }
        delete (VulkanImageView*)m_PhysicalTextureView;
#endif
        m_PhysicalTextureView = nullptr;
    }

    if (m_PhysicalTexture && m_Allocation && m_Rhi)
    {
#if !ZENGINE_VT_GPU
        m_PhysicalTexture = nullptr;
        m_Allocation = nullptr;
#else
        auto vulkan_rhi = static_cast<VulkanRHI*>(m_Rhi);
        if (vulkan_rhi)
        {
            VmaAllocator allocator = vulkan_rhi->getVmaAllocator();
            if (allocator)
            {
                // 从包装对象里取回真正的 VkImage（同上：不能直接强转指针）。
                vmaDestroyImage(allocator, ((VulkanImage*)m_PhysicalTexture)->getResource(), m_Allocation);
            }
        }
        delete (VulkanImage*)m_PhysicalTexture;  // 包装对象是我们 new 的，要自己释放
        m_PhysicalTexture = nullptr;
        m_Allocation = nullptr;
#endif
    }

    m_Slots.clear();
    m_HasContent = false;
    m_Rhi = nullptr;
}

uint32_t PhysicalTexturePool::AllocateSlot(const VirtualTexturePageID& page_id)
{
    std::lock_guard<std::mutex> lock(m_SlotMutex);

    // Find first free slot
    for (uint32_t i = 0; i < m_Slots.size(); ++i)
    {
        if (!m_Slots[i].is_allocated)
        {
            m_Slots[i].is_allocated = true;
            m_Slots[i].page_id = page_id;
            m_Slots[i].last_access_time = 0;
            return i;
        }
    }

    // 池满：不偷槽位（见头文件注释——旧实现在这里持锁调 FindLRUSlot 自死锁，
    // 且偷来的槽位不通知原主人，造成页表别名）。由管理器走 UnloadPage 驱逐后重试。
    return UINT32_MAX;
}

void PhysicalTexturePool::FreeSlot(uint32_t slot_index)
{
    if (slot_index >= m_Slots.size())
    {
        return;
    }

    std::lock_guard<std::mutex> lock(m_SlotMutex);
    m_Slots[slot_index].is_allocated = false;
    m_Slots[slot_index].page_id = VirtualTexturePageID();
    m_Slots[slot_index].last_access_time = 0;
}

void PhysicalTexturePool::GetSlotCoordinates(uint32_t slot_index, uint32_t& x, uint32_t& y) const
{
    x = slot_index % m_PoolWidthPages;
    y = slot_index / m_PoolWidthPages;
}

uint32_t PhysicalTexturePool::GetSlotIndex(uint32_t x, uint32_t y) const
{
    if (x >= m_PoolWidthPages || y >= m_PoolHeightPages)
    {
        return UINT32_MAX;
    }
    return y * m_PoolWidthPages + x;
}

void PhysicalTexturePool::UpdateSlotAccessTime(uint32_t slot_index, uint64_t time)
{
    if (slot_index >= m_Slots.size())
    {
        return;
    }

    std::lock_guard<std::mutex> lock(m_SlotMutex);
    m_Slots[slot_index].last_access_time = time;
}

uint32_t PhysicalTexturePool::FindLRUSlot() const
{
    std::lock_guard<std::mutex> lock(m_SlotMutex);

    uint64_t oldest_time = UINT64_MAX;
    uint32_t lru_slot = UINT32_MAX;

    for (uint32_t i = 0; i < m_Slots.size(); ++i)
    {
        if (m_Slots[i].is_allocated && m_Slots[i].last_access_time < oldest_time)
        {
            oldest_time = m_Slots[i].last_access_time;
            lru_slot = i;
        }
    }

    return lru_slot;
}

const PhysicalTextureSlot& PhysicalTexturePool::GetSlot(uint32_t slot_index) const
{
    static PhysicalTextureSlot empty_slot;
    if (slot_index >= m_Slots.size())
    {
        return empty_slot;
    }
    return m_Slots[slot_index];
}

PhysicalTextureSlot& PhysicalTexturePool::GetSlot(uint32_t slot_index)
{
    static PhysicalTextureSlot empty_slot;
    if (slot_index >= m_Slots.size())
    {
        return empty_slot;
    }
    return m_Slots[slot_index];
}

bool PhysicalTexturePool::IsSlotAllocated(uint32_t slot_index) const
{
    if (slot_index >= m_Slots.size())
    {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_SlotMutex);
    return m_Slots[slot_index].is_allocated;
}

//////////////////////////////////////////////////////////////////////////
// UploadImageRegion —— 见 VirtualTextureTypes.h 的契约说明。
//////////////////////////////////////////////////////////////////////////

bool UploadImageRegion(RHI* rhi,
                       RHIImage* image,
                       const void* data,
                       size_t data_bytes,
                       uint32_t offset_x,
                       uint32_t offset_y,
                       uint32_t width,
                       uint32_t height,
                       uint32_t bytes_per_pixel,
                       bool discard_prior_content)
{
    if (rhi == nullptr || image == nullptr || data == nullptr || data_bytes == 0 || width == 0 || height == 0 ||
        bytes_per_pixel == 0)
    {
        return false;
    }

    const size_t needed =
        static_cast<size_t>(width) * static_cast<size_t>(height) * static_cast<size_t>(bytes_per_pixel);
    if (data_bytes < needed)
    {
        return false;
    }

#if !ZENGINE_VT_GPU
    return false;
#else
    // 非 Vulkan 后端：整段"暂存 + barrier + 拷贝 + barrier"由 RHI 自己完成。
    if (!VirtualTextureUsesVulkanPath(rhi))
    {
        return rhi->UploadImageRegionImmediate(
            image, data, data_bytes, offset_x, offset_y, width, height, bytes_per_pixel, discard_prior_content);
    }

    auto vulkan_rhi = static_cast<VulkanRHI*>(rhi);
    if (!vulkan_rhi)
    {
        return false;
    }

    VmaAllocator allocator = vulkan_rhi->getVmaAllocator();
    if (!allocator)
    {
        return false;
    }

    // ---- staging buffer（host-visible，写一次就丢）----
    VkBufferCreateInfo buffer_info {};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = needed;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo staging_alloc_info {};
    staging_alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
    staging_alloc_info.flags =
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VkBuffer staging_buffer = VK_NULL_HANDLE;
    VmaAllocation staging_allocation = nullptr;
    VmaAllocationInfo staging_info {};
    if (vmaCreateBuffer(allocator, &buffer_info, &staging_alloc_info, &staging_buffer, &staging_allocation,
                        &staging_info) != VK_SUCCESS)
    {
        return false;
    }

    bool mapped_ok = false;
    if (staging_info.pMappedData != nullptr)
    {
        memcpy(staging_info.pMappedData, data, needed);
        mapped_ok = true;
    }
    else
    {
        void* mapped = nullptr;
        if (vmaMapMemory(allocator, staging_allocation, &mapped) == VK_SUCCESS && mapped != nullptr)
        {
            memcpy(mapped, data, needed);
            vmaUnmapMemory(allocator, staging_allocation);
            mapped_ok = true;
        }
    }

    if (!mapped_ok)
    {
        vmaDestroyBuffer(allocator, staging_buffer, staging_allocation);
        return false;
    }

    // ---- barrier → TRANSFER_DST ----
    RHICommandBuffer* rhi_command_buffer = vulkan_rhi->BeginSingleTimeCommands();
    VkCommandBuffer command_buffer = ((VulkanCommandBuffer*)rhi_command_buffer)->getResource();

    // RHIImage* 是 VulkanImage 包装对象，真正的句柄要 getResource() 取。
    VkImage image_handle = ((VulkanImage*)image)->getResource();

    VkImageMemoryBarrier to_dst {};
    to_dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_dst.oldLayout = discard_prior_content ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.image = image_handle;
    to_dst.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    to_dst.subresourceRange.baseMipLevel = 0;
    to_dst.subresourceRange.levelCount = 1;
    to_dst.subresourceRange.baseArrayLayer = 0;
    to_dst.subresourceRange.layerCount = 1;
    to_dst.srcAccessMask = discard_prior_content ? 0 : VK_ACCESS_SHADER_READ_BIT;
    to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(command_buffer,
                         discard_prior_content ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                                               : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0,
                         0,
                         nullptr,
                         0,
                         nullptr,
                         1,
                         &to_dst);

    // ---- copy（offset 是本函数的全部意义所在）----
    VkBufferImageCopy region {};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;  // 0 = 紧凑打包；Vulkan 单位是 texel 不是字节
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {(int32_t)offset_x, (int32_t)offset_y, 0};
    region.imageExtent = {width, height, 1};
    vkCmdCopyBufferToImage(command_buffer, staging_buffer, image_handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // ---- barrier → SHADER_READ_ONLY ----
    VkImageMemoryBarrier to_read = to_dst;
    to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    to_read.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(command_buffer,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0,
                         0,
                         nullptr,
                         0,
                         nullptr,
                         1,
                         &to_read);

    vulkan_rhi->EndSingleTimeCommands(rhi_command_buffer);

    vmaDestroyBuffer(allocator, staging_buffer, staging_allocation);
    return true;
#endif
}