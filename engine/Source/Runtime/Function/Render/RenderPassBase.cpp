#include "Runtime/Function/Render/RenderPassBase.h"

#include "Runtime/Core/Base/Macro.h"

void RenderPassBase::PostInitialize() {}
void RenderPassBase::SetCommonInfo(RenderPassCommonInfo common_info)
{
    m_Rhi = common_info.rhi;
    m_RenderResource = common_info.render_resource;
}
void RenderPassBase::PreparePassData(std::shared_ptr<RenderResourceBase> render_resource) {}
void RenderPassBase::InitializeUIRenderBackend(WindowUI* window_ui) {}
void RenderPassBase::AppendToDrawList(RHIDrawList& out_draw_list, const RenderPassContext& context)
{
    (void)out_draw_list;
    (void)context;
}