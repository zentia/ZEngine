#include "Runtime/Function/Render/Graph/RenderGraphBuilder.h"

#include "Runtime/Function/Render/Graph/RenderGraph.h"

RenderGraphBuilder::RenderGraphBuilder(RenderGraph& graph, uint32_t pass_index)
    : m_Graph(graph), m_PassIndex(pass_index)
{
}

RGTextureHandle RenderGraphBuilder::CreateTexture(const char* name, const RGTextureDesc& desc)
{
    RGTextureDesc copy = desc;
    copy.name = name != nullptr ? name : desc.name;
    return m_Graph.CreateTexture(copy.name.c_str(), copy);
}

RGBufferHandle RenderGraphBuilder::CreateBuffer(const char* name, const RGBufferDesc& desc)
{
    RGBufferDesc copy = desc;
    copy.name = name != nullptr ? name : desc.name;
    return m_Graph.CreateBuffer(copy.name.c_str(), copy);
}

RGTextureHandle RenderGraphBuilder::ImportTexture(const char* name, const RGExternalTexture& external)
{
    return m_Graph.ImportTexture(name, external);
}

RGBufferHandle RenderGraphBuilder::ImportBuffer(const char* name, const RGExternalBuffer& external)
{
    return m_Graph.ImportBuffer(name, external);
}

RGTextureHandle RenderGraphBuilder::ReadTexture(RGTextureHandle handle, RGResourceState state)
{
    if (!Declare(handle.index, true, state, false))
    {
        return RGTextureHandle {};
    }
    return handle;
}

RGTextureHandle RenderGraphBuilder::WriteTexture(RGTextureHandle handle, RGResourceState state)
{
    if (!Declare(handle.index, true, state, true))
    {
        return RGTextureHandle {};
    }
    return handle;
}

RGBufferHandle RenderGraphBuilder::ReadBuffer(RGBufferHandle handle, RGResourceState state)
{
    if (!Declare(handle.index, false, state, false))
    {
        return RGBufferHandle {};
    }
    return handle;
}

RGBufferHandle RenderGraphBuilder::WriteBuffer(RGBufferHandle handle, RGResourceState state)
{
    if (!Declare(handle.index, false, state, true))
    {
        return RGBufferHandle {};
    }
    return handle;
}

void RenderGraphBuilder::KeepAlive(RGTextureHandle handle)
{
    if (handle.index >= m_Graph.GetResourceCount())
    {
        return;
    }
    m_Graph.MarkExtracted(handle.index);
}

void RenderGraphBuilder::KeepAlive(RGBufferHandle handle)
{
    if (handle.index >= m_Graph.GetResourceCount())
    {
        return;
    }
    m_Graph.MarkExtracted(handle.index);
}

bool RenderGraphBuilder::IsDeclared(RGTextureHandle handle) const
{
    for (const RGResourceUse& use : m_Reads)
    {
        if (use.resource_index == handle.index)
        {
            return true;
        }
    }
    for (const RGResourceUse& use : m_Writes)
    {
        if (use.resource_index == handle.index)
        {
            return true;
        }
    }
    return false;
}

bool RenderGraphBuilder::IsDeclared(RGBufferHandle handle) const
{
    for (const RGResourceUse& use : m_Reads)
    {
        if (use.resource_index == handle.index)
        {
            return true;
        }
    }
    for (const RGResourceUse& use : m_Writes)
    {
        if (use.resource_index == handle.index)
        {
            return true;
        }
    }
    return false;
}

bool RenderGraphBuilder::Declare(uint32_t resource_index,
                                 bool is_texture,
                                 RGResourceState state,
                                 bool is_write)
{
    if (resource_index == kRGInvalidIndex || resource_index >= m_Graph.GetResourceCount())
    {
        m_Graph.AddSetupError("pass '" + m_Graph.GetPasses()[m_PassIndex].name +
                         "' declared a handle that is not valid for this graph (stale handle?)");
        return false;
    }

    // A resource declared twice in one pass must ask for the same state, and must not be
    // both read and written (no RW textures in this model).
    const char* resource_name = m_Graph.GetResources()[resource_index].name.c_str();
    for (const RGResourceUse& use : m_Reads)
    {
        if (use.resource_index == resource_index)
        {
            if (use.state != state)
            {
                m_Graph.AddSetupError(std::string("pass '") + m_Graph.GetPasses()[m_PassIndex].name + "' uses " +
                                 resource_name + " in two different states");
            }
            else if (is_write)
            {
                m_Graph.AddSetupError(std::string("pass '") + m_Graph.GetPasses()[m_PassIndex].name + "' reads and writes " +
                                 resource_name + " in the same pass");
            }
            return false;
        }
    }
    for (const RGResourceUse& use : m_Writes)
    {
        if (use.resource_index == resource_index)
        {
            if (use.state != state)
            {
                m_Graph.AddSetupError(std::string("pass '") + m_Graph.GetPasses()[m_PassIndex].name + "' uses " +
                                 resource_name + " in two different states");
            }
            else if (!is_write)
            {
                m_Graph.AddSetupError(std::string("pass '") + m_Graph.GetPasses()[m_PassIndex].name + "' reads and writes " +
                                 resource_name + " in the same pass");
            }
            return false;
        }
    }

    if (!m_Graph.DeclareUse(m_PassIndex, resource_index, is_texture, state, is_write))
    {
        return false;
    }

    // Remembered so a second declaration in this same pass is caught above.
    RGResourceUse use;
    use.resource_index = resource_index;
    use.state = state;
    use.is_write = is_write;
    (is_write ? m_Writes : m_Reads).push_back(use);
    return true;
}
