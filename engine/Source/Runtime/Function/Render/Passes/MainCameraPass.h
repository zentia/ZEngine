#pragma once

#include "Runtime/Function/Render/Passes/BindlessTonemapPass.h"
#include "Runtime/Function/Render/Passes/ColorGradingPass.h"
#include "Runtime/Function/Render/Passes/CombineUIPass.h"
#include "Runtime/Function/Render/Passes/FXAAPass.h"
#include "Runtime/Function/Render/Passes/MainCameraFramebufferResources.h"
#include "Runtime/Function/Render/Passes/ParticlePass.h"
#include "Runtime/Function/Render/Passes/UIPass.h"
#include "Runtime/Function/Render/RenderPass.h"

#include <array>
#include <functional>
#include <map>
#include <string>
#include <vector>

class RenderResourceBase;
class RenderCamera;

namespace MegaLights
{
class MegaLightsSystem;
}

class MainCameraPass : public RenderPass
{
public:
    struct MeshGBufferPipelineKey
    {
        std::string vertex_shader_file;
        std::string fragment_shader_file;
        std::string vertex_entry {"main"};
        std::string fragment_entry {"main"};
        std::string include_directory;
        std::string source_language {"HLSL"};
        std::string render_pipeline {"StandardLit"};
        std::string light_mode {"GBuffer"};
        std::string cull {"Back"};
        std::string ztest {"LEqual"};
        std::string blend {"Off"};
        bool zwrite {true};
        std::map<std::string, std::string> shader_macros;

        bool operator<(const MeshGBufferPipelineKey& rhs) const;
    };

    // 1: per mesh layout

    // 2: global layout
    // 3: mesh per material layout
    // 4: sky box layout
    // 5: axis layout
    // 6: billboard type particle layout
    // 7: gbuffer lighting
    // 8: megalights spatial (sampled gbuffer)
    enum LayoutType : uint8_t
    {
        _per_mesh = 0,
        _mesh_global,
        _mesh_per_material,
        _skybox,
        _axis,
        _particle,
        _deferred_lighting,
        _megalights_spatial_surfaces,
        _layout_type_count
    };

    // 1. model
    // 2. sky box
    // 3. axis
    // 4. billboard type particle
    enum RenderPipeLineType : uint8_t
    {
        _render_pipeline_type_mesh_gbuffer = 0,
        _render_pipeline_type_mesh_transparent,
        _render_pipeline_type_deferred_lighting,
        _render_pipeline_type_megalights_deferred,
        _render_pipeline_type_megalights_spatial,
        _render_pipeline_type_skybox,

        _render_pipeline_type_axis,
        _render_pipeline_type_particle,
        _render_pipeline_type_count
    };

    void Initialize(const RenderPassInitInfo* init_info) override final;

    void PreparePassData(std::shared_ptr<RenderResourceBase> render_resource) override final;

    // 渲染回调类型
    using RenderCallback = std::function<void()>;

    void Draw(ColorGradingPass& color_grading_pass,
              FXAAPass& fxaa_pass,
              BindlessTonemapPass& tone_mapping_pass,
              RenderPass& ui_pass,
              CombineUIPass& combine_ui_pass,
              ParticlePass& particle_pass,
              uint32_t current_swapchain_image_index,
              const std::vector<RenderCallback>& post_ui_callbacks = {});

    RHIImageView* m_PointLightShadowColorImageView;
    RHIImageView* m_DirectionalLightShadowColorImageView;

    bool m_IsShowAxis {false};
    std::array<bool, 2> m_IsShowSkybox {true, true};
    bool m_EnableFxaa {false};
    size_t m_SelectedAxis {3};
    std::array<MeshPerframeStorageBufferObject, 2> m_MeshPerframeStorageBufferObjects;

    MeshPerframeStorageBufferObject m_MeshPerframeStorageBufferObject;
    AxisStorageBufferObject m_AxisStorageBufferObject;
    const std::vector<RenderMeshNode>* m_ActiveMainCameraVisibleMeshNodes {nullptr};
    MegaLights::MegaLightsSystem* m_MegaLightsSystem {nullptr};

    void UpdateAfterFramebufferRecreate();

    RHICommandBuffer* GetRenderCommandBuffer();

    void SetParticlePass(std::shared_ptr<ParticlePass> pass);

    // PR-V4 part 2: getter for RP1 (gbuffer + lighting). Provided so that
    // ParticlePass / mesh-gbuffer pipelines can bind to the right render
    // pass. RP2 is exposed via the inherited m_Framebuffer.render_pass /
    // GetRenderPass() so existing UI/post-FX wiring stays unchanged.
    RHIRenderPass* getRP1RenderPass() const { return m_Framebuffer1.render_pass; }

private:
    void SetupParticlePass();
    void SetupAttachments();
    void SetupRenderPass();  // builds RP1 (m_Framebuffer1) + RP2 (m_Framebuffer)
    void SetupRenderPass1();
    void SetupRenderPass2();
    void SetupDescriptorSetLayout();
    void SetupPipelines();
    void SetupDescriptorSet();
    void SetupFramebufferDescriptorSet();
    void SetupSwapchainFramebuffers();  // builds RP1 fb + RP2 per-swap-image fbs

    void SetupModelGlobalDescriptorSet();
    void UpdateMegaLightsDescriptorSets(ViewportType viewport_type);
    void UpdateMegaLightsSpatialDescriptorSets(ViewportType viewport_type);
    void SetupMegaLightsSpatialSurfacesDescriptorSet();
    void SetupSkyboxDescriptorSet();
    void SetupAxisDescriptorSet();
    void SetupParticleDescriptorSet();
    void SetupGbufferLightingDescriptorSet();

    void SetPerViewportData(ViewportType viewport_type);
    bool IsViewportValid(ViewportType viewport_type) const;
    void SetViewportScissor(ViewportType viewport_type);
    void SetFullscreenViewportScissor();

    void DrawMeshGbuffer(ViewportType viewport_type);
    void DrawMeshTransparent(ViewportType viewport_type);
    void DrawDeferredLighting(ViewportType viewport_type);
    void DrawMegaLightsSpatialDenoise(ViewportType viewport_type);
    void DrawSkybox(ViewportType viewport_type);

    RHIPipeline* GetOrCreateMeshGBufferPipeline(const struct VulkanPBRMaterial& material);
    RHIPipeline* GetOrCreateMeshTransparentPipeline(const struct VulkanPBRMaterial& material);
    void DrawAxis();

private:
    // PR-V4 part 2: RP1 (gbuffer + lighting) shares the same backing image
    // attachments as the original single-RP topology -- we just slice the
    // attachment set: RP1 uses [gbuffer_a, gbuffer_b, gbuffer_c, backup_odd,
    // depth] and RP2 uses [backup_odd, backup_even, post_process_odd,
    // post_process_even, swap_chain_image]. backup_odd is shared as the
    // hand-off buffer (RP1 writes it with finalLayout=SHADER_READ_ONLY,
    // BindlessTonemapPass samples it via bindless to write backup_even,
    // RP2 reads backup_even via subpassInput from color_grading and reads
    // backup_odd via bindless from tonemap output -> via subpassInput from
    // combine_ui).
    Framebuffer m_Framebuffer1;  // RP1
    RHIFramebuffer* m_Rp1Framebuffer {nullptr};
    std::vector<RHIFramebuffer*> m_SwapchainFramebuffers;  // RP2 fbs (per swapchain image)
    std::shared_ptr<ParticlePass> m_ParticlePass;
    std::map<MeshGBufferPipelineKey, RHIPipeline*> m_MeshGbufferMaterialPipelines;
    std::map<MeshGBufferPipelineKey, RHIPipeline*> m_MeshTransparentMaterialPipelines;
};
