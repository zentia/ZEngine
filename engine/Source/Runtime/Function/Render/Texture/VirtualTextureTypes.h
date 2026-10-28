#pragma once

#include "Runtime/Core/Base/Hash.h"
#include "Runtime/Core/Math/AxisAligned.h"
#include "Runtime/Function/Render/Interface/RHIStruct.h"
#include "Runtime/Function/Render/RenderType.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#if defined(Z_HAS_VULKAN)
    #include <vma/vk_mem_alloc.h>
#elif defined(__APPLE__) || defined(__EMSCRIPTEN__)
using VmaAllocation = void*;
#else
struct VmaAllocation_T;
typedef struct VmaAllocation_T* VmaAllocation;
#endif

// VT 的 GPU 路径（页池图 / 页表纹理 / 页上传）是 Vulkan 直连代码。Apple 走 Metal、
// Emscripten 走 WebGL2，这两处一律编译掉，只剩纯 CPU 簿记（InitializeLogical）。
//
// 必须用一个宏统一守卫，而不是各处散写 __APPLE__ / __EMSCRIPTEN__：WASM 冒烟测试
// （scripts/verify_vt_wasm.sh）用一个**影子 VulkanRHI.h** 只提供最小 Vk/vma 表面，
// 任何漏守卫的 Vulkan 符号（VulkanImage、VK_FORMAT_*、vmaCreateBuffer…）都会让它
// 编译失败——2026-10-27 第一版 GPU 上传就这样把 WASM 测试打红了而回归没跑到。
#if !defined(__APPLE__) && !defined(__EMSCRIPTEN__)
    #define ZENGINE_VT_GPU 1
#else
    #define ZENGINE_VT_GPU 0
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

    // World-space footprint captured at RequestPages time. The skeleton stored
    // NONE of these, which is why UpdatePagePriorities() had nothing to
    // recompute from (it was a TODO). Update() now reprojects this bounds to the
    // screen each frame using the live camera, so pages can be re-prioritized
    // and distance-gated without re-deriving world positions.
    Vector3 object_position;        // World position of the owning object
    AxisAlignedBox object_bounds;   // World-space bounds of the owning object

    VirtualTexturePage()
        : state(VirtualTexturePageState::Unloaded), priority(VirtualTexturePagePriority::Normal),
          physical_slot(UINT32_MAX), last_access_time(0), screen_size(0.0f), distance(0.0f), is_visible(false),
          object_position(0.0f, 0.0f, 0.0f), object_bounds()
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

    // Headless / logical initialization: set up the slot grid WITHOUT creating a
    // Vulkan image. Slot allocation / LRU / bookkeeping are pure CPU and work
    // identically; only the GPU texture itself is absent. Used by the WASM
    // verification harness (and any platform where VT has no real RHI yet) so the
    // streaming policy (load / evict / priority) can be exercised and asserted
    // against without a device. See VirtualTextureTypes.cpp.
    bool InitializeLogical(uint32_t pool_width_pages, uint32_t pool_height_pages, uint32_t page_size);

    // True when the pool was created via InitializeLogical (no GPU texture).
    bool isLogicalOnly() const { return m_LogicalOnly; }

    // Shutdown the pool
    void Shutdown();

    // Allocate a FREE physical slot for a page. Returns UINT32_MAX when the pool
    // is full -- it never steals an occupied slot.
    //
    // It used to fall back to "take the LRU slot" internally. That had two bugs:
    // (1) it called FindLRUSlot() while already holding m_SlotMutex, and
    //     FindLRUSlot() takes the same non-recursive mutex => self-deadlock the
    //     first time the pool filled up;
    // (2) even without the deadlock it re-assigned the slot WITHOUT unloading the
    //     previous owner, so the old page kept state=Loaded + the same
    //     physical_slot and its page table kept pointing at pixels that now
    //     belong to someone else (silent aliasing).
    // Eviction is the manager's job: VirtualTextureManager::LoadPage evicts the
    // LRU page through UnloadPage (which invalidates the owner's page table) and
    // retries.
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

    // Page-upload bookkeeping for UploadImageRegion's discard_prior_content flag.
    //
    // The pool image starts UNDEFINED. The FIRST page upload may pass
    // discard=true (there is nothing to lose), but every upload after that MUST
    // pass false -- otherwise the barrier's oldLayout=UNDEFINED silently
    // invalidates every page already resident in the pool. Callers check
    // hasContent() before uploading and call markContentUploaded() after a
    // successful upload.
    bool hasContent() const { return m_HasContent; }
    void markContentUploaded() { m_HasContent = true; }

private:
    RHI* m_Rhi;
    RHIImage* m_PhysicalTexture;
    RHIImageView* m_PhysicalTextureView;
    VmaAllocation m_Allocation;

    uint32_t m_PoolWidthPages;   // Pool width in pages
    uint32_t m_PoolHeightPages;  // Pool height in pages
    uint32_t m_PageSize;         // Page size in pixels
    RHIFormat m_Format;          // Texture format
    bool m_LogicalOnly;          // Set when initialized via InitializeLogical (no GPU texture)
    bool m_HasContent;           // True once at least one page has been uploaded (see hasContent())

    std::vector<PhysicalTextureSlot> m_Slots;  // Physical slots
    mutable std::mutex m_SlotMutex;            // Mutex for slot access
};

// ============================================================================
// GPU 页上传原语（Vulkan 直连；非 Vulkan 平台直接返回 false）。
// ----------------------------------------------------------------------------
// 把 CPU 侧字节写进一张 2D 图像的**矩形区域**，全程走一次性命令缓冲：
//   barrier(→TRANSFER_DST) → vkCmdCopyBufferToImage → barrier(→SHADER_READ_ONLY)。
//
// 为什么不用现成的 VulkanUtil::CopyBufferToImage：那个 helper 把 imageOffset 硬编码成
// (0,0,0)。页上传要写的是物理页池里**该页槽位对应的那一小块**（px = slot_x * page_size），
// 不是整图，所以必须带 offset。
//
// bufferRowLength / bufferImageHeight 必须传 0（紧凑打包）：Vulkan 把这两个字段当
// **texel** 数而不是字节数，传 width*bytes_per_pixel 会按倍数溢出（HiZ 切法一踩过）。
//
// discard_prior_content：
//   true  → oldLayout = UNDEFINED（**丢弃**整图既有内容，只能用于"这张图首次上传"）；
//   false → oldLayout = SHADER_READ_ONLY（保留其它区域已上传的页，逐页增量上传用这个）。
// 用错方向会静默丢页：往已经上传过部分的页池里再传一页时若传 true，之前那些页会被作废。
bool UploadImageRegion(RHI* rhi,
                       RHIImage* image,
                       const void* data,
                       size_t data_bytes,
                       uint32_t offset_x,
                       uint32_t offset_y,
                       uint32_t width,
                       uint32_t height,
                       uint32_t bytes_per_pixel,
                       bool discard_prior_content);

// ---------------------------------------------------------------------------
// 后端中立的 VT 图像管理（池图 / 页表纹理共用）。
//
// VT 的 GPU 路径最初是 Vulkan 直连（vmaCreateImage / static_cast<VulkanRHI*>）。DX12 后端没有
// VMA，也没有"把 VkImage 包进 RHIImage"这回事——它的 RHIImage 本来就是 DX12Image，直接走
// RHI::CreateImage 即可。所以：
//   * Vulkan：保持原有 VMA 专用内存路径（已被大量测试和真实编辑器验证，不动）；
//   * 其它后端（目前 DX12）：RHI::CreateImage / CreateImageView / DestroyImage / FreeMemory，
//     上传走 RHI::UploadImageRegionImmediate。
// 这三个函数把"选哪条路"收在一处，调用方不再出现 static_cast<VulkanRHI*>。
// ---------------------------------------------------------------------------

// 本后端能不能跑 VT 的 GPU 路径：Vulkan，或声明了 SupportsVirtualTextureUpload() 的后端。
bool VirtualTextureBackendSupported(RHI* rhi);

// true = Vulkan 直连路径；false = 通用 RHI 路径。
bool VirtualTextureUsesVulkanPath(RHI* rhi);

// 通用 RHI 路径下建一张"可采样 + 可拷出"的 2D 图和它的 view。backend_alloc 收 RHIDeviceMemory*。
bool VirtualTextureCreateGenericImage(RHI* rhi,
                                      uint32_t width,
                                      uint32_t height,
                                      RHIFormat format,
                                      RHIImage*& image,
                                      RHIImageView*& view,
                                      void*& backend_alloc);

// 与上面成对。三个指针允许为空（幂等）。调用前提：没有 GPU 工作还在用这张图。
void VirtualTextureDestroyGenericImage(RHI* rhi, RHIImage* image, RHIImageView* view, void* backend_alloc);
