#pragma once

#include "Runtime/Function/Render/RenderResourceBase.h"
#include "VirtualTextureTypes.h"

#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

class RHI;
class RenderResourceBase;

/// <summary>
/// Virtual Texture Resource
/// Represents a single virtual texture with its pages and page table
/// </summary>
class VirtualTextureResource
{
public:
    VirtualTextureResource();
    ~VirtualTextureResource();

    // Initialize virtual texture from file
    bool Initialize(const std::string& texture_path,
                    uint32_t virtual_width,
                    uint32_t virtual_height,
                    uint32_t page_size,
                    bool is_srgb = false);

    // Initialize virtual texture from existing texture data
    bool Initialize(std::shared_ptr<TextureData> texture_data, uint32_t page_size);

    // Shutdown virtual texture
    void Shutdown();

    // Get virtual texture handle
    VirtualTextureHandle getHandle() const { return m_Handle; }

    // Get virtual texture dimensions
    uint32_t getVirtualWidth() const { return m_VirtualWidth; }
    uint32_t getVirtualHeight() const { return m_VirtualHeight; }

    // Get page size
    uint32_t getPageSize() const { return m_PageSize; }

    // Get number of pages in X and Y directions
    uint32_t getPagesX() const { return m_PagesX; }
    uint32_t getPagesY() const { return m_PagesY; }

    // Get number of mip levels
    uint32_t getMipLevels() const { return m_MipLevels; }

    // Calculate number of pages for a mip level
    void CalculatePagesForMip(uint32_t mip_level, uint32_t& pages_x, uint32_t& pages_y) const;

    // Get page at specific coordinates and mip level
    VirtualTexturePage* GetPage(uint32_t mip_level, uint32_t page_x, uint32_t page_y);
    const VirtualTexturePage* GetPage(uint32_t mip_level, uint32_t page_x, uint32_t page_y) const;

    // Get page ID
    VirtualTexturePageID GetPageID(uint32_t mip_level, uint32_t page_x, uint32_t page_y) const;

    // Calculate required pages based on UV coordinates and mip level
    void CalculateRequiredPages(float u_min,
                                float u_max,
                                float v_min,
                                float v_max,
                                uint32_t mip_level,
                                std::vector<VirtualTexturePageID>& out_pages) const;

    // Calculate required mip level based on screen size and distance
    uint32_t CalculateRequiredMipLevel(float screen_size, float distance, float fov) const;

    // Get page table entry
    PageTableEntry GetPageTableEntry(uint32_t mip_level, uint32_t page_x, uint32_t page_y) const;

    // Set page table entry
    void SetPageTableEntry(uint32_t mip_level, uint32_t page_x, uint32_t page_y, const PageTableEntry& entry);

    // Update page state
    void UpdatePageState(const VirtualTexturePageID& page_id, VirtualTexturePageState state);

    // Update page priority
    void UpdatePagePriority(const VirtualTexturePageID& page_id, VirtualTexturePagePriority priority);

    // Get texture format
    RHIFormat getFormat() const { return m_Format; }

    // Get texture path
    const std::string& getPath() const { return m_TexturePath; }

    // Check if initialized
    bool isInitialized() const { return m_IsInitialized; }

    // Get all pages
    const std::unordered_map<VirtualTexturePageID, VirtualTexturePage, VirtualTexturePageIDHash>& getAllPages() const
    {
        return m_Pages;
    }

    // Get page table (for rendering)
    // Returns a 2D texture where each texel represents a page table entry
    // Format: R32G32_UINT (physical_slot, mip_level)
    RHIImage* GetPageTableTexture() const { return m_PageTableTexture; }
    RHIImageView* getPageTableTextureView() const { return m_PageTableTextureView; }

    // Update page table texture (call after modifying page table entries)
    void UpdatePageTableTexture(RHI* rhi);

private:
    VirtualTextureHandle m_Handle;  // Virtual texture handle
    std::string m_TexturePath;      // Source texture path
    uint32_t m_VirtualWidth;        // Virtual texture width
    uint32_t m_VirtualHeight;       // Virtual texture height
    uint32_t m_PageSize;            // Page size in pixels
    uint32_t m_PagesX;              // Number of pages in X direction (mip 0)
    uint32_t m_PagesY;              // Number of pages in Y direction (mip 0)
    uint32_t m_MipLevels;           // Number of mip levels
    RHIFormat m_Format;             // Texture format
    bool m_IsSrgb;                  // Is sRGB texture
    bool m_IsInitialized;           // Is initialized

    // Page storage
    std::unordered_map<VirtualTexturePageID, VirtualTexturePage, VirtualTexturePageIDHash> m_Pages;
    mutable std::mutex m_PagesMutex;

    // Page table (sparse, only stores loaded pages)
    std::unordered_map<VirtualTexturePageID, PageTableEntry, VirtualTexturePageIDHash> m_PageTable;
    mutable std::mutex m_PageTableMutex;

    // Page table texture (for GPU lookup)
    RHIImage* m_PageTableTexture;
    RHIImageView* m_PageTableTextureView;
    VmaAllocation m_PageTableAllocation;

    // Initialize pages
    void InitializePages();

    // Generate next handle
    static VirtualTextureHandle s_NextHandle;
};