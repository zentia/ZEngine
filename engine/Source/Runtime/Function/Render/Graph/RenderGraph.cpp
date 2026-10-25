#include "Runtime/Function/Render/Graph/RenderGraph.h"

#include <algorithm>
#include <queue>
#include <set>
#include <utility>

size_t RGResource::GetByteSize() const { return is_texture ? texture_desc.GetByteSize() : buffer_desc.GetByteSize(); }

namespace
{
    bool IsWriteState(RGResourceState state)
    {
        switch (state)
        {
            case RGResourceState::RenderTarget:
            case RGResourceState::DepthStencilWrite:
            case RGResourceState::UnorderedAccess:
            case RGResourceState::CopyDest:
                return true;
            default:
                return false;
        }
    }

    bool IsReadState(RGResourceState state)
    {
        switch (state)
        {
            case RGResourceState::ShaderResource:
            case RGResourceState::DepthStencilRead:
            case RGResourceState::UnorderedAccess:
            case RGResourceState::CopySource:
                return true;
            default:
                return false;
        }
    }
}  // namespace

// =====================================================================================
// lifecycle
// =====================================================================================

RenderGraph::RenderGraph(std::string name)
    : m_Name(std::move(name))
{
}

void RenderGraph::Reset()
{
    ++m_Generation;
    m_Resources.clear();
    m_Passes.clear();
    m_SetupErrors.clear();
    m_Compiled = RGCompiledGraph {};
    m_CompiledUpToDate = false;
}

// =====================================================================================
// Setup
// =====================================================================================

uint32_t RenderGraph::AddPass(const char* name,
                              RGPassType type,
                              RGSetupFn setup,
                              RGExecuteFn execute,
                              RGPassFlags flags)
{
    RGPass pass;
    pass.name = name != nullptr ? name : "Unnamed";
    pass.type = type;
    pass.flags = flags;
    pass.execute = std::move(execute);
    m_Passes.push_back(std::move(pass));

    const uint32_t pass_index = static_cast<uint32_t>(m_Passes.size()) - 1;

    // Setup runs NOW: it only declares reads / writes and may create resources, so no
    // RHI object is touched here. A null setup is legal -- a pass with no declared
    // resources is simply a pass with nothing to order against.
    if (setup)
    {
        RenderGraphBuilder builder(*this, pass_index);
        setup(builder);
    }

    m_CompiledUpToDate = false;
    return pass_index;
}

RGTextureHandle RenderGraph::CreateTexture(const char* name, const RGTextureDesc& desc)
{
    RGResource resource;
    resource.name = name != nullptr ? name : "Texture";
    resource.is_texture = true;
    resource.texture_desc = desc;
    resource.texture_desc.name = resource.name;
    resource.generation = m_Generation;
    m_Resources.push_back(std::move(resource));
    return RGTextureHandle {static_cast<uint32_t>(m_Resources.size()) - 1, m_Generation};
}

RGBufferHandle RenderGraph::CreateBuffer(const char* name, const RGBufferDesc& desc)
{
    RGResource resource;
    resource.name = name != nullptr ? name : "Buffer";
    resource.is_texture = false;
    resource.buffer_desc = desc;
    resource.buffer_desc.name = resource.name;
    resource.generation = m_Generation;
    m_Resources.push_back(std::move(resource));
    return RGBufferHandle {static_cast<uint32_t>(m_Resources.size()) - 1, m_Generation};
}

RGTextureHandle RenderGraph::ImportTexture(const char* name, const RGExternalTexture& external)
{
    RGResource resource;
    resource.name = name != nullptr ? name : "ExternalTexture";
    resource.is_texture = true;
    resource.external = true;
    resource.external_texture = external;
    resource.generation = m_Generation;

    // Keep the descriptor roughly in sync so GetByteSize() / logging stay meaningful
    // even though nothing is allocated for it.
    resource.texture_desc.name = resource.name;
    resource.texture_desc.transient = false;
    // The only descriptor field the executor reads for an import: it picks the
    // barrier aspect (COLOR vs DEPTH[|STENCIL]) from it.
    resource.texture_desc.format = external.format;

    m_Resources.push_back(std::move(resource));
    return RGTextureHandle {static_cast<uint32_t>(m_Resources.size()) - 1, m_Generation};
}

RGBufferHandle RenderGraph::ImportBuffer(const char* name, const RGExternalBuffer& external)
{
    RGResource resource;
    resource.name = name != nullptr ? name : "ExternalBuffer";
    resource.is_texture = false;
    resource.external = true;
    resource.external_buffer = external;
    resource.generation = m_Generation;
    resource.buffer_desc.name = resource.name;
    resource.buffer_desc.transient = false;

    m_Resources.push_back(std::move(resource));
    return RGBufferHandle {static_cast<uint32_t>(m_Resources.size()) - 1, m_Generation};
}

bool RenderGraph::DeclareUse(uint32_t pass_index,
                             uint32_t resource_index,
                             bool is_texture,
                             RGResourceState state,
                             bool is_write)
{
    if (resource_index >= m_Resources.size())
    {
        AddSetupError("pass declares a resource index that does not exist");
        return false;
    }

    RGResource& resource = m_Resources[resource_index];
    if (resource.is_texture != is_texture)
    {
        AddSetupError("pass '" + m_Passes[pass_index].name + "' uses " + resource.name +
                 " as a " + (is_texture ? "texture" : "buffer") + " but it was created as the other kind");
        return false;
    }

    RGResourceUse use;
    use.resource_index = resource_index;
    use.state = state;
    use.is_write = is_write;
    if (is_write)
    {
        m_Passes[pass_index].writes.push_back(use);
        resource.writers.push_back(pass_index);
        if (resource.producer_pass == kRGInvalidIndex)
        {
            resource.producer_pass = pass_index;
        }
    }
    else
    {
        m_Passes[pass_index].reads.push_back(use);
        resource.readers.push_back(pass_index);
    }

    m_CompiledUpToDate = false;
    return true;
}

void RenderGraph::MarkExtracted(uint32_t resource_index)
{
    if (resource_index < m_Resources.size())
    {
        m_Resources[resource_index].extracted = true;
    }
}

// =====================================================================================
// Compile
// =====================================================================================

const RGCompiledGraph& RenderGraph::Compile()
{
    if (m_CompiledUpToDate)
    {
        return m_Compiled;
    }

    m_Compiled = RGCompiledGraph {};
    m_Compiled.errors = m_SetupErrors;

    ValidateResources();

    std::vector<std::vector<uint32_t>> edges(m_Passes.size());
    std::vector<uint32_t> indegree(m_Passes.size(), 0);
    BuildEdges(edges, indegree);

    std::vector<uint32_t> topo_order;
    const bool sorted = TopologicalSort(edges, indegree, topo_order);
    if (!sorted)
    {
        m_Compiled.valid = false;
        m_CompiledUpToDate = true;
        return m_Compiled;
    }

    std::vector<uint32_t> kept_order;
    CullPasses(topo_order, kept_order);

    for (size_t i = 0; i < kept_order.size(); ++i)
    {
        m_Passes[kept_order[i]].order = static_cast<uint32_t>(i);
    }
    for (const uint32_t pass_index : topo_order)
    {
        if (m_Passes[pass_index].order == kRGInvalidIndex)
        {
            m_Passes[pass_index].culled = true;
            m_Compiled.culled_passes.push_back(pass_index);
        }
    }

    m_Compiled.pass_order = kept_order;

    ComputeLifetimes(kept_order);
    AliasMemory();
    DeriveBarriers(kept_order);
    ComputeMemoryStats(kept_order);

    m_Compiled.valid = m_Compiled.errors.empty();
    m_CompiledUpToDate = true;
    return m_Compiled;
}

void RenderGraph::ValidateResources()
{
    for (const RGResource& resource : m_Resources)
    {
        // Imported resources were allocated by the caller: we never allocate them, so
        // there is nothing to validate about their descriptor.
        if (resource.external)
        {
            continue;
        }
        if (resource.is_texture)
        {
            if (resource.texture_desc.format == RGFormat::Unknown)
            {
                AddError("texture '" + resource.name + "' has no format");
            }
            if (resource.texture_desc.width == 0 || resource.texture_desc.height == 0)
            {
                AddError("texture '" + resource.name + "' has a zero extent");
            }
            if (resource.texture_desc.usage == RGTextureUsage::None)
            {
                AddWarning("texture '" + resource.name + "' declares no usage flags");
            }
        }
        else if (resource.buffer_desc.size_bytes == 0)
        {
            AddError("buffer '" + resource.name + "' has a zero size");
        }

        if (!resource.external && resource.readers.empty() && resource.writers.empty())
        {
            AddWarning("resource '" + resource.name + "' is never read or written");
        }
        if (!resource.external && resource.writers.empty() && !resource.readers.empty())
        {
            AddError("resource '" + resource.name + "' is read but never written");
        }
    }

    for (const RGPass& pass : m_Passes)
    {
        for (const RGResourceUse& use : pass.writes)
        {
            if (!IsWriteState(use.state))
            {
                AddError("pass '" + pass.name + "' writes " + m_Resources[use.resource_index].name +
                         " with the non-write state " + RGToString(use.state));
            }
        }
        for (const RGResourceUse& use : pass.reads)
        {
            if (!IsReadState(use.state))
            {
                AddError("pass '" + pass.name + "' reads " + m_Resources[use.resource_index].name +
                         " with the non-read state " + RGToString(use.state));
            }
        }
    }
}

void RenderGraph::BuildEdges(std::vector<std::vector<uint32_t>>& edges, std::vector<uint32_t>& indegree) const
{
    for (const RGResource& resource : m_Resources)
    {
        // Every user of this resource, ordered by the pass's declaration index.
        std::vector<std::pair<uint32_t, bool>> users;  // (pass index, is_write)
        users.reserve(resource.readers.size() + resource.writers.size());
        for (const uint32_t reader : resource.readers)
        {
            users.emplace_back(reader, false);
        }
        for (const uint32_t writer : resource.writers)
        {
            users.emplace_back(writer, true);
        }
        std::sort(users.begin(), users.end(),
                  [](const std::pair<uint32_t, bool>& a, const std::pair<uint32_t, bool>& b) {
                      return a.first < b.first;
                  });

        const auto add_edge = [&](uint32_t from, uint32_t to) {
            if (from == to)
            {
                return;
            }
            std::vector<uint32_t>& out = edges[from];
            if (std::find(out.begin(), out.end(), to) == out.end())
            {
                out.push_back(to);
                ++indegree[to];
            }
        };

        uint32_t last_writer = kRGInvalidIndex;
        std::vector<uint32_t> pending_readers;
        for (const auto& [pass_index, is_write] : users)
        {
            if (is_write)
            {
                // Everything read since the previous write must land before this one
                // (write-after-read), and the previous write before it (write-after-write).
                for (const uint32_t reader : pending_readers)
                {
                    add_edge(reader, pass_index);
                }
                pending_readers.clear();
                if (last_writer != kRGInvalidIndex)
                {
                    add_edge(last_writer, pass_index);
                }
                last_writer = pass_index;
            }
            else
            {
                if (last_writer != kRGInvalidIndex)
                {
                    add_edge(last_writer, pass_index);
                }
                pending_readers.push_back(pass_index);
            }
        }
    }
}

bool RenderGraph::TopologicalSort(const std::vector<std::vector<uint32_t>>& edges,
                                  std::vector<uint32_t>& indegree,
                                  std::vector<uint32_t>& order)
{
    // Smallest index first: among the passes that are ready, run the one declared first.
    // That keeps the execution order as close to the author's intent as the DAG allows,
    // and -- more importantly -- makes Compile() deterministic.
    std::priority_queue<uint32_t, std::vector<uint32_t>, std::greater<uint32_t>> ready;
    for (uint32_t i = 0; i < m_Passes.size(); ++i)
    {
        if (indegree[i] == 0)
        {
            ready.push(i);
        }
    }

    order.reserve(m_Passes.size());
    while (!ready.empty())
    {
        const uint32_t pass_index = ready.top();
        ready.pop();
        order.push_back(pass_index);

        for (const uint32_t next : edges[pass_index])
        {
            if (--indegree[next] == 0)
            {
                ready.push(next);
            }
        }
    }

    if (order.size() != m_Passes.size())
    {
        std::string involved;
        for (uint32_t i = 0; i < m_Passes.size(); ++i)
        {
            if (indegree[i] != 0)
            {
                if (!involved.empty())
                {
                    involved += ", ";
                }
                involved += m_Passes[i].name;
            }
        }
        AddError("the graph contains a cycle; passes still waiting on a dependency: " + involved);
        return false;
    }
    return true;
}

void RenderGraph::CullPasses(const std::vector<uint32_t>& order, std::vector<uint32_t>& kept) const
{
    // Reverse adjacency: from a pass to the passes that must run before it.
    std::vector<std::vector<uint32_t>> producers(m_Passes.size());
    for (const uint32_t pass_index : order)
    {
        for (const RGResourceUse& use : m_Passes[pass_index].reads)
        {
            const RGResource& resource = m_Resources[use.resource_index];
            if (resource.producer_pass != kRGInvalidIndex && resource.producer_pass != pass_index)
            {
                producers[pass_index].push_back(resource.producer_pass);
            }
        }
    }

    std::vector<bool> keep(m_Passes.size(), false);
    std::vector<uint32_t> stack;
    const auto mark = [&](uint32_t pass_index) {
        if (!keep[pass_index])
        {
            keep[pass_index] = true;
            stack.push_back(pass_index);
        }
    };

    // Roots: a pass is worth running if it produces something the graph cannot see --
    // an external/imported target, an extracted resource, or an unmodellable side effect.
    for (uint32_t i = 0; i < m_Passes.size(); ++i)
    {
        const RGPass& pass = m_Passes[i];
        if ((pass.flags & RGPassFlags::NeverCull) != RGPassFlags::None)
        {
            mark(i);
            continue;
        }
        for (const RGResourceUse& use : pass.writes)
        {
            const RGResource& resource = m_Resources[use.resource_index];
            if (resource.external || resource.extracted)
            {
                mark(i);
                break;
            }
        }
    }

    while (!stack.empty())
    {
        const uint32_t pass_index = stack.back();
        stack.pop_back();
        for (const uint32_t producer : producers[pass_index])
        {
            mark(producer);
        }
    }

    for (const uint32_t pass_index : order)
    {
        if (keep[pass_index])
        {
            kept.push_back(pass_index);
        }
    }
}

void RenderGraph::ComputeLifetimes(const std::vector<uint32_t>& kept_order)
{
    m_Compiled.lifetimes.assign(m_Resources.size(), RGResourceLifetime {});
    for (size_t i = 0; i < m_Resources.size(); ++i)
    {
        m_Compiled.lifetimes[i].resource_index = static_cast<uint32_t>(i);
        m_Compiled.lifetimes[i].byte_size = m_Resources[i].GetByteSize();
    }

    for (size_t position = 0; position < kept_order.size(); ++position)
    {
        const RGPass& pass = m_Passes[kept_order[position]];
        const auto touch = [&](const RGResourceUse& use) {
            RGResourceLifetime& lifetime = m_Compiled.lifetimes[use.resource_index];
            if (lifetime.first_pass == kRGInvalidIndex)
            {
                lifetime.first_pass = static_cast<uint32_t>(position);
            }
            lifetime.last_pass = static_cast<uint32_t>(position);
        };
        for (const RGResourceUse& use : pass.reads)
        {
            touch(use);
        }
        for (const RGResourceUse& use : pass.writes)
        {
            touch(use);
        }
    }
}

void RenderGraph::AliasMemory()
{
    struct Slot
    {
        uint32_t last_pass {0};
        size_t byte_size {0};
        bool is_texture {true};
    };

    std::vector<uint32_t> transient;
    transient.reserve(m_Resources.size());
    for (uint32_t i = 0; i < m_Resources.size(); ++i)
    {
        const RGResource& resource = m_Resources[i];
        const RGResourceLifetime& lifetime = m_Compiled.lifetimes[i];
        if (resource.external)
        {
            continue;  // caller owns the memory; never aliased
        }
        if (lifetime.first_pass == kRGInvalidIndex || lifetime.byte_size == 0)
        {
            continue;
        }
        transient.push_back(i);
    }

    std::sort(transient.begin(), transient.end(), [this](uint32_t a, uint32_t b) {
        const RGResourceLifetime& la = m_Compiled.lifetimes[a];
        const RGResourceLifetime& lb = m_Compiled.lifetimes[b];
        if (la.first_pass != lb.first_pass)
        {
            return la.first_pass < lb.first_pass;
        }
        return la.byte_size > lb.byte_size;
    });

    std::vector<Slot> slots;
    for (const uint32_t resource_index : transient)
    {
        RGResourceLifetime& lifetime = m_Compiled.lifetimes[resource_index];
        const bool is_texture = m_Resources[resource_index].is_texture;

        // An extracted resource outlives the graph, so it must never share memory with
        // anything -- it gets a dedicated slot of its own.
        if (m_Resources[resource_index].extracted)
        {
            Slot slot;
            slot.last_pass = lifetime.last_pass;
            slot.byte_size = lifetime.byte_size;
            slot.is_texture = is_texture;
            slots.push_back(slot);
            lifetime.memory_slot = static_cast<uint32_t>(slots.size()) - 1;
            continue;
        }

        size_t chosen = kRGInvalidIndex;
        for (size_t s = 0; s < slots.size(); ++s)
        {
            if (slots[s].is_texture != is_texture)
            {
                continue;
            }
            if (slots[s].byte_size < lifetime.byte_size)
            {
                continue;
            }
            if (slots[s].last_pass >= lifetime.first_pass)
            {
                continue;  // lifetimes overlap
            }
            chosen = s;
            break;
        }

        if (chosen == kRGInvalidIndex)
        {
            Slot slot;
            slot.last_pass = lifetime.last_pass;
            slot.byte_size = lifetime.byte_size;
            slot.is_texture = is_texture;
            slots.push_back(slot);
            chosen = slots.size() - 1;
        }
        else
        {
            slots[chosen].last_pass = lifetime.last_pass;
            lifetime.aliased = true;
        }
        lifetime.memory_slot = static_cast<uint32_t>(chosen);
    }

    m_Compiled.total_allocated_bytes = 0;
    for (const Slot& slot : slots)
    {
        m_Compiled.total_allocated_bytes += slot.byte_size;
    }
}

void RenderGraph::DeriveBarriers(const std::vector<uint32_t>& kept_order)
{
    // Where each resource currently is: imported resources start in the state the caller
    // reported, freshly created ones are Undefined.
    std::vector<RGResourceState> current(m_Resources.size(), RGResourceState::Undefined);
    for (size_t i = 0; i < m_Resources.size(); ++i)
    {
        current[i] = m_Resources[i].external ? m_Resources[i].GetExternalState() : RGResourceState::Undefined;
    }

    m_Compiled.barrier_offsets.reserve(kept_order.size() + 1);
    for (size_t position = 0; position < kept_order.size(); ++position)
    {
        const RGPass& pass = m_Passes[kept_order[position]];
        m_Compiled.barrier_offsets.push_back(static_cast<uint32_t>(m_Compiled.barriers.size()));

        const auto require = [&](const RGResourceUse& use) {
            const uint32_t resource_index = use.resource_index;
            if (current[resource_index] == use.state)
            {
                return;  // already there: emitting one would be a no-op barrier
            }
            RGBarrier barrier;
            barrier.resource_index = resource_index;
            barrier.is_texture = m_Resources[resource_index].is_texture;
            barrier.before = current[resource_index];
            barrier.after = use.state;
            m_Compiled.barriers.push_back(barrier);
            current[resource_index] = use.state;
        };

        for (const RGResourceUse& use : pass.reads)
        {
            require(use);
        }
        for (const RGResourceUse& use : pass.writes)
        {
            require(use);
        }
    }
    m_Compiled.barrier_offsets.push_back(static_cast<uint32_t>(m_Compiled.barriers.size()));
}

void RenderGraph::ComputeMemoryStats(const std::vector<uint32_t>& kept_order)
{
    m_Compiled.peak_live_bytes = 0;
    for (size_t position = 0; position < kept_order.size(); ++position)
    {
        size_t live = 0;
        for (const RGResourceLifetime& lifetime : m_Compiled.lifetimes)
        {
            if (m_Resources[lifetime.resource_index].external)
            {
                continue;
            }
            if (lifetime.first_pass == kRGInvalidIndex)
            {
                continue;
            }
            if (lifetime.first_pass <= position && position <= lifetime.last_pass)
            {
                live += lifetime.byte_size;
            }
        }
        m_Compiled.peak_live_bytes = std::max(m_Compiled.peak_live_bytes, live);
    }
}

void RenderGraph::AddError(std::string message)
{
    m_Compiled.errors.push_back(std::move(message));
}

void RenderGraph::AddWarning(std::string message)
{
    m_Compiled.warnings.push_back(std::move(message));
}

void RenderGraph::AddSetupError(std::string message)
{
    m_SetupErrors.push_back(std::move(message));
}
