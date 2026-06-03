#pragma once

#include "Runtime/Function/Render/RenderPipelineBase.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

class RHI;
class RHIRenderPass;
class RHIImageView;
class RenderResourceBase;
class RenderSwapContext;
class RHIDrawList;

struct RenderSwapData;

// Per-frame state a render path needs to assemble its draw list. Carried into
// RenderPassBase::AppendToDrawList so leaf passes stop taking long arg lists.
// (Milestone 1: the Desktop module keeps its composite-pass lambdas inline; the
// Mobile module is the first consumer of the AppendToDrawList hook.)
struct RenderPassContext
{
    std::vector<std::function<void()>> post_ui_callbacks;
    std::array<bool, 2> show_skybox {true, true};
    bool show_axis {false};
    size_t selected_axis {3};
};

// A render path ("Desktop" = deferred + forward, "Mobile" = forward) expressed
// as a first-class, runtime-swappable object that owns the pass assembly and
// the ordered per-frame draw-list build for ONE path. It deliberately operates
// on its host RenderPipelineBase's pass slots / shared state (friendship) so the
// existing external accessors -- RenderSystem reading m_MainCameraPass, the editor
// calling GetUIRenderPass(), etc. -- keep working unchanged while the *path* itself
// becomes the thing we swap. Each concrete pass still branches internally on the
// active RHI (DX12 vs Vulkan); this layer only abstracts which passes run, in what
// order, under which path.
class RenderPipelineModule
{
public:
    explicit RenderPipelineModule(RenderPipelineBase& host) : m_Pipeline(host) {}
    virtual ~RenderPipelineModule() = default;

    virtual const char* GetName() const = 0;

    // Create + initialize the path's passes (the body that used to live in
    // RenderPipeline::Initialize).
    virtual void Setup(const RenderPipelineInitInfo& init_info) = 0;

    // Release the path's passes / framebuffers. Default no-op; Phase B's runtime
    // switch overrides this for a clean teardown before swapping paths.
    virtual void Shutdown() {}

    virtual void BuildDrawLists(std::shared_ptr<RenderResourceBase> render_resource, RHIDrawList& out_draw_list) = 0;
    virtual void SubmitDrawLists(std::shared_ptr<RHI> rhi,
                                 std::shared_ptr<RenderResourceBase> render_resource,
                                 const RHIDrawList& draw_list) = 0;
    virtual void UpdateAfterRecreate() = 0;

    virtual uint32_t GetGuidOfPickedMesh(const Vector2& picked_uv)
    {
        (void)picked_uv;
        return 0;
    }
    virtual RHIRenderPass* GetUIRenderPass() const { return nullptr; }
    virtual RHIImageView* GetUiLayerColorView() const { return nullptr; }
    virtual void FinishDx12ShadowPassDescriptorSetup() {}

    // Default mirrors RenderPipelineBase::ConsumeParticleSwapData (just clears
    // pending flags) -- used by paths without a ParticlePass.
    virtual void ConsumeParticleSwapData(RenderSwapData& swap_data, RenderSwapContext& swap_context);

protected:
    RenderPipelineBase& m_Pipeline;
};
