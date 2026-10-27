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

bool VirtualTextureManager::Initialize(RHI* rhi, RenderResourceBase* render_resource)
{
    // VT 的 GPU 路径（池图、页表纹理、页上传）全是 Vulkan 直连，内部多处
    // static_cast<VulkanRHI*>(rhi)。Windows 的**默认** RHI 是 DX12（只有 --rhi vulkan 才用
    // Vulkan），而 Z_HAS_VULKAN 在 Windows 上同样被定义——所以帧钩子在 DX12 运行时也会走到这里，
    // 把一个 DX12RHI* 当成 VulkanRHI* 用就是未定义行为。必须在入口用类型检查挡住，而不是指望
    // 各处 static_cast 碰巧不崩。仓库里 BindlessTextureBlitPipeline 用的是同一手法。
    if (dynamic_cast<VulkanRHI*>(rhi) == nullptr)
    {
        return false;
    }

    m_OwnerThread = std::this_thread::get_id();
    m_Rhi = rhi;
    m_RenderResource = render_resource;

    // Create default physical texture pool
    if (!m_PhysicalPool)
    {
        if (!CreatePhysicalTexturePool(m_Config.physical_pool_width,
                                       m_Config.physical_pool_height,
                                       m_Config.page_size,
                                       m_Config.physical_pool_format))
        {
            // 不留一个"有池对象、没 GPU 图、零槽位"的半死管理器。
            m_PhysicalPool.reset();
            m_Rhi = nullptr;
            m_RenderResource = nullptr;
            return false;
        }
    }

    // Initialize page loading thread
    if (m_Config.enable_async_loading)
    {
        InitializePageLoadingThread();
    }
    return true;
}

void VirtualTextureManager::InitializeLogical(uint32_t pool_width_pages,
                                              uint32_t pool_height_pages,
                                              uint32_t page_size)
{
    m_OwnerThread = std::this_thread::get_id();
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
    // 旧实现持着 m_VirtualTexturesMutex 调 UnloadPages → GetVirtualTexture 再锁同一把
    // 非递归 std::mutex ⇒ 自死锁（原生平台第一次 Unregister 就挂死）。WASM 冒烟测试
    // 一直 15/15 是因为 Emscripten 不开 pthreads 时 mutex 是空操作，掩盖了它。
    //
    // 现在：先不持锁卸页（释放槽位、页表项置无效），再持锁摘掉条目，最后在锁外
    // Shutdown（会释放页表纹理，涉及 GPU 销毁，不该持容器锁做）。
    UnloadAllPagesNoFlush(handle);

    std::shared_ptr<VirtualTextureResource> resource;
    {
        std::lock_guard<std::mutex> lock(m_VirtualTexturesMutex);
        auto it = m_VirtualTextures.find(handle);
        if (it == m_VirtualTextures.end())
        {
            return;
        }
        resource = it->second.resource;
        m_VirtualTextures.erase(it);
    }

    if (resource)
    {
        resource->Shutdown();
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
    // enable_mip_streaming=false 把请求钉在 mip 0。这个开关此前是死字段（全仓库没人读）；
    // 现在它有了真实含义：只有 mip 0 的页会被上传（ExtractPagePixels 只抠 mip 0 区域，GPU
    // 页表也只覆盖 mip 0 网格），所以没有 mip 链之前，生产者必须把它关掉，否则请求 mip>0 会
    // 白白占用槽位、挤掉有用的 mip 0 页。
    uint32_t mip_level =
        m_Config.enable_mip_streaming ? resource->CalculateRequiredMipLevel(approx_screen_size, distance, 0.0f) : 0u;

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

            // 幂等：已驻留的不再请求；已在队列里 / 正在准备 / 已准备待提交的不重复入队。
            if (resource->GetPageTableEntry(mip_level, x, y).is_valid)
            {
                continue;
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
                if (!m_PendingPages.insert(page_id).second)
                {
                    continue;
                }
                m_PageLoadQueue.push(task);
            }
        }
    }

    // 旧代码从不 notify（全文件唯一一处 notify 是关停），所以只要 cv.wait 的谓词没被
    // 虚假唤醒碰上，后台线程就一直睡着、实际上从没处理过任何请求。
    m_PageLoadingCv.notify_all();
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

    // 本帧所有驻留变化（加载 + 预算驱逐，可能跨多个 VT）合并成每个脏页表一次上传。
    FlushPageTables();

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

    // 按需重算，而不是返回缓存的 m_Stats —— 此前**没有任何地方写过 m_Stats**
    // （LoadPage / UnloadPage 都不碰它），所以统计恒为默认零值：页明明全加载了，
    // GetStats 却报 loaded=0、pool_total=0。缓存没有写入者就等于死的，这里直接从
    // 资源与页池的当前状态算。
    VirtualTextureStats stats {};

    {
        std::lock_guard<std::mutex> textures_lock(m_VirtualTexturesMutex);
        stats.total_virtual_textures = static_cast<uint32_t>(m_VirtualTextures.size());
        for (const auto& entry_pair : m_VirtualTextures)
        {
            const VirtualTextureEntry& entry = entry_pair.second;
            if (!entry.resource)
            {
                continue;
            }
            for (const auto& page_pair : entry.resource->getAllPages())
            {
                ++stats.total_pages;
                switch (page_pair.second.state)
                {
                    case VirtualTexturePageState::Loaded:
                        ++stats.loaded_pages;
                        break;
                    case VirtualTexturePageState::Loading:
                        ++stats.loading_pages;
                        break;
                    case VirtualTexturePageState::Unloaded:
                        ++stats.unloaded_pages;
                        break;
                    default:
                        break;
                }
            }
        }
    }

    if (m_PhysicalPool)
    {
        stats.physical_pool_total_slots = m_PhysicalPool->getTotalSlots();
        uint32_t used = 0;
        for (uint32_t slot = 0; slot < stats.physical_pool_total_slots; ++slot)
        {
            if (m_PhysicalPool->IsSlotAllocated(slot))
            {
                ++used;
            }
        }
        stats.physical_pool_used_slots = used;
        stats.total_memory_used = static_cast<size_t>(used) * m_PhysicalPool->getPageSize() *
                                  m_PhysicalPool->getPageSize() * 4u;  // 4B/texel (R8G8B8A8)
    }

    stats.memory_budget = m_Config.memory_budget_mb * 1024u * 1024u;
    stats.memory_usage_ratio =
        stats.memory_budget > 0 ? static_cast<float>(stats.total_memory_used) / static_cast<float>(stats.memory_budget)
                                : 0.0f;

    return stats;
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

    // 整批一次：本 VT 的页表 + 被驱逐页所属的其它 VT 的页表。
    FlushPageTables();
    return true;
}

void VirtualTextureManager::UnloadPages(VirtualTextureHandle handle)
{
    UnloadAllPagesNoFlush(handle);
    FlushPageTables();
}

void VirtualTextureManager::FlushPageTables()
{
    if (m_Rhi == nullptr)
    {
        return;  // logical (headless WASM) pool: 没有 GPU 页表可传
    }
    CheckOwnerThread();

    // 锁内只拷 shared_ptr，锁外做 GPU 提交（提交要 vkQueueWaitIdle，不该持容器锁）。
    std::vector<std::shared_ptr<VirtualTextureResource>> resources;
    {
        std::lock_guard<std::mutex> lock(m_VirtualTexturesMutex);
        resources.reserve(m_VirtualTextures.size());
        for (auto& pair : m_VirtualTextures)
        {
            if (pair.second.resource)
            {
                resources.push_back(pair.second.resource);
            }
        }
    }

    for (auto& resource : resources)
    {
        resource->FlushPageTableTexture(m_Rhi);
    }
}

bool VirtualTextureManager::EvictLRUPageForAllocation()
{
    if (!m_PhysicalPool)
    {
        return false;
    }

    const uint32_t victim_slot = m_PhysicalPool->FindLRUSlot();
    if (victim_slot == UINT32_MAX)
    {
        return false;
    }

    const VirtualTexturePageID victim = m_PhysicalPool->GetSlot(victim_slot).page_id;
    UnloadPage(victim, victim.virtual_texture_id);

    // 主人已注销/找不到（UnloadPage 什么都没做）时，仍要把槽位还回去，否则这个
    // 槽位永远"已分配"却没人能释放 —— 池子会一格一格地漏光。
    if (m_PhysicalPool->IsSlotAllocated(victim_slot))
    {
        m_PhysicalPool->FreeSlot(victim_slot);
    }
    return true;
}

void VirtualTextureManager::UnloadAllPagesNoFlush(VirtualTextureHandle handle)
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
        {
            // 置停止标志必须持有 cv 所用的那把锁：否则 worker 刚判完谓词（假）、还没
            // 进入 wait 时我们 notify，这次唤醒就丢了，join 永远等下去。
            std::lock_guard<std::mutex> lock(m_PageLoadQueueMutex);
            m_ShouldStop = true;
        }
        m_PageLoadingCv.notify_all();
        m_PageLoadingThread.join();
    }

    // 丢弃没来得及提交的预备页（它们只是 CPU 缓冲，没有 GPU 对象要释放），连同请求队列与
    // 待处理集合一起清空——三者必须同进同出，否则残留的 pending 项会让之后对同一页的请求
    // 被当成"已在队列里"而永远不再入队。
    std::lock_guard<std::mutex> lock(m_PageLoadQueueMutex);
    while (!m_PreparedQueue.empty())
    {
        m_PreparedQueue.pop();
    }
    m_PageLoadQueue = std::priority_queue<PageLoadTask>();
    m_PendingPages.clear();
    m_WorkerInFlight = 0;
}

void VirtualTextureManager::PageLoadingThreadFunction()
{
    // Worker 线程**只做 CPU 活**：从保留的源像素里把页抠出来（ExtractPagePixels），
    // 交给 owner 线程提交。它从不碰 RHI、不改页状态、不动页池 —— 上一版直接在这里调
    // LoadPage，而 LoadPage 会分配槽位、提交 GPU 命令缓冲，和渲染线程并发就是数据竞争
    // + 并发 queue submit（只是因为从没 notify 过，这条路径从未真的跑起来）。
    //
    // 背压：已准备好但还没提交的页最多 2*max_concurrent_loads 个。每个预备页是一块
    // page_size^2*4 字节的 CPU 缓冲，不设上限的话 worker 会把整张大纹理的所有页都抠
    // 出来堆在内存里，而 owner 线程每帧只提交 max_concurrent_loads 个。
    const size_t backlog_cap = static_cast<size_t>(std::max<uint32_t>(1u, m_Config.max_concurrent_loads)) * 2u;

    for (;;)
    {
        PageLoadTask task;
        {
            std::unique_lock<std::mutex> lock(m_PageLoadQueueMutex);
            m_PageLoadingCv.wait(lock, [&] {
                return m_ShouldStop || (!m_PageLoadQueue.empty() && m_PreparedQueue.size() < backlog_cap);
            });
            if (m_ShouldStop)
            {
                return;
            }
            task = m_PageLoadQueue.top();
            m_PageLoadQueue.pop();
            ++m_WorkerInFlight;  // 取走到入预备队列之间不能"凭空消失"，见头文件注释
        }
        auto finish_without_page = [this, &task] {
            std::lock_guard<std::mutex> lock(m_PageLoadQueueMutex);
            --m_WorkerInFlight;
            m_PendingPages.erase(task.page_id);  // 没进预备队列 ⇒ 不再待处理，允许重新请求
        };

        auto resource = GetVirtualTexture(task.virtual_texture_handle);
        if (!resource)
        {
            finish_without_page();
            continue;  // 已注销
        }

        // RequestPages 每次都把整个 mip 的所有页重新入队，已驻留的页不必再抠一遍。
        // （读页表项带锁；与 owner 线程的竞争只会多抠一页，提交侧还会再按状态过滤。）
        if (resource->GetPageTableEntry(task.page_id.mip_level, task.page_id.page_x, task.page_id.page_y).is_valid)
        {
            finish_without_page();
            continue;
        }

        PreparedPage prepared;
        prepared.page_id = task.page_id;
        prepared.handle = task.virtual_texture_handle;
        // 只有 mip 0 有像素可传（ExtractPagePixels 只抠 mip 0 区域）：对 mip>0 调它会把
        // 错位的区域当成该页的内容传进槽位，比不传更糟。
        prepared.has_pixels = task.page_id.mip_level == 0 &&
                              resource->ExtractPagePixels(task.page_id.page_x,
                                                          task.page_id.page_y,
                                                          prepared.pixels,
                                                          prepared.width,
                                                          prepared.height);

        {
            std::lock_guard<std::mutex> lock(m_PageLoadQueueMutex);
            m_PreparedQueue.push(std::move(prepared));
            --m_WorkerInFlight;  // 与入队同一把锁：任何时刻页都恰好在一个地方
            const uint32_t depth = static_cast<uint32_t>(m_PreparedQueue.size());
            uint32_t peak = m_PreparedBacklogPeak.load();
            while (depth > peak && !m_PreparedBacklogPeak.compare_exchange_weak(peak, depth))
            {
            }
        }
        ++m_AsyncPreparedCount;
    }
}

void VirtualTextureManager::ProcessPageLoadRequests(float delta_time)
{
    // owner 线程调用。每帧最多提交 max_concurrent_loads 页。
    uint32_t processed = 0;
    const uint32_t max_pages_per_frame = m_Config.max_concurrent_loads;

    // 异步模式：worker 已经把像素抠好放进 m_PreparedQueue，这里只做提交
    // （槽位分配 + GPU 上传 + 页表簿记），全部在 owner 线程。
    if (m_PageLoadingThread.joinable())
    {
        while (processed < max_pages_per_frame)
        {
            PreparedPage prepared;
            {
                std::lock_guard<std::mutex> lock(m_PageLoadQueueMutex);
                if (m_PreparedQueue.empty())
                {
                    break;
                }
                prepared = std::move(m_PreparedQueue.front());
                m_PreparedQueue.pop();
                m_PendingPages.erase(prepared.page_id);  // 本次提交后（成败皆然）不再"待处理"
            }

            auto resource = GetVirtualTexture(prepared.handle);
            if (resource)
            {
                VirtualTexturePage* page =
                    resource->GetPage(prepared.page_id.mip_level, prepared.page_id.page_x, prepared.page_id.page_y);
                if (page && page->state == VirtualTexturePageState::Unloaded)
                {
                    LoadPage(prepared.page_id, prepared.handle, &prepared);
                }
            }
            ++processed;
        }
        // 预备队列腾出了位置，worker 可能正卡在背压上。
        m_PageLoadingCv.notify_all();
        return;
    }

    // 同步模式（没有 worker）：原样从请求队列取，行内抠像素 + 提交。
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
            m_PendingPages.erase(task.page_id);
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

void VirtualTextureManager::CheckOwnerThread()
{
    // 只在真有 GPU 的管理器上有意义；逻辑池（InitializeLogical）没有 GPU 工作可违规。
    if (m_Rhi != nullptr && std::this_thread::get_id() != m_OwnerThread)
    {
        ++m_GpuThreadViolations;
    }
}

uint32_t VirtualTextureManager::getPendingLoadCount() const
{
    std::lock_guard<std::mutex> lock(m_PageLoadQueueMutex);
    return static_cast<uint32_t>(m_PageLoadQueue.size() + m_PreparedQueue.size()) + m_WorkerInFlight;
}

void VirtualTextureManager::PumpLoads()
{
    ProcessPageLoadRequests(0.0f);
    FlushPageTables();
}

bool VirtualTextureManager::LoadPage(const VirtualTexturePageID& page_id,
                                     VirtualTextureHandle handle,
                                     const PreparedPage* prepared)
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

    // 没有池就别动页状态：旧代码先把页标成 Loading、再发现没池返回 false，页永远卡在
    // Loading（之后的请求都因 state != Unloaded 被过滤）。
    if (!m_PhysicalPool)
    {
        return false;
    }

    // Update state to loading
    page->state = VirtualTexturePageState::Loading;
    resource->UpdatePageState(page_id, VirtualTexturePageState::Loading);

    // 页数据从**保留下来的源像素**里取（见 VirtualTextureResource::getSourceData），
    // 不再从磁盘读 —— 本工程还没有 VT 页的磁盘/DDC 资产管线，硬造一条"读盘"路径
    // 只会得到一个永远填不满的假实现。真正要传的字节在下面分配槽位之后才动手。
    //
    // 仍缺的两块（如实留白，不伪造）：① mip 链（目前只传 mip 0 的页）；② 走
    // RegisterVirtualTexture(path,...) 建的资源没有源像素，其页只做簿记、槽位留空。

    // Allocate physical slot
    if (!m_PhysicalPool)
    {
        return false;
    }

    uint32_t physical_slot = m_PhysicalPool->AllocateSlot(page_id);
    if (physical_slot == UINT32_MAX && EvictLRUPageForAllocation())
    {
        // 池满：AllocateSlot 不再偷槽位，由这里经 UnloadPage 驱逐 LRU 页（其主人的
        // 页表随之标脏、批末统一刷新），然后重试一次。
        physical_slot = m_PhysicalPool->AllocateSlot(page_id);
    }
    if (physical_slot == UINT32_MAX)
    {
        page->state = VirtualTexturePageState::Unloaded;
        resource->UpdatePageState(page_id, VirtualTexturePageState::Unloaded);
        return false;
    }

    // Update page
    page->physical_slot = physical_slot;
    page->state = VirtualTexturePageState::Loaded;
    resource->UpdatePageState(page_id, VirtualTexturePageState::Loaded);

    // Stamp the access clock so the LRU evictor has a real ordering key (was:
    // every loaded slot tied at last_access_time == 0). 用单调计数而非毫秒时钟：
    // 同一毫秒内加载的一批页时间戳曾全部相同，LRU 退化成"槽位下标最小者"。
    const uint64_t now = ++m_AccessTick;
    page->last_access_time = now;
    m_PhysicalPool->UpdateSlotAccessTime(physical_slot, now);

    // Update page table entry
    PageTableEntry entry;
    entry.physical_slot = physical_slot;
    entry.mip_level = page_id.mip_level;
    entry.is_valid = true;
    resource->SetPageTableEntry(page_id.mip_level, page_id.page_x, page_id.page_y, entry);

    // ---- 把页像素真正传进物理页池（GPU 侧）----
    // 有源像素才传；没有（path 方式注册的资源）就只做簿记，槽位留空 —— 如实留白，
    // 不伪造一份"看起来传过了"的数据。
    if (prepared != nullptr)
    {
        // 异步路径：像素已由 worker 抠好，这里（owner 线程）只负责上传。
        if (prepared->has_pixels)
        {
            UploadPageToPhysicalPool(
                page_id, handle, prepared->pixels.data(), prepared->width, prepared->height);
        }
    }
    else
    {
        std::vector<uint8_t> page_pixels;
        uint32_t page_pixel_width = 0;
        uint32_t page_pixel_height = 0;
        if (page_id.mip_level == 0 && resource->ExtractPagePixels(
                page_id.page_x, page_id.page_y, page_pixels, page_pixel_width, page_pixel_height))
        {
            UploadPageToPhysicalPool(page_id, handle, page_pixels.data(), page_pixel_width, page_pixel_height);
        }
    }

    // 页表不在这里逐页上传：SetPageTableEntry 已标脏，由批入口（ForceLoadPages /
    // UnloadPages / Update / PumpLoads）末尾的 FlushPageTables 每个脏表传一次。
    // LoadPage 本身只在 owner 线程跑：异步模式下 worker 只抠像素（见 PumpLoads 的线程契约）。

    return true;
}

void VirtualTextureManager::UploadPageToPhysicalPool(const VirtualTexturePageID& page_id,
                                                     VirtualTextureHandle handle,
                                                     const void* page_data,
                                                     uint32_t page_width,
                                                     uint32_t page_height)
{
    if (m_Rhi == nullptr || !m_PhysicalPool || page_data == nullptr || page_width == 0 || page_height == 0)
    {
        return;
    }

    RHIImage* pool_image = m_PhysicalPool->getPhysicalTexture();
    if (pool_image == nullptr)
    {
        return;  // logical-only pool (InitializeLogical): 纯 CPU 簿记，没有 GPU 图可传
    }
    CheckOwnerThread();

    auto resource = GetVirtualTexture(handle);
    if (!resource)
    {
        return;
    }

    const uint32_t bytes_per_pixel = resource->getBytesPerPixel();
    if (bytes_per_pixel == 0)
    {
        return;
    }

    VirtualTexturePage* page = resource->GetPage(page_id.mip_level, page_id.page_x, page_id.page_y);
    if (!page || page->physical_slot == UINT32_MAX)
    {
        return;
    }

    // 槽位坐标是**页单位**（GetSlotCoordinates：x = index % widthPages），
    // 乘 page_size 才是像素偏移。
    uint32_t slot_x = 0;
    uint32_t slot_y = 0;
    m_PhysicalPool->GetSlotCoordinates(page->physical_slot, slot_x, slot_y);

    const uint32_t page_size = m_PhysicalPool->getPageSize();
    if (page_width > page_size || page_height > page_size)
    {
        return;
    }

    const uint32_t offset_x = slot_x * page_size;
    const uint32_t offset_y = slot_y * page_size;

    // 传**整页块**（page_size × page_size），不是 page_width × page_height。
    // 原因：UploadImageRegion 按"紧凑行距 = width"拷贝，而源缓冲的行距是 page_size
    // （ExtractPagePixels 对边缘页补了 0）。若按真实宽高传，残缺页从第二行起就会
    // 错位。整页传则行距恒等于 page_size，与缓冲布局一致，补的 0 落在槽位尾部
    // （采样不会碰到，UV 只映射真实区域）。
    const size_t data_bytes =
        static_cast<size_t>(page_size) * static_cast<size_t>(page_size) * bytes_per_pixel;

    // discard 只在页池还从没传过任何页时为 true。之后必须 false —— 否则 barrier 的
    // oldLayout=UNDEFINED 会把此前已驻留的所有页静默作废。
    const bool discard_prior_content = !m_PhysicalPool->hasContent();
    if (UploadImageRegion(m_Rhi,
                          pool_image,
                          page_data,
                          data_bytes,
                          offset_x,
                          offset_y,
                          page_size,
                          page_size,
                          bytes_per_pixel,
                          discard_prior_content))
    {
        m_PhysicalPool->markContentUploaded();
    }
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

            float screen_size = CalculatePageScreenSize(page.id, *resource, camera_position,
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
                                                     const VirtualTextureResource& resource,
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
    if (screen_height == 0 || screen_width == 0)
    {
        return 0.0f;
    }

    uint32_t pages_x = 0, pages_y = 0;
    resource.CalculatePagesForMip(page_id.mip_level, pages_x, pages_y);
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