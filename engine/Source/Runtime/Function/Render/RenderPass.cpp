#include "Runtime/Function/Render/RenderPass.h"

#include "Runtime/Core/Base/Macro.h"
#if !defined(__APPLE__) && !defined(__EMSCRIPTEN__)
    #include "Runtime/Function/Render/Interface/Vulkan/VulkanRHI.h"
#endif
#include "Runtime/Function/Render/RenderResource.h"

VisiableNodes RenderPass::m_VisiableNodes;

void RenderPass::RefreshGlobalRenderResourcePointer()
{
    if (auto render_resource = std::dynamic_pointer_cast<RenderResource>(m_RenderResource))
    {
        m_GlobalRenderResource = &render_resource->m_GlobalRenderResource;
    }
    else
    {
        m_GlobalRenderResource = nullptr;
    }
}

bool RenderPass::EnsureGlobalRenderResourceReady()
{
    RefreshGlobalRenderResourcePointer();

    auto render_resource = std::dynamic_pointer_cast<RenderResource>(m_RenderResource);
    if (render_resource == nullptr || m_Rhi == nullptr)
    {
        return false;
    }

    if (m_Rhi->getGraphicsAPI() == GraphicsAPI::DirectX12 &&
        render_resource->m_GlobalRenderResource.m_StorageBuffer.m_GlobalUploadRingbuffer == nullptr)
    {
        render_resource->CreateAndMapStorageBuffer(m_Rhi);
    }

    RefreshGlobalRenderResourcePointer();
    return m_GlobalRenderResource != nullptr;
}

void RenderPass::Initialize(const RenderPassInitInfo* init_info)
{
    (void)init_info;
    m_Rhi = GET_SYSTEM(RHI);
    RefreshGlobalRenderResourcePointer();
}
void RenderPass::Draw() {}

void RenderPass::PostInitialize() {}

RHIRenderPass* RenderPass::GetRenderPass() const
{
    return m_Framebuffer.render_pass;
}

std::vector<RHIImageView*> RenderPass::GetFramebufferImageViews() const
{
    std::vector<RHIImageView*> image_views;
    for (auto& attach : m_Framebuffer.attachments)
    {
        image_views.push_back(attach.view);
    }
    return image_views;
}

std::vector<RHIDescriptorSetLayout*> RenderPass::GetDescriptorSetLayouts() const
{
    std::vector<RHIDescriptorSetLayout*> layouts;
    for (auto& desc : m_DescriptorInfos)
    {
        layouts.push_back(desc.layout);
    }
    return layouts;
}