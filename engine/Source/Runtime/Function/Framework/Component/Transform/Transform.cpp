#include "Runtime/Function/Framework/Component/Transform/Transform.h"

#include "Application/Application.h"
#include "Runtime/BaseClasses/TypeManager.h"
#include "Runtime/Function/Framework/Component/Rigidbody/RigidbodyComponent.h"
#include "Runtime/Function/Framework/Component/Transform/TransformChangeDispatch.h"
#include "Runtime/Core/Math/LargeWorldCoordinates.h"
#include "Runtime/Function/Framework/Component/Transform/TransformHierarchy.h"
#include "Runtime/Function/Framework/Component/Transform/TransformSceneRoots.h"
#include "Runtime/Function/Framework/World/WorldManager.h"

#include <algorithm>
#include <cassert>
#include <climits>

IMPLEMENT_REGISTER_CLASS(Transform)
IMPLEMENT_OBJECT_SERIALIZE(Transform)
INSTANTIATE_TEMPLATE_TRANSFER_EXPORTED(Transform)

namespace
{
    struct TransformLegacyAliasRegistrar
    {
        TransformLegacyAliasRegistrar()
        {
            TypeManager::GetInstance().RegisterClassNameAlias("TransformComponent", TypeOf<Transform>());
        }
    };
    static TransformLegacyAliasRegistrar s_TransformLegacyAliasRegistrar;

    TransformChangeSystemHandle g_RendererChangeSystem;
    TransformChangeSystemHandle g_PhysicsChangeSystem;
    bool g_TransformSystemsRegistered {false};

    void EnsureTransformSystemsRegistered()
    {
        if (g_TransformSystemsRegistered)
        {
            return;
        }

        TransformChangeDispatch& dispatch = GetTransformChangeDispatch();
        g_RendererChangeSystem =
            dispatch.RegisterSystem("Renderer", TransformChangeDispatch::kInterestedInGlobalTRS);
        g_PhysicsChangeSystem =
            dispatch.RegisterSystem("Physics", TransformChangeDispatch::kInterestedInGlobalTRS);
        g_TransformSystemsRegistered = true;
    }

    LocalTransform MakeLocalTransform(const Vector3d& position, const Quaternion& rotation, const Vector3& scale)
    {
        return LocalTransform(position, rotation, scale);
    }

    Level* GetActiveLevel()
    {
        WorldManager* world_manager = GET_SYSTEM(WorldManager);
        return world_manager != nullptr ? world_manager->getCurrentActiveLevel() : nullptr;
    }
}  // namespace

TransformChangeSystemHandle Transform::GetRendererChangeSystem()
{
    EnsureTransformSystemsRegistered();
    return g_RendererChangeSystem;
}

TransformChangeSystemHandle Transform::GetPhysicsChangeSystem()
{
    EnsureTransformSystemsRegistered();
    return g_PhysicsChangeSystem;
}

template<typename TransferFunction>
void Transform::Transfer(TransferFunction& transfer)
{
    Super::Transfer(transfer);

    transfer.Transfer(m_LocalRotation, "m_LocalRotation");
    if constexpr (TransferFunction::IsReading())
    {
        transfer.Transfer(m_LocalPositionLegacy, "m_LocalPosition");
        transfer.Transfer(m_LocalPosition, "m_LocalPositionD");
        if (m_LocalPosition == Vector3d::ZERO && m_LocalPositionLegacy != Vector3::ZERO)
        {
            m_LocalPosition = Vector3d(m_LocalPositionLegacy);
        }
    }
    else
    {
        transfer.Transfer(m_LocalPosition, "m_LocalPositionD");
        m_LocalPositionLegacy = m_LocalPosition.ToVector3();
        transfer.Transfer(m_LocalPositionLegacy, "m_LocalPosition");
    }
    transfer.Transfer(m_LocalScale, "m_LocalScale");

    if constexpr (TransferFunction::IsWriting())
    {
        m_LegacyTransformBlob = MakeLocalTransform(m_LocalPosition, m_LocalRotation, m_LocalScale);
        transfer.Transfer(m_LegacyTransformBlob, "transform");
        transfer.Transfer(m_Father, "m_Father");
        transfer.Transfer(m_Children, "m_Children");
    }
    else
    {
        transfer.Transfer(m_LegacyTransformBlob, "transform");

        transfer.Transfer(m_Father, "m_Father");
        PPtr<Transform> legacy_parent;
        transfer.Transfer(legacy_parent, "m_parent");
        if (m_Father == nullptr && legacy_parent != nullptr)
        {
            m_Father = legacy_parent;
        }

        transfer.Transfer(m_Children, "m_Children");
        std::vector<PPtr<Transform>> legacy_children;
        transfer.Transfer(legacy_children, "m_children");
        if (m_Children.empty() && !legacy_children.empty())
        {
            m_Children = std::move(legacy_children);
        }
    }
}

void Transform::SetSceneRootLinks(Transform* prev, Transform* next)
{
    m_PrevSceneRoot = prev;
    m_NextSceneRoot = next;
}

void Transform::ApplySerializedLocalToBuffers()
{
    const LocalTransform default_trs;
    const LocalTransform serialized_local =
        MakeLocalTransform(m_LocalPosition, m_LocalRotation, m_LocalScale);
    if (serialized_local.m_Position == default_trs.m_Position &&
        serialized_local.m_Rotation == default_trs.m_Rotation &&
        serialized_local.m_Scale == default_trs.m_Scale &&
        (m_LegacyTransformBlob.m_Position != default_trs.m_Position ||
         m_LegacyTransformBlob.m_Rotation != default_trs.m_Rotation ||
         m_LegacyTransformBlob.m_Scale != default_trs.m_Scale))
    {
        m_LocalPosition = m_LegacyTransformBlob.m_Position;
        m_LocalPositionLegacy = m_LocalPosition.ToVector3();
        m_LocalRotation = m_LegacyTransformBlob.m_Rotation;
        m_LocalScale = m_LegacyTransformBlob.m_Scale;
    }

    const LocalTransform local_trs = MakeLocalTransform(m_LocalPosition, m_LocalRotation, m_LocalScale);
    m_LocalTransformBuffer[0] = local_trs;
    m_LocalTransformBuffer[1] = local_trs;
}

void Transform::SyncSerializedToHierarchy()
{
    if (!IsTransformHierarchyInitialized())
    {
        return;
    }

    TransformInternal::InitLocalTRS(GetTransformAccess(),
                                    m_LocalPosition,
                                    m_LocalRotation,
                                    m_LocalScale);
}

void Transform::SyncHierarchyToSerialized()
{
    if (!IsTransformHierarchyInitialized())
    {
        return;
    }

    const LocalTransform& trs = GetLocalTRS(GetTransformAccessReadOnly());
    m_LocalPosition = trs.m_Position;
    m_LocalRotation = trs.m_Rotation;
    m_LocalScale = trs.m_Scale;
    m_LocalTransformBuffer[m_CurrentIndex] = trs;
    m_LocalTransformBuffer[m_NextIndex] = trs;
}

void Transform::WriteLocalToSerializedFields()
{
    if (IsTransformHierarchyInitialized())
    {
        SyncHierarchyToSerialized();
        return;
    }

    m_LocalPosition = m_LocalTransformBuffer[m_CurrentIndex].m_Position;
    m_LocalRotation = m_LocalTransformBuffer[m_CurrentIndex].m_Rotation;
    m_LocalScale = m_LocalTransformBuffer[m_CurrentIndex].m_Scale;
}

void Transform::MarkTransformDirty(bool scale_changed)
{
    m_IsDirty = true;
    if (scale_changed)
    {
        m_IsScaleDirty = true;
    }
}

void Transform::RegisterDefaultChangeInterests()
{
    EnsureTransformSystemsRegistered();
    if (!IsTransformHierarchyInitialized())
    {
        return;
    }

    TransformAccessReadOnly access = GetTransformAccessReadOnly();
    SetDispatchInterested(g_RendererChangeSystem, true);
    SetDispatchInterested(g_PhysicsChangeSystem, true);
}

void Transform::UpdateSceneRootRegistration(Transform* old_father)
{
    Level* level = GetActiveLevel();
    if (level == nullptr)
    {
        return;
    }

    TransformSceneRoots& roots = GetTransformSceneRootsForLevel(level);
    if (old_father == nullptr)
    {
        roots.OnTransformLeftRoot(this);
    }
    if (m_Father == nullptr)
    {
        roots.OnTransformBecameRoot(this);
    }
}

void Transform::PostLoadResource(GameObject* parent_gobject)
{
    m_ParentObject = parent_gobject;
    ApplySerializedLocalToBuffers();
    MarkTransformDirty(true);

    if (m_Father == nullptr && parent_gobject != nullptr)
    {
        UpdateSceneRootRegistration(nullptr);
    }
}

void Transform::OnSerializedFieldsUpdated()
{
    ApplySerializedLocalToBuffers();
    SyncSerializedToHierarchy();
    if (IsTransformHierarchyInitialized())
    {
        TransformInternal::OnLocalTRSChanged(GetTransformAccess());
        QueueChanges();
    }
    MarkTransformDirty(true);
}

Vector3d Transform::GetLocalPositionD() const
{
    if (IsTransformHierarchyInitialized())
    {
        return GetLocalTRS(GetTransformAccessReadOnly()).m_Position;
    }
    return m_LocalPosition;
}

Vector3 Transform::GetLocalPosition() const
{
    return GetLocalPositionD().ToVector3();
}

Quaternion Transform::GetLocalRotation() const
{
    if (IsTransformHierarchyInitialized())
    {
        return GetLocalTRS(GetTransformAccessReadOnly()).m_Rotation;
    }
    return m_LocalRotation;
}

Vector3 Transform::GetLocalScale() const
{
    if (IsTransformHierarchyInitialized())
    {
        return GetLocalTRS(GetTransformAccessReadOnly()).m_Scale;
    }
    return m_LocalScale;
}

void Transform::SetLocalPosition(const Vector3d& local_position)
{
    m_LocalPosition = local_position;
    m_LocalPositionLegacy = local_position.ToVector3();
    m_LocalTransformBuffer[m_CurrentIndex].m_Position = local_position;
    m_LocalTransformBuffer[m_NextIndex].m_Position = local_position;

    if (IsTransformHierarchyInitialized())
    {
        GetLocalTRSWritable(GetTransformAccess()).m_Position = local_position;
        TransformInternal::OnLocalPositionChanged(GetTransformAccess());
        QueueChanges();
    }

    MarkTransformDirty(false);
}

void Transform::SetLocalRotation(const Quaternion& local_rotation)
{
    m_LocalRotation = local_rotation;
    m_LocalTransformBuffer[m_CurrentIndex].m_Rotation = local_rotation;
    m_LocalTransformBuffer[m_NextIndex].m_Rotation = local_rotation;

    if (IsTransformHierarchyInitialized())
    {
        GetLocalTRSWritable(GetTransformAccess()).m_Rotation = local_rotation;
        TransformInternal::OnLocalRotationChanged(GetTransformAccess());
        QueueChanges();
    }

    MarkTransformDirty(false);
}

void Transform::SetLocalScale(const Vector3& local_scale)
{
    m_LocalScale = local_scale;
    m_LocalTransformBuffer[m_CurrentIndex].m_Scale = local_scale;
    m_LocalTransformBuffer[m_NextIndex].m_Scale = local_scale;

    if (IsTransformHierarchyInitialized())
    {
        GetLocalTRSWritable(GetTransformAccess()).m_Scale = local_scale;
        TransformInternal::OnLocalScaleChanged(GetTransformAccess());
        QueueChanges();
    }

    MarkTransformDirty(true);
}

void Transform::SetLocalPositionAndRotation(const Vector3& local_position, const Quaternion& local_rotation)
{
    SetLocalPosition(local_position);
    SetLocalRotation(local_rotation);
}

Matrix4x4 Transform::GetLocalMatrix() const
{
    if (IsTransformHierarchyInitialized())
    {
        return GetLocalTRS(GetTransformAccessReadOnly()).getMatrix();
    }
    return m_LocalTransformBuffer[m_CurrentIndex].getMatrix();
}

Matrix4x4 Transform::GetLocalToWorldMatrix() const
{
    if (IsTransformHierarchyInitialized())
    {
        return CalculateGlobalMatrix(GetTransformAccessReadOnly());
    }

    Matrix4x4 world = GetLocalMatrix();
    Transform* parent = m_Father;
    while (parent != nullptr)
    {
        world = parent->GetLocalMatrix() * world;
        parent = parent->GetParent();
    }
    return world;
}

Matrix4x4 Transform::GetWorldToLocalMatrix() const
{
    return GetLocalToWorldMatrix().InverseAffine();
}

Vector3d Transform::GetWorldPositionD() const
{
    if (IsTransformHierarchyInitialized())
    {
        return CalculateGlobalPositionD(GetTransformAccessReadOnly());
    }
    return Vector3d(GetLocalToWorldMatrix().GetTrans());
}

Vector3 Transform::GetPosition() const
{
    if (LargeWorldCoordinates::IsEnabled())
    {
        return LargeWorldCoordinates::WorldToRender(GetWorldPositionD());
    }
    return GetWorldPositionD().ToVector3();
}

Quaternion Transform::GetRotation() const
{
    if (IsTransformHierarchyInitialized())
    {
        return CalculateGlobalRotation(GetTransformAccessReadOnly());
    }

    Vector3 position;
    Vector3 scale;
    Quaternion rotation;
    GetLocalToWorldMatrix().Decomposition(position, scale, rotation);
    return rotation;
}

Vector3 Transform::GetLossyScale() const
{
    if (IsTransformHierarchyInitialized())
    {
        return CalculateGlobalScaleLossy(GetTransformAccessReadOnly());
    }

    Vector3 position;
    Vector3 scale;
    Quaternion rotation;
    GetLocalToWorldMatrix().Decomposition(position, scale, rotation);
    return scale;
}

void Transform::GetPositionAndRotation(Vector3& world_position, Quaternion& world_rotation) const
{
    world_position = GetPosition();
    world_rotation = GetRotation();
}

void Transform::GetLocalPositionAndRotation(Vector3& local_position, Quaternion& local_rotation) const
{
    local_position = GetLocalPosition();
    local_rotation = GetLocalRotation();
}

void Transform::SetPosition(const Vector3d& world_position)
{
    if (m_Father == nullptr)
    {
        SetLocalPosition(world_position);
        return;
    }

    const Vector3d parent_world = m_Father->GetWorldPositionD();
    const Quaternion parent_rotation = m_Father->GetRotation();
    const Vector3 parent_scale = m_Father->GetLossyScale();
    Vector3 delta_float = (world_position - parent_world).ToVector3();
    delta_float = parent_rotation.inverse() * delta_float;
    Vector3d delta(delta_float);
    if (std::abs(parent_scale.x) > 1e-8)
    {
        delta.x /= parent_scale.x;
    }
    if (std::abs(parent_scale.y) > 1e-8)
    {
        delta.y /= parent_scale.y;
    }
    if (std::abs(parent_scale.z) > 1e-8)
    {
        delta.z /= parent_scale.z;
    }
    SetLocalPosition(delta);
}

void Transform::SetRotation(const Quaternion& world_rotation)
{
    if (m_Father == nullptr)
    {
        SetLocalRotation(world_rotation);
        return;
    }

    const Quaternion parent_rotation = m_Father->GetRotation();
    SetLocalRotation(parent_rotation.inverse() * world_rotation);
}

void Transform::SetPositionAndRotation(const Vector3& world_position, const Quaternion& world_rotation)
{
    SetPosition(world_position);
    SetRotation(world_rotation);
}

Vector3 Transform::TransformPoint(const Vector3& local_point) const
{
    if (IsTransformHierarchyInitialized())
    {
        return ::TransformPoint(GetTransformAccessReadOnly(), local_point);
    }
    Vector4 point(local_point.x, local_point.y, local_point.z, 1.0f);
    point = GetLocalToWorldMatrix() * point;
    return Vector3(point.x, point.y, point.z);
}

Vector3 Transform::TransformDirection(const Vector3& local_direction) const
{
    if (IsTransformHierarchyInitialized())
    {
        return ::TransformDirection(GetTransformAccessReadOnly(), local_direction);
    }
    return GetRotation() * local_direction;
}

Vector3 Transform::TransformVector(const Vector3& local_vector) const
{
    if (IsTransformHierarchyInitialized())
    {
        return ::TransformVector(GetTransformAccessReadOnly(), local_vector);
    }
    Matrix4x4 matrix = GetLocalToWorldMatrix();
    return Vector3(matrix[0][0] * local_vector.x + matrix[0][1] * local_vector.y + matrix[0][2] * local_vector.z,
                   matrix[1][0] * local_vector.x + matrix[1][1] * local_vector.y + matrix[1][2] * local_vector.z,
                   matrix[2][0] * local_vector.x + matrix[2][1] * local_vector.y + matrix[2][2] * local_vector.z);
}

Vector3 Transform::InverseTransformPoint(const Vector3& world_point) const
{
    if (IsTransformHierarchyInitialized())
    {
        return ::InverseTransformPoint(GetTransformAccessReadOnly(), world_point);
    }
    Vector4 point(world_point.x, world_point.y, world_point.z, 1.0f);
    point = GetWorldToLocalMatrix() * point;
    return Vector3(point.x, point.y, point.z);
}

Vector3 Transform::InverseTransformDirection(const Vector3& world_direction) const
{
    if (IsTransformHierarchyInitialized())
    {
        return ::InverseTransformDirection(GetTransformAccessReadOnly(), world_direction);
    }
    return GetRotation().inverse() * world_direction;
}

Vector3 Transform::InverseTransformVector(const Vector3& world_vector) const
{
    if (IsTransformHierarchyInitialized())
    {
        return ::InverseTransformVector(GetTransformAccessReadOnly(), world_vector);
    }
    Matrix4x4 matrix = GetWorldToLocalMatrix();
    return Vector3(matrix[0][0] * world_vector.x + matrix[0][1] * world_vector.y + matrix[0][2] * world_vector.z,
                 matrix[1][0] * world_vector.x + matrix[1][1] * world_vector.y + matrix[1][2] * world_vector.z,
                 matrix[2][0] * world_vector.x + matrix[2][1] * world_vector.y + matrix[2][2] * world_vector.z);
}

Transform* Transform::GetChild(int index) const
{
    if (index < 0 || index >= static_cast<int>(m_Children.size()))
    {
        return nullptr;
    }
    return m_Children[static_cast<size_t>(index)];
}

Transform* Transform::GetRoot()
{
    Transform* root = this;
    while (root->GetParent() != nullptr)
    {
        root = root->GetParent();
    }
    return root;
}

const Transform* Transform::GetRoot() const
{
    return const_cast<Transform*>(this)->GetRoot();
}

int Transform::GetSiblingIndex() const
{
    if (m_Father == nullptr)
    {
        return 0;
    }

    const std::vector<PPtr<Transform>>& siblings = m_Father->m_Children;
    for (size_t i = 0; i < siblings.size(); ++i)
    {
        if (siblings[i] == this)
        {
            return static_cast<int>(i);
        }
    }
    return 0;
}

void Transform::SetSiblingIndex(int new_index)
{
    Transform* parent = m_Father;
    if (parent == nullptr)
    {
        return;
    }

    std::vector<PPtr<Transform>>& siblings = parent->m_Children;
    if (siblings.empty())
    {
        return;
    }

    if (new_index > static_cast<int>(siblings.size()) - 1)
    {
        new_index = static_cast<int>(siblings.size()) - 1;
    }

    if (new_index >= 0 && new_index < static_cast<int>(siblings.size()) && siblings[static_cast<size_t>(new_index)] == this)
    {
        return;
    }

    const int current_index = GetSiblingIndex();
    PPtr<Transform> self = this;
    siblings.erase(siblings.begin() + current_index);

    if (new_index < 0)
    {
        new_index = 0;
    }
    if (new_index > static_cast<int>(siblings.size()))
    {
        new_index = static_cast<int>(siblings.size());
    }
    siblings.insert(siblings.begin() + new_index, self);

    bool hierarchy_updated = false;
    if (IsTransformHierarchyInitialized() && parent->IsTransformHierarchyInitialized())
    {
        TransformHierarchy& hierarchy = *m_TransformData.hierarchy;
        const uint32_t thread_first = m_TransformData.index;
        const uint32_t thread_last = FindLastChildIndex();
        if (thread_first > 0)
        {
            const uint32_t thread_prev = (new_index > 0)
                                               ? static_cast<Transform*>(siblings[static_cast<size_t>(new_index - 1)])
                                                     ->FindLastChildIndex()
                                               : parent->m_TransformData.index;
            DetachTransformThread(hierarchy, thread_first, thread_last);
            InsertTransformThreadAfter(hierarchy, thread_prev, thread_first, thread_last);
            hierarchy_updated = true;
        }
    }

    if (!hierarchy_updated)
    {
        parent->RebuildTransformHierarchy();
    }
    else
    {
        const TransformChangeDispatch::InterestType sibling_interest =
            TransformChangeDispatch::kInterestedInSiblingOrder;
        const TransformChangeSystemMask sibling_mask =
            GetTransformChangeDispatch().GetChangeMaskForInterest(sibling_interest);
        TransformInternal::OnTransformChangedMask(GetTransformAccess(), 0, sibling_mask, 0);
        QueueChanges();
        GetTransformChangeDispatch().NotifyParentHierarchyChanged(parent->GetTransformAccessReadOnly());
    }

    MarkTransformDirty(true);
}

void Transform::SetAsLastSibling()
{
    SetSiblingIndex(INT_MAX);
}

TransformAccess Transform::GetTransformAccess()
{
    return m_TransformData;
}

void Transform::SetDispatchInterested(TransformChangeSystemHandle system, bool interested)
{
    if (!IsTransformHierarchyInitialized())
    {
        return;
    }
    GetTransformChangeDispatch().SetSystemInterested(GetTransformAccessReadOnly(), system, interested);
}

void Transform::SetDispatchInterestedAll(bool interested)
{
    EnsureTransformSystemsRegistered();
    SetDispatchInterested(g_RendererChangeSystem, interested);
    SetDispatchInterested(g_PhysicsChangeSystem, interested);
}

void Transform::QueueChanges()
{
    if (!IsTransformHierarchyInitialized())
    {
        return;
    }
    GetTransformChangeDispatch().QueueTransformChangeIfHasChanged(GetTransformAccess());
}

uint32_t Transform::CountNodesDeep() const
{
    uint32_t count = 1;
    for (const PPtr<Transform>& child : m_Children)
    {
        if (Transform* child_transform = child)
        {
            count += child_transform->CountNodesDeep();
        }
    }
    return count;
}

uint32_t Transform::InitializeTransformHierarchyRecursive(TransformHierarchy& hierarchy,
                                                          int& index,
                                                          int32_t parent_index)
{
    const uint32_t new_index = static_cast<uint32_t>(index);
    index = hierarchy.next_indices[new_index];

    TransformHierarchy* old_hierarchy = m_TransformData.hierarchy;
    const uint32_t old_index = m_TransformData.index;

    m_TransformData.hierarchy = &hierarchy;
    m_TransformData.index = new_index;
    hierarchy.parent_indices[new_index] = parent_index;
    hierarchy.transform_pointers[new_index] = this;

    if (old_hierarchy == nullptr)
    {
        SyncSerializedToHierarchy();
        RegisterDefaultChangeInterests();
    }
    else
    {
        hierarchy.local_transforms[new_index] = old_hierarchy->local_transforms[old_index];
        hierarchy.system_changed[new_index] = old_hierarchy->system_changed[old_index];
        hierarchy.system_interested[new_index] = old_hierarchy->system_interested[old_index];
    }

    hierarchy.combined_system_changed |= hierarchy.system_changed[new_index];
    hierarchy.combined_system_interest |= hierarchy.system_interested[new_index];

    uint32_t count = 1;
    for (const PPtr<Transform>& child : m_Children)
    {
        if (Transform* child_transform = child)
        {
            count += child_transform->InitializeTransformHierarchyRecursive(hierarchy, index, static_cast<int32_t>(new_index));
        }
    }

    hierarchy.deep_child_count[new_index] = count;
    return count;
}

void Transform::EnsureTransformHierarchyExists()
{
    if (IsTransformHierarchyInitialized())
    {
        return;
    }
    RebuildTransformHierarchy();
}

void Transform::RebuildTransformHierarchy()
{
    Transform* root = GetRoot();
    TransformHierarchy* old_hierarchy = root->m_TransformData.hierarchy;

    const uint32_t node_count = root->CountNodesDeep();
    if (node_count == 0)
    {
        return;
    }

    TransformHierarchy* hierarchy = CreateTransformHierarchy(node_count);
    AllocateTransformThread(*hierarchy, 0, node_count - 1);

    int index = 0;
    root->InitializeTransformHierarchyRecursive(*hierarchy, index, -1);
    assert(index == -1);
    assert(GetDeepChildCount(*hierarchy, 0) == node_count);

    DestroyTransformHierarchy(old_hierarchy);
    QueueChanges();
}

void Transform::Tick(float delta_time)
{
    (void)delta_time;
    std::swap(m_CurrentIndex, m_NextIndex);

    if (m_IsDirty)
    {
        TryUpdateRigidBodyComponent();
    }

    if (!g_isPlaying)
    {
        m_LocalTransformBuffer[m_NextIndex] = m_LocalTransformBuffer[m_CurrentIndex];
        WriteLocalToSerializedFields();
    }
}

void Transform::TryUpdateRigidBodyComponent()
{
    if (!m_ParentObject)
    {
        return;
    }

    RigidBodyComponent* rigid_body_component = m_ParentObject->tryGetComponent(RigidBodyComponent);
    if (rigid_body_component)
    {
        rigid_body_component->UpdateGlobalTransform(GetLocalTransformConst(), m_IsScaleDirty);
        m_IsScaleDirty = false;
    }
}

LocalTransform Transform::GetLocalTransform() const
{
    return m_LocalTransformBuffer[m_CurrentIndex];
}

void Transform::DetachFromParentList()
{
    Transform* parent = m_Father;
    if (parent == nullptr)
    {
        return;
    }

    auto& siblings = parent->m_Children;
    siblings.erase(std::remove_if(siblings.begin(),
                                  siblings.end(),
                                  [this](const PPtr<Transform>& child) { return static_cast<Transform*>(child) == this; }),
                   siblings.end());
}

void Transform::AttachToParentList()
{
    Transform* parent = m_Father;
    if (parent == nullptr)
    {
        return;
    }

    auto& siblings = parent->m_Children;
    for (const PPtr<Transform>& child : siblings)
    {
        if (static_cast<Transform*>(child) == this)
        {
            return;
        }
    }
    siblings.emplace_back(this);
}

uint32_t Transform::FindLastChildIndex() const
{
    if (!IsTransformHierarchyInitialized())
    {
        return 0;
    }

    const Transform* cur = this;
    while (!cur->m_Children.empty())
    {
        Transform* last_child = cur->m_Children.back();
        if (last_child == nullptr)
        {
            break;
        }
        cur = last_child;
    }
    return cur->m_TransformData.index;
}

void Transform::EnsureCapacityIncrease(uint32_t node_count_increase)
{
    Transform* root = GetRoot();
    if (!root->IsTransformHierarchyInitialized())
    {
        return;
    }

    TransformHierarchy& hierarchy = *root->m_TransformData.hierarchy;
    const uint32_t required_capacity = GetDeepChildCount(hierarchy, 0) + node_count_increase;
    if (required_capacity <= hierarchy.transform_capacity)
    {
        return;
    }

    GrowTransformHierarchyCapacity(&hierarchy, required_capacity * 2);
}

bool Transform::ApplySetParentHierarchy(Transform* previous_father, Transform* new_father)
{
    if (!IsTransformHierarchyInitialized())
    {
        return false;
    }

    if (new_father != nullptr)
    {
        new_father->EnsureTransformHierarchyExists();
        if (!new_father->IsTransformHierarchyInitialized())
        {
            return false;
        }
    }

    if (previous_father != nullptr)
    {
        previous_father->EnsureTransformHierarchyExists();
        if (!previous_father->IsTransformHierarchyInitialized())
        {
            return false;
        }
    }

    const uint32_t node_count = GetDeepChildCount(m_TransformData);
    if (new_father != nullptr &&
        (previous_father == nullptr || previous_father->m_TransformData.hierarchy != new_father->m_TransformData.hierarchy))
    {
        new_father->EnsureCapacityIncrease(node_count);
        if (GetDeepChildCount(*new_father->m_TransformData.hierarchy, 0) + node_count >
            new_father->m_TransformData.hierarchy->transform_capacity)
        {
            return false;
        }
    }

    const uint32_t transform_thread_insert_index =
        new_father != nullptr ? new_father->FindLastChildIndex() : 0;

    if (previous_father == nullptr && new_father != nullptr)
    {
        TransformHierarchy* old_hierarchy = m_TransformData.hierarchy;
        TransformHierarchy* new_hierarchy = new_father->m_TransformData.hierarchy;
        uint32_t new_first = 0;
        uint32_t new_last = 0;
        AddTransformSubhierarchy(*old_hierarchy, 0, *new_hierarchy, new_first, new_last, 0);
        InsertTransformThreadAfter(*new_hierarchy, transform_thread_insert_index, new_first, new_last);
        UpdateDeepChildCountUpwards(*new_hierarchy, static_cast<int32_t>(new_father->m_TransformData.index),
                                    static_cast<int32_t>(node_count));
        UpdateTransformAccessors(*new_hierarchy, new_first);
        DestroyTransformHierarchy(old_hierarchy);
        return true;
    }

    if (previous_father != nullptr && new_father == nullptr)
    {
        TransformHierarchy* old_hierarchy = m_TransformData.hierarchy;
        const uint32_t old_first = m_TransformData.index;
        const uint32_t old_last = FindLastChildIndex();
        if (old_first == 0)
        {
            return false;
        }

        TransformHierarchy* new_hierarchy = CreateTransformHierarchy(node_count);
        CopyTransformSubhierarchy(*old_hierarchy, old_first, *new_hierarchy, 0);
        DetachTransformThread(*old_hierarchy, old_first, old_last);
        FreeTransformThread(*old_hierarchy, old_first, old_last);
        UpdateDeepChildCountUpwards(*old_hierarchy,
                                    static_cast<int32_t>(previous_father->m_TransformData.index),
                                    -static_cast<int32_t>(node_count));
        UpdateTransformAccessors(*new_hierarchy, 0);
        return true;
    }

    if (previous_father != nullptr && new_father != nullptr &&
        previous_father->m_TransformData.hierarchy == new_father->m_TransformData.hierarchy)
    {
        TransformHierarchy* hierarchy = m_TransformData.hierarchy;
        const uint32_t first = m_TransformData.index;
        const uint32_t last = FindLastChildIndex();
        if (first == 0)
        {
            return false;
        }

        DetachTransformThread(*hierarchy, first, last);
        UpdateDeepChildCountUpwards(*hierarchy,
                                    static_cast<int32_t>(previous_father->m_TransformData.index),
                                    -static_cast<int32_t>(node_count));
        InsertTransformThreadAfter(*hierarchy, transform_thread_insert_index, first, last);
        UpdateDeepChildCountUpwards(*hierarchy,
                                    static_cast<int32_t>(new_father->m_TransformData.index),
                                    static_cast<int32_t>(node_count));
        UpdateTransformAccessors(*hierarchy, first);
        return true;
    }

    if (previous_father != nullptr && new_father != nullptr)
    {
        TransformHierarchy* old_hierarchy = m_TransformData.hierarchy;
        const uint32_t old_first = m_TransformData.index;
        const uint32_t old_last = FindLastChildIndex();
        if (old_first == 0)
        {
            return false;
        }

        TransformHierarchy* new_hierarchy = new_father->m_TransformData.hierarchy;
        uint32_t new_first = 0;
        uint32_t new_last = 0;
        AddTransformSubhierarchy(*old_hierarchy, m_TransformData.index, *new_hierarchy, new_first, new_last, 0);
        DetachTransformThread(*old_hierarchy, old_first, old_last);
        UpdateDeepChildCountUpwards(*old_hierarchy,
                                    static_cast<int32_t>(previous_father->m_TransformData.index),
                                    -static_cast<int32_t>(node_count));
        FreeTransformThread(*old_hierarchy, old_first, old_last);
        InsertTransformThreadAfter(*new_hierarchy, transform_thread_insert_index, new_first, new_last);
        UpdateDeepChildCountUpwards(*new_hierarchy,
                                    static_cast<int32_t>(new_father->m_TransformData.index),
                                    static_cast<int32_t>(node_count));
        UpdateTransformAccessors(*new_hierarchy, new_first);
        return true;
    }

    return false;
}

bool Transform::SetParent(Transform* new_parent, SetParentOptions options)
{
    {
        Transform* probe = new_parent;
        while (probe != nullptr)
        {
            if (probe == this)
            {
                return false;
            }
            probe = probe->GetParent();
        }
    }

    Transform* const old_father = m_Father;
    if (old_father == new_parent)
    {
        return true;
    }

    Transform* const old_root = GetRoot();

    Matrix4x4 world_before;
    const bool preserve_world = (options & kWorldPositionStays) != 0;
    if (preserve_world)
    {
        world_before = GetLocalToWorldMatrix();
    }

    DetachFromParentList();
    m_Father = new_parent;
    AttachToParentList();
    UpdateSceneRootRegistration(old_father);

    const bool hierarchy_updated = ApplySetParentHierarchy(old_father, new_parent);
    if (!hierarchy_updated)
    {
        Transform* const new_root = GetRoot();
        new_root->RebuildTransformHierarchy();
        if (old_root != new_root && old_root != nullptr)
        {
            old_root->RebuildTransformHierarchy();
        }
    }
    else if (IsTransformHierarchyInitialized())
    {
        const TransformChangeDispatch::InterestType parenting_interest =
            static_cast<TransformChangeDispatch::InterestType>(
                TransformChangeDispatch::kInterestedInGlobalTRS |
                TransformChangeDispatch::kInterestedInParentHierarchy);
        const TransformChangeSystemMask common_mask =
            GetTransformChangeDispatch().GetChangeMaskForInterest(parenting_interest);
        TransformInternal::OnTransformChangedMask(GetTransformAccess(), 0, common_mask, 0);
        QueueChanges();
        GetTransformChangeDispatch().NotifyParentHierarchyChanged(GetTransformAccessReadOnly());
        if (old_father != nullptr && old_father->IsTransformHierarchyInitialized())
        {
            GetTransformChangeDispatch().NotifyParentHierarchyChanged(old_father->GetTransformAccessReadOnly());
        }
    }

    if (preserve_world)
    {
        Matrix4x4 local_after;
        if (new_parent != nullptr)
        {
            local_after = new_parent->GetWorldToLocalMatrix() * world_before;
        }
        else
        {
            local_after = world_before;
        }

        Vector3 new_position;
        Vector3 new_scale;
        Quaternion new_rotation;
        local_after.Decomposition(new_position, new_scale, new_rotation);
        SetLocalPosition(Vector3d(new_position));
        SetLocalScale(new_scale);
        SetLocalRotation(new_rotation);
    }

    MarkTransformDirty(true);
    return true;
}
