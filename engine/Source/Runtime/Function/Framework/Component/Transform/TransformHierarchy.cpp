#include "Runtime/Function/Framework/Component/Transform/TransformHierarchy.h"

#include "Runtime/Core/Math/LargeWorldCoordinates.h"
#include "Runtime/Function/Framework/Component/Transform/Transform.h"
#include "Runtime/Function/Framework/Component/Transform/TransformChangeDispatch.h"

#include <EASTL/vector.h>

#include <algorithm>
#include <cassert>
#include <cstring>
#include <new>

namespace
{
    Vector3 MultiplyMatrix3x3(const Matrix4x4& matrix, const Vector3& vector)
    {
        return Vector3(matrix[0][0] * vector.x + matrix[0][1] * vector.y + matrix[0][2] * vector.z,
                       matrix[1][0] * vector.x + matrix[1][1] * vector.y + matrix[1][2] * vector.z,
                       matrix[2][0] * vector.x + matrix[2][1] * vector.y + matrix[2][2] * vector.z);
    }

    struct ChangeMaskCache
    {
        TransformChangeSystemMask local_t {0};
        TransformChangeSystemMask local_r {0};
        TransformChangeSystemMask local_s {0};
        TransformChangeSystemMask global_t {0};
        TransformChangeSystemMask global_r {0};
        TransformChangeSystemMask global_s {0};
    };

    ChangeMaskCache g_ChangeMaskCache;

    void EnsureChangeMaskCacheInitialized()
    {
        if (g_ChangeMaskCache.global_t != 0)
        {
            return;
        }

        TransformChangeDispatch& dispatch = GetTransformChangeDispatch();
        g_ChangeMaskCache.local_t = dispatch.GetInterestMask(TransformChangeDispatch::kInterestedInLocalT);
        g_ChangeMaskCache.local_r = dispatch.GetInterestMask(TransformChangeDispatch::kInterestedInLocalR);
        g_ChangeMaskCache.local_s = dispatch.GetInterestMask(TransformChangeDispatch::kInterestedInLocalS);
        g_ChangeMaskCache.global_t = dispatch.GetInterestMask(TransformChangeDispatch::kInterestedInGlobalT);
        g_ChangeMaskCache.global_r = dispatch.GetInterestMask(TransformChangeDispatch::kInterestedInGlobalR);
        g_ChangeMaskCache.global_s = dispatch.GetInterestMask(TransformChangeDispatch::kInterestedInGlobalS);
    }

    template<typename T>
    T* AllocateArray(size_t count)
    {
        if (count == 0)
        {
            return nullptr;
        }
        return new T[count]();
    }

    template<typename T>
    void DeallocateArray(T* ptr)
    {
        delete[] ptr;
    }

    void CopyTransform(TransformHierarchy& src_hierarchy,
                       uint32_t src_index,
                       TransformHierarchy& dst_hierarchy,
                       uint32_t dst_index,
                       TransformChangeSystemMask change_mask)
    {
        dst_hierarchy.local_transforms[dst_index] = src_hierarchy.local_transforms[src_index];
        dst_hierarchy.deep_child_count[dst_index] = src_hierarchy.deep_child_count[src_index];
        dst_hierarchy.transform_pointers[dst_index] = src_hierarchy.transform_pointers[src_index];

        const TransformChangeSystemMask dst_interest = src_hierarchy.system_interested[src_index];
        const TransformChangeSystemMask dst_changed = dst_interest & (src_hierarchy.system_changed[src_index] | change_mask);
        dst_hierarchy.system_changed[dst_index] = dst_changed;
        dst_hierarchy.system_interested[dst_index] = dst_interest;
        dst_hierarchy.combined_system_changed |= dst_changed;
        dst_hierarchy.combined_system_interest |= dst_interest;
        dst_hierarchy.parent_indices[dst_index] = src_hierarchy.parent_indices[src_index];
    }
}  // namespace

TransformHierarchy* CreateTransformHierarchy(uint32_t transform_capacity)
{
    if (transform_capacity == 0)
    {
        return nullptr;
    }

    auto* hierarchy = new TransformHierarchy();
    hierarchy->transform_capacity = transform_capacity;
    hierarchy->local_transforms = AllocateArray<LocalTransform>(transform_capacity);
    hierarchy->parent_indices = AllocateArray<int32_t>(transform_capacity);
    hierarchy->deep_child_count = AllocateArray<uint32_t>(transform_capacity);
    hierarchy->transform_pointers = AllocateArray<Transform*>(transform_capacity);
    hierarchy->system_changed = AllocateArray<TransformChangeSystemMask>(transform_capacity);
    hierarchy->system_interested = AllocateArray<TransformChangeSystemMask>(transform_capacity);
    hierarchy->next_indices = AllocateArray<int32_t>(transform_capacity);
    hierarchy->prev_indices = AllocateArray<int32_t>(transform_capacity);

    for (uint32_t i = 0; i < transform_capacity; ++i)
    {
        hierarchy->parent_indices[i] = -1;
        hierarchy->deep_child_count[i] = 0;
        hierarchy->transform_pointers[i] = nullptr;
        hierarchy->system_changed[i] = 0;
        hierarchy->system_interested[i] = 0;
        hierarchy->prev_indices[i] = static_cast<int32_t>(i) - 1;
        hierarchy->next_indices[i] = static_cast<int32_t>(i) + 1;
    }
    hierarchy->prev_indices[0] = -1;
    hierarchy->next_indices[transform_capacity - 1] = -1;
    hierarchy->first_free_index = 0;

    GetTransformChangeDispatch().AddTransformHierarchy(*hierarchy);
    return hierarchy;
}

void DestroyTransformHierarchy(TransformHierarchy* hierarchy)
{
    if (hierarchy == nullptr)
    {
        return;
    }

    GetTransformChangeDispatch().RemoveTransformHierarchy(*hierarchy);

    DeallocateArray(hierarchy->local_transforms);
    DeallocateArray(hierarchy->parent_indices);
    DeallocateArray(hierarchy->deep_child_count);
    DeallocateArray(hierarchy->transform_pointers);
    DeallocateArray(hierarchy->system_changed);
    DeallocateArray(hierarchy->system_interested);
    DeallocateArray(hierarchy->next_indices);
    DeallocateArray(hierarchy->prev_indices);
    delete hierarchy;
}

void AllocateTransformThread(TransformHierarchy& hierarchy, uint32_t thread_first, uint32_t thread_last)
{
    assert(thread_first < hierarchy.transform_capacity);
    assert(thread_last < hierarchy.transform_capacity);

    hierarchy.first_free_index = hierarchy.next_indices[thread_last];
    hierarchy.prev_indices[thread_first] = -1;
    hierarchy.next_indices[thread_last] = -1;
}

void InsertTransformThreadAfter(TransformHierarchy& hierarchy,
                                uint32_t index,
                                uint32_t thread_first,
                                uint32_t thread_last)
{
    const int32_t next = hierarchy.next_indices[index];
    hierarchy.next_indices[index] = static_cast<int32_t>(thread_first);
    hierarchy.prev_indices[thread_first] = static_cast<int32_t>(index);
    hierarchy.next_indices[thread_last] = next;
    if (next >= 0)
    {
        hierarchy.prev_indices[static_cast<uint32_t>(next)] = static_cast<int32_t>(thread_last);
    }
}

void DetachTransformThread(TransformHierarchy& hierarchy, uint32_t thread_first, uint32_t thread_last)
{
    assert(thread_first > 0);
    assert(thread_last > 0);

    const int32_t prev = hierarchy.prev_indices[thread_first];
    const int32_t next = hierarchy.next_indices[thread_last];
    hierarchy.prev_indices[thread_first] = -1;
    hierarchy.next_indices[static_cast<uint32_t>(prev)] = next;
    hierarchy.next_indices[thread_last] = -1;
    if (next >= 0)
    {
        hierarchy.prev_indices[static_cast<uint32_t>(next)] = prev;
    }
}

void FreeTransformThread(TransformHierarchy& hierarchy, uint32_t thread_first, uint32_t thread_last)
{
    assert(thread_first > 0);
    assert(thread_last > 0);
    assert(hierarchy.prev_indices[thread_first] == -1);
    assert(hierarchy.next_indices[thread_last] == -1);

    for (int32_t cur = static_cast<int32_t>(thread_first); cur >= 0; cur = hierarchy.next_indices[static_cast<uint32_t>(cur)])
    {
        hierarchy.system_changed[static_cast<uint32_t>(cur)] = 0;
    }

    const int32_t next = hierarchy.first_free_index;
    hierarchy.first_free_index = static_cast<int32_t>(thread_first);
    hierarchy.next_indices[thread_last] = next;
    if (next >= 0)
    {
        hierarchy.prev_indices[static_cast<uint32_t>(next)] = static_cast<int32_t>(thread_last);
    }
}

void AddTransformSubhierarchy(TransformHierarchy& src_hierarchy,
                              uint32_t src_index,
                              TransformHierarchy& dst_hierarchy,
                              uint32_t& dst_first,
                              uint32_t& dst_last,
                              TransformChangeSystemMask change_mask)
{
    const uint32_t count = GetDeepChildCount(src_hierarchy, src_index);

    const uint32_t first = static_cast<uint32_t>(dst_hierarchy.first_free_index);
    uint32_t last = first;

    CopyTransform(src_hierarchy, src_index, dst_hierarchy, last, change_mask);

    int32_t cur = src_hierarchy.next_indices[src_index];
    for (uint32_t i = 1; i < count; ++i)
    {
        last = static_cast<uint32_t>(dst_hierarchy.next_indices[last]);
        CopyTransform(src_hierarchy, static_cast<uint32_t>(cur), dst_hierarchy, last, change_mask);
        cur = src_hierarchy.next_indices[static_cast<uint32_t>(cur)];
    }

    AllocateTransformThread(dst_hierarchy, first, last);
    dst_first = first;
    dst_last = last;
}

void CopyTransformSubhierarchy(TransformHierarchy& src_hierarchy,
                               uint32_t src_index,
                               TransformHierarchy& dst_hierarchy,
                               TransformChangeSystemMask change_mask)
{
    const uint32_t count = GetDeepChildCount(src_hierarchy, src_index);

    AllocateTransformThread(dst_hierarchy, 0, count - 1);

    int32_t cur = static_cast<int32_t>(src_index);
    for (uint32_t i = 0; i < count; ++i)
    {
        CopyTransform(src_hierarchy, static_cast<uint32_t>(cur), dst_hierarchy, i, change_mask);
        cur = src_hierarchy.next_indices[static_cast<uint32_t>(cur)];
    }
}

void UpdateDeepChildCountUpwards(TransformHierarchy& hierarchy, int32_t index, int32_t added_node_count)
{
    while (index >= 0)
    {
        assert(GetDeepChildCount(hierarchy, static_cast<uint32_t>(index)) + added_node_count > 0);
        hierarchy.deep_child_count[static_cast<uint32_t>(index)] =
            static_cast<uint32_t>(static_cast<int32_t>(hierarchy.deep_child_count[static_cast<uint32_t>(index)]) + added_node_count);
        index = hierarchy.parent_indices[static_cast<uint32_t>(index)];
    }
}

bool GrowTransformHierarchyCapacity(TransformHierarchy* hierarchy, uint32_t min_capacity)
{
    if (hierarchy == nullptr || min_capacity <= hierarchy->transform_capacity)
    {
        return true;
    }

    const uint32_t used = hierarchy->deep_child_count[0];
    if (used == 0 || used > min_capacity)
    {
        return false;
    }

    const uint32_t new_capacity = std::max(min_capacity, hierarchy->transform_capacity * 2u);

    LocalTransform* new_local = AllocateArray<LocalTransform>(new_capacity);
    int32_t* new_parent = AllocateArray<int32_t>(new_capacity);
    uint32_t* new_deep = AllocateArray<uint32_t>(new_capacity);
    Transform** new_pointers = AllocateArray<Transform*>(new_capacity);
    TransformChangeSystemMask* new_changed = AllocateArray<TransformChangeSystemMask>(new_capacity);
    TransformChangeSystemMask* new_interested = AllocateArray<TransformChangeSystemMask>(new_capacity);
    int32_t* new_next = AllocateArray<int32_t>(new_capacity);
    int32_t* new_prev = AllocateArray<int32_t>(new_capacity);

    const uint32_t old_capacity = hierarchy->transform_capacity;
    std::memcpy(new_local, hierarchy->local_transforms, sizeof(LocalTransform) * old_capacity);
    std::memcpy(new_parent, hierarchy->parent_indices, sizeof(int32_t) * old_capacity);
    std::memcpy(new_deep, hierarchy->deep_child_count, sizeof(uint32_t) * old_capacity);
    std::memcpy(new_pointers, hierarchy->transform_pointers, sizeof(Transform*) * old_capacity);
    std::memcpy(new_changed, hierarchy->system_changed, sizeof(TransformChangeSystemMask) * old_capacity);
    std::memcpy(new_interested, hierarchy->system_interested, sizeof(TransformChangeSystemMask) * old_capacity);
    std::memcpy(new_next, hierarchy->next_indices, sizeof(int32_t) * old_capacity);
    std::memcpy(new_prev, hierarchy->prev_indices, sizeof(int32_t) * old_capacity);

    for (uint32_t i = old_capacity; i < new_capacity; ++i)
    {
        new_parent[i] = -1;
        new_deep[i] = 0;
        new_pointers[i] = nullptr;
        new_changed[i] = 0;
        new_interested[i] = 0;
        new_prev[i] = static_cast<int32_t>(i) - 1;
        new_next[i] = static_cast<int32_t>(i) + 1;
    }
    new_prev[old_capacity] = -1;
    new_next[new_capacity - 1] = -1;
    hierarchy->first_free_index = static_cast<int32_t>(used);

    DeallocateArray(hierarchy->local_transforms);
    DeallocateArray(hierarchy->parent_indices);
    DeallocateArray(hierarchy->deep_child_count);
    DeallocateArray(hierarchy->transform_pointers);
    DeallocateArray(hierarchy->system_changed);
    DeallocateArray(hierarchy->system_interested);
    DeallocateArray(hierarchy->next_indices);
    DeallocateArray(hierarchy->prev_indices);

    hierarchy->transform_capacity = new_capacity;
    hierarchy->local_transforms = new_local;
    hierarchy->parent_indices = new_parent;
    hierarchy->deep_child_count = new_deep;
    hierarchy->transform_pointers = new_pointers;
    hierarchy->system_changed = new_changed;
    hierarchy->system_interested = new_interested;
    hierarchy->next_indices = new_next;
    hierarchy->prev_indices = new_prev;

    return true;
}

void UpdateTransformAccessors(TransformHierarchy& hierarchy, uint32_t index)
{
    const uint32_t count = GetDeepChildCount(hierarchy, index);

    Transform& root_transform = *hierarchy.transform_pointers[index];
    root_transform.m_TransformData.hierarchy = &hierarchy;
    root_transform.m_TransformData.index = index;
    if (index == 0)
    {
        hierarchy.parent_indices[index] = -1;
    }
    else
    {
        hierarchy.parent_indices[index] = static_cast<int32_t>(root_transform.m_Father->m_TransformData.index);
    }

    int32_t cur = hierarchy.next_indices[index];
    for (uint32_t i = 1; i < count; ++i)
    {
        Transform& transform = *hierarchy.transform_pointers[static_cast<uint32_t>(cur)];
        transform.m_TransformData.hierarchy = &hierarchy;
        transform.m_TransformData.index = static_cast<uint32_t>(cur);
        hierarchy.parent_indices[static_cast<uint32_t>(cur)] = static_cast<int32_t>(transform.m_Father->m_TransformData.index);
        cur = hierarchy.next_indices[static_cast<uint32_t>(cur)];
    }
}

TransformAccessReadOnly GetParent(TransformAccessReadOnly access)
{
    if (!access.IsValid())
    {
        return TransformAccessReadOnly();
    }

    const int32_t parent_index = access.hierarchy->parent_indices[access.index];
    if (parent_index < 0)
    {
        return TransformAccessReadOnly();
    }
    return TransformAccessReadOnly(access.hierarchy, static_cast<uint32_t>(parent_index));
}

uint32_t GetDeepChildCount(const TransformHierarchy& hierarchy, uint32_t index)
{
    return hierarchy.deep_child_count[index];
}

uint32_t GetDeepChildCount(TransformAccessReadOnly access)
{
    if (!access.IsValid())
    {
        return 0;
    }
    return GetDeepChildCount(*access.hierarchy, access.index);
}

const LocalTransform& GetLocalTRS(TransformAccessReadOnly access)
{
    assert(access.IsValid());
    return access.hierarchy->local_transforms[access.index];
}

LocalTransform& GetLocalTRSWritable(TransformAccess access)
{
    assert(access.IsValid());
    return access.hierarchy->local_transforms[access.index];
}

namespace
{
    void CollectTransformChain(TransformAccessReadOnly access, eastl::vector<int32_t>& out_chain)
    {
        out_chain.clear();
        if (!access.IsValid())
        {
            return;
        }

        for (int32_t index = access.index; index >= 0; index = access.hierarchy->parent_indices[index])
        {
            out_chain.push_back(index);
        }

        if (out_chain.size() > 1)
        {
            eastl::reverse(out_chain.begin(), out_chain.end());
        }
    }

    Matrix4x4 CalculateGlobalMatrixLegacy(TransformAccessReadOnly access)
    {
        Matrix4x4 world = GetLocalTRS(access).getMatrix();
        int32_t parent = access.hierarchy->parent_indices[access.index];
        while (parent >= 0)
        {
            world = access.hierarchy->local_transforms[parent].getMatrix() * world;
            parent = access.hierarchy->parent_indices[parent];
        }
        return world;
    }

    Vector3d ComposeGlobalPositionD(TransformAccessReadOnly access)
    {
        eastl::vector<int32_t> chain;
        CollectTransformChain(access, chain);

        Vector3d position {0.0, 0.0, 0.0};
        Quaternion rotation {Quaternion::IDENTITY};
        Vector3 scale {Vector3::UNIT_SCALE};

        for (const int32_t index : chain)
        {
            const LocalTransform& local = access.hierarchy->local_transforms[index];
            const Vector3 scaled_local(
                static_cast<float>(local.m_Position.x * scale.x),
                static_cast<float>(local.m_Position.y * scale.y),
                static_cast<float>(local.m_Position.z * scale.z));
            position = position + Vector3d(rotation * scaled_local);
            rotation = rotation * local.m_Rotation;
            scale = scale * local.m_Scale;
        }

        return position;
    }
}  // namespace

Matrix4x4 CalculateGlobalMatrix(TransformAccessReadOnly access)
{
    if (!access.IsValid())
    {
        return Matrix4x4::IDENTITY;
    }

    if (LargeWorldCoordinates::IsEnabled())
    {
        const Vector3d world_position = ComposeGlobalPositionD(access);
        Vector3 legacy_position;
        Vector3 scale;
        Quaternion rotation;
        CalculateGlobalMatrixLegacy(access).Decomposition(legacy_position, scale, rotation);
        const Vector3 render_position = LargeWorldCoordinates::WorldToRender(world_position);
        Matrix4x4 world;
        world.MakeTransform(render_position, scale, rotation);
        return world;
    }

    return CalculateGlobalMatrixLegacy(access);
}

Vector3d CalculateGlobalPositionD(TransformAccessReadOnly access)
{
    if (!access.IsValid())
    {
        return Vector3d::ZERO;
    }

    return ComposeGlobalPositionD(access);
}

Vector3 CalculateGlobalPosition(TransformAccessReadOnly access)
{
    const Vector3d world_position = CalculateGlobalPositionD(access);
    if (LargeWorldCoordinates::IsEnabled())
    {
        return LargeWorldCoordinates::WorldToRender(world_position);
    }
    return world_position.ToVector3();
}

Quaternion CalculateGlobalRotation(TransformAccessReadOnly access)
{
    Vector3 position;
    Vector3 scale;
    Quaternion rotation;
    CalculateGlobalMatrix(access).Decomposition(position, scale, rotation);
    return rotation;
}

Vector3 CalculateGlobalScaleLossy(TransformAccessReadOnly access)
{
    Vector3 position;
    Vector3 scale;
    Quaternion rotation;
    CalculateGlobalMatrix(access).Decomposition(position, scale, rotation);
    return scale;
}

Vector3 TransformPoint(TransformAccessReadOnly access, const Vector3& local_point)
{
    Vector4 point(local_point.x, local_point.y, local_point.z, 1.0f);
    point = CalculateGlobalMatrix(access) * point;
    return Vector3(point.x, point.y, point.z);
}

Vector3 TransformDirection(TransformAccessReadOnly access, const Vector3& local_direction)
{
    return CalculateGlobalRotation(access) * local_direction;
}

Vector3 TransformVector(TransformAccessReadOnly access, const Vector3& local_vector)
{
    return MultiplyMatrix3x3(CalculateGlobalMatrix(access), local_vector);
}

Vector3 InverseTransformPoint(TransformAccessReadOnly access, const Vector3& world_point)
{
    Vector4 point(world_point.x, world_point.y, world_point.z, 1.0f);
    point = CalculateGlobalMatrix(access).InverseAffine() * point;
    return Vector3(point.x, point.y, point.z);
}

Vector3 InverseTransformDirection(TransformAccessReadOnly access, const Vector3& world_direction)
{
    return CalculateGlobalRotation(access).inverse() * world_direction;
}

Vector3 InverseTransformVector(TransformAccessReadOnly access, const Vector3& world_vector)
{
    return MultiplyMatrix3x3(CalculateGlobalMatrix(access).InverseAffine(), world_vector);
}

namespace TransformInternal
{
    void InitLocalTRS(TransformAccess access,
                      const Vector3d& position,
                      const Quaternion& rotation,
                      const Vector3& scale)
    {
        LocalTransform& trs = GetLocalTRSWritable(access);
        trs.m_Position = position;
        trs.m_Rotation = rotation;
        trs.m_Scale = scale;
    }

    void OnTransformChangedMask(TransformAccess access,
                                TransformChangeSystemMask local_only_mask,
                                TransformChangeSystemMask common_mask,
                                TransformChangeSystemMask children_only_mask)
    {
        EnsureChangeMaskCacheInitialized();

        TransformHierarchy& hierarchy = *access.hierarchy;
        const uint32_t index = access.index;

        const TransformChangeSystemMask local_system_interested =
            hierarchy.system_interested[index] & (local_only_mask | common_mask);
        hierarchy.system_changed[index] |= local_system_interested;
        hierarchy.combined_system_changed |= local_system_interested;

        const uint32_t count = GetDeepChildCount(hierarchy, index);
        int32_t cur = hierarchy.next_indices[index];
        for (uint32_t i = 1; i < count; ++i)
        {
            const TransformChangeSystemMask child_interested =
                hierarchy.system_interested[static_cast<uint32_t>(cur)] & (common_mask | children_only_mask);
            hierarchy.system_changed[static_cast<uint32_t>(cur)] |= child_interested;
            hierarchy.combined_system_changed |= child_interested;
            cur = hierarchy.next_indices[static_cast<uint32_t>(cur)];
        }
    }

    void OnLocalPositionChanged(TransformAccess access)
    {
        EnsureChangeMaskCacheInitialized();
        OnTransformChangedMask(access, g_ChangeMaskCache.local_t, g_ChangeMaskCache.global_t, g_ChangeMaskCache.global_t);
    }

    void OnLocalRotationChanged(TransformAccess access)
    {
        EnsureChangeMaskCacheInitialized();
        OnTransformChangedMask(access, g_ChangeMaskCache.local_r, g_ChangeMaskCache.global_r, g_ChangeMaskCache.global_r);
    }

    void OnLocalScaleChanged(TransformAccess access)
    {
        EnsureChangeMaskCacheInitialized();
        OnTransformChangedMask(access,
                              g_ChangeMaskCache.local_s,
                              g_ChangeMaskCache.global_s,
                              g_ChangeMaskCache.global_r | g_ChangeMaskCache.global_t);
    }

    void OnLocalTRSChanged(TransformAccess access)
    {
        EnsureChangeMaskCacheInitialized();
        const TransformChangeSystemMask local_mask =
            g_ChangeMaskCache.local_t | g_ChangeMaskCache.local_r | g_ChangeMaskCache.local_s;
        const TransformChangeSystemMask global_mask =
            g_ChangeMaskCache.global_t | g_ChangeMaskCache.global_r | g_ChangeMaskCache.global_s;
        OnTransformChangedMask(access, local_mask, global_mask, 0);
    }
}  // namespace TransformInternal
