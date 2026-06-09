#pragma once

#include "Runtime/Function/Render/RenderSystem.h"
#include "Runtime/Function/Render/Passes/UIPass.h"
#include "Runtime/Function/Render/Interface/RHI.h"

#include <memory>

// MinimalRenderSystem：独立工具（如 ASTCPreview）专用渲染系统。
//
// 只配置 UIPass，不加载 3D 渲染通道（MainCameraPass、ShadowPass 等）。
// 通过 OOP 重载 Initialize() 和 Tick()，实现最小 UI 渲染循环。
//
// 架构：
//   RegisterRuntimeLight() 注册 MinimalRenderSystem 代替 RenderSystem。
//   MinimalRenderSystem::Initialize() 只创建 UIPass 并初始化 UI 渲染后端。
//   MinimalRenderSystem::Tick() 只执行 PrepareContext + UI Present + PollEvents。
//
// 注意：此类不包含 3D 网格渲染、阴影、后处理等。适用于纯 UI 工具。

class MinimalRenderSystem : public RenderSystem
{
public:
    std::string GetName() const override { return "MinimalRenderSystem"; }

    // 依赖：RHI（渲染硬件接口）、UISystem（UI 系统）
    std::vector<std::type_index> GetDependencies() const override;

    // 与基类 RenderSystem 同阶段，确保 DebugDrawManager 等依赖 RenderSystem
    // 的系统能在 Rendering 阶段找到已初始化的 RenderSystem。
    // UISystem 不在本类依赖中（避免循环），其指针在 Initialize() 中获取、
    // 真正调用在 Tick() 中（此时所有系统已初始化完毕）。
    SystemInitPhase GetInitPhase() const override { return SystemInitPhase::Rendering; }

    // 重载 Initialize()：只配置 UIPass，不加载 3D 渲染通道
    bool Initialize() override;

    // 重载 Tick()：只做 UI 渲染，不做 3D 渲染
    void Tick(float delta_time) override;

    // 重载 Shutdown()：清理 UIPass
    void Shutdown() override;

private:
    // 初始化 DX12 模式下的 UI 渲染通道
    bool InitializeDX12();

    // 初始化 Vulkan 模式下的 UI 渲染通道
    bool InitializeVulkan();

    // 执行 UI 渲染（被 Tick() 调用）
    void RenderUI();

private:
    // 自己持有 RHI 指针（RenderSystem::m_Rhi 是 private，无法直接访问）
    RHI* m_Rhi {nullptr};

    // UIPass 由 MinimalRenderSystem 自己持有，不依赖 RenderSystem 的 m_RenderPipeline
    std::shared_ptr<UIPass> m_UiPass;
};


