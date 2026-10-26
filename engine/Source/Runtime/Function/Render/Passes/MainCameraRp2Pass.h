#pragma once

#include "Runtime/Function/Render/Graph/RenderGraphResources.h"
#include "Runtime/Function/Render/Passes/MainCameraFramebufferResources.h"
#include "Runtime/Function/Render/RenderPass.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

class MainCameraRp2Pass : public RenderPass
{
public:
    void SetFramebufferResources(MainCameraFramebufferResources* resources) { m_FbResources = resources; }

    void SetUiPass(RenderPass* ui_pass) { m_UiPass = ui_pass; }

    /// Recorded inside the Combine UI render pass, right after the scene/UI composite, i.e.
    /// straight onto the swapchain and before anything the post-RP2 overlay (ZSlate / ImGui)
    /// paints. The argument is that render pass (subpass 0), for pipeline creation. Used by
    /// DX12 for GPU-Spine, which has no UI layer to draw into there. Empty = nothing.
    void SetCombineOverlay(std::function<void(RHIRenderPass*)> overlay) { m_CombineOverlay = std::move(overlay); }

    bool Initialize(bool enable_fxaa);
    void Shutdown();

    void UpdateAfterFramebufferRecreate();

    void DrawRP2(uint32_t swapchain_image_index, const std::vector<std::function<void()>>& post_ui_callbacks);

    // Rebind color-grading LUT after UploadGlobalRenderResource (level hot-reload).
    void RefreshColorGradingDescriptorBindings();

    bool isReady() const { return m_Initialized; }

    /// True when RP2 is driven by the render graph. Default ON: the graph path was
    /// verified bit-identical to the hand-written chain by the in-frame A/B capture on
    /// 2026-10-26. Set ZENGINE_RG_RP2=0 to force the hand-written chain.
    bool IsRenderGraphPathEnabled() const { return m_UseRenderGraph; }

    /// Turns the graph path on / off at runtime (building the graph objects on first use).
    /// Used by the in-frame A/B capture (DX12RenderGraphAbCapture), which forces each path
    /// once and restores the previous setting.
    bool SetRenderGraphPathEnabled(bool enabled);

    /// Whether the most recent DrawRP2() was actually drawn by the graph (false when the
    /// graph path is off OR when it rejected the frame and the hand-written chain ran).
    bool LastDrawUsedRenderGraph() const { return m_LastDrawUsedGraph; }

private:
    enum LayoutType : uint8_t
    {
        _color_grading = 0,
        _fxaa,
        _combine_ui,
        _layout_count
    };

    enum PipelineType : uint8_t
    {
        _pipeline_color_grading = 0,
        _pipeline_fxaa,
        _pipeline_combine_ui,
        _pipeline_count
    };

    void SetupDescriptorSetLayouts();
    void SetupPipelines();
    void SetupDescriptorSets();
    void UpdateDescriptorBindings();
    void EnsureFallbackLutTexture();
    void EnsureFallbackUiClearTexture();

    /// The command buffer is threaded through instead of being pulled from
    /// `m_Rhi->GetCurrentCommandBuffer()` inside each one: the render graph hands its pass
    /// callbacks the buffer it was given, and tying the draws to that makes it impossible
    /// for a callback to record into a different buffer than the barriers around it.
    void DrawColorGrading(RHICommandBuffer* cmd);
    void DrawFxaa(RHICommandBuffer* cmd);
    void DrawCombineUi(RHICommandBuffer* cmd);

    // ---- render graph path (ZENGINE_RG_RP2) ----------------------------------------
    /// Reads the switch and, when it is on, builds the executor / pool / graph. Called
    /// from Initialize(); harmless when the switch is off.
    void SetupRenderGraphPath();

    /// Declares and executes the RP2 chain through the graph. Returns false when the
    /// graph cannot be used, in which case the caller runs the hand-written path. Every
    /// rejection happens BEFORE the first command is recorded, so a false return never
    /// leaves a half-drawn frame behind.
    bool DrawRP2WithGraph(uint32_t swapchain_image_index,
                          const std::vector<std::function<void()>>& post_ui_callbacks);

    RHIShader* LoadShader(const char* hlsl_relative_path, ShaderStage stage);

    std::function<void(RHIRenderPass*)> m_CombineOverlay;

    MainCameraFramebufferResources* m_FbResources {nullptr};
    RenderPass* m_UiPass {nullptr};
    bool m_EnableFxaa {false};
    bool m_Initialized {false};

    RHISampler* m_FallbackSampler {nullptr};
    RHIImageView* m_FallbackLutView {nullptr};
    // 1x1 transparent; combine_ui t1 on DX12 when legacy UIPass is skipped.
    RHIImageView* m_FallbackUiClearView {nullptr};

    // ---- render graph path ----------------------------------------------------------
    // Owned here and rebuilt every frame's Setup; the graph itself is reused across
    // frames and Reset() at the top of DrawRP2WithGraph().
    bool m_UseRenderGraph {false};
    std::unique_ptr<RGRhiDeviceAdapter> m_RgDevice;
    std::unique_ptr<RenderGraphResourcePool> m_RgPool;
    std::unique_ptr<RenderGraphExecutor> m_RgExecutor;
    std::unique_ptr<RenderGraph> m_Rp2Graph;
    /// Compile errors are logged once and then the pass silently keeps using the
    /// hand-written path; without this the log would repeat every frame.
    bool m_RgFallbackLogged {false};
    bool m_LastDrawUsedGraph {false};
};
