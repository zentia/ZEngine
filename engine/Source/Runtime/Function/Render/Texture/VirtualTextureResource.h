#pragma once

#include "Runtime/Function/Render/RenderResourceBase.h"
#include "VirtualTextureTypes.h"

#include <atomic>
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

    // Bytes per texel for getFormat(). 0 means "unknown/unhandled format" -- the
    // GPU upload path refuses to touch a page it cannot size.
    uint32_t getBytesPerPixel() const;

    // Source pixels retained from the TextureData Initialize() overload.
    //
    // Without this there is nothing to upload: the old Initialize() copied out
    // only width/height/format and dropped the pixels, so "load page data from
    // disk" had no source to read from. Keeping the shared_ptr alive is what
    // makes the GPU upload verifiable -- the A/B test can compare what landed in
    // the physical pool against the original source texels.
    const std::shared_ptr<TextureData>& getSourceData() const { return m_SourceData; }

    // Extract one mip-0 page's pixels out of the retained source data.
    //
    // Pages at the right/bottom edge of the texture are partial: the source is
    // not necessarily an exact multiple of page_size. Those pages are padded
    // with the last valid row/column replicated? No -- they are padded with
    // zero bytes, and out_width/out_height report the REAL (unpadded) extent so
    // the upload copies only real texels into the slot. Padding the tail keeps
    // the staging buffer exactly page_size*page_size*bytes_per_pixel.
    bool ExtractPagePixels(uint32_t page_x,
                           uint32_t page_y,
                           std::vector<uint8_t>& out_pixels,
                           uint32_t& out_width,
                           uint32_t& out_height) const;

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

    // Update page table texture: unconditionally (re)pack the CPU page table and
    // upload it. Creates the texture on first call. Clears the dirty flag.
    //
    // Texel layout (R32G32_SFLOAT, one texel per mip-0 page, row-major):
    //   .x = physical slot index (>= 0) if resident, kPageTableNotResident if not
    //   .y = mip level
    // The "not resident" sentinel is -1.0f, NOT float(UINT32_MAX): that rounds to
    // 2^32, and converting it back to uint32 is undefined behaviour -- a shader
    // (or test) can only safely test "x < 0".
    void UpdatePageTableTexture(RHI* rhi);

    static constexpr float kPageTableNotResident = -1.0f;

    // ---- Page-table texture lifecycle (the two interfaces that were missing) ----
    //
    // (1) Invalidation sync. SetPageTableEntry marks the table dirty; nothing
    //     uploads per page any more. FlushPageTableTexture uploads only if dirty,
    //     so a batch (ForceLoadPages / UnloadPages / one Update() frame / an
    //     eviction that hits ANOTHER virtual texture) costs exactly one upload.
    //     Before this, UnloadPage cleared the CPU entry and never touched the GPU
    //     table -- the GPU kept saying "resident in slot N" after slot N had been
    //     handed to a different page, i.e. it sampled someone else's pixels.
    //     Returns true if an upload happened.
    bool FlushPageTableTexture(RHI* rhi);
    bool isPageTableDirty() const { return m_PageTableDirty.load(); }

    // (2) Release. Destroys the image view, the VkImage + VMA allocation and both
    //     wrapper objects. Shutdown() calls it; the old Shutdown only nulled the
    //     pointers with a comment claiming "destroyed by manager" -- the manager
    //     never did, so every registered VT leaked its page table. Safe to call
    //     twice. After release the table is dirty again, so the next flush
    //     recreates it.
    void ReleasePageTableTexture();

    // Number of GPU page-table uploads so far (diagnostic; the A/B test uses it
    // to prove batching -- one upload per batch, zero for a clean table).
    uint32_t getPageTableUploadCount() const { return m_PageTableUploadCount; }

private:
    VirtualTextureHandle m_Handle;  // Virtual texture handle
    std::string m_TexturePath;      // Source texture path
    // Retained source pixels (TextureData Initialize() overload only). See
    // getSourceData(). Null when the resource was built from a path/size pair --
    // in that case there is genuinely nothing on the CPU to upload yet.
    std::shared_ptr<TextureData> m_SourceData;
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
    // Same discard_prior_content guard as PhysicalTexturePool::m_HasContent: the
    // page table is re-uploaded every time any page's mapping changes, so only
    // the very first upload may treat the image as UNDEFINED.
    bool m_PageTableHasContent;
    // Set by SetPageTableEntry, cleared by a successful upload. Atomic because
    // entries are written from the page-loading thread as well.
    //
    // Starts FALSE on purpose (lazy creation): a VT that has never had a resident
    // page has no stale GPU state to sync, so FlushPageTables must not allocate a
    // page table for it just because some OTHER texture triggered a flush. Until
    // the first residency change GetPageTableTexture() returns null -- consumers
    // treat null as "nothing resident".
    std::atomic<bool> m_PageTableDirty {false};
    uint32_t m_PageTableUploadCount {0};
    // The RHI that created the page table texture; Release needs it (Shutdown()
    // takes no RHI parameter).
    RHI* m_PageTableRhi {nullptr};

    // Initialize pages
    void InitializePages();

    // Generate next handle
    static VirtualTextureHandle s_NextHandle;
};