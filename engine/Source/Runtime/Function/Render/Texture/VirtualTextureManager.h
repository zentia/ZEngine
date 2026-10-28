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
#include <unordered_set>
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

    // Initialize the virtual texture manager. Returns false when the physical page
    // pool could not be created (the manager is then left uninitialised: no pool, no
    // worker thread). Callers MUST check it: it used to be void and swallowed the
    // failure, so a pool too large for the device (the default config asks for a
    // 262144 x 262144 px = 256 GiB image) left a manager that looked alive but failed
    // every page load silently.
    bool Initialize(RHI* rhi, RenderResourceBase* render_resource);

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

    // The part of Update() that COMMITS pending page loads: drains up to
    // max_concurrent_loads prepared/queued pages (slot allocation, GPU upload,
    // page-table bookkeeping), then flushes dirty page tables. Update() does the
    // same plus priority recompute and budget eviction; this entry point exists so
    // a caller (and the async-loading test) can pump loads without a camera.
    //
    // THREADING CONTRACT -- the whole point of the async design: the page-loading
    // worker thread only ever does CPU work (extracting page pixels from the
    // retained source). Everything that touches the GPU or mutates residency state
    // (LoadPage, UnloadPage, UploadPageToPhysicalPool, FlushPageTables, Shutdown's
    // resource release) runs on the OWNER thread: the thread that called
    // Initialize(). Update(), PumpLoads(), ForceLoadPages() and UnloadPages() must
    // be called from it.
    void PumpLoads();

    // Diagnostics for the threading contract (tests assert on these).
    //   getGpuThreadViolationCount: GPU work observed off the owner thread. Must be 0.
    //   getAsyncPreparedCount:      pages whose pixels the worker thread prepared
    //                               (proves the async path really ran).
    //   getPreparedBacklogPeak:     max pages ever waiting between worker and commit
    //                               (back-pressure keeps it bounded).
    uint32_t getGpuThreadViolationCount() const { return m_GpuThreadViolations.load(); }
    uint32_t getAsyncPreparedCount() const { return m_AsyncPreparedCount.load(); }
    // 页上传的成败计数。LoadPage 在上传失败时页状态仍是 Loaded（页表项驻留），所以"N/N 页驻留"只证明
    // CPU 侧记账，不证明像素真进了显存；这两个计数才是 GPU 侧的证据。failures 应恒为 0。
    uint32_t getPageUploadCount() const { return m_PageUploadCount.load(); }
    uint32_t getPageUploadFailureCount() const { return m_PageUploadFailures.load(); }
    uint32_t getPreparedBacklogPeak() const { return m_PreparedBacklogPeak.load(); }

    // Requests still queued for the worker + pages the worker is preparing + pages
    // prepared but not yet committed. 0 means every request so far has been fully
    // processed.
    uint32_t getPendingLoadCount() const;

private:
    // Pixels the worker thread prepared for one page, handed to the owner thread to
    // commit. Carries no GPU object, so the worker never needs the RHI.
    struct PreparedPage
    {
        VirtualTexturePageID page_id {};
        VirtualTextureHandle handle {0};
        bool has_pixels {false};  // false: source has no pixels (path-registered VT) -> bookkeeping only
        std::vector<uint8_t> pixels;
        uint32_t width {0};   // real (unpadded) extent; pixels is always page_size^2
        uint32_t height {0};
    };

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

    // Load a page (owner thread only). `prepared` carries pixels already extracted by
    // the worker; null means "extract them inline" (the synchronous path).
    bool LoadPage(const VirtualTexturePageID& page_id,
                  VirtualTextureHandle handle,
                  const PreparedPage* prepared = nullptr);

    // Records a violation if called off the owner thread (see PumpLoads' contract).
    void CheckOwnerThread();

    // Upload page to physical pool
    void UploadPageToPhysicalPool(const VirtualTexturePageID& page_id,
                                  VirtualTextureHandle handle,
                                  const void* page_data,
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

    // Calculate page screen size. Takes the RESOURCE, not its handle: the only caller
    // (UpdatePagePriorities) already holds m_VirtualTexturesMutex while iterating, and
    // the handle version re-locked that same non-recursive mutex through
    // GetVirtualTexture() -- a guaranteed self-deadlock the first time Update() met a
    // page that had ever been requested. It never fired before because nothing called
    // RequestPages; WASM can't see it either (no pthreads => std::mutex is a no-op).
    float CalculatePageScreenSize(const VirtualTexturePageID& page_id,
                                  const VirtualTextureResource& resource,
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

    // Pages that are queued, being prepared, or prepared-but-uncommitted. RequestPages
    // is called every frame by a producer and used to re-enqueue the whole mip each
    // time (a 36-page texture reached 19200 queued tasks after 600 frames); this set
    // makes it idempotent. Guarded by m_PageLoadQueueMutex like the queues it mirrors.
    std::unordered_set<VirtualTexturePageID, VirtualTexturePageIDHash> m_PendingPages;

    // Page load requests (worker input) and prepared pages (worker output). ONE mutex
    // guards both queues AND is the one the condition variable waits on -- the old
    // code waited on a different mutex than the queue's, so the wait predicate read
    // the queue unsynchronized and a push could never wake the worker.
    std::priority_queue<PageLoadTask> m_PageLoadQueue;
    std::queue<PreparedPage> m_PreparedQueue;
    mutable std::mutex m_PageLoadQueueMutex;

    // Page loading thread
    std::thread m_PageLoadingThread;
    std::atomic<bool> m_ShouldStop;
    std::condition_variable m_PageLoadingCv;

    // Owner thread = the thread that called Initialize()/InitializeLogical().
    std::thread::id m_OwnerThread;
    std::atomic<uint32_t> m_GpuThreadViolations {0};
    std::atomic<uint32_t> m_PageUploadCount {0};
    std::atomic<uint32_t> m_PageUploadFailures {0};
    std::atomic<uint32_t> m_AsyncPreparedCount {0};
    std::atomic<uint32_t> m_PreparedBacklogPeak {0};
    // Pages the worker has popped from the request queue but not yet pushed to the
    // prepared queue. Without it there is a window where a page is in neither queue,
    // so "pending == 0" would wrongly mean idle. Modified only under m_PageLoadQueueMutex.
    uint32_t m_WorkerInFlight {0};

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