#pragma once

#include "Runtime/Function/Render/Interface/RHI.h"

class RHI;
class RenderResourceBase;
class WindowUI;
class RHIDrawList;

struct RenderPassContext;

struct RenderPassInitInfo
{
};

struct RenderPassCommonInfo
{
    RHI* rhi;
    std::shared_ptr<RenderResourceBase> render_resource;
};

class RenderPassBase
{
public:
    virtual void Initialize(const RenderPassInitInfo* init_info) = 0;
    virtual void PostInitialize();
    virtual void SetCommonInfo(RenderPassCommonInfo common_info);
    virtual void PreparePassData(std::shared_ptr<RenderResourceBase> render_resource);
    virtual void InitializeUIRenderBackend(WindowUI* window_ui);

    // Uniform render-path hook: a pass appends its named draw-list entry(ies) for
    // the current frame. Default no-op. Render-path modules call this to assemble
    // their ordered pass list without a backend-specific switch in the module body.
    //
    // 第一个真实消费者：`MainCameraPass`（Vulkan 的延迟 + 前向链）。它把
    // "RP1 / BindlessTonemap / RP2" 三段各贡献成一个条目，于是
    // `Draw(ColorGradingPass&, FXAAPass&, BindlessTonemapPass&, RenderPass&,
    // CombineUIPass&, ParticlePass&, uint32_t, ...)` 那个 7 参数签名被拆掉
    // （`doc/RENDER_GRAPH.md` §1 记为「Pass 图硬编码在函数签名里」）。
    // 一个条目 = 一对配平的 Begin/EndRenderPass。
    virtual void AppendToDrawList(RHIDrawList& out_draw_list, const RenderPassContext& context);

protected:
    RHI* m_Rhi;
    std::shared_ptr<RenderResourceBase> m_RenderResource;
};