#pragma once

#include "Runtime/Core/Base/Hash.h"
#include "Runtime/Function/Render/Interface/RHIStruct.h"
#include "Runtime/Function/Render/RenderType.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#if defined(__APPLE__) || defined(__EMSCRIPTEN__)
using VmaAllocation = void*;
#else
    #include <vma/vk_mem_alloc.h>
#endif

// Forward declarations
class RHI;
class VirtualTextureManager;
class VirtualTextureResource;

/// <summary>
/// Virtual texture page state
/// </summary>
enum class VirtualTexturePageState : uint8_t
{
    Unloaded = 0,  // Page is not loaded
    Loading,       // Page is being loaded
    Loaded,        // Page is loaded in physical pool
    Unloading,     // Page is being unloaded
    Error          // Error occurred during loading
};

/// <summary>
/// Virtual texture page priority
/// </summary>
enum class VirtualTexturePagePriority : uint8_t
{
    Low = 0,
    Normal,
    High,
    Critical
};

/// <summary>
/// Virtual texture page identifier
/// </summary>
struct VirtualTexturePageID
{
    uint32_t virtual_texture_id;  // Virtual texture resource ID
    uint32_t mip_level;           // Mip level (0 = highest resolution)
    uint32_t page_x;              // Page X coordinate
    uint32_t page_y;              // Page Y coordinate

    bool operator==(const VirtualTexturePageID& other) const
    {
        return virtual_texture_id == other.virtual_texture_id && mip_level == other.mip_level &&
               page_x == other.page_x && page_y == other.page_y;
    }

    size_t hash() const
    {
        size_t h = 0;
        hash_combine(h, virtual_texture_id);
        hash_combine(h, mip_level);
        hash_combine(h, page_x);
        hash_combine(h, page_y);
        return h;
    }
};

/// <summary>
/// Hash function for VirtualTexturePageID
/// </summary>
struct VirtualTexturePageIDHash
{
    size_t operator()(const VirtualTexturePageID& id) const { return id.hash(); }
};

/// <summary>
/// Virtual texture page
/// Represents a single page in a virtual texture
/// </summary>
struct VirtualTexturePage
{
    VirtualTexturePageID id;              // Page identifier
    VirtualTexturePageState state;        // Current state
    VirtualTexturePagePriority priority;  // Loading priority
    uint32_t physical_slot;               // Physical slot index in pool (if loaded)
    uint64_t last_access_time;            // Last access time (for LRU)
    float screen_size;                    // Screen space size
    float distance;                       // Distance from camera
    bool is_visible;                      // Is page currently visible

    VirtualTexturePage()
        : state(VirtualTexturePageState::Unloaded), priority(VirtualTexturePagePriority::Normal),
          physical_slot(UINT32_MAX), last_access_time(0), screen_size(0.0f), distance(0.0f), is_visible(false)
    {
    }
};

/// <summary>
/// Physical texture slot in the pool
/// </summary>
struct PhysicalTextureSlot
{
    VirtualTexturePageID page_id;  // Page using this slot
    bool is_allocated;             // Is slot allocated
    uint64_t last_access_time;     // Last access time (for LRU)
    uint32_t x;                    // Slot X position in pool
    uint32_t y;                    // Slot Y position in pool

    PhysicalTextureSlot()
        : is_allocated(false), last_access_time(0), x(0), y(0) {}
};

/// <summary>
/// Page table entry
/// Maps virtual page to physical slot
/// </summary>
struct PageTableEntry
{
    uint32_t physical_slot;  // Physical slot index
    uint32_t mip_level;      // Mip level
    bool is_valid;           // Is entry valid

    PageTableEntry()
        : physical_slot(UINT32_MAX), mip_level(0), is_valid(false) {}
};

/// <summary>
/// Virtual texture resource handle
/// </summary>
using VirtualTextureHandle = uint64_t;

/// <summary>
/// Virtual texture configuration
/// </summary>
struct VirtualTextureConfig
{
    uint32_t page_size;              // Page size in pixels (default: 128)
    uint32_t physical_pool_width;    // Physical pool width in pages (default: 2048)
    uint32_t physical_pool_height;   // Physical pool height in pages (default: 2048)
    size_t memory_budget_mb;         // Memory budget in MB (default: 512)
    uint32_t max_concurrent_loads;   // Maximum concurrent page loads (default: 4)
    float min_screen_size;           // Minimum screen size to load page (default: 0.01)
    float max_distance;              // Maximum distance to load page (default: 2000.0)
    bool enable_async_loading;       // Enable asynchronous loading (default: true)
    bool enable_mip_streaming;       // Enable mip level streaming (default: true)
    RHIFormat physical_pool_format;  // Physical pool format

    VirtualTextureConfig()
        : page_size(128), physical_pool_width(2048), physical_pool_height(2048), memory_budget_mb(512),
          max_concurrent_loads(4), min_screen_size(0.01f), max_distance(2000.0f), enable_async_loading(true),
          enable_mip_streaming(true), physical_pool_format(RHIFormat::RHI_FORMAT_R8G8B8A8_UNORM)
    {
    }
};

/// <summary>
/// Virtual texture statistics
/// </summary>
struct VirtualTextureStats
{
    uint32_t total_virtual_textures;     // Total number of virtual textures
    uint32_t total_pages;                // Total number of pages
    uint32_t loaded_pages;               // Number of loaded pages
    uint32_t loading_pages;              // Number of pages being loaded
    uint32_t unloaded_pages;             // Number of unloaded pages
    size_t total_memory_used;            // Total GPU memory used (bytes)
    size_t memory_budget;                // Memory budget (bytes)
    float memory_usage_ratio;            // Memory usage ratio (0.0 - 1.0)
    uint32_t physical_pool_used_slots;   // Number of used physical slots
    uint32_t physical_pool_total_slots;  // Total number of physical slots
};

/// <summary>
/// Page load request
/// </summary>
struct PageLoadRequest
{
    VirtualTexturePageID page_id;         // Page to load
    VirtualTexturePagePriority priority;  // Loading priority
    float screen_size;                    // Screen space size
    float distance;                       // Distance from camera
    bool is_visible;                      // Is page visible
    uint64_t request_id;                  // Unique request ID

    PageLoadRequest()
        : priority(VirtualTexturePagePriority::Normal), screen_size(0.0f), distance(0.0f), is_visible(false),
          request_id(0)
    {
    }
};

/// <summary>
/// Physical texture pool
/// Manages the physical texture that stores loaded pages
/// </summary>
class PhysicalTexturePool
{
public:
    PhysicalTexturePool();
    ~PhysicalTexturePool();

    // Initialize the physical texture pool
    bool Initialize(RHI* rhi,
                    uint32_t pool_width_pages,
                    uint32_t pool_height_pages,
                    uint32_t page_size,
                    RHIFormat format);

    // Shutdown the pool
    void Shutdown();

    // Allocate a physical slot for a page
    uint32_t AllocateSlot(const VirtualTexturePageID& page_id);

    // Free a physical slot
    void FreeSlot(uint32_t slot_index);

    // Get slot coordinates from index
    void GetSlotCoordinates(uint32_t slot_index, uint32_t& x, uint32_t& y) const;

    // Get slot index from coordinates
    uint32_t GetSlotIndex(uint32_t x, uint32_t y) const;

    // Update slot access time (for LRU)
    void UpdateSlotAccessTime(uint32_t slot_index, uint64_t time);

    // Find least recently used slot
    uint32_t FindLRUSlot() const;

    // Get physical texture resources
    RHIImage* getPhysicalTexture() const { return m_PhysicalTexture; }
    RHIImageView* getPhysicalTextureView() const { return m_PhysicalTextureView; }
    VmaAllocation getAllocation() const { return m_Allocation; }

    // Get pool dimensions
    uint32_t getPoolWidthPages() const { return m_PoolWidthPages; }
    uint32_t getPoolHeightPages() const { return m_PoolHeightPages; }
    uint32_t getPageSize() const { return m_PageSize; }
    uint32_t getTotalSlots() const { return m_PoolWidthPages * m_PoolHeightPages; }

    // Get slot information
    const PhysicalTextureSlot& GetSlot(uint32_t slot_index) const;
    PhysicalTextureSlot& GetSlot(uint32_t slot_index);

    // Check if slot is allocated
    bool IsSlotAllocated(uint32_t slot_index) const;

private:
    RHI* m_Rhi;
    RHIImage* m_PhysicalTexture;
    RHIImageView* m_PhysicalTextureView;
    VmaAllocation m_Allocation;

    uint32_t m_PoolWidthPages;   // Pool width in pages
    uint32_t m_PoolHeightPages;  // Pool height in pages
    uint32_t m_PageSize;         // Page size in pixels
    RHIFormat m_Format;          // Texture format

    std::vector<PhysicalTextureSlot> m_Slots;  // Physical slots
    mutable std::mutex m_SlotMutex;            // Mutex for slot access
};