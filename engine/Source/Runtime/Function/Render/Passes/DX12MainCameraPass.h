#pragma once

#include "Runtime/Function/Render/Interface/DX12/Utility/BindlessTextureBlitPipeline.h"
#include "Runtime/Function/Render/Passes/BindlessTonemapPass.h"
#include "Runtime/Function/Render/Passes/MainCameraFramebufferResources.h"
#include "Runtime/Function/Render/Passes/MainCameraRp1Pass.h"
#include "Runtime/Function/Render/Passes/MainCameraRp2Pass.h"
#include "Runtime/Function/Render/RenderPass.h"

#include <d3d12.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

#include <array>
#include <functional>
#include <memory>
#include <vector>

class RenderCamera;

class DX12MainCameraPass final : public RenderPass

{
public:
    using RenderCallback = std::function<void()>;

    void Initialize(const RenderPassInitInfo* init_info) override;
    void PreparePassData(std::shared_ptr<RenderResourceBase> render_resource) override;
    void Draw(const std::vector<RenderCallback>& post_ui_callbacks, const std::array<bool, 2>& skybox_visible);

    // DX-B1: swapchain resize hook (called from RenderPipeline::passUpdateAfterRecreateSwapchain).
    void UpdateAfterFramebufferRecreate();

    const MainCameraFramebufferResources& getFramebufferResources() const { return m_FramebufferResources; }
    bool hasFramebufferResources() const { return m_FramebufferResourcesReady; }

    // DX-B8: parity with Vulkan MainCameraPass for Editor / pipeline queries.
    // UE-style: RP2 now uses separate HDR/LDR render passes.
    // - getRp2HdrRenderPass(): for color_grading and fxaa (HDR format).
    // - getRp2LdrRenderPass(): for combine_ui (LDR/swapchain format).
    RHIRenderPass* getRp2HdrRenderPass() const;
    RHIRenderPass* getRp2LdrRenderPass() const;
    RHIImageView* getUiLayerColorView() const;
    std::vector<RHIImageView*> getFramebufferImageViews() const;

    MainCameraRp1Pass& getRp1Pass() { return m_Rp1Pass; }
    const MainCameraRp1Pass& getRp1Pass() const { return m_Rp1Pass; }
    bool isRp1Ready() const { return m_Rp1Ready; }

    MainCameraRp2Pass& getRp2Pass() { return m_Rp2Pass; }
    const MainCameraRp2Pass& getRp2Pass() const { return m_Rp2Pass; }
    bool isRp2Ready() const { return m_Rp2Ready; }

    // After UploadGlobalRenderResource: skybox bindless slot + RP1/RP2 descriptor refresh.
    void OnGlobalRenderResourceUploaded();

    BindlessTonemapPass& getTonemapPass() { return m_TonemapPass; }
    const BindlessTonemapPass& getTonemapPass() const { return m_TonemapPass; }
    bool isTonemapReady() const { return m_TonemapReady; }

    // Parity with Vulkan MainCameraPass: editor transform gizmo visibility.
    bool m_IsShowAxis {false};
    size_t m_SelectedAxis {3};

    // Composited on the swapchain overlay from EditorUIPass::Draw (after ZSlate UI).
    void DrawAxis();

    // Legacy swapchain-overlay sky helpers (compiled; no live call sites on DX12 editor).
    // Active sky path: RP1 mesh SkyPass (MainCameraRp1Pass::DrawSkyMeshPass) -> backup_odd HDR before deferred.
    // See doc/rendering/DX12_SKYBOX_RENDERING.md.
    void DrawEditorSkyboxOverlays(const std::array<bool, 2>& skybox_visible);

    // DX-B2: shadow maps for RP1 deferred (wired from shadow passes after init).
    RHIImageView* m_PointLightShadowColorImageView {nullptr};
    RHIImageView* m_DirectionalLightShadowColorImageView {nullptr};

private:
    enum RenderPipelineType : uint8_t
    {
        _render_pipeline_type_skybox = 0,
        _render_pipeline_type_scene_grid,
        _render_pipeline_type_count
    };

    // ---- Setup helpers ---------------------------------------------------
    // Build the shared "bindless production" root signature used by
    // both skybox and scene_grid. Layout:
    //   Root param 0: 32-bit constants (1 DWORD) at b0/space0 — bindless index
    //   Root param 1: Root CBV at b1/space0 — per-draw UBO
    //   Static samplers s0..s3 (LinearWrap/LinearClamp/PointWrap/PointClamp)
    //   Flag: CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED
    bool BuildBindlessProductionRootSignature();
    bool BuildSkyboxOverlayRootSignature();

    bool SetupSkyboxResources();
    bool SetupSceneGridResources();
    bool SetupAxisResources();

    // ---- Draw helpers ----------------------------------------------------
    void DrawSkybox(ViewportType viewport_type);
    void DrawSkyboxPreview();
    void DrawSkyboxInRp1Forward(ViewportType viewport_type);
    void DrawSkyboxWithCamera(const std::shared_ptr<RenderCamera>& camera,
                              const RHIViewport& viewport,
                              size_t viewport_slot,
                              bool swapchain_overlay);
    void DrawSceneGrid();

    void TryLateInitializeSkybox();
    void RefreshSkyboxCubemapDescriptor();

    // ---- Bindless production pipeline state ------------------------------
    // Shared root signature for all production draws.
    ComPtr<ID3D12RootSignature> m_BindlessRootSignature;

    // Skybox PSOs (descriptor-table cubemap, not bindless heap).
    ComPtr<ID3D12RootSignature> m_SkyboxOverlayRootSignature;
    ComPtr<ID3D12PipelineState> m_SkyboxPso;
    ComPtr<ID3D12PipelineState> m_SkyboxForwardPso;

    // Scene grid PSO (same root signature, no bindless texture used)
    ComPtr<ID3D12PipelineState> m_SceneGridPso;

    // Per-viewport constant buffers (UBO data pushed via root CBV).
    // 3 slots: [0]=game, [1]=scene, [2]=preview.
    static constexpr size_t kSkyboxViewportCount = 3;
    RHIBuffer* m_SkyboxConstantBuffers[kSkyboxViewportCount] = {};
    RHIDeviceMemory* m_SkyboxConstantBufferMemories[kSkyboxViewportCount] = {};

    RHIBuffer* m_SceneGridConstantBuffer = nullptr;
    RHIDeviceMemory* m_SceneGridConstantBufferMemory = nullptr;

    bool m_SkyboxReady = false;
    bool m_SkyboxSetupAborted = false;
    bool m_SceneGridReady = false;

    ComPtr<ID3D12RootSignature> m_AxisRootSignature;
    ComPtr<ID3D12PipelineState> m_AxisPso;
    RHIBuffer* m_AxisPerFrameConstantBuffer = nullptr;
    RHIDeviceMemory* m_AxisPerFrameConstantBufferMemory = nullptr;
    RHIBuffer* m_AxisDrawConstantBuffer = nullptr;
    RHIDeviceMemory* m_AxisDrawConstantBufferMemory = nullptr;
    RHIBuffer* m_AxisHostVisibleVertexBuffer = nullptr;
    RHIDeviceMemory* m_AxisHostVisibleVertexBufferMemory = nullptr;
    size_t m_AxisHostVisibleVertexCapacity = 0;
    RHIBuffer* m_AxisHostVisibleIndexBuffer = nullptr;
    RHIDeviceMemory* m_AxisHostVisibleIndexBufferMemory = nullptr;
    size_t m_AxisHostVisibleIndexCapacity = 0;
    std::array<MainCameraPerFrame, 2> m_MainCameraPerFrameByViewport {};
    bool m_AxisReady = false;

    MainCameraFramebufferResources m_FramebufferResources;
    bool m_FramebufferResourcesReady {false};

    MainCameraRp1Pass m_Rp1Pass;
    bool m_Rp1Ready {false};

    BindlessTonemapPass m_TonemapPass;
    bool m_TonemapReady {false};

    MainCameraRp2Pass m_Rp2Pass;
    bool m_Rp2Ready {false};
    bool m_EnableFxaa {false};
};
