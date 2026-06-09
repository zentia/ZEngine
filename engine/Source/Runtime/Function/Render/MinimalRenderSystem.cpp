#include "MinimalRenderSystem.h"

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Function/Render/Interface/RHI.h"
#include "Runtime/Function/Render/Passes/UIPass.h"
#include "Runtime/UI/UISystem.h"

#include <cstring>

// ---------------------------------------------------------------------------
//  MinimalRenderSystem
// ---------------------------------------------------------------------------
//  独立工具（TexPreview 等）专用渲染系统。
//  只配置 UIPass，不加载 3D 渲染通道。
// ---------------------------------------------------------------------------

std::vector<std::type_index> MinimalRenderSystem::GetDependencies() const
{
    // 只依赖 RHI。UISystem 依赖 RenderSystem（即我们），如果我们也依赖
    // UISystem 就形成循环依赖。Initialize() 中通过 GET_SYSTEM(UISystem)
    // 获取指针（此时 UISystem 已注册但未初始化），仅存储指针；
    // 真正使用 UISystem 的 PreRender() 在 Tick() 中调用，此时所有系统
    // 均已初始化完毕。
    return {GET_SYSTEM_TYPE(RHI)};
}

bool MinimalRenderSystem::Initialize()
{
    m_Rhi = GET_SYSTEM(RHI);
    if (m_Rhi == nullptr)
    {
        LOG_ERROR(ZRender, "MinimalRenderSystem: RHI not found");
        return false;
    }

    // 根据 RHI 类型初始化 UI 渲染通道
#if defined(_WIN32)
    if (m_Rhi->getGraphicsAPI() == GraphicsAPI::DirectX12)
    {
        if (!InitializeDX12())
        {
            LOG_ERROR(ZRender, "MinimalRenderSystem: DX12 UI init failed");
            return false;
        }
    }
#endif

#if defined(Z_HAS_VULKAN)
    if (m_Rhi->getGraphicsAPI() == GraphicsAPI::Vulkan)
    {
        if (!InitializeVulkan())
        {
            LOG_ERROR(ZRender, "MinimalRenderSystem: Vulkan UI init failed");
            return false;
        }
    }
#endif

    // 将 UISystem 绑定到 UIPass
    // UISystem 继承自 WindowUI，所以可以隐式转换为 WindowUI*
    auto* ui_system = GET_SYSTEM(UISystem);
    if (m_UiPass && ui_system)
    {
        m_UiPass->InitializeUIRenderBackend(ui_system);
    }

    LOG_INFO(ZRender, "MinimalRenderSystem: initialized (API={})",
             static_cast<int>(m_Rhi->getGraphicsAPI()));
    return true;
}

void MinimalRenderSystem::Tick(float)
{
    if (m_Rhi == nullptr)
    {
        return;
    }

    m_Rhi->PrepareContext();

    // Open command list, transition backbuffer PRESENT→RENDER_TARGET,
    // bind swapchain RTV, and clear the render target.
    // Returns true if the frame should be skipped (swapchain not ready).
    if (m_Rhi->PrepareBeforePass(nullptr))
    {
        return;
    }

    // 执行 UI 渲染
    RenderUI();

    // Transition backbuffer RENDER_TARGET→PRESENT, close command list,
    // execute on GPU, and present the swapchain.
    m_Rhi->SubmitRendering(nullptr);
}

void MinimalRenderSystem::Shutdown()
{
    m_UiPass.reset();
    m_Rhi = nullptr;
}

// ---------------------------------------------------------------------------
//  DX12 初始化
// ---------------------------------------------------------------------------
#ifdef _WIN32
bool MinimalRenderSystem::InitializeDX12()
{
    // 创建 UIPass
    m_UiPass = std::make_shared<UIPass>();

    // 对于 DX12，我们尝试用 nullptr 初始化 UIPass。
    // DX12 不使用 render pass 对象（不像 Vulkan），
    // Draw() 内部会通过 RHI 接口设置 RTV。
    UIPassInitInfo ui_init_info {};
    ui_init_info.render_pass = nullptr;

    RenderPassCommonInfo common_info {};
    common_info.rhi = m_Rhi;
    m_UiPass->SetCommonInfo(common_info);
    m_UiPass->Initialize(&ui_init_info);

    LOG_INFO(ZRender, "MinimalRenderSystem: DX12 UI pass initialized (pipeline_ready={})",
             m_UiPass->IsPipelineReady());
    return true;
}
#endif  // _WIN32

// ---------------------------------------------------------------------------
//  Vulkan 初始化
// ---------------------------------------------------------------------------
#ifdef Z_HAS_VULKAN
bool MinimalRenderSystem::InitializeVulkan()
{
    // Vulkan 需要一个有效的 VkRenderPass
    // TODO: 实现最小化的 render_pass 创建
    LOG_WARNING(ZRender, "MinimalRenderSystem: Vulkan path not yet implemented");
    return false;
}
#endif  // Z_HAS_VULKAN

// ---------------------------------------------------------------------------
//  RenderUI：在 Tick() 中调用，执行 UI 渲染
// ---------------------------------------------------------------------------
void MinimalRenderSystem::RenderUI()
{
    if (m_Rhi == nullptr || m_UiPass == nullptr)
    {
        return;
    }

    if (!m_UiPass->IsPipelineReady())
    {
        return;
    }

    // 获取 swapchain 尺寸
    const RHIExtent2D& extent = m_Rhi->GetSwapchainInfo().extent;
    if (extent.width == 0 || extent.height == 0)
    {
        return;
    }

    // 对于 DX12：UIPass::Draw() 内部会设置 RTV 并渲染
    // 对于 Vulkan：需要确保 render pass 已经 begun
    //
    // 关键：Draw() 内部会调用 m_WindowUi->PreRender()
    // （记录 UI 命令到 UiRenderBatch），然后上传并绘制。

    // 调用 UIPass::Draw() 渲染 UI
    m_UiPass->Draw();
}


