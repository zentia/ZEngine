#pragma once

#include "Runtime/Function/Render/Passes/MainCameraFramebufferResources.h"
#include "Runtime/Function/Render/Passes/MainCameraPassShaderCommon.h"
#include "Runtime/Function/Render/RenderPass.h"

#include <array>
#include <functional>
#include <map>

class RenderResourceBase;

namespace MegaLights
{
class MegaLightsSystem;
}

// DX-B3: RP1 (gbuffer / deferred / forward) for DX12 and future Vulkan dedup.
class MainCameraRp1Pass : public RenderPass
{
public:
    enum LayoutType : uint8_t
    {
        _per_mesh = 0,
        _mesh_global,
        _mesh_per_material,
        _deferred_lighting,
        _layout_type_count
    };

    enum RenderPipelineType : uint8_t
    {
        _render_pipeline_type_mesh_gbuffer = 0,
        _render_pipeline_type_mesh_gbuffer_nocull,
        _render_pipeline_type_mesh_transparent,
        _render_pipeline_type_deferred_lighting,
        _render_pipeline_type_megalights_deferred,
        _render_pipeline_type_megalights_spatial,
        _render_pipeline_type_count
    };

    void SetFramebufferResources(MainCameraFramebufferResources* resources) { m_FbResources = resources; }

    void SetShadowImageViews(RHIImageView* directional_shadow, RHIImageView* point_light_shadow)
    {
        m_DirectionalLightShadowColorImageView = directional_shadow;
        m_PointLightShadowColorImageView = point_light_shadow;
    }

    void SetPerMeshLayout(RHIDescriptorSetLayout* layout) { m_ExternalPerMeshLayout = layout; }

    // DX12 bindless skybox draw inside RP1 forward subpass (Vulkan parity).
    void SetSkyboxDrawCallback(std::function<void(ViewportType)> callback)
    {
        m_SkyboxDrawCallback = std::move(callback);
    }

    // Rebind mesh-global IBL + shadow textures after UploadGlobalRenderResource (level hot-reload).
    void RefreshMeshGlobalIblDescriptors();

    // Rebind the deferred-lighting G-buffer/depth input-attachment descriptors after the MainCamera
    // framebuffer + RHI depth target are recreated on a window resize (otherwise scene -> white).
    void RefreshDeferredLightingInputAttachments();

    bool Initialize();
    void Shutdown();

    void PreparePassData(std::shared_ptr<RenderResourceBase> render_resource) override;

    void DrawRP1(const std::array<bool, 2>& skybox_visible);

    RHIDescriptorSetLayout* GetPerMeshDescriptorSetLayout() const
    {
        return m_DescriptorInfos[_per_mesh].layout;
    }

    RHIDescriptorSetLayout* GetMaterialDescriptorSetLayout() const
    {
        return m_DescriptorInfos[_mesh_per_material].layout;
    }

    // Stable pointer for RenderResource::m_MaterialDescriptorSetLayout (DX12 init order).
    RHIDescriptorSetLayout*& GetMaterialDescriptorSetLayoutPtr() { return m_MaterialDescriptorSetLayoutPtr; }

    std::array<MainCameraPerFrame, 2> m_MainCameraPerFrameByViewport;
    MainCameraPerFrame m_MainCameraPerFrame;
    MegaLights::MegaLightsSystem* m_MegaLightsSystem {nullptr};

private:
    void SetupDescriptorSetLayouts();
    void SetupPipelines();
    void SetupDescriptorSets();
    void EnsureFallbackIblTextures();

    std::function<void(ViewportType)> m_SkyboxDrawCallback;

    void SetPerViewportData(ViewportType viewport_type);
    bool IsViewportValid(ViewportType viewport_type) const;
    void SetViewportScissor(ViewportType viewport_type);

    void DrawMeshGbuffer(ViewportType viewport_type);
    void DrawDeferredLighting(ViewportType viewport_type);
    void UpdateMegaLightsDescriptorSets(ViewportType viewport_type);
    void UpdateMegaLightsSpatialDescriptorSets(ViewportType viewport_type);
    void DrawMegaLightsSpatialDenoise(ViewportType viewport_type);
    void DrawMeshTransparent(ViewportType viewport_type);

    RHIPipeline* GetOrCreateMeshGBufferPipeline(const GpuPBRMaterial& material);
    RHIPipeline* GetOrCreateMeshTransparentPipeline(const GpuPBRMaterial& material);

    RHIShader* LoadBuiltinShader(const char* hlsl_relative_path, ShaderStage stage);

    MainCameraFramebufferResources* m_FbResources {nullptr};
    RHIDescriptorSetLayout* m_ExternalPerMeshLayout {nullptr};
    RHIDescriptorSetLayout* m_MaterialDescriptorSetLayoutPtr {nullptr};

    RHIImageView* m_DirectionalLightShadowColorImageView {nullptr};
    RHIImageView* m_PointLightShadowColorImageView {nullptr};

    bool m_Initialized {false};

    // Fallback 1x1 IBL when UploadGlobalRenderResource has not populated real IBL yet (DX12 path).
    RHISampler* m_FallbackSampler {nullptr};
    RHIImageView* m_FallbackBrdfView {nullptr};
    RHIImageView* m_FallbackCubeView {nullptr};

    const std::vector<RenderMeshNode>* m_ActiveMainCameraVisibleMeshNodes {nullptr};

    std::map<MainCameraPassShaderCommon::MeshPipelineKey, RHIPipeline*> m_MeshGbufferMaterialPipelines;
    std::map<MainCameraPassShaderCommon::MeshPipelineKey, RHIPipeline*> m_MeshTransparentMaterialPipelines;
};
