// DX-B0: DX12 render-pass storage + subpass description deep copy.

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Function/Render/Interface/DX12/DX12RHIResource.h"
#include "Runtime/Function/Render/RenderType.h"

namespace
{
    bool IsDepthAttachmentFormat(RHIFormat format)
    {
        return format == RHI_FORMAT_D16_UNORM || format == RHI_FORMAT_D32_SFLOAT ||
               format == RHI_FORMAT_D24_UNORM_S8_UINT || format == RHI_FORMAT_D32_SFLOAT_S8_UINT;
    }

    void CopyAttachmentReferences(std::vector<RHIAttachmentReference>& out,
                                  const RHIAttachmentReference* refs,
                                  uint32_t count)
    {
        if (refs == nullptr || count == 0)
        {
            out.clear();
            return;
        }
        out.assign(refs, refs + count);
    }
}  // namespace

void DX12RenderPass::setAttachments(const RHIAttachmentDescription* attachments, uint32_t count)
{
    m_Attachments.assign(attachments, attachments + count);
    m_Dependencies.clear();
    m_Subpasses.clear();
    rebuildImplicitSubpassFromAttachments();
}

bool DX12RenderPass::setCreateInfo(const RHIRenderPassCreateInfo* create_info)
{
    if (create_info == nullptr)
    {
        return false;
    }

    m_Attachments.clear();
    m_Subpasses.clear();
    m_Dependencies.clear();

    if (create_info->attachmentCount > 0 && create_info->pAttachments != nullptr)
    {
        m_Attachments.assign(create_info->pAttachments,
                             create_info->pAttachments + create_info->attachmentCount);
    }

    if (create_info->dependencyCount > 0 && create_info->pDependencies != nullptr)
    {
        m_Dependencies.assign(create_info->pDependencies,
                              create_info->pDependencies + create_info->dependencyCount);
    }

    if (create_info->subpassCount > 0 && create_info->pSubpasses != nullptr)
    {
        m_Subpasses.resize(create_info->subpassCount);
        for (uint32_t subpass_index = 0; subpass_index < create_info->subpassCount; ++subpass_index)
        {
            const RHISubpassDescription& src = create_info->pSubpasses[subpass_index];
            DX12SubpassDescriptionStorage& dst_storage = m_Subpasses[subpass_index];

            CopyAttachmentReferences(dst_storage.input_attachments, src.pInputAttachments, src.inputAttachmentCount);
            CopyAttachmentReferences(dst_storage.color_attachments, src.pColorAttachments, src.colorAttachmentCount);
            CopyAttachmentReferences(dst_storage.resolve_attachments, src.pResolveAttachments, src.colorAttachmentCount);

            if (src.preserveAttachmentCount > 0 && src.pPreserveAttachments != nullptr)
            {
                dst_storage.preserve_attachments.assign(src.pPreserveAttachments,
                                                        src.pPreserveAttachments + src.preserveAttachmentCount);
            }

            dst_storage.has_depth_stencil = (src.pDepthStencilAttachment != nullptr);
            if (dst_storage.has_depth_stencil)
            {
                dst_storage.depth_stencil = *src.pDepthStencilAttachment;
            }

            RHISubpassDescription& dst = dst_storage.desc;
            dst = src;
            dst.pInputAttachments = dst_storage.input_attachments.empty() ? nullptr : dst_storage.input_attachments.data();
            dst.inputAttachmentCount = static_cast<uint32_t>(dst_storage.input_attachments.size());
            dst.pColorAttachments = dst_storage.color_attachments.empty() ? nullptr : dst_storage.color_attachments.data();
            dst.colorAttachmentCount = static_cast<uint32_t>(dst_storage.color_attachments.size());
            dst.pResolveAttachments = dst_storage.resolve_attachments.empty() ? nullptr : dst_storage.resolve_attachments.data();
            dst.pDepthStencilAttachment = dst_storage.has_depth_stencil ? &dst_storage.depth_stencil : nullptr;
            dst.pPreserveAttachments = dst_storage.preserve_attachments.empty() ? nullptr : dst_storage.preserve_attachments.data();
            dst.preserveAttachmentCount = static_cast<uint32_t>(dst_storage.preserve_attachments.size());
        }
        return true;
    }

    rebuildImplicitSubpassFromAttachments();
    return true;
}

const RHISubpassDescription* DX12RenderPass::getSubpass(uint32_t index) const
{
    if (index >= m_Subpasses.size())
    {
        return nullptr;
    }
    return &m_Subpasses[index].desc;
}

void DX12RenderPass::rebuildImplicitSubpassFromAttachments()
{
    if (m_Attachments.empty())
    {
        return;
    }

    DX12SubpassDescriptionStorage& storage = m_Subpasses.emplace_back();
    storage.color_attachments.reserve(m_Attachments.size());

    for (uint32_t attachment_index = 0; attachment_index < m_Attachments.size(); ++attachment_index)
    {
        if (IsDepthAttachmentFormat(m_Attachments[attachment_index].format))
        {
            storage.depth_stencil.attachment = attachment_index;
            storage.depth_stencil.layout = RHI_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            storage.has_depth_stencil = true;
            continue;
        }

        RHIAttachmentReference color_ref {};
        color_ref.attachment = attachment_index;
        color_ref.layout = RHI_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        storage.color_attachments.push_back(color_ref);
    }

    RHISubpassDescription& desc = storage.desc;
    desc.pipelineBindPoint = RHI_PIPELINE_BIND_POINT_GRAPHICS;
    desc.pColorAttachments = storage.color_attachments.empty() ? nullptr : storage.color_attachments.data();
    desc.colorAttachmentCount = static_cast<uint32_t>(storage.color_attachments.size());
    desc.pDepthStencilAttachment = storage.has_depth_stencil ? &storage.depth_stencil : nullptr;
}

void DX12Framebuffer::setAttachmentView(uint32_t attachment_index, DX12ImageView* view)
{
    if (attachment_index >= m_AttachmentViews.size())
    {
        m_AttachmentViews.resize(attachment_index + 1, nullptr);
    }
    m_AttachmentViews[attachment_index] = view;
}

DX12ImageView* DX12Framebuffer::getAttachmentView(uint32_t attachment_index) const
{
    if (attachment_index >= m_AttachmentViews.size())
    {
        return nullptr;
    }
    return m_AttachmentViews[attachment_index];
}
