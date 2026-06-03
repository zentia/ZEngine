#include "Runtime/Function/Framework/Component/Transform/TransformChangeDispatch.h"

#include <algorithm>
#include <cassert>

namespace
{
    TransformChangeDispatch g_TransformChangeDispatch;
}  // namespace

TransformChangeDispatch& GetTransformChangeDispatch()
{
    return g_TransformChangeDispatch;
}

TransformChangeSystemHandle TransformChangeDispatch::RegisterSystem(const char* name, InterestType interest_type)
{
    assert(m_Systems.size() < static_cast<size_t>(kMaxTransformChangeSystems));

    RegisteredSystem system;
    system.name = (name != nullptr) ? name : "UnnamedTransformSystem";
    system.interest_type = interest_type;
    m_Systems.push_back(std::move(system));
    return TransformChangeSystemHandle(static_cast<uint32_t>(m_Systems.size() - 1));
}

void TransformChangeDispatch::UnregisterSystem(TransformChangeSystemHandle& system)
{
    if (!system.IsValid() || system.index >= m_Systems.size())
    {
        system.index = TransformChangeSystemHandle::kInvalidIndex;
        return;
    }

    const TransformChangeSystemMask removed_mask = system.Mask();
    for (TransformHierarchy* hierarchy : m_Hierarchies)
    {
        if (hierarchy == nullptr)
        {
            continue;
        }

        hierarchy->combined_system_interest &= ~removed_mask;
        for (uint32_t i = 0; i < hierarchy->transform_capacity; ++i)
        {
            hierarchy->system_interested[i] &= ~removed_mask;
            hierarchy->system_changed[i] &= ~removed_mask;
        }
    }

    m_Systems.erase(m_Systems.begin() + static_cast<std::ptrdiff_t>(system.index));
    for (size_t i = 0; i < m_Systems.size(); ++i)
    {
        (void)m_Systems[i];
    }
    system.index = TransformChangeSystemHandle::kInvalidIndex;
}

TransformChangeSystemMask TransformChangeDispatch::GetInterestMask(InterestType interest_type) const
{
    TransformChangeSystemMask mask = 0;
    for (size_t i = 0; i < m_Systems.size(); ++i)
    {
        if ((m_Systems[i].interest_type & interest_type) != 0)
        {
            mask |= TransformChangeSystemMask(1) << i;
        }
    }
    return mask;
}

void TransformChangeDispatch::SetSystemInterested(TransformAccessReadOnly access,
                                                  TransformChangeSystemHandle system,
                                                  bool interested)
{
    if (!access.IsValid() || !system.IsValid())
    {
        return;
    }

    TransformHierarchy& hierarchy = *access.hierarchy;
    TransformChangeSystemMask& slot = hierarchy.system_interested[access.index];
    if (interested)
    {
        slot |= system.Mask();
        hierarchy.combined_system_interest |= system.Mask();
    }
    else
    {
        slot &= ~system.Mask();
        hierarchy.system_changed[access.index] &= ~system.Mask();
    }
}

void TransformChangeDispatch::SetSystemInterested(TransformAccessReadOnly access,
                                                  InterestType interest_type,
                                                  bool interested)
{
    if (!access.IsValid())
    {
        return;
    }

    for (size_t i = 0; i < m_Systems.size(); ++i)
    {
        if ((m_Systems[i].interest_type & interest_type) != 0)
        {
            SetSystemInterested(access, TransformChangeSystemHandle(static_cast<uint32_t>(i)), interested);
        }
    }
}

void TransformChangeDispatch::QueueTransformChangeIfHasChanged(TransformAccess access)
{
    (void)access;
}

TransformChangeSystemMask TransformChangeDispatch::GetChangeMaskForInterest(InterestType interest_type) const
{
    return GetInterestMask(interest_type);
}

void TransformChangeDispatch::NotifyParentHierarchyChanged(TransformAccessReadOnly access)
{
    if (!access.IsValid())
    {
        return;
    }

    const TransformChangeSystemMask parent_mask = GetInterestMask(kInterestedInParentHierarchy);
    if (parent_mask == 0)
    {
        return;
    }

    TransformHierarchy& hierarchy = *access.hierarchy;
    int32_t index = static_cast<int32_t>(access.index);
    while (index >= 0)
    {
        const uint32_t uindex = static_cast<uint32_t>(index);
        const TransformChangeSystemMask interested = hierarchy.system_interested[uindex] & parent_mask;
        hierarchy.system_changed[uindex] |= interested;
        hierarchy.combined_system_changed |= interested;
        index = hierarchy.parent_indices[uindex];
    }
}

void TransformChangeDispatch::AddTransformHierarchy(TransformHierarchy& hierarchy)
{
    hierarchy.change_dispatch_index = static_cast<int32_t>(m_Hierarchies.size());
    m_Hierarchies.push_back(&hierarchy);
}

void TransformChangeDispatch::RemoveTransformHierarchy(TransformHierarchy& hierarchy)
{
    if (hierarchy.change_dispatch_index < 0 ||
        hierarchy.change_dispatch_index >= static_cast<int32_t>(m_Hierarchies.size()))
    {
        return;
    }

    m_Hierarchies[static_cast<size_t>(hierarchy.change_dispatch_index)] = nullptr;
    hierarchy.change_dispatch_index = -1;
}

void TransformChangeDispatch::DispatchChanges()
{
    if (m_Systems.empty())
    {
        return;
    }

    std::vector<TransformChange> batch;
    for (size_t system_index = 0; system_index < m_Systems.size(); ++system_index)
    {
        const TransformChangeSystemMask system_mask = TransformChangeSystemMask(1) << system_index;
        batch.clear();

        for (TransformHierarchy* hierarchy : m_Hierarchies)
        {
            if (hierarchy == nullptr || hierarchy->combined_system_changed == 0)
            {
                continue;
            }

            if (hierarchy->transform_capacity == 0 || hierarchy->deep_child_count[0] == 0)
            {
                continue;
            }

            int32_t cur = 0;
            const uint32_t node_count = hierarchy->deep_child_count[0];
            for (uint32_t node_index = 0; node_index < node_count && cur >= 0; ++node_index)
            {
                const uint32_t uindex = static_cast<uint32_t>(cur);
                if ((hierarchy->system_changed[uindex] & system_mask) != 0)
                {
                    batch.push_back({TransformAccessReadOnly(hierarchy, uindex), system_mask});
                    hierarchy->system_changed[uindex] &= ~system_mask;
                }
                cur = hierarchy->next_indices[uindex];
            }

            hierarchy->combined_system_changed = 0;
            for (uint32_t i = 0; i < hierarchy->transform_capacity; ++i)
            {
                hierarchy->combined_system_changed |= hierarchy->system_changed[i];
            }
        }

        if (batch.empty())
        {
            continue;
        }

        for (const TransformChangeBatchCallback& callback : m_Systems[system_index].batch_callbacks)
        {
            if (callback)
            {
                callback(batch.data(), batch.size());
            }
        }
    }
}

void TransformChangeDispatch::RegisterBatchCallback(TransformChangeSystemHandle system,
                                                    TransformChangeBatchCallback callback)
{
    if (!system.IsValid() || system.index >= m_Systems.size() || !callback)
    {
        return;
    }
    m_Systems[system.index].batch_callbacks.push_back(std::move(callback));
}

void TransformChangeDispatch::ClearBatchCallbacks(TransformChangeSystemHandle system)
{
    if (!system.IsValid() || system.index >= m_Systems.size())
    {
        return;
    }
    m_Systems[system.index].batch_callbacks.clear();
}
