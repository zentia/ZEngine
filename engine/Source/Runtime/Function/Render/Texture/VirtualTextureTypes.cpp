#include "VirtualTextureTypes.h"

#include "Runtime/Function/Render/Interface/RHI.h"
#if defined(__APPLE__)
    #include <TargetConditionals.h>
#endif
#if !defined(__APPLE__)
    #include "Runtime/Function/Render/Interface/Vulkan/VulkanRHI.h"
#endif

#include <algorithm>
#include <cstdint>
#include <mutex>

//////////////////////////////////////////////////////////////////////////
// PhysicalTexturePool Implementation
//////////////////////////////////////////////////////////////////////////

PhysicalTexturePool::PhysicalTexturePool()
    : m_Rhi(nullptr), m_PhysicalTexture(nullptr), m_PhysicalTextureView(nullptr), m_Allocation(nullptr),
      m_PoolWidthPages(0), m_PoolHeightPages(0), m_PageSize(0), m_Format(RHIFormat::RHI_FORMAT_R8G8B8A8_UNORM)
{
}

PhysicalTexturePool::~PhysicalTexturePool()
{
    Shutdown();
}

bool PhysicalTexturePool::Initialize(std::shared_ptr<RHI> rhi,
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

#if defined(__APPLE__)
    return false;
#else
    // Calculate physical texture dimensions
    uint32_t physical_width = pool_width_pages * page_size;
    uint32_t physical_height = pool_height_pages * page_size;

    // Create physical texture
    auto vulkan_rhi = std::static_pointer_cast<VulkanRHI>(rhi);
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
    image_create_info.extent.width = physical_width;
    image_create_info.extent.height = physical_height;
    image_create_info.extent.depth = 1;
    image_create_info.mipLevels = 1;
    image_create_info.arrayLayers = 1;
    image_create_info.format = format;
    image_create_info.tiling = RHIImageTiling::RHI_IMAGE_TILING_OPTIMAL;
    image_create_info.initialLayout = RHIImageLayout::RHI_IMAGE_LAYOUT_UNDEFINED;
    image_create_info.usage =
        RHIImageUsageFlagBits::RHI_IMAGE_USAGE_SAMPLED_BIT | RHIImageUsageFlagBits::RHI_IMAGE_USAGE_TRANSFER_DST_BIT;
    image_create_info.samples = RHISampleCountFlagBits::RHI_SAMPLE_COUNT_1_BIT;
    image_create_info.flags = 0;

    VmaAllocationCreateInfo alloc_info = {};
    alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
    alloc_info.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;

    VkImageCreateInfo vk_image_info = {};
    vk_image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    vk_image_info.imageType = VK_IMAGE_TYPE_2D;
    vk_image_info.extent.width = physical_width;
    vk_image_info.extent.height = physical_height;
    vk_image_info.extent.depth = 1;
    vk_image_info.mipLevels = 1;
    vk_image_info.arrayLayers = 1;
    vk_image_info.format = (VkFormat)format;
    vk_image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    vk_image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    vk_image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
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

    m_PhysicalTexture = (RHIImage*)vk_image;
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

    // Initialize slots
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
#endif
}

void PhysicalTexturePool::Shutdown()
{
    if (m_PhysicalTextureView)
    {
        // Image view will be destroyed by RHI
        m_PhysicalTextureView = nullptr;
    }

    if (m_PhysicalTexture && m_Allocation && m_Rhi)
    {
#if defined(__APPLE__)
        m_PhysicalTexture = nullptr;
        m_Allocation = nullptr;
#else
        auto vulkan_rhi = std::static_pointer_cast<VulkanRHI>(m_Rhi);
        if (vulkan_rhi)
        {
            VmaAllocator allocator = vulkan_rhi->getVmaAllocator();
            if (allocator)
            {
                vmaDestroyImage(allocator, (VkImage)m_PhysicalTexture, m_Allocation);
            }
        }
        m_PhysicalTexture = nullptr;
        m_Allocation = nullptr;
#endif
    }

    m_Slots.clear();
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

    // No free slot, use LRU
    uint32_t lru_slot = FindLRUSlot();
    if (lru_slot != UINT32_MAX)
    {
        m_Slots[lru_slot].is_allocated = true;
        m_Slots[lru_slot].page_id = page_id;
        m_Slots[lru_slot].last_access_time = 0;
        return lru_slot;
    }

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