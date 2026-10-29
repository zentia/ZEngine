#include "VirtualTextureProducer.h"

#include "Runtime/Core/Math/AxisAligned.h"
#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Function/Render/Interface/RHI.h"
#include "Runtime/Function/Render/RenderType.h"
#include "VirtualTextureManager.h"
#include "VirtualTextureResource.h"

#if ZENGINE_VT_GPU
    #include "Runtime/Core/Base/Macro.h"
#endif

#include <algorithm>

namespace
{
    // 只有 RGBA8 的源能切成页（ExtractPagePixels / 页池都是 RGBA8）。cooked 的 BC7/ASTC 纹理
    // 是 4x4 块压缩，按像素行列去切会把块边界切碎——所以生产者加载时一律 allow_cooked=false，
    // 这里再核一次，拒绝悄悄把不认识的格式当 RGBA8 切。
    bool IsPageableFormat(const TextureData& texture)
    {
        return texture.IsValid() && texture.m_Width > 0 && texture.m_Height > 0 &&
               (texture.m_Format == RHIFormat::RHI_FORMAT_R8G8B8A8_UNORM ||
                texture.m_Format == RHIFormat::RHI_FORMAT_R8G8B8A8_SRGB);
    }
}  // namespace

VirtualTextureProducer::~VirtualTextureProducer()
{
    // 不在析构里 Shutdown：那会在 RHI 可能已销毁之后访问它。调用方必须显式 Shutdown。
    // 这里只断言式地保证不泄漏句柄——真正的释放在 Shutdown。
}

void VirtualTextureProducer::Bind(VirtualTextureManager* manager,
                                  TextureLoader loader,
                                  void* loader_user,
                                  const Config& config)
{
    m_Manager = manager;
    m_Loader = loader;
    m_LoaderUser = loader_user;
    m_Config = config;
}

void VirtualTextureProducer::ApplyManagerConfig()
{
    if (m_ManagerConfigApplied || m_Manager == nullptr)
    {
        return;
    }
    m_ManagerConfigApplied = true;

    // 默认 VirtualTextureConfig 要 2048x2048 页 = 262144x262144 px = 256 GiB 的池图，任何设备都
    // 放不下。生产者给一个量得出来的小池（默认 16x16 页 = 2048 px = 16 MiB）。
    VirtualTextureConfig config = m_Manager->getConfig();
    config.page_size = m_Config.page_size;
    config.physical_pool_width = m_Config.pool_pages_per_side;
    config.physical_pool_height = m_Config.pool_pages_per_side;
    config.max_concurrent_loads = m_Config.max_concurrent_loads;
    // 没有 mip 链之前只有 mip 0 的页能上传：关掉 mip 流送，请求才会钉在 mip 0。
    config.enable_mip_streaming = false;
    config.page_border = m_Config.page_border;
    config.resident_mip_chain = m_Config.resident_mip_chain;
    config.memory_budget_mb =
        std::max<size_t>(1, (static_cast<size_t>(m_Config.pool_pages_per_side) * m_Config.pool_pages_per_side *
                             (m_Config.page_size + 2u * m_Config.page_border) *
                             (m_Config.page_size + 2u * m_Config.page_border) * 4u +
                         (1024u * 1024u - 1u)) /  // 向上取整：预算比整池小一点，满池时会被预算驱逐误伤
                                (1024u * 1024u));
    m_Manager->SetConfig(config);
}

bool VirtualTextureProducer::EnsureManager(RHI* rhi, RenderResourceBase* render_resource)
{
    if (m_Manager == nullptr || rhi == nullptr)
    {
        return false;
    }
    if (m_Manager->getPhysicalTexturePool() != nullptr)
    {
        return true;  // 已经有人（本类或别人）把它拉起来了
    }

    // 管理器的 Initialize 只放行 Vulkan / DX12（Metal、WebGL2 没有 VT GPU 路径）。
    if (!m_Manager->Initialize(rhi, render_resource))
    {
#if ZENGINE_VT_GPU
        LOG_ERROR(ZRender,
                  "virtual texture producer: manager initialisation failed (virtual textures need a Vulkan or "
                  "DX12 backend and a pool that fits the device); streaming is disabled for this path");
#endif
        return false;
    }
    m_BroughtUpManager = true;
    return true;
}

void VirtualTextureProducer::RetireHandle(uint64_t handle)
{
    if (handle == 0)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(m_HandoffMutex);
    // Stop advertising the handle to RHI-thread consumers in the same critical section that queues it
    // for destruction (see getGpuVisibleHandle()).
    if (m_GpuVisibleHandle == handle)
    {
        m_GpuVisibleHandle = 0;
    }
    m_Retired.push_back(handle);
}

uint64_t VirtualTextureProducer::getGpuVisibleHandle() const
{
    std::lock_guard<std::mutex> lock(m_HandoffMutex);
    return m_GpuVisibleHandle;
}

void VirtualTextureProducer::ReleaseTexture()
{
    // RENDER 线程：不能在这里 Unregister——它会销毁页表纹理（GPU 图），而 RHI 线程可能正在用它做
    // Update。只把句柄交给 RHI 线程，由 TickGpu 销毁。
    RetireHandle(m_Handle);
    m_Handle = 0;
    m_ActivePath.clear();
    m_Settled = false;
}

bool VirtualTextureProducer::TickCpu(bool enabled,
                                     const std::string& source_path,
                                     const Vector3& camera_position,
                                     const Vector3& camera_forward)
{
    if (m_Manager == nullptr)
    {
        return false;
    }

    // ---- 关掉 / 路径清空：释放纹理，保持管理器（下次开启不必重建池）----
    if (!enabled || source_path.empty())
    {
        if (m_Handle != 0)
        {
            ReleaseTexture();
        }
        m_FailedPath.clear();  // 关掉再打开应当重新尝试
        return false;
    }

    // ---- 路径变了：换纹理 ----
    if (m_Handle != 0 && source_path != m_ActivePath)
    {
        ReleaseTexture();
    }

    // ---- RHI 线程报告池建不起来：这是配置问题，换路径也不会好，记忆住别每帧重试 ----
    {
        std::lock_guard<std::mutex> lock(m_HandoffMutex);
        if (m_PoolFailed && m_Handle == 0 && source_path != m_FailedPath)
        {
            m_FailedPath = source_path;
            ++m_FailureCount;
        }
    }

    // ---- 这个路径之前失败过：不再每帧重试（每帧重读磁盘 + 刷日志）----
    if (m_Handle == 0 && source_path == m_FailedPath)
    {
        return false;
    }

    if (m_Handle == 0)
    {
        std::shared_ptr<TextureData> texture = m_Loader ? m_Loader(m_LoaderUser, source_path) : nullptr;
        if (!texture || !IsPageableFormat(*texture))
        {
#if ZENGINE_VT_GPU
            LOG_ERROR(ZRender,
                      "virtual texture producer: cannot stream '{}' ({})",
                      source_path,
                      texture ? "not an uncompressed RGBA8 image" : "failed to load");
#endif
            m_FailedPath = source_path;
            ++m_FailureCount;
            return false;
        }

        // RegisterVirtualTexture 是纯 CPU（只建资源 + 页表项 + 登记，管理器内部有锁），所以可以在
        // 池还没建的时候就做；页会留在队列里，等 RHI 线程建好池后被提交。
        ApplyManagerConfig();
        m_Handle = m_Manager->RegisterVirtualTexture(texture, m_Config.page_size);
        if (m_Handle == 0)
        {
            m_FailedPath = source_path;
            ++m_FailureCount;
            return false;
        }
        m_ActivePath = source_path;
        m_FailedPath.clear();
        m_FrameCounter = 0;
        m_Settled = false;
        {
            std::lock_guard<std::mutex> lock(m_HandoffMutex);
            m_WantPool = true;  // 告诉 RHI 线程：有纹理要流送了，需要池
            m_GpuVisibleHandle = m_Handle;
        }
#if ZENGINE_VT_GPU
        auto resource = m_Manager->GetVirtualTexture(m_Handle);
        if (resource)
        {
            LOG_INFO(ZRender,
                     "virtual texture producer: streaming '{}' ({}x{} px, {}x{} pages of {} px)",
                     source_path,
                     resource->getVirtualWidth(),
                     resource->getVirtualHeight(),
                     resource->getPagesX(),
                     resource->getPagesY(),
                     m_Config.page_size);
        }
#endif
    }

    // ---- 保持页被请求 ----
    // 把纹理锚在相机前方 anchor_distance 处。**不是**真实的世界位置——引擎里还没有哪个物体
    // 引用这张虚拟纹理，没有真实 footprint 可投影；锚点只用来驱动"屏幕占比 → 优先级"这条
    // 现有逻辑，让 Update 的优先级/距离闸门有合理输入。RequestPages 是幂等的，每帧调用代价
    // 只是一遍页表项检查。
    Vector3 forward = camera_forward;
    if (forward.length() < 1.0e-4f)
    {
        forward = Vector3(0.0f, 0.0f, -1.0f);
    }
    forward.normalise();
    const Vector3 object_position = camera_position + forward * m_Config.anchor_distance;
    const Vector3 half(m_Config.anchor_half_extent, m_Config.anchor_half_extent, 0.01f);
    m_Manager->RequestPages(m_Handle, camera_position, object_position, AxisAlignedBox(object_position, half), 1.0f);

    // ---- 周期性汇报驻留情况（流送"落定"时记一条，之后按间隔复述）----
    ++m_FrameCounter;
    uint32_t resident = 0;
    uint32_t total = 0;
    GetResidency(resident, total);
    const bool complete = total > 0 && resident == total;
    const bool log_now = (complete && !m_Settled) ||
                         (m_Config.log_every_frames != 0 && m_FrameCounter % m_Config.log_every_frames == 0);
    if (complete)
    {
        m_Settled = true;
    }
#if ZENGINE_VT_GPU
    if (log_now)
    {
        LOG_INFO(ZRender,
                 "virtual texture producer: '{}' {}/{} pages resident (pending loads {}; GPU page uploads ok {} "
                 "failed {})",
                 m_ActivePath,
                 resident,
                 total,
                 m_Manager->getPendingLoadCount(),
                 m_Manager->getPageUploadCount(),
                 m_Manager->getPageUploadFailureCount());
    }
#else
    (void)log_now;
#endif
    return true;
}

void VirtualTextureProducer::TickGpu(RHI* rhi,
                                     RenderResourceBase* render_resource,
                                     const Vector3& camera_position,
                                     const Vector3& camera_forward,
                                     float fov_radians,
                                     float aspect,
                                     uint32_t screen_width,
                                     uint32_t screen_height,
                                     float delta_time)
{
    if (m_Manager == nullptr || rhi == nullptr)
    {
        return;
    }

    // ---- 1. 销毁渲染线程不要了的纹理（含它的页表 GPU 图）----
    std::vector<uint64_t> retired;
    bool want_pool = false;
    {
        std::lock_guard<std::mutex> lock(m_HandoffMutex);
        retired.swap(m_Retired);
        want_pool = m_WantPool;
    }
    for (uint64_t handle : retired)
    {
        m_Manager->UnregisterVirtualTexture(handle);
    }

    // ---- 2. 需要时建池（只尝试一次，失败回报给渲染线程）----
    if (want_pool && m_Manager->getPhysicalTexturePool() == nullptr)
    {
        bool already_failed = false;
        {
            std::lock_guard<std::mutex> lock(m_HandoffMutex);
            already_failed = m_PoolFailed;
        }
        if (!already_failed && !EnsureManager(rhi, render_resource))
        {
            std::lock_guard<std::mutex> lock(m_HandoffMutex);
            m_PoolFailed = true;
        }
    }

    // ---- 3. 池就绪后提交页（上传 + 页表刷新），并推进优先级 / 驱逐 ----
    if (m_Manager->getPhysicalTexturePool() != nullptr)
    {
        m_Manager->Update(camera_position, camera_forward, fov_radians, aspect, screen_width, screen_height, delta_time);
    }
}

bool VirtualTextureProducer::Tick(RHI* rhi,
                                  RenderResourceBase* render_resource,
                                  bool enabled,
                                  const std::string& source_path,
                                  const Vector3& camera_position,
                                  const Vector3& camera_forward)
{
    // 单线程调用者（测试、单线程渲染路径）：两半背靠背，语义与拆分前一致——纹理在 Tick 返回前就
    // 已注册且池已建（旧实现先建池再注册；这里先注册再建池，但同一次调用内两者都完成）。
    //
    // 注意这里**不**调用 Manager.Update：旧 Tick 也不调，Update 由调用方（渲染系统的帧钩子 / 测试
    // 的 Frame::Run）自己调。只有 TickGpu 在 RHI 线程路径上替调用方做这件事。
    const bool streaming = TickCpu(enabled, source_path, camera_position, camera_forward);

    std::vector<uint64_t> retired;
    bool want_pool = false;
    {
        std::lock_guard<std::mutex> lock(m_HandoffMutex);
        retired.swap(m_Retired);
        want_pool = m_WantPool;
    }
    for (uint64_t handle : retired)
    {
        m_Manager->UnregisterVirtualTexture(handle);
    }
    if (want_pool && m_Manager->getPhysicalTexturePool() == nullptr && rhi != nullptr)
    {
        bool already_failed = false;
        {
            std::lock_guard<std::mutex> lock(m_HandoffMutex);
            already_failed = m_PoolFailed;
        }
        if (!already_failed && !EnsureManager(rhi, render_resource))
        {
            std::lock_guard<std::mutex> lock(m_HandoffMutex);
            m_PoolFailed = true;
        }
    }
    return streaming;
}

void VirtualTextureProducer::GetResidency(uint32_t& resident, uint32_t& total) const
{
    resident = 0;
    total = 0;
    if (m_Manager == nullptr || m_Handle == 0)
    {
        return;
    }
    auto resource = m_Manager->GetVirtualTexture(m_Handle);
    if (!resource)
    {
        return;
    }
    total = resource->getPagesX() * resource->getPagesY();
    for (uint32_t y = 0; y < resource->getPagesY(); ++y)
    {
        for (uint32_t x = 0; x < resource->getPagesX(); ++x)
        {
            if (resource->GetPageTableEntry(0, x, y).is_valid)
            {
                ++resident;
            }
        }
    }
}

void VirtualTextureProducer::Shutdown()
{
    // 调用约定：此时渲染线程已不会再 TickCpu、RHI 线程已不会再 TickGpu（关停路径上两者都已排空），
    // 所以这里可以同时碰两边的状态。ReleaseTexture 现在只是把句柄入队，必须随后**自己**把队列
    // 排空——否则下面拆池之后，这些句柄的页表纹理就泄漏了（没人再会 Unregister 它们）。
    ReleaseTexture();
    std::vector<uint64_t> retired;
    {
        std::lock_guard<std::mutex> lock(m_HandoffMutex);
        retired.swap(m_Retired);
        m_WantPool = false;
        m_PoolFailed = false;
    }
    if (m_Manager != nullptr)
    {
        for (uint64_t handle : retired)
        {
            m_Manager->UnregisterVirtualTexture(handle);
        }
    }
    m_FailedPath.clear();
    m_ManagerConfigApplied = false;  // 下次开始流送时重新把本生产者的布局推给管理器
    if (m_BroughtUpManager && m_Manager != nullptr)
    {
        // 只拆自己拉起来的：别人初始化的管理器不归我们关。
        m_Manager->Shutdown();
        m_BroughtUpManager = false;
    }
}
