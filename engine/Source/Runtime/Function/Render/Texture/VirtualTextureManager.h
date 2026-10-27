#pragma once

#include "Runtime/Core/Math/AxisAligned.h"
#include "Runtime/Function/Render/RenderResourceBase.h"
#include "VirtualTextureResource.h"
#include "VirtualTextureTypes.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <vector>

class RHI;
class RenderResourceBase;

/// <summary>
/// Virtual Texture Manager
/// Manages virtual textures, physical texture pool, and page loading
/// Similar to UE's VirtualTexture system
/// </summary>
class VirtualTextureManager
{
public:
    VirtualTextureManager();
    ~VirtualTextureManager();

    // Process-wide lazy singleton. The frame loop (RenderSystem::
    // UpdateVirtualTextureForFrame) drives the streaming update through this
    // instance, gated by r.VirtualTexture.Enable. Created on first use so a
    // project that never opts in pays zero cost (and, on platforms where VT
    // doesn't compile, is never touched at all).
    static VirtualTextureManager& Get();

    // Initialize the virtual texture manager
    void Initialize(RHI* rhi, RenderResourceBase* render_resource);

    // Headless / logical init: build the physical pool as a logical (no-GPU)
    // pool and start the loading thread. Slot bookkeeping + streaming policy
    // (load/evict/priority) work identically to the real path; only the GPU
    // texture is absent. Used by the WASM harness (and platforms where VT has
    // no real RHI yet) to exercise the policy headlessly.
    void InitializeLogical(uint32_t pool_width_pages = 16,
                           uint32_t pool_height_pages = 16,
                           uint32_t page_size = 128);

    // Shutdown the virtual texture manager
    void Shutdown();

    // Create physical texture pool
    bool CreatePhysicalTexturePool(uint32_t pool_width_pages,
                                   uint32_t pool_height_pages,
                                   uint32_t page_size,
                                   RHIFormat format);

    // Register a virtual texture
    // Returns a handle to the virtual texture
    VirtualTextureHandle RegisterVirtualTexture(const std::string& texture_path,
                                                uint32_t virtual_width,
                                                uint32_t virtual_height,
                                                uint32_t page_size = 128,
                                                bool is_srgb = false);

    // Register a virtual texture from existing texture data
    VirtualTextureHandle RegisterVirtualTexture(std::shared_ptr<TextureData> texture_data, uint32_t page_size = 128);

    // Unregister a virtual texture
    void UnregisterVirtualTexture(VirtualTextureHandle handle);

    // Request pages for a virtual texture based on camera and object position
    void RequestPages(VirtualTextureHandle handle,
                      const Vector3& camera_position,
                      const Vector3& object_position,
                      const AxisAlignedBox& object_bounds,
                      float screen_size);

    // Update virtual texture system (call each frame)
    void Update(const Vector3& camera_position,
                const Vector3& camera_forward,
                float fov,
                float aspect_ratio,
                uint32_t screen_width,
                uint32_t screen_height,
                float delta_time);

    // Get virtual texture resource
    std::shared_ptr<VirtualTextureResource> GetVirtualTexture(VirtualTextureHandle handle) const;

    // Get physical texture pool
    std::shared_ptr<PhysicalTexturePool> getPhysicalTexturePool() const { return m_PhysicalPool; }

    // Get page table texture for a virtual texture
    RHIImageView* GetPageTableTexture(VirtualTextureHandle handle) const;

    // Get statistics
    VirtualTextureStats GetStats() const;

    // Set configuration
    void SetConfig(const VirtualTextureConfig& config);

    // Get configuration
    const VirtualTextureConfig& getConfig() const { return m_Config; }

    // Force load pages for a virtual texture (blocking)
    bool ForceLoadPages(VirtualTextureHandle handle, uint32_t mip_level = 0);

    // Unload all pages for a virtual texture
    void UnloadPages(VirtualTextureHandle handle);

    // Check if pages are loaded for a virtual texture
    bool ArePagesLoaded(VirtualTextureHandle handle, uint32_t mip_level) const;

    // Upload every DIRTY page table to the GPU (one upload per dirty VT, zero for
    // clean ones). Called automatically at the end of ForceLoadPages /
    // UnloadPages / Update; exposed for callers that mutate residency through
    // other paths. Must run on the thread that owns GPU submission.
    //
    // Note an eviction triggered while loading VT "B" dirties VT "A"'s table, so
    // this walks ALL registered textures, not just the one being loaded.
    void FlushPageTables();

private:
    // Page loading task
    struct PageLoadTask
    {
        VirtualTexturePageID page_id;
        VirtualTextureHandle virtual_texture_handle;
        VirtualTexturePagePriority priority;
        float screen_size;
        float distance;
        uint64_t request_time;

        bool operator<(const PageLoadTask& other) const
        {
            // Higher priority first
            if (priority != other.priority)
            {
                return static_cast<uint8_t>(priority) < static_cast<uint8_t>(other.priority);
            }
            // Then by screen size (larger first)
            return screen_size < other.screen_size;
        }
    };

    // Virtual texture entry
    struct VirtualTextureEntry
    {
        VirtualTextureHandle handle;
        std::shared_ptr<VirtualTextureResource> resource;
        bool is_registered;
    };

    // Initialize page loading thread
    void InitializePageLoadingThread();

    // Shutdown page loading thread
    void ShutdownPageLoadingThread();

    // Page loading thread function
    void PageLoadingThreadFunction();

    // Process page load requests
    void ProcessPageLoadRequests(float delta_time);

    // Load a page
    bool LoadPage(const VirtualTexturePageID& page_id, VirtualTextureHandle handle);

    // Upload page to physical pool
    void UploadPageToPhysicalPool(const VirtualTexturePageID& page_id,
                                  VirtualTextureHandle handle,
                                  void* page_data,
                                  uint32_t page_width,
                                  uint32_t page_height);

    // Unload a page. Marks the owner's page table dirty; does NOT upload it --
    // the batch entry point (ForceLoadPages / UnloadPages / Update) flushes once.
    void UnloadPage(const VirtualTexturePageID& page_id, VirtualTextureHandle handle);

    // Unload every loaded page of one texture without flushing (Unregister uses
    // it: the table is about to be released, re-uploading it would be wasted).
    void UnloadAllPagesNoFlush(VirtualTextureHandle handle);

    // Pool full: evict the least-recently-used resident page THROUGH UnloadPage
    // (so its owner's page table is invalidated). Returns false if nothing could
    // be evicted.
    bool EvictLRUPageForAllocation();

    // Update page priorities based on camera and objects
    void UpdatePagePriorities(const Vector3& camera_position,
                              const Vector3& camera_forward,
                              float fov,
                              float aspect_ratio,
                              uint32_t screen_width,
                              uint32_t screen_height);

    // Evict least recently used pages if memory budget exceeded
    void EvictLRUPages();

    // Calculate page screen size
    float CalculatePageScreenSize(const VirtualTexturePageID& page_id,
                                  VirtualTextureHandle handle,
                                  const Vector3& camera_position,
                                  const Vector3& object_position,
                                  const AxisAlignedBox& object_bounds,
                                  float fov,
                                  float aspect_ratio,
                                  uint32_t screen_width,
                                  uint32_t screen_height) const;

    RHI* m_Rhi;
    RenderResourceBase* m_RenderResource;

    // Configuration
    VirtualTextureConfig m_Config;

    // Physical texture pool
    std::shared_ptr<PhysicalTexturePool> m_PhysicalPool;

    // Virtual textures
    std::unordered_map<VirtualTextureHandle, VirtualTextureEntry> m_VirtualTextures;
    mutable std::mutex m_VirtualTexturesMutex;

    // Page load requests
    std::priority_queue<PageLoadTask> m_PageLoadQueue;
    std::mutex m_PageLoadQueueMutex;

    // Page loading thread
    std::thread m_PageLoadingThread;
    std::atomic<bool> m_ShouldStop;
    std::condition_variable m_PageLoadingCv;
    std::mutex m_PageLoadingMutex;

    // Statistics
    mutable std::mutex m_StatsMutex;
    VirtualTextureStats m_Stats;

    // Monotonic LRU clock. It used to be steady_clock milliseconds: a batch that
    // loads N pages within the same millisecond stamped them all identically, so
    // "least recently used" degenerated into "lowest slot index". A counter gives
    // a strict load order.
    std::atomic<uint64_t> m_AccessTick {0};

    // Next handle
    static VirtualTextureHandle s_NextHandle;
};