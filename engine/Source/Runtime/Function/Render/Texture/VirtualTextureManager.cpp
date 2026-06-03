#include "VirtualTextureManager.h"

#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Function/Render/Interface/RHI.h"
#include "Runtime/Function/Render/Interface/Vulkan/VulkanRHI.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>

VirtualTextureHandle VirtualTextureManager::s_NextHandle = 1;

VirtualTextureManager::VirtualTextureManager()
    : m_Rhi(nullptr), m_RenderResource(nullptr), m_ShouldStop(false) {}

VirtualTextureManager::~VirtualTextureManager()
{
    Shutdown();
}

void VirtualTextureManager::Initialize(std::shared_ptr<RHI> rhi, RenderResourceBase* render_resource)
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

    // Calculate required mip level
    // TODO: Use more sophisticated mip level calculation
    uint32_t mip_level = resource->CalculateRequiredMipLevel(screen_size, 0.0f, 0.0f);

    // Calculate required pages (simplified: request all pages at mip level)
    // In a real implementation, this would calculate based on UV coordinates
    uint32_t pages_x, pages_y;
    resource->CalculatePagesForMip(mip_level, pages_x, pages_y);

    // Create page load requests
    for (uint32_t y = 0; y < pages_y; ++y)
    {
        for (uint32_t x = 0; x < pages_x; ++x)
        {
            VirtualTexturePageID page_id = resource->GetPageID(mip_level, x, y);

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
            task.distance = (object_position - camera_position).length();
            task.request_time = 0;  // TODO: Use actual time

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

    // Update statistics
    {
        std::lock_guard<std::mutex> lock(m_StatsMutex);
        m_Stats.total_virtual_textures = static_cast<uint32_t>(m_VirtualTextures.size());
        // TODO: Update other statistics
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
    // TODO: Implement priority update based on camera and objects
    // This would iterate through all pages and update their priorities
}

void VirtualTextureManager::EvictLRUPages()
{
    // TODO: Implement LRU eviction
    // This would:
    // 1. Check if memory budget is exceeded
    // 2. Find least recently used pages
    // 3. Unload them
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
    // TODO: Implement screen size calculation
    // This would calculate the screen space size of a page based on:
    // - Object position and bounds
    // - Camera position and FOV
    // - Page position in texture
    return 0.0f;
}