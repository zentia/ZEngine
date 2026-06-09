#pragma once

#include "Runtime/Function/Render/Passes/MainCameraFramebufferResources.h"
#include "Runtime/Function/Render/RenderPass.h"

#include <cstdint>
#include <functional>
#include <vector>

class MainCameraRp2Pass : public RenderPass
{
public:
    void SetFramebufferResources(MainCameraFramebufferResources* resources) { m_FbResources = resources; }

    void SetUiPass(RenderPass* ui_pass) { m_UiPass = ui_pass; }

    bool Initialize(bool enable_fxaa);
    void Shutdown();

    void UpdateAfterFramebufferRecreate();

    void DrawRP2(uint32_t swapchain_image_index, const std::vector<std::function<void()>>& post_ui_callbacks);

    // Rebind color-grading LUT after UploadGlobalRenderResource (level hot-reload).
    void RefreshColorGradingDescriptorBindings();

    bool isReady() const { return m_Initialized; }

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

    void DrawColorGrading();
    void DrawFxaa();
    void DrawCombineUi();

    RHIShader* LoadShader(const char* hlsl_relative_path, ShaderStage stage);

    MainCameraFramebufferResources* m_FbResources {nullptr};
    RenderPass* m_UiPass {nullptr};
    bool m_EnableFxaa {false};
    bool m_Initialized {false};

    RHISampler* m_FallbackSampler {nullptr};
    RHIImageView* m_FallbackLutView {nullptr};
    // 1x1 transparent; combine_ui t1 on DX12 when legacy UIPass is skipped.
    RHIImageView* m_FallbackUiClearView {nullptr};
};
