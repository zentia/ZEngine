#pragma once

#include "Runtime/Function/Render/Pipeline/RenderPipelineModule.h"

#include <memory>

class DesktopRenderPipelineModule;

// Mobile render path (Milestone 1 skeleton).
//
// The end-state target is a bespoke G-buffer-free FORWARD pipeline:
//   optional shadow -> forward opaque -> forward transparent -> skybox -> tonemap -> UI
// reusing the mesh_forward shaders (HLSL under engine/shader/hlsl/rp1, GLSL under
// engine/shader/glsl) that the desktop path already maintains for transparent draws.
//
// At this milestone we deliberately ship a SKELETON that proves the architecture
// (a second, structurally-distinct, runtime-swappable module) without yet forking
// the main-camera pass into a forward-only variant. To do that the Mobile module
// COMPOSES the existing main-camera assembly (via an internal Desktop module so the
// scene renders correctly and the Desktop<->Mobile runtime switch is verifiable
// end-to-end on the working DX12 backend) while applying the first forward-lite
// FEATURE STRIP at draw-list assembly time:
//   - no point-light shadow pass (point lights are unshadowed under Mobile)
//   - no MegaLights stochastic direct lighting
//
// Documented TODOs to graduate this from skeleton to a real forward path (each is a
// follow-up sub-milestone, see doc/rendering/CROSS_PLATFORM_PIPELINE.md):
//   - replace the composed deferred main-camera pass with a single-pass forward
//     opaque+transparent pass driven by the mesh_forward shaders (no G-buffer)
//   - drop SSAO / dynamic GI / Lumen; swap to a lightmap + simple ambient path
//   - simplified single-cascade directional shadow
//
// Each concrete pass still branches internally on the active RHI (DX12 vs Vulkan);
// this layer only abstracts which passes run, in what order, under which path.
class MobileRenderPipelineModule : public RenderPipelineModule
{
public:
    explicit MobileRenderPipelineModule(RenderPipelineBase& host);
    ~MobileRenderPipelineModule() override;

    const char* GetName() const override { return "Mobile"; }

    void Setup(const RenderPipelineInitInfo& init_info) override;
    void Shutdown() override;
    void BuildDrawLists(std::shared_ptr<RenderResourceBase> render_resource, RHIDrawList& out_draw_list) override;
    void SubmitDrawLists(RHI* rhi,
                         std::shared_ptr<RenderResourceBase> render_resource,
                         const RHIDrawList& draw_list) override;
    void UpdateAfterRecreate() override;

    uint32_t GetGuidOfPickedMesh(const Vector2& picked_uv) override;
    RHIRenderPass* GetUIRenderPass() const override;
    RHIImageView* GetUiLayerColorView() const override;
    void FinishDx12ShadowPassDescriptorSetup() override;
    void ConsumeParticleSwapData(RenderSwapData& swap_data, RenderSwapContext& swap_context) override;

private:
    // Shared main-camera + post assembly the skeleton composes. Operates on the same
    // host RenderPipelineBase pass slots as a standalone Desktop module would, so all
    // external accessors (RenderSystem reading m_MainCameraPass, etc.) keep working.
    std::unique_ptr<DesktopRenderPipelineModule> m_Shared;
};
