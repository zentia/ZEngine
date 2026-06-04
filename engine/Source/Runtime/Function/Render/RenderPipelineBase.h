#pragma once

#include "Runtime/Core/Math/Vector2.h"
#include "Runtime/Function/Render/RenderPassBase.h"
#include "Runtime/Function/Render/RenderingThread/RHIDrawList.h"

#include <array>
#include <functional>
#include <memory>
#include <vector>

class RHI;
class RHIRenderPass;
class RenderResourceBase;
class RenderSwapContext;
class WindowUI;
class RenderPipelineModule;
class DesktopRenderPipelineModule;
class MobileRenderPipelineModule;

struct RenderSwapData;

struct RenderPipelineInitInfo
{
    bool enable_fxaa {false};
    std::shared_ptr<RenderResourceBase> render_resource;
    std::string ui_pass_name;
};

class RenderPipelineBase
{
    friend class RenderSystem;
    // Render-path modules own the pass assembly + draw-list build, operating on
    // this host's pass slots / shared state. They are the only other class family
    // allowed to touch the protected pass members directly.
    friend class RenderPipelineModule;
    friend class DesktopRenderPipelineModule;
    friend class MobileRenderPipelineModule;

public:
    // 渲染回调类型
    using RenderCallback = std::function<void()>;

    virtual ~RenderPipelineBase() = default;

    virtual void clear() {};

    virtual void Initialize(RenderPipelineInitInfo init_info) = 0;

    virtual void PreparePassData(std::shared_ptr<RenderResourceBase> render_resource);
    virtual void BuildDrawLists(std::shared_ptr<RenderResourceBase> render_resource, RHIDrawList& out_draw_list);
    virtual void SubmitDrawLists(RHI* rhi,
                                 std::shared_ptr<RenderResourceBase> render_resource,
                                 const RHIDrawList& draw_list);
    virtual void DeferredRender(RHI* rhi, std::shared_ptr<RenderResourceBase> render_resource);

    // Consume logic-thread particle swap payloads on the render thread. Default
    // implementation only clears pending flags (DX12 / backends without ParticlePass).
    virtual void ConsumeParticleSwapData(RenderSwapData& swap_data, RenderSwapContext& swap_context);

    void InitializeUIRenderBackend(WindowUI* window_ui);
    virtual uint32_t GetGuidOfPickedMesh(const Vector2& picked_uv) = 0;

    void SetAxisVisibleState(bool state);
    bool IsAxisVisibleState() const;
    void SetSkyboxVisibleState(ViewportType viewport_type, bool state);
    bool IsSkyboxVisibleState(ViewportType viewport_type) const;
    void SetSelectedAxis(size_t selected_axis);
    size_t GetSelectedAxis() const;

    // UI 渲染后的回调注册接口（供 Editor 注入 ImGui 渲染）
    void registerPostUIRenderCallback(RenderCallback callback)
    {
        m_PostUiRenderCallbacks.push_back(std::move(callback));
    }

    void clearPostUIRenderCallbacks() { m_PostUiRenderCallbacks.clear(); }

    // 获取回调列表（供 MainCameraPass 调用）
    const std::vector<RenderCallback>& getPostUIRenderCallbacks() const { return m_PostUiRenderCallbacks; }

    // 获取 RHI（供 Editor 创建 UI Pass 使用）
    RHI* GetRHI() const { return m_Rhi; }

    // 获取 UI subpass 使用的 render pass（供 Editor 创建 UI Pass 使用）
    virtual RHIRenderPass* GetUIRenderPass() const { return nullptr; }

    // RP2 UI subpass color target (backup_even). nullptr when not on the main-camera path.
    virtual RHIImageView* GetUiLayerColorView() const { return nullptr; }

    void registerFramebufferRecreateCallback(RenderCallback callback)
    {
        m_FramebufferRecreateCallbacks.push_back(std::move(callback));
    }

    void clearFramebufferRecreateCallbacks() { m_FramebufferRecreateCallbacks.clear(); }

    // Invoked when SubmitDrawLists bails before post-UI callbacks (swapchain not ready, etc.).
    void registerSkippedFrameCallback(RenderCallback callback)
    {
        m_SkippedFrameCallbacks.push_back(std::move(callback));
    }

    void clearSkippedFrameCallbacks() { m_SkippedFrameCallbacks.clear(); }

    void notifySkippedFrameRender() const;

    const std::vector<RenderCallback>& getFramebufferRecreateCallbacks() const
    {
        return m_FramebufferRecreateCallbacks;
    }

    // 获取 render resource（供 Editor 创建 UI Pass 使用）
    virtual std::shared_ptr<RenderResourceBase> GetRenderResource() const { return m_RenderResource; }

    RenderPassBase* GetMainCameraPass() const { return m_MainCameraPass.get(); }

protected:
    RHI* m_Rhi;

    // Render resource the pipeline was initialized with. Lives on the base so the
    // active render-path module (friend) can read it (e.g. SubmitDrawLists fallback)
    // and GetRenderResource() can return it for the editor UI pass.
    std::shared_ptr<RenderResourceBase> m_RenderResource;

    bool m_IsShowAxis {false};
    std::array<bool, 2> m_IsShowSkybox {true, true};
    size_t m_SelectedAxis {3};

    std::shared_ptr<RenderPassBase> m_DirectionalLightPass;
    std::shared_ptr<RenderPassBase> m_PointLightShadowPass;
    std::shared_ptr<RenderPassBase> m_MainCameraPass;
    std::shared_ptr<RenderPassBase> m_LumenPass;
    std::shared_ptr<RenderPassBase> m_ColorGradingPass;
    std::shared_ptr<RenderPassBase> m_FxaaPass;
    std::shared_ptr<RenderPassBase> m_ToneMappingPass;
    std::shared_ptr<RenderPassBase> m_UiPass;
    std::shared_ptr<RenderPassBase> m_CombineUiPass;
    std::shared_ptr<RenderPassBase> m_PickPass;
    std::shared_ptr<RenderPassBase> m_ParticlePass;

    // UI 渲染后的回调列表
    std::vector<RenderCallback> m_PostUiRenderCallbacks;

    // Swapchain / main-camera framebuffer recreate (e.g. ImGui DX12 device objects).
    std::vector<RenderCallback> m_FramebufferRecreateCallbacks;

    // Editor ImGui: release game-thread wait when the RHI frame is skipped.
    std::vector<RenderCallback> m_SkippedFrameCallbacks;
};