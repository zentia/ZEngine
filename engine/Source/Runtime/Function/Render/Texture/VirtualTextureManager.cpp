#include "VirtualTextureManager.h"

#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Function/Render/Interface/RHI.h"
#include "Runtime/Function/Render/Interface/Vulkan/VulkanRHI.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>

VirtualTextureHandle VirtualTextureManager::s_NextHandle = 1;

VirtualTextureManager::VirtualTextureManager()
    : m_Rhi(nullptr), m_RenderResource(nullptr), m_ShouldStop(false) {}

VirtualTextureManager::~VirtualTextureManager()
{
    Shutdown();
}

VirtualTextureManager& VirtualTextureManager::Get()
{
    // Meyers singleton. Constructed on first Get() call, which only happens from
    // the (Vulkan-guarded, CVar-gated) frame-loop hook, so a dormant project
    // never pays for it.
    static VirtualTextureManager s_Instance;
    return s_Instance;
}

void VirtualTextureManager::Initialize(RHI* rhi, RenderResourceBase* render_resource)
{
    m_Rhi = rhi;
    m_RenderResource = render_resource;

    // Create default physical texture pool
    if (!m_PhysicalPool)
    {
        CreatePhysicalTexturePool(m_Config.physical_pool_width,
                                  m_Config.physical_pool_height,
                                  m_Config.page_size,
                                  m_Config.physical_pool_format);
    }

    // Initialize page loading thread
    if (m_Config.enable_async_loading)
    {
        InitializePageLoadingThread();
    }
}

void VirtualTextureManager::InitializeLogical(uint32_t pool_width_pages,
                                              uint32_t pool_height_pages,
                                              uint32_t page_size)
{
    m_Rhi = nullptr;
    m_RenderResource = nullptr;

    // Build the physical pool as a logical (no-GPU) pool. Slot allocation / LRU
    // work identically; only the Vulkan texture is skipped.
    if (!m_PhysicalPool)
    {
        m_PhysicalPool = std::make_shared<PhysicalTexturePool>();
        m_PhysicalPool->InitializeLogical(pool_width_pages, pool_height_pages, page_size);
    }

    if (m_Config.enable_async_loading)
    {
        InitializePageLoadingThread();
    }
}

void VirtualTextureManager::Shutdown()
{
    m_ShouldStop = true;

    // Shutdown page loading thread
    ShutdownPageLoadingThread();

    // Unregister all virtual textures
    {
        std::lock_guard<std::mutex> lock(m_VirtualTexturesMutex);
        for (auto& pair : m_VirtualTextures)
        {
            if (pair.second.resource)
            {
                pair.second.resource->Shutdown();
            }
        }
        m_VirtualTextures.clear();
    }

    // Shutdown physical pool
    if (m_PhysicalPool)
    {
        m_PhysicalPool->Shutdown();
        m_PhysicalPool = nullptr;
    }

    m_Rhi = nullptr;
    m_RenderResource = nullptr;
}

bool VirtualTextureManager::CreatePhysicalTexturePool(uint32_t pool_width_pages,
                                                      uint32_t pool_height_pages,
                                                      uint32_t page_size,
                                                      RHIFormat format)
{
    if (m_PhysicalPool)
    {
        m_PhysicalPool->Shutdown();
    }

    m_PhysicalPool = std::make_shared<PhysicalTexturePool>();
    return m_PhysicalPool->Initialize(m_Rhi, pool_width_pages, pool_height_pages, page_size, format);
}

VirtualTextureHandle VirtualTextureManager::RegisterVirtualTexture(const std::string& texture_path,
                                                                   uint32_t virtual_width,
                                                                   uint32_t virtual_height,
                                                                   uint32_t page_size,
                                                                   bool is_srgb)
{
    auto resource = std::make_shared<VirtualTextureResource>();
    if (!resource->Initialize(texture_path, virtual_width, virtual_height, page_size, is_srgb))
    {
        return 0;
    }

    VirtualTextureHandle handle = resource->getHandle();

    VirtualTextureEntry entry;
    entry.handle = handle;
    entry.resource = resource;
    entry.is_registered = true;

    {
        std::lock_guard<std::mutex> lock(m_VirtualTexturesMutex);
        m_VirtualTextures[handle] = entry;
    }

    return handle;
}

VirtualTextureHandle VirtualTextureManager::RegisterVirtualTexture(std::shared_ptr<TextureData> texture_data,
                                                                   uint32_t page_size)
{
    if (!texture_data)
    {
        return 0;
    }

    auto resource = std::make_shared<VirtualTextureResource>();
    if (!resource->Initialize(texture_data, page_size))
    {
        return 0;
    }

    VirtualTextureHandle handle = resource->getHandle();

    VirtualTextureEntry entry;
    entry.handle = handle;
    entry.resource = resource;
    entry.is_registered = true;

    {
        std::lock_guard<std::mutex> lock(m_VirtualTexturesMutex);
        m_VirtualTextures[handle] = entry;
    }

    return handle;
}

void VirtualTextureManager::UnregisterVirtualTexture(VirtualTextureHandle handle)
{
    std::lock_guard<std::mutex> lock(m_VirtualTexturesMutex);
    auto it = m_VirtualTextures.find(handle);
    if (it != m_VirtualTextures.end())
    {
        // Unload all pages
        UnloadPages(handle);

        // Shutdown resource
        if (it->second.resource)
        {
            it->second.resource->Shutdown();
        }

        m_VirtualTextures.erase(it);
    }
}

void VirtualTextureManager::RequestPages(VirtualTextureHandle handle,
                                         const Vector3& camera_position,
                                         const Vector3& object_position,
                                         const AxisAlignedBox& object_bounds,
                                         float screen_size)
{
    std::lock_guard<std::mutex> lock(m_VirtualTexturesMutex);
    auto it = m_VirtualTextures.find(handle);
    if (it == m_VirtualTextures.end() || !it->second.resource)
    {
        return;
    }

    auto resource = it->second.resource;

    // Distance from camera to the object drives both mip selection and priority.
    float distance = (object_position - camera_position).length();

    // Calculate required mip level from the object's screen footprint (was:
    // hardcoded screen_size/0/0 which always returned the coarsest mip).
    float approx_screen_size = screen_size;
    uint32_t mip_level = resource->CalculateRequiredMipLevel(approx_screen_size, distance, 0.0f);

    // Calculate required pages (simplified: request all pages at mip level)
    // In a real implementation, this would calculate based on UV coordinates
    uint32_t pages_x, pages_y;
    resource->CalculatePagesForMip(mip_level, pages_x, pages_y);

    uint64_t now = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());

    // Create page load requests
    for (uint32_t y = 0; y < pages_y; ++y)
    {
        for (uint32_t x = 0; x < pages_x; ++x)
        {
            VirtualTexturePageID page_id = resource->GetPageID(mip_level, x, y);

            // Stamp the page with the object's world footprint so Update() can
            // reproject it to the live screen each frame (the skeleton stored
            // nothing here, which left UpdatePagePriorities with nothing to
            // recompute from).
            VirtualTexturePage* page = resource->GetPage(mip_level, x, y);
            if (page)
            {
                page->object_position = object_position;
                page->object_bounds = object_bounds;
            }

            // Calculate priority based on screen size
            VirtualTexturePagePriority priority = VirtualTexturePagePriority::Normal;
            if (screen_size > 0.5f)
            {
                priority = VirtualTexturePagePriority::Critical;
            }
            else if (screen_size > 0.1f)
            {
                priority = VirtualTexturePagePriority::High;
            }
            else if (screen_size < 0.01f)
            {
                priority = VirtualTexturePagePriority::Low;
            }

            PageLoadTask task;
            task.page_id = page_id;
            task.virtual_texture_handle = handle;
            task.priority = priority;
            task.screen_size = screen_size;
            task.distance = distance;
            task.request_time = now;

            {
                std::lock_guard<std::mutex> queue_lock(m_PageLoadQueueMutex);
                m_PageLoadQueue.push(task);
            }
        }
    }
}

void VirtualTextureManager::Update(const Vector3& camera_position,
                                   const Vector3& camera_forward,
                                   float fov,
                                   float aspect_ratio,
                                   uint32_t screen_width,
                                   uint32_t screen_height,
                                   float delta_time)
{
    // Update page priorities
    UpdatePagePriorities(camera_position, camera_forward, fov, aspect_ratio, screen_width, screen_height);

    // Process page load requests
    ProcessPageLoadRequests(delta_time);

    // Evict LRU pages if memory budget exceeded
    EvictLRUPages();

    // Update statistics. Two separate mutexes: m_VirtualTexturesMutex guards the
    // container we iterate, m_StatsMutex guards m_Stats. Always take the VT
    // mutex FIRST (it is the outer lock everywhere else too) to avoid a
    // lock-order inversion / data race with Register/RequestPages.
    uint32_t total_pages = 0, loaded_pages = 0, loading_pages = 0, unloaded_pages = 0;
    {
        std::lock_guard<std::mutex> vt_lock(m_VirtualTexturesMutex);
        for (auto& pair : m_VirtualTextures)
        {
            if (!pair.second.resource)
            {
                continue;
            }
            const auto& pages = pair.second.resource->getAllPages();
            total_pages += static_cast<uint32_t>(pages.size());
            for (const auto& page_pair : pages)
            {
                switch (page_pair.second.state)
                {
                    case VirtualTexturePageState::Loaded:
                        ++loaded_pages;
                        break;
                    case VirtualTexturePageState::Loading:
                    case VirtualTexturePageState::Unloading:
                        ++loading_pages;
                        break;
                    default:
                        ++unloaded_pages;
                        break;
                }
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_StatsMutex);
        m_Stats.total_virtual_textures = static_cast<uint32_t>(m_VirtualTextures.size());
        m_Stats.total_pages = total_pages;
        m_Stats.loaded_pages = loaded_pages;
        m_Stats.loading_pages = loading_pages;
        m_Stats.unloaded_pages = unloaded_pages;

        if (m_PhysicalPool)
        {
            m_Stats.physical_pool_total_slots = m_PhysicalPool->getTotalSlots();
            m_Stats.physical_pool_used_slots = 0;
            for (uint32_t i = 0; i < m_Stats.physical_pool_total_slots; ++i)
            {
                if (m_PhysicalPool->IsSlotAllocated(i))
                {
                    ++m_Stats.physical_pool_used_slots;
                }
            }
            const size_t bytes_per_pixel = 4;  // R8G8B8A8
            const size_t per_slot =
                static_cast<size_t>(m_PhysicalPool->getPageSize()) * m_PhysicalPool->getPageSize() * bytes_per_pixel;
            m_Stats.total_memory_used = m_Stats.physical_pool_used_slots * per_slot;
        }
        m_Stats.memory_budget = static_cast<size_t>(m_Config.memory_budget_mb) * 1024 * 1024;
        m_Stats.memory_usage_ratio = (m_Stats.memory_budget > 0)
                                         ? static_cast<float>(m_Stats.total_memory_used) /
                                               static_cast<float>(m_Stats.memory_budget)
                                         : 0.0f;
    }
}

std::shared_ptr<VirtualTextureResource> VirtualTextureManager::GetVirtualTexture(VirtualTextureHandle handle) const
{
    std::lock_guard<std::mutex> lock(m_VirtualTexturesMutex);
    auto it = m_VirtualTextures.find(handle);
    if (it != m_VirtualTextures.end())
    {
        return it->second.resource;
    }
    return nullptr;
}

RHIImageView* VirtualTextureManager::GetPageTableTexture(VirtualTextureHandle handle) const
{
    auto resource = GetVirtualTexture(handle);
    if (resource)
    {
        return resource->getPageTableTextureView();
    }
    return nullptr;
}

VirtualTextureStats VirtualTextureManager::GetStats() const
{
    std::lock_guard<std::mutex> lock(m_StatsMutex);
    return m_Stats;
}

void VirtualTextureManager::SetConfig(const VirtualTextureConfig& config)
{
    m_Config = config;
}

bool VirtualTextureManager::ForceLoadPages(VirtualTextureHandle handle, uint32_t mip_level)
{
    auto resource = GetVirtualTexture(handle);
    if (!resource)
    {
        return false;
    }

    uint32_t pages_x, pages_y;
    resource->CalculatePagesForMip(mip_level, pages_x, pages_y);

    for (uint32_t y = 0; y < pages_y; ++y)
    {
        for (uint32_t x = 0; x < pages_x; ++x)
        {
            VirtualTexturePageID page_id = resource->GetPageID(mip_level, x, y);
            LoadPage(page_id, handle);
        }
    }

    return true;
}

void VirtualTextureManager::UnloadPages(VirtualTextureHandle handle)
{
    auto resource = GetVirtualTexture(handle);
    if (!resource)
    {
        return;
    }

    const auto& pages = resource->getAllPages();
    for (const auto& pair : pages)
    {
        if (pair.second.state == VirtualTexturePageState::Loaded)
        {
            UnloadPage(pair.first, handle);
        }
    }
}

bool VirtualTextureManager::ArePagesLoaded(VirtualTextureHandle handle, uint32_t mip_level) const
{
    auto resource = GetVirtualTexture(handle);
    if (!resource)
    {
        return false;
    }

    uint32_t pages_x, pages_y;
    resource->CalculatePagesForMip(mip_level, pages_x, pages_y);

    for (uint32_t y = 0; y < pages_y; ++y)
    {
        for (uint32_t x = 0; x < pages_x; ++x)
        {
            const VirtualTexturePage* page = resource->GetPage(mip_level, x, y);
            if (!page || page->state != VirtualTexturePageState::Loaded)
            {
                return false;
            }
        }
    }

    return true;
}

void VirtualTextureManager::InitializePageLoadingThread()
{
    if (m_PageLoadingThread.joinable())
    {
        return;
    }

    m_ShouldStop = false;
    m_PageLoadingThread = std::thread(&VirtualTextureManager::PageLoadingThreadFunction, this);
}

void VirtualTextureManager::ShutdownPageLoadingThread()
{
    if (m_PageLoadingThread.joinable())
    {
        m_ShouldStop = true;
        m_PageLoadingCv.notify_all();
        m_PageLoadingThread.join();
    }
}

void VirtualTextureManager::PageLoadingThreadFunction()
{
    while (!m_ShouldStop)
    {
        std::unique_lock<std::mutex> lock(m_PageLoadingMutex);
        m_PageLoadingCv.wait(lock, [this] { return !m_PageLoadQueue.empty() || m_ShouldStop; });

        if (m_ShouldStop)
        {
            break;
        }

        // Process a limited number of pages per frame
        uint32_t processed = 0;
        const uint32_t max_pages_per_frame = m_Config.max_concurrent_loads;

        while (processed < max_pages_per_frame && !m_PageLoadQueue.empty())
        {
            PageLoadTask task;
            {
                std::lock_guard<std::mutex> queue_lock(m_PageLoadQueueMutex);
                if (m_PageLoadQueue.empty())
                {
                    break;
                }
                task = m_PageLoadQueue.top();
                m_PageLoadQueue.pop();
            }

            // Load page
            LoadPage(task.page_id, task.virtual_texture_handle);

            ++processed;
        }
    }
}

void VirtualTextureManager::ProcessPageLoadRequests(float delta_time)
{
    // This is called on the main thread
    // Process a limited number of requests per frame
    uint32_t processed = 0;
    const uint32_t max_pages_per_frame = m_Config.max_concurrent_loads;

    while (processed < max_pages_per_frame)
    {
        PageLoadTask task;
        {
            std::lock_guard<std::mutex> lock(m_PageLoadQueueMutex);
            if (m_PageLoadQueue.empty())
            {
                break;
            }
            task = m_PageLoadQueue.top();
            m_PageLoadQueue.pop();
        }

        // Check if page is already loaded
        auto resource = GetVirtualTexture(task.virtual_texture_handle);
        if (resource)
        {
            VirtualTexturePage* page =
                resource->GetPage(task.page_id.mip_level, task.page_id.page_x, task.page_id.page_y);

            if (page && page->state == VirtualTexturePageState::Unloaded)
            {
                LoadPage(task.page_id, task.virtual_texture_handle);
            }
        }

        ++processed;
    }
}

bool VirtualTextureManager::LoadPage(const VirtualTexturePageID& page_id, VirtualTextureHandle handle)
{
    auto resource = GetVirtualTexture(handle);
    if (!resource)
    {
        return false;
    }

    VirtualTexturePage* page = resource->GetPage(page_id.mip_level, page_id.page_x, page_id.page_y);
    if (!page || page->state == VirtualTexturePageState::Loaded)
    {
        return false;
    }

    // Update state to loading
    page->state = VirtualTexturePageState::Loading;
    resource->UpdatePageState(page_id, VirtualTexturePageState::Loading);

    // TODO: Load page data from disk
    // For now, this is a placeholder
    // In a real implementation, this would:
    // 1. Load the source texture
    // 2. Extract the specific page region
    // 3. Generate mip levels if needed
    // 4. Upload to physical pool

    // Allocate physical slot
    if (!m_PhysicalPool)
    {
        return false;
    }

    uint32_t physical_slot = m_PhysicalPool->AllocateSlot(page_id);
    if (physical_slot == UINT32_MAX)
    {
        page->state = VirtualTexturePageState::Unloaded;
        return false;
    }

    // Update page
    page->physical_slot = physical_slot;
    page->state = VirtualTexturePageState::Loaded;
    resource->UpdatePageState(page_id, VirtualTexturePageState::Loaded);

    // Stamp the access clock so the LRU evictor has a real ordering key (was:
    // every loaded slot tied at last_access_time == 0).
    uint64_t now = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    page->last_access_time = now;
    m_PhysicalPool->UpdateSlotAccessTime(physical_slot, now);

    // Update page table entry
    PageTableEntry entry;
    entry.physical_slot = physical_slot;
    entry.mip_level = page_id.mip_level;
    entry.is_valid = true;
    resource->SetPageTableEntry(page_id.mip_level, page_id.page_x, page_id.page_y, entry);

    return true;
}

void VirtualTextureManager::UploadPageToPhysicalPool(const VirtualTexturePageID& page_id,
                                                     VirtualTextureHandle handle,
                                                     void* page_data,
                                                     uint32_t page_width,
                                                     uint32_t page_height)
{
    // TODO: Implement page upload to physical pool
    // This would:
    // 1. Get physical slot coordinates
    // 2. Create staging buffer
    // 3. Copy page data to staging buffer
    // 4. Transfer to physical texture at correct location
}

void VirtualTextureManager::UnloadPage(const VirtualTexturePageID& page_id, VirtualTextureHandle handle)
{
    auto resource = GetVirtualTexture(handle);
    if (!resource)
    {
        return;
    }

    VirtualTexturePage* page = resource->GetPage(page_id.mip_level, page_id.page_x, page_id.page_y);
    if (!page || page->state != VirtualTexturePageState::Loaded)
    {
        return;
    }

    // Free physical slot
    if (m_PhysicalPool && page->physical_slot != UINT32_MAX)
    {
        m_PhysicalPool->FreeSlot(page->physical_slot);
    }

    // Update page
    page->physical_slot = UINT32_MAX;
    page->state = VirtualTexturePageState::Unloaded;
    resource->UpdatePageState(page_id, VirtualTexturePageState::Unloaded);

    // Clear page table entry
    PageTableEntry entry;
    entry.is_valid = false;
    resource->SetPageTableEntry(page_id.mip_level, page_id.page_x, page_id.page_y, entry);
}

void VirtualTextureManager::UpdatePagePriorities(const Vector3& camera_position,
                                                 const Vector3& camera_forward,
                                                 float fov,
                                                 float aspect_ratio,
                                                 uint32_t screen_width,
                                                 uint32_t screen_height)
{
    std::lock_guard<std::mutex> lock(m_VirtualTexturesMutex);
    for (auto& pair : m_VirtualTextures)
    {
        if (!pair.second.resource)
        {
            continue;
        }
        auto resource = pair.second.resource;
        const auto& pages = resource->getAllPages();
        for (const auto& page_pair : pages)
        {
            const VirtualTexturePage& page = page_pair.second;
            if (page.object_bounds.getHalfExtent().isZero() && page.distance == 0.0f)
            {
                // Page was never requested yet (no world footprint to reproject).
                continue;
            }

            float screen_size = CalculatePageScreenSize(page.id, pair.first, camera_position,
                                                        page.object_position, page.object_bounds, fov,
                                                        aspect_ratio, screen_width, screen_height);
            float distance = (page.object_position - camera_position).length();

            // Distance gate: beyond max_distance the page is not worth streaming.
            bool visible = screen_size >= m_Config.min_screen_size && distance <= m_Config.max_distance;

            VirtualTexturePagePriority priority = VirtualTexturePagePriority::Normal;
            if (!visible)
            {
                priority = VirtualTexturePagePriority::Low;
            }
            else if (screen_size > 0.5f)
            {
                priority = VirtualTexturePagePriority::Critical;
            }
            else if (screen_size > 0.1f)
            {
                priority = VirtualTexturePagePriority::High;
            }
            else if (screen_size < m_Config.min_screen_size)
            {
                priority = VirtualTexturePagePriority::Low;
            }

            // Persist the recomputed values (getAllPages returns a const ref, so
            // mutate through the non-const accessors).
            resource->UpdatePagePriority(page.id, priority);
            VirtualTexturePage* mutable_page = resource->GetPage(page.id.mip_level, page.id.page_x, page.id.page_y);
            if (mutable_page)
            {
                mutable_page->screen_size = screen_size;
                mutable_page->distance = distance;
                mutable_page->is_visible = visible;
            }
        }
    }
}

void VirtualTextureManager::EvictLRUPages()
{
    if (!m_PhysicalPool || m_Config.memory_budget_mb == 0)
    {
        return;
    }

    // Budget in bytes. Each loaded page occupies one physical slot =
    // page_size^2 pixels * bytes-per-pixel (4 for R8G8B8A8). We track the total
    // via GetStats() rather than a separate counter, so compute the per-slot
    // cost here to avoid a round trip per eviction.
    const size_t bytes_per_pixel = 4;  // R8G8B8A8
    const uint32_t page_size = m_PhysicalPool->getPageSize();
    const size_t bytes_per_slot = static_cast<size_t>(page_size) * page_size * bytes_per_pixel;
    const size_t budget_bytes = static_cast<size_t>(m_Config.memory_budget_mb) * 1024 * 1024;

    // Collect the loaded slots with their last-access time, then evict oldest
    // first until we are back under budget.
    std::vector<std::pair<uint64_t, uint32_t>> loaded;  // (last_access_time, slot_index)
    {
        uint32_t total_slots = m_PhysicalPool->getTotalSlots();
        for (uint32_t i = 0; i < total_slots; ++i)
        {
            if (m_PhysicalPool->IsSlotAllocated(i))
            {
                const PhysicalTextureSlot& slot = m_PhysicalPool->GetSlot(i);
                loaded.emplace_back(slot.last_access_time, i);
            }
        }
    }

    size_t used_bytes = loaded.size() * bytes_per_slot;
    if (used_bytes <= budget_bytes)
    {
        return;
    }

    // Oldest first (smallest last_access_time).
    std::sort(loaded.begin(), loaded.end(),
              [](const std::pair<uint64_t, uint32_t>& a, const std::pair<uint64_t, uint32_t>& b) {
                  return a.first < b.first;
              });

    for (const auto& entry : loaded)
    {
        if (used_bytes <= budget_bytes)
        {
            break;
        }
        uint32_t slot_index = entry.second;
        const PhysicalTextureSlot& slot = m_PhysicalPool->GetSlot(slot_index);
        VirtualTexturePageID page_id = slot.page_id;
        if (page_id.virtual_texture_id == 0 && page_id.mip_level == 0 && page_id.page_x == 0 &&
            page_id.page_y == 0)
        {
            // Slot claims to be allocated but has no owning page id; free it.
            m_PhysicalPool->FreeSlot(slot_index);
            used_bytes -= bytes_per_slot;
            continue;
        }

        UnloadPage(page_id, page_id.virtual_texture_id);
        used_bytes -= bytes_per_slot;
    }
}

float VirtualTextureManager::CalculatePageScreenSize(const VirtualTexturePageID& page_id,
                                                     VirtualTextureHandle handle,
                                                     const Vector3& camera_position,
                                                     const Vector3& object_position,
                                                     const AxisAlignedBox& object_bounds,
                                                     float fov,
                                                     float aspect_ratio,
                                                     uint32_t screen_width,
                                                     uint32_t screen_height) const
{
    // World-space footprint of ONE page at this mip level, then project it to a
    // screen-height fraction. A virtual texture maps the WHOLE object bounds
    // onto its mip-0 grid, so a page at mip m covers 1/pages_x(m) of the object
    // in each dimension (a mip-m page spans 2^m texels = 2^m x the mip-0 page
    // region = 1/pages_x(m) of the object width). We use the object's X-Y
    // footprint (the surface the texture is sampled from).
    auto resource = GetVirtualTexture(handle);
    if (!resource || screen_height == 0 || screen_width == 0)
    {
        return 0.0f;
    }

    uint32_t pages_x = 0, pages_y = 0;
    resource->CalculatePagesForMip(page_id.mip_level, pages_x, pages_y);
    if (pages_x == 0 || pages_y == 0)
    {
        return 0.0f;
    }

    // Per-page world half-extent (the texture is sampled across the object's
    // X-Y footprint; Z is irrelevant for a 2D page's screen footprint).
    const Vector3 half = object_bounds.getHalfExtent();
    const float page_half_x = half.x / static_cast<float>(pages_x);
    const float page_half_y = half.y / static_cast<float>(pages_y);
    const float page_half = std::max(page_half_x, page_half_y);
    if (page_half <= 0.0f)
    {
        return 0.0f;
    }

    // Distance from camera to the object (page centre ~= object centre; a finer
    // model would offset by the page's sub-region, but object distance is the
    // standard VT feedback approximation and avoids a per-page bounds store).
    float distance = (object_position - camera_position).length();
    if (distance <= 1e-4f)
    {
        distance = 1e-4f;  // avoid div-by-zero when the camera sits on the object
    }

    // Angular size -> screen-height fraction via the pinhole model:
    //   screen_pixels = (world_half / distance) * (screen_height/2) / tan(fov/2)
    // Normalise by screen_height to get a [0,1] fraction (matches the thresholds
    // used in RequestPages: >0.5 critical, >0.1 high, <0.01 low).
    float half_fov = fov * 0.5f;
    float tan_half = std::tan(half_fov);
    if (tan_half <= 1e-6f)
    {
        tan_half = 1e-6f;  // degenerate fov guard
    }
    // Aspect-correct world width at the object's depth:
    float world_width_at_depth = 2.0f * distance * tan_half;
    float screen_pixels = (page_half / world_width_at_depth) * static_cast<float>(screen_width);
    return screen_pixels / static_cast<float>(screen_height);
}