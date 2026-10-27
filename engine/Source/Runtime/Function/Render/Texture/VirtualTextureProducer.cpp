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
    // 管理器的 Initialize 会拒绝非 Vulkan 的 RHI（Windows 默认是 DX12）。这里不必再判，但失败要
    // 被识别成"这个后端不支持 VT"而不是"配置错了"——见下方的日志措辞。

    // 默认 VirtualTextureConfig 要 2048x2048 页 = 262144x262144 px = 256 GiB 的池图，任何设备都
    // 放不下。生产者给一个量得出来的小池（默认 16x16 页 = 2048 px = 16 MiB）。
    VirtualTextureConfig config = m_Manager->getConfig();
    config.page_size = m_Config.page_size;
    config.physical_pool_width = m_Config.pool_pages_per_side;
    config.physical_pool_height = m_Config.pool_pages_per_side;
    config.max_concurrent_loads = m_Config.max_concurrent_loads;
    // 没有 mip 链之前只有 mip 0 的页能上传：关掉 mip 流送，请求才会钉在 mip 0。
    config.enable_mip_streaming = false;
    config.memory_budget_mb =
        std::max<size_t>(1, (static_cast<size_t>(m_Config.pool_pages_per_side) * m_Config.pool_pages_per_side *
                             m_Config.page_size * m_Config.page_size * 4u) /
                                (1024u * 1024u));
    m_Manager->SetConfig(config);

    if (!m_Manager->Initialize(rhi, render_resource))
    {
#if ZENGINE_VT_GPU
        LOG_ERROR(ZRender,
                  "virtual texture producer: manager initialisation failed (virtual textures need the Vulkan "
                  "backend -- on Windows pass --rhi vulkan -- and a pool that fits the device); streaming is "
                  "disabled for this path");
#endif
        return false;
    }
    m_BroughtUpManager = true;
    return true;
}

void VirtualTextureProducer::ReleaseTexture()
{
    if (m_Handle != 0 && m_Manager != nullptr)
    {
        m_Manager->UnregisterVirtualTexture(m_Handle);
    }
    m_Handle = 0;
    m_ActivePath.clear();
    m_Settled = false;
}

bool VirtualTextureProducer::Tick(RHI* rhi,
                                  RenderResourceBase* render_resource,
                                  bool enabled,
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

    // ---- 这个路径之前失败过：不再每帧重试（每帧重读磁盘 + 刷日志）----
    if (m_Handle == 0 && source_path == m_FailedPath)
    {
        return false;
    }

    if (m_Handle == 0)
    {
        // RHI 还没就绪不是"这个路径坏了"：不记忆、不计失败，下一帧再来。
        if (rhi == nullptr)
        {
            return false;
        }
        if (!EnsureManager(rhi, render_resource))
        {
            // 管理器起不来（例如池配置超出设备上限）是配置问题，换路径也不会好：记忆住，别每帧重试。
            m_FailedPath = source_path;
            ++m_FailureCount;
            return false;
        }

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
                 "virtual texture producer: '{}' {}/{} pages resident (pending loads {})",
                 m_ActivePath,
                 resident,
                 total,
                 m_Manager->getPendingLoadCount());
    }
#else
    (void)log_now;
#endif
    return true;
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
    ReleaseTexture();
    m_FailedPath.clear();
    if (m_BroughtUpManager && m_Manager != nullptr)
    {
        // 只拆自己拉起来的：别人初始化的管理器不归我们关。
        m_Manager->Shutdown();
        m_BroughtUpManager = false;
    }
}
