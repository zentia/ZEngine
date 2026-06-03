#include "Nanite.h"

#include "Runtime/Function/Render/Interface/RHI.h"
#include "Runtime/Function/Render/RenderResourceBase.h"

bool NaniteSystem::Initialize(std::shared_ptr<RHI> rhi, std::shared_ptr<RenderResourceBase> render_resource)
{
    m_Renderer = std::make_shared<NaniteRenderer>();
    return m_Renderer->Initialize(rhi, render_resource, m_Config);
}

void NaniteSystem::Shutdown()
{
    if (m_Renderer)
    {
        m_Renderer->clear();
        m_Renderer.reset();
    }
}