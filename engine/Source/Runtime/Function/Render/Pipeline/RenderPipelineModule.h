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
//
// 第一个真实消费者：`MainCameraPass`（Vulkan），Desktop 与 Mobile 两个路径模块都
// 已经改用它，主相机那条链不再以 7 个形参的形式出现在 C++ 签名里。
//
// ⚠️ 这里**没有** swapchain 索引：它在 `BuildDrawLists` 之后才由
// `PrepareBeforePass()`（acquire）确定，放进 context 会让 pass 读到上一帧的索引。
// 需要它的 pass 在条目执行时自己从 RHI 读。
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
    virtual void SubmitDrawLists(RHI* rhi,
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

    // Apple(Metal) 专用：RP1 画完之后场景所在的那个离屏 attachment。
    //
    // Metal 没有持久 swapchain image，也没有 RP2（tonemap / combine UI 那些 TU
    // 在 Apple 上都不存在），所以场景结果是**离屏**的，得有人把它采样到 swapchain
    // 上 —— 调用方需要一个"场景色在哪"的入口。其它平台返回 nullptr（它们直接
    // 画进 swapchain，不需要这一步）。见 AppleSceneBlit.h。
    //
    // RP2 起来之后这里返回的是 **RP2 的输出**（backup_even，已做过显示变换）；
    // RP2 没起来时才是 RP1 的 backup_odd（线性 HDR）。
    virtual RHIImageView* GetSceneColorView() const { return nullptr; }

    // Apple(Metal) 专用：上面那张图**是否已经做过显示变换**。
    //
    // 判据必须和 GetSceneColorView() 逐字一致：调用方拿它决定 blit 还要不要
    // tonemap。差一帧的后果是二次 tonemap（画面发灰）或漏 tonemap（>1 死白）。
    // 每帧问，不要缓存 —— RP2 跟着 framebuffer 走，swapchain 头几帧还是 0x0。
    //
    // 其它平台恒 false（RP2 自己把结果写进 swapchain，不走这条路径）。
    virtual bool IsSceneColorTonemapped() const { return false; }

    // SSAO 的每帧统计（心跳探针）。只有 Desktop 模块在 Apple 上有实现。
    virtual SsaoFrameStats GetAmbientOcclusionStats() const { return {}; }

    virtual void FinishDx12ShadowPassDescriptorSetup() {}

    // Default mirrors RenderPipelineBase::ConsumeParticleSwapData (just clears
    // pending flags) -- used by paths without a ParticlePass.
    virtual void ConsumeParticleSwapData(RenderSwapData& swap_data, RenderSwapContext& swap_context);

protected:
    RenderPipelineBase& m_Pipeline;
};
