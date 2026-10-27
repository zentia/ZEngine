#include "VirtualTextureResource.h"

#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Function/Render/Interface/RHI.h"
#include "Runtime/Function/Render/Interface/Vulkan/VulkanRHI.h"
#include "Runtime/Function/Render/Interface/Vulkan/VulkanRHIResource.h"

#include <algorithm>
#include <cmath>
#include <cstring>

VirtualTextureHandle VirtualTextureResource::s_NextHandle = 1;

VirtualTextureResource::VirtualTextureResource()
    : m_Handle(0), m_VirtualWidth(0), m_VirtualHeight(0), m_PageSize(128), m_PagesX(0), m_PagesY(0),
      m_MipLevels(0), m_Format(RHIFormat::RHI_FORMAT_R8G8B8A8_UNORM), m_IsSrgb(false), m_IsInitialized(false),
      m_PageTableTexture(nullptr), m_PageTableTextureView(nullptr), m_PageTableAllocation(nullptr),
      m_PageTableHasContent(false)
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

    // Retain the source pixels. This is what makes the GPU upload path possible
    // at all: without it the only thing left on the CPU is width/height/format
    // and "extract page region from source" has nothing to read.
    m_SourceData = texture_data;

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

uint32_t VirtualTextureResource::getBytesPerPixel() const
{
    // 只认 VT 自己会产出的那两种格式。其余返回 0 —— GPU 上传路径拒绝碰一个自己
    // 量不出大小的页（比按猜的 stride 拷出一堆错位像素安全）。
    switch (m_Format)
    {
        case RHIFormat::RHI_FORMAT_R8G8B8A8_UNORM:
        case RHIFormat::RHI_FORMAT_R8G8B8A8_SRGB:
            return 4;
        default:
            return 0;
    }
}

bool VirtualTextureResource::ExtractPagePixels(uint32_t page_x,
                                               uint32_t page_y,
                                               std::vector<uint8_t>& out_pixels,
                                               uint32_t& out_width,
                                               uint32_t& out_height) const
{
    out_width = 0;
    out_height = 0;

    if (!m_SourceData || !m_SourceData->IsValid() || !m_IsInitialized)
    {
        return false;
    }

    const uint32_t bytes_per_pixel = getBytesPerPixel();
    if (bytes_per_pixel == 0 || page_x >= m_PagesX || page_y >= m_PagesY)
    {
        return false;
    }

    const uint32_t origin_x = page_x * m_PageSize;
    const uint32_t origin_y = page_y * m_PageSize;
    if (origin_x >= m_VirtualWidth || origin_y >= m_VirtualHeight)
    {
        return false;
    }

    // 边缘页是残缺的（源尺寸不一定是 page_size 的整数倍）：只拷真实存在的那部分，
    // 尾部补 0，把真实尺寸报回去让上传只拷真实 texel。
    const uint32_t copy_width = std::min(m_PageSize, m_VirtualWidth - origin_x);
    const uint32_t copy_height = std::min(m_PageSize, m_VirtualHeight - origin_y);

    out_pixels.assign(static_cast<size_t>(m_PageSize) * static_cast<size_t>(m_PageSize) * bytes_per_pixel, 0);

    const size_t source_stride = static_cast<size_t>(m_VirtualWidth) * bytes_per_pixel;
    const size_t dest_stride = static_cast<size_t>(m_PageSize) * bytes_per_pixel;
    const uint8_t* source = m_SourceData->m_Pixels;

    for (uint32_t row = 0; row < copy_height; ++row)
    {
        const uint8_t* source_row =
            source + static_cast<size_t>(origin_y + row) * source_stride + static_cast<size_t>(origin_x) * bytes_per_pixel;
        uint8_t* dest_row = out_pixels.data() + static_cast<size_t>(row) * dest_stride;
        memcpy(dest_row, source_row, static_cast<size_t>(copy_width) * bytes_per_pixel);
    }

    out_width = copy_width;
    out_height = copy_height;
    return true;
}

void VirtualTextureResource::UpdatePageTableTexture(RHI* rhi)
{
    if (rhi == nullptr || !m_IsInitialized || m_PagesX == 0 || m_PagesY == 0)
    {
        return;
    }

#if defined(__APPLE__) || defined(__EMSCRIPTEN__)
    return;
#else
    // 格式选 R32G32_SFLOAT 而不是注释里写的 R32G32_UINT —— 本工程 RHIFormat 枚举
    // 里没有 R32G32_UINT（只有 SFLOAT 那几个），为一个页表去动 4 个后端的格式表不值
    // 当。槽位下标是小整数，f32 能精确表示（< 2^24），语义无损。
    const RHIFormat table_format = RHIFormat::RHI_FORMAT_R32G32_SFLOAT;
    constexpr uint32_t kTableBytesPerTexel = 8;  // 2 x float32

    auto vulkan_rhi = static_cast<VulkanRHI*>(rhi);
    if (!vulkan_rhi)
    {
        return;
    }

    VmaAllocator allocator = vulkan_rhi->getVmaAllocator();
    if (!allocator)
    {
        return;
    }

    // ---- 首次调用时建图（mip 0 的页网格，一 texel 一个 PageTableEntry）----
    if (m_PageTableTexture == nullptr)
    {
        VkImageCreateInfo image_info {};
        image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.extent.width = m_PagesX;
        image_info.extent.height = m_PagesY;
        image_info.extent.depth = 1;
        image_info.mipLevels = 1;
        image_info.arrayLayers = 1;
        image_info.format = VK_FORMAT_R32G32_SFLOAT;
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        // 同页池：TRANSFER_SRC 只为 A/B 验证（把页表拷回 host 逐 texel 比对）。
        image_info.usage =
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        image_info.samples = VK_SAMPLE_COUNT_1_BIT;
        image_info.flags = 0;

        VmaAllocationCreateInfo alloc_info = {};
        alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
        alloc_info.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;

        VkImage vk_image = VK_NULL_HANDLE;
        VmaAllocation vk_allocation = nullptr;
        VmaAllocationInfo alloc_info_result {};
        if (vmaCreateImage(allocator, &image_info, &alloc_info, &vk_image, &vk_allocation, &alloc_info_result) !=
            VK_SUCCESS)
        {
            return;
        }

        // 同 PhysicalTexturePool::Initialize：必须包一层 VulkanImage 对象，不能直接
        // 把 VkImage 句柄当 RHIImage* 用（RHI 侧会当对象解引用 → 访问违例）。
        m_PageTableTexture = new VulkanImage();
        ((VulkanImage*)m_PageTableTexture)->setResource(vk_image);
        m_PageTableAllocation = vk_allocation;

        rhi->CreateImageView(m_PageTableTexture,
                             table_format,
                             RHIImageAspectFlagBits::RHI_IMAGE_ASPECT_COLOR_BIT,
                             RHIImageViewType::RHI_IMAGE_VIEW_TYPE_2D,
                             1,
                             1,
                             m_PageTableTextureView);
    }

    // ---- 打包页表：行主序，每 texel = (physical_slot, mip_level) ----
    // 未驻留的项写 UINT32_MAX（f32 能表示），shader 侧拿它和槽位总数比就知道无效。
    std::vector<uint8_t> table_bytes(static_cast<size_t>(m_PagesX) * static_cast<size_t>(m_PagesY) *
                                     kTableBytesPerTexel);
    float* texels = reinterpret_cast<float*>(table_bytes.data());
    for (uint32_t page_y = 0; page_y < m_PagesY; ++page_y)
    {
        for (uint32_t page_x = 0; page_x < m_PagesX; ++page_x)
        {
            const PageTableEntry entry = GetPageTableEntry(0, page_x, page_y);
            const size_t index = (static_cast<size_t>(page_y) * m_PagesX + page_x) * 2;
            texels[index + 0] = static_cast<float>(entry.physical_slot);  // UINT32_MAX when invalid
            texels[index + 1] = static_cast<float>(entry.mip_level);
        }
    }

    // discard 只在"这张图还从没传过"时为 true；之后必须 false，否则 oldLayout=
    // UNDEFINED 会把整张页表作废。
    const bool discard = !m_PageTableHasContent;
    if (UploadImageRegion(rhi,
                          m_PageTableTexture,
                          table_bytes.data(),
                          table_bytes.size(),
                          0,
                          0,
                          m_PagesX,
                          m_PagesY,
                          kTableBytesPerTexel,
                          discard))
    {
        m_PageTableHasContent = true;
    }
#endif
}