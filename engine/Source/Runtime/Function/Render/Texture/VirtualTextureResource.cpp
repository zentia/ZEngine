#include "VirtualTextureResource.h"

#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Function/Render/Interface/RHI.h"
#include "Runtime/Function/Render/Interface/Vulkan/VulkanRHI.h"

#include <algorithm>
#include <cmath>

VirtualTextureHandle VirtualTextureResource::s_NextHandle = 1;

VirtualTextureResource::VirtualTextureResource()
    : m_Handle(0), m_VirtualWidth(0), m_VirtualHeight(0), m_PageSize(128), m_PagesX(0), m_PagesY(0),
      m_MipLevels(0), m_Format(RHIFormat::RHI_FORMAT_R8G8B8A8_UNORM), m_IsSrgb(false), m_IsInitialized(false),
      m_PageTableTexture(nullptr), m_PageTableTextureView(nullptr), m_PageTableAllocation(nullptr)
{
    m_Handle = s_NextHandle++;
}

VirtualTextureResource::~VirtualTextureResource()
{
    Shutdown();
}

bool VirtualTextureResource::Initialize(const std::string& texture_path,
                                        uint32_t virtual_width,
                                        uint32_t virtual_height,
                                        uint32_t page_size,
                                        bool is_srgb)
{
    if (m_IsInitialized)
    {
        Shutdown();
    }

    m_TexturePath = texture_path;
    m_VirtualWidth = virtual_width;
    m_VirtualHeight = virtual_height;
    m_PageSize = page_size;
    m_IsSrgb = is_srgb;

    // Calculate number of pages
    m_PagesX = (virtual_width + page_size - 1) / page_size;
    m_PagesY = (virtual_height + page_size - 1) / page_size;

    // Calculate number of mip levels
    uint32_t max_dim = std::max(virtual_width, virtual_height);
    m_MipLevels = static_cast<uint32_t>(std::floor(std::log2(max_dim))) + 1;

    // Set format
    m_Format = is_srgb ? RHIFormat::RHI_FORMAT_R8G8B8A8_SRGB : RHIFormat::RHI_FORMAT_R8G8B8A8_UNORM;

    // Initialize pages
    InitializePages();

    m_IsInitialized = true;
    return true;
}

bool VirtualTextureResource::Initialize(std::shared_ptr<TextureData> texture_data, uint32_t page_size)
{
    if (!texture_data)
    {
        return false;
    }

    if (m_IsInitialized)
    {
        Shutdown();
    }

    m_VirtualWidth = texture_data->m_Width;
    m_VirtualHeight = texture_data->m_Height;
    m_PageSize = page_size;
    m_Format = texture_data->m_Format;
    m_IsSrgb = (m_Format == RHIFormat::RHI_FORMAT_R8G8B8A8_SRGB);

    // Calculate number of pages
    m_PagesX = (m_VirtualWidth + page_size - 1) / page_size;
    m_PagesY = (m_VirtualHeight + page_size - 1) / page_size;

    // Calculate number of mip levels
    uint32_t max_dim = std::max(m_VirtualWidth, m_VirtualHeight);
    m_MipLevels = static_cast<uint32_t>(std::floor(std::log2(max_dim))) + 1;

    // Initialize pages
    InitializePages();

    m_IsInitialized = true;
    return true;
}

void VirtualTextureResource::Shutdown()
{
    if (m_PageTableTextureView)
    {
        m_PageTableTextureView = nullptr;
    }

    if (m_PageTableTexture && m_PageTableAllocation)
    {
        // Page table texture will be destroyed by manager
        m_PageTableTexture = nullptr;
        m_PageTableAllocation = nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(m_PagesMutex);
        m_Pages.clear();
    }

    {
        std::lock_guard<std::mutex> lock(m_PageTableMutex);
        m_PageTable.clear();
    }

    m_IsInitialized = false;
}

void VirtualTextureResource::InitializePages()
{
    std::lock_guard<std::mutex> lock(m_PagesMutex);

    for (uint32_t mip = 0; mip < m_MipLevels; ++mip)
    {
        uint32_t pages_x, pages_y;
        CalculatePagesForMip(mip, pages_x, pages_y);

        for (uint32_t y = 0; y < pages_y; ++y)
        {
            for (uint32_t x = 0; x < pages_x; ++x)
            {
                VirtualTexturePageID page_id;
                page_id.virtual_texture_id = m_Handle;
                page_id.mip_level = mip;
                page_id.page_x = x;
                page_id.page_y = y;

                VirtualTexturePage page;
                page.id = page_id;
                page.state = VirtualTexturePageState::Unloaded;
                page.priority = VirtualTexturePagePriority::Normal;
                page.physical_slot = UINT32_MAX;

                m_Pages[page_id] = page;
            }
        }
    }
}

VirtualTexturePage* VirtualTextureResource::GetPage(uint32_t mip_level, uint32_t page_x, uint32_t page_y)
{
    VirtualTexturePageID page_id = GetPageID(mip_level, page_x, page_y);
    std::lock_guard<std::mutex> lock(m_PagesMutex);
    auto it = m_Pages.find(page_id);
    if (it != m_Pages.end())
    {
        return &it->second;
    }
    return nullptr;
}

const VirtualTexturePage* VirtualTextureResource::GetPage(uint32_t mip_level, uint32_t page_x, uint32_t page_y) const
{
    VirtualTexturePageID page_id = GetPageID(mip_level, page_x, page_y);
    std::lock_guard<std::mutex> lock(m_PagesMutex);
    auto it = m_Pages.find(page_id);
    if (it != m_Pages.end())
    {
        return &it->second;
    }
    return nullptr;
}

VirtualTexturePageID VirtualTextureResource::GetPageID(uint32_t mip_level, uint32_t page_x, uint32_t page_y) const
{
    VirtualTexturePageID page_id;
    page_id.virtual_texture_id = m_Handle;
    page_id.mip_level = mip_level;
    page_id.page_x = page_x;
    page_id.page_y = page_y;
    return page_id;
}

void VirtualTextureResource::CalculateRequiredPages(float u_min,
                                                    float u_max,
                                                    float v_min,
                                                    float v_max,
                                                    uint32_t mip_level,
                                                    std::vector<VirtualTexturePageID>& out_pages) const
{
    out_pages.clear();

    uint32_t pages_x, pages_y;
    CalculatePagesForMip(mip_level, pages_x, pages_y);

    // Convert UV to page coordinates
    uint32_t page_x_min = static_cast<uint32_t>(u_min * pages_x);
    uint32_t page_x_max = static_cast<uint32_t>(u_max * pages_x);
    uint32_t page_y_min = static_cast<uint32_t>(v_min * pages_y);
    uint32_t page_y_max = static_cast<uint32_t>(v_max * pages_y);

    // Clamp to valid range
    page_x_min = std::min(page_x_min, pages_x - 1);
    page_x_max = std::min(page_x_max, pages_x - 1);
    page_y_min = std::min(page_y_min, pages_y - 1);
    page_y_max = std::min(page_y_max, pages_y - 1);

    // Add all pages in range
    for (uint32_t y = page_y_min; y <= page_y_max; ++y)
    {
        for (uint32_t x = page_x_min; x <= page_x_max; ++x)
        {
            out_pages.push_back(GetPageID(mip_level, x, y));
        }
    }
}

uint32_t VirtualTextureResource::CalculateRequiredMipLevel(float screen_size, float distance, float fov) const
{
    // Simple mip level calculation based on screen size
    // More sophisticated calculations can be added later
    float mip_factor = screen_size;
    if (mip_factor < 0.01f)
    {
        return m_MipLevels - 1;  // Use lowest mip
    }
    else if (mip_factor > 1.0f)
    {
        return 0;  // Use highest mip
    }

    // Calculate mip level based on screen size
    float mip_level_float = -std::log2(mip_factor) + std::log2(1.0f);
    uint32_t mip_level =
        static_cast<uint32_t>(std::max(0.0f, std::min(static_cast<float>(m_MipLevels - 1), mip_level_float)));

    return mip_level;
}

PageTableEntry VirtualTextureResource::GetPageTableEntry(uint32_t mip_level, uint32_t page_x, uint32_t page_y) const
{
    VirtualTexturePageID page_id = GetPageID(mip_level, page_x, page_y);
    std::lock_guard<std::mutex> lock(m_PageTableMutex);
    auto it = m_PageTable.find(page_id);
    if (it != m_PageTable.end())
    {
        return it->second;
    }
    return PageTableEntry();  // Return invalid entry
}

void VirtualTextureResource::SetPageTableEntry(uint32_t mip_level,
                                               uint32_t page_x,
                                               uint32_t page_y,
                                               const PageTableEntry& entry)
{
    VirtualTexturePageID page_id = GetPageID(mip_level, page_x, page_y);
    std::lock_guard<std::mutex> lock(m_PageTableMutex);
    m_PageTable[page_id] = entry;
}

void VirtualTextureResource::UpdatePageState(const VirtualTexturePageID& page_id, VirtualTexturePageState state)
{
    std::lock_guard<std::mutex> lock(m_PagesMutex);
    auto it = m_Pages.find(page_id);
    if (it != m_Pages.end())
    {
        it->second.state = state;
    }
}

void VirtualTextureResource::UpdatePagePriority(const VirtualTexturePageID& page_id,
                                                VirtualTexturePagePriority priority)
{
    std::lock_guard<std::mutex> lock(m_PagesMutex);
    auto it = m_Pages.find(page_id);
    if (it != m_Pages.end())
    {
        it->second.priority = priority;
    }
}

void VirtualTextureResource::CalculatePagesForMip(uint32_t mip_level, uint32_t& pages_x, uint32_t& pages_y) const
{
    uint32_t mip_width = m_VirtualWidth >> mip_level;
    uint32_t mip_height = m_VirtualHeight >> mip_level;
    pages_x = (mip_width + m_PageSize - 1) / m_PageSize;
    pages_y = (mip_height + m_PageSize - 1) / m_PageSize;
}

void VirtualTextureResource::UpdatePageTableTexture(RHI* rhi)
{
    // TODO: Implement page table texture update
    // This would create/update a 2D texture where each texel represents a page table entry
    // Format: R32G32_UINT (physical_slot, mip_level)
    // For now, this is a placeholder
}