#include "VirtualTextureResource.h"

#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Function/Render/Interface/RHI.h"
#if ZENGINE_VT_GPU
    #include "Runtime/Function/Render/Interface/Vulkan/VulkanRHI.h"
    #include "Runtime/Function/Render/Interface/Vulkan/VulkanRHIResource.h"
#endif

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

    // 全新的页表全是"不驻留"，没有要同步给 GPU 的东西（见 m_PageTableDirty 注释）。
    // 重复 Initialize 会先 Shutdown→Release 把它置脏，这里归零。
    m_PageTableDirty.store(false);

    m_IsInitialized = true;
    return true;
}

bool VirtualTextureResource::Initialize(std::shared_ptr<TextureData> texture_data,
                                        uint32_t page_size,
                                        uint32_t page_border,
                                        bool build_mip_chain)
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

    // ---- sampling contract v2: gutter + mip chain ----
    m_PageBorder = page_border;
    m_SampledMipCount = 1;
    m_HasMipChain = false;
    m_MipChain.clear();
    if (build_mip_chain && getBytesPerPixel() == 4 && texture_data->m_Pixels != nullptr)
    {
        m_SampledMipCount = std::min(VTRef::SampledMipCount(m_VirtualWidth, m_VirtualHeight, page_size), m_MipLevels);
        if (m_SampledMipCount > 1)
        {
            std::vector<VTRef::Image> chain = VTRef::BuildMipChain(
                texture_data->m_Pixels, m_VirtualWidth, m_VirtualHeight, m_SampledMipCount);
            chain.erase(chain.begin());  // mip 0 stays m_SourceData: no second copy of the biggest level
            m_MipChain = std::move(chain);
            m_HasMipChain = true;
        }
    }

    // Initialize pages
    InitializePages();

    // 全新的页表全是"不驻留"，没有要同步给 GPU 的东西（见 m_PageTableDirty 注释）。
    // 重复 Initialize 会先 Shutdown→Release 把它置脏，这里归零。
    m_PageTableDirty.store(false);

    m_IsInitialized = true;
    return true;
}

void VirtualTextureResource::Shutdown()
{
    // 旧实现只把指针置空、注释说"由 manager 销毁"——manager 从没销毁过，每个注册过
    // 的 VT 都泄漏一张页表纹理（VkImage + VMA 分配 + VkImageView + 两个包装对象）。
    ReleasePageTableTexture();

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
    // GPU 页表只覆盖 mip 0 网格（见 UpdatePageTableTexture），但标脏不区分 mip：
    // 多传一次的代价远小于漏传一次（漏传 = 采到别人的像素）。
    m_PageTableDirty.store(true);
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
    // A mip level always has at least one texel per axis (a 4096x16 texture's mip 5 is 128x1, not 128x0).
    uint32_t mip_width = VTRef::MipDim(m_VirtualWidth, mip_level);
    uint32_t mip_height = VTRef::MipDim(m_VirtualHeight, mip_level);
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
    return ExtractPagePixels(0u, page_x, page_y, out_pixels, out_width, out_height);
}

const uint8_t* VirtualTextureResource::getMipPixels(uint32_t mip, uint32_t& width, uint32_t& height) const
{
    width = 0;
    height = 0;
    if (!m_SourceData || mip >= m_SampledMipCount)
    {
        return nullptr;
    }
    if (mip == 0)
    {
        width = m_VirtualWidth;
        height = m_VirtualHeight;
        return m_SourceData->m_Pixels;
    }
    if (mip - 1 >= m_MipChain.size())
    {
        return nullptr;
    }
    width = m_MipChain[mip - 1].w;
    height = m_MipChain[mip - 1].h;
    return m_MipChain[mip - 1].rgba.data();
}

uint32_t VirtualTextureResource::getPageTableHeight() const
{
    return VTRef::AtlasHeight(m_VirtualWidth, m_VirtualHeight, m_PageSize, m_SampledMipCount);
}

uint32_t VirtualTextureResource::getMipRowOffset(uint32_t mip) const
{
    return VTRef::MipRowOffset(m_VirtualWidth, m_VirtualHeight, m_PageSize, mip);
}

bool VirtualTextureResource::BuildShaderParams(const PhysicalTexturePool& pool, VirtualTextureShaderParams& out) const
{
    out = {};
    if (!m_IsInitialized || pool.getPageSize() != m_PageSize || pool.getPageBorder() != m_PageBorder)
    {
        return false;
    }
    out.lookup[0] = pool.getPageSize();
    out.lookup[1] = pool.getPoolWidthPages();
    out.lookup[2] = m_PagesX;
    out.lookup[3] = m_PagesY;
    out.extent[0] = m_VirtualWidth;
    out.extent[1] = m_VirtualHeight;
    out.v2[0] = m_PageBorder;
    out.v2[1] = m_SampledMipCount;
    return true;
}

bool VirtualTextureResource::ExtractPagePixels(uint32_t mip,
                                               uint32_t page_x,
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
    if (bytes_per_pixel == 0)
    {
        return false;
    }

    uint32_t mip_width = 0, mip_height = 0;
    const uint8_t* source = getMipPixels(mip, mip_width, mip_height);
    if (source == nullptr || mip_width == 0 || mip_height == 0)
    {
        return false;
    }
    uint32_t pages_x = 0, pages_y = 0;
    CalculatePagesForMip(mip, pages_x, pages_y);
    if (page_x >= pages_x || page_y >= pages_y)
    {
        return false;
    }

    const uint32_t origin_x = page_x * m_PageSize;
    const uint32_t origin_y = page_y * m_PageSize;
    if (origin_x >= mip_width || origin_y >= mip_height)
    {
        return false;
    }

    // 边缘页是残缺的（mip 尺寸不一定是 page_size 的整数倍）：真实尺寸报回去，缓冲照整块大小给。
    const uint32_t copy_width = std::min(m_PageSize, mip_width - origin_x);
    const uint32_t copy_height = std::min(m_PageSize, mip_height - origin_y);

    const uint32_t border = m_PageBorder;
    const uint32_t block = m_PageSize + 2u * border;
    out_pixels.assign(static_cast<size_t>(block) * static_cast<size_t>(block) * bytes_per_pixel, 0);

    const size_t source_stride = static_cast<size_t>(mip_width) * bytes_per_pixel;
    if (border == 0)
    {
        // 原始布局：只拷真实存在的那部分，尾部补 0（槽位 == 页，尾巴永远采不到）。
        const size_t dest_stride = static_cast<size_t>(block) * bytes_per_pixel;
        for (uint32_t row = 0; row < copy_height; ++row)
        {
            const uint8_t* source_row = source + static_cast<size_t>(origin_y + row) * source_stride +
                                        static_cast<size_t>(origin_x) * bytes_per_pixel;
            std::memcpy(out_pixels.data() + static_cast<size_t>(row) * dest_stride,
                        source_row,
                        static_cast<size_t>(copy_width) * bytes_per_pixel);
        }
    }
    else
    {
        // v2：块里每个 texel（页内、gutter、残缺页的尾部）都等于 mip 图像上 clamp-to-edge 后的那个 texel。
        // 这正是 clamp 寻址的双线性采样在 mip 图像边缘会看到的东西，所以 gutter 对相邻页是"真邻居"，
        // 对图像边缘是"复制边缘"。
        for (uint32_t y = 0; y < block; ++y)
        {
            const int64_t sy = std::clamp<int64_t>(static_cast<int64_t>(origin_y) + y - border, 0, mip_height - 1);
            for (uint32_t x = 0; x < block; ++x)
            {
                const int64_t sx = std::clamp<int64_t>(static_cast<int64_t>(origin_x) + x - border, 0, mip_width - 1);
                std::memcpy(out_pixels.data() + (static_cast<size_t>(y) * block + x) * bytes_per_pixel,
                            source + static_cast<size_t>(sy) * source_stride + static_cast<size_t>(sx) * bytes_per_pixel,
                            bytes_per_pixel);
            }
        }
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

#if !ZENGINE_VT_GPU
    return;
#else
    // 格式选 R32G32_SFLOAT 而不是注释里写的 R32G32_UINT —— 本工程 RHIFormat 枚举
    // 里没有 R32G32_UINT（只有 SFLOAT 那几个），为一个页表去动 4 个后端的格式表不值
    // 当。槽位下标是小整数，f32 能精确表示（< 2^24），语义无损。
    const RHIFormat table_format = RHIFormat::RHI_FORMAT_R32G32_SFLOAT;
    constexpr uint32_t kTableBytesPerTexel = 8;  // 2 x float32
    // 页表是个"图集"：宽 = mip 0 的 pages_x，高 = 各采样 mip 的 pages_y 之和，mip m 的页网格从第
    // getMipRowOffset(m) 行开始（理由见 VirtualTextureMath.h）。只有一个采样 mip 时与原来的 mip 0 网格完全一致。
    const uint32_t table_width = m_PagesX;
    const uint32_t table_height = getPageTableHeight();

    if (!VirtualTextureBackendSupported(rhi))
    {
        return;
    }
    const bool vulkan_path = VirtualTextureUsesVulkanPath(rhi);
    VmaAllocator allocator = nullptr;
    if (vulkan_path)
    {
        allocator = static_cast<VulkanRHI*>(rhi)->getVmaAllocator();
        if (!allocator)
        {
            return;
        }
    }

    // ---- 首次调用时建图（mip 0 的页网格，一 texel 一个 PageTableEntry）----
    if (m_PageTableTexture == nullptr && !vulkan_path)
    {
        // 非 Vulkan 后端（DX12）：通用 RHI 路径。
        void* generic_alloc = nullptr;
        if (!VirtualTextureCreateGenericImage(rhi,
                                              table_width,
                                              table_height,
                                              table_format,
                                              m_PageTableTexture,
                                              m_PageTableTextureView,
                                              generic_alloc,
                                              m_PageTableBindlessSlot))
        {
            return;
        }
        m_PageTableAllocation = static_cast<VmaAllocation>(generic_alloc);
        m_PageTableRhi = rhi;
        m_PageTableHasContent = false;
    }
    else if (m_PageTableTexture == nullptr)
    {
        VkImageCreateInfo image_info {};
        image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.extent.width = table_width;
        image_info.extent.height = table_height;
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
        m_PageTableRhi = rhi;
        m_PageTableHasContent = false;  // 新图是 UNDEFINED，首传必须 discard

        rhi->CreateImageView(m_PageTableTexture,
                             table_format,
                             RHIImageAspectFlagBits::RHI_IMAGE_ASPECT_COLOR_BIT,
                             RHIImageViewType::RHI_IMAGE_VIEW_TYPE_2D,
                             1,
                             1,
                             m_PageTableTextureView);
    }

    // ---- 打包页表：行主序，每 texel = (physical_slot, mip_level) ----
    // 未驻留写 kPageTableNotResident（-1）。**不能写 float(UINT32_MAX)**：它舍入成
    // 2^32，再转回 uint32 是未定义行为，shader/测试都只能安全地判 "x < 0"。
    // 判据只看 is_valid，不看 physical_slot：UnloadPage 写的无效项槽位是默认值，
    // 将来任何路径若写了"无效但槽位非 MAX"的项，也不能让 GPU 以为它驻留。
    std::vector<uint8_t> table_bytes(static_cast<size_t>(table_width) * static_cast<size_t>(table_height) *
                                     kTableBytesPerTexel);
    float* texels = reinterpret_cast<float*>(table_bytes.data());
    for (size_t i = 0; i < static_cast<size_t>(table_width) * table_height; ++i)
    {
        // 图集里没有页网格覆盖的 texel（较粗的 mip 比 mip 0 窄）：永远不会被 shader 读到，但也写成"不驻留"。
        texels[i * 2 + 0] = kPageTableNotResident;
        texels[i * 2 + 1] = 0.0f;
    }
    for (uint32_t mip = 0; mip < m_SampledMipCount; ++mip)
    {
        uint32_t mip_pages_x = 0, mip_pages_y = 0;
        CalculatePagesForMip(mip, mip_pages_x, mip_pages_y);
        const uint32_t row_offset = getMipRowOffset(mip);
        for (uint32_t page_y = 0; page_y < mip_pages_y; ++page_y)
        {
            for (uint32_t page_x = 0; page_x < mip_pages_x; ++page_x)
            {
                const PageTableEntry entry = GetPageTableEntry(mip, page_x, page_y);
                const size_t index = (static_cast<size_t>(row_offset + page_y) * table_width + page_x) * 2;
                const bool resident = entry.is_valid && entry.physical_slot != UINT32_MAX;
                texels[index + 0] = resident ? static_cast<float>(entry.physical_slot) : kPageTableNotResident;
                texels[index + 1] = static_cast<float>(entry.mip_level);
            }
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
                          table_width,
                          table_height,
                          kTableBytesPerTexel,
                          discard))
    {
        m_PageTableHasContent = true;
        m_PageTableDirty.store(false);
        ++m_PageTableUploadCount;
    }
#endif
}

bool VirtualTextureResource::FlushPageTableTexture(RHI* rhi)
{
    if (!m_PageTableDirty.load())
    {
        return false;
    }
    const uint32_t before = m_PageTableUploadCount;
    UpdatePageTableTexture(rhi);
    return m_PageTableUploadCount != before;
}

void VirtualTextureResource::ReleasePageTableTexture()
{
#if ZENGINE_VT_GPU
    // 调用前提：没有 GPU 工作还在读这张页表。VT 的所有提交都是 single-time
    // （EndSingleTimeCommands 内 vkQueueWaitIdle），所以在本系统内成立；将来若有
    // 渲染帧把它绑进描述符集，这里要改成延迟到帧围栏之后再销毁。
    if (m_PageTableRhi != nullptr && m_PageTableTexture != nullptr && !VirtualTextureUsesVulkanPath(m_PageTableRhi))
    {
        // 通用 RHI 路径（DX12）：一把销毁 view / 图 / 内存。
        VirtualTextureDestroyGenericImage(
            m_PageTableRhi, m_PageTableTexture, m_PageTableTextureView, m_PageTableAllocation, m_PageTableBindlessSlot);
        m_PageTableBindlessSlot = 0xFFFFFFFFu;
        m_PageTableTextureView = nullptr;
        m_PageTableTexture = nullptr;
        m_PageTableAllocation = nullptr;
    }

    if (m_PageTableTextureView != nullptr)
    {
        if (m_PageTableRhi != nullptr)
        {
            m_PageTableRhi->DestroyImageView(m_PageTableTextureView);
        }
        delete (VulkanImageView*)m_PageTableTextureView;  // VulkanRHI::CreateImageView new 的包装
    }

    if (m_PageTableTexture != nullptr)
    {
        auto vulkan_rhi = static_cast<VulkanRHI*>(m_PageTableRhi);
        VmaAllocator allocator = vulkan_rhi ? vulkan_rhi->getVmaAllocator() : nullptr;
        if (allocator != nullptr && m_PageTableAllocation != nullptr)
        {
            vmaDestroyImage(allocator, ((VulkanImage*)m_PageTableTexture)->getResource(), m_PageTableAllocation);
        }
        delete (VulkanImage*)m_PageTableTexture;
    }
#endif
    m_PageTableTextureView = nullptr;
    m_PageTableTexture = nullptr;
    m_PageTableAllocation = nullptr;
    m_PageTableRhi = nullptr;
    m_PageTableHasContent = false;
    m_PageTableDirty.store(true);  // 下次 Flush 会重建
}