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
    // (Milestone 1: the Desktop module keeps its composite-pass lambdas inline; the
    // Mobile forward module is the first consumer of this hook.)
    virtual void AppendToDrawList(RHIDrawList& out_draw_list, const RenderPassContext& context);

protected:
    RHI* m_Rhi;
    std::shared_ptr<RenderResourceBase> m_RenderResource;
};