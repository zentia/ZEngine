#include "Runtime/Function/Framework/Component/Transform/TransformComponent.h"

#include "Application/Application.h"
#include "Runtime/Function/Framework/Component/Rigidbody/RigidbodyComponent.h"

#include <algorithm>

IMPLEMENT_REGISTER_CLASS(TransformComponent)
IMPLEMENT_OBJECT_SERIALIZE(TransformComponent)
INSTANTIATE_TEMPLATE_TRANSFER_EXPORTED(TransformComponent)

template<typename TransferFunction>
void TransformComponent::Transfer(TransferFunction& transfer)
{
    Super::Transfer(transfer);
    transfer.Transfer(m_Transform, "transform");
    // Hierarchy fields. Old assets without these fields will simply read the defaults
    // (m_Parent = null PPtr, m_Children = empty vector), preserving backwards compatibility.
    transfer.Transfer(m_Parent, "m_parent");
    transfer.Transfer(m_Children, "m_children");
}

void TransformComponent::PostLoadResource(GameObject* parent_gobject)
{
    m_ParentObject = parent_gobject;
    m_TransformBuffer[0] = m_Transform;
    m_TransformBuffer[1] = m_Transform;
    m_IsDirty = true;
}

void TransformComponent::OnSerializedFieldsUpdated()
{
    const bool has_transform_changed = m_TransformBuffer[0].m_Position != m_Transform.m_Position ||
                                       m_TransformBuffer[0].m_Scale != m_Transform.m_Scale ||
                                       m_TransformBuffer[0].m_Rotation != m_Transform.m_Rotation;
    if (!has_transform_changed)
    {
        return;
    }

    m_TransformBuffer[0] = m_Transform;
    m_TransformBuffer[1] = m_Transform;
    m_IsDirty = true;
    m_IsScaleDirty = true;
}

void TransformComponent::SetPosition(const Vector3& new_translation)
{
    m_TransformBuffer[m_CurrentIndex].m_Position = new_translation;
    m_TransformBuffer[m_NextIndex].m_Position = new_translation;
    m_Transform.m_Position = new_translation;
    m_IsDirty = true;
}

void TransformComponent::SetScale(const Vector3& new_scale)
{
    m_TransformBuffer[m_CurrentIndex].m_Scale = new_scale;
    m_TransformBuffer[m_NextIndex].m_Scale = new_scale;
    m_Transform.m_Scale = new_scale;
    m_IsDirty = true;
    m_IsScaleDirty = true;
}

void TransformComponent::SetRotation(const Quaternion& new_rotation)
{
    m_TransformBuffer[m_CurrentIndex].m_Rotation = new_rotation;
    m_TransformBuffer[m_NextIndex].m_Rotation = new_rotation;
    m_Transform.m_Rotation = new_rotation;
    m_IsDirty = true;
}

void TransformComponent::Tick(float delta_time)
{
    std::swap(m_CurrentIndex, m_NextIndex);

    if (m_IsDirty)
    {
        // update transform component, dirty flag will be reset in mesh component
        TryUpdateRigidBodyComponent();
    }

    if (g_isEditorMode)
    {
        m_TransformBuffer[m_NextIndex] = m_Transform;
    }
}

void TransformComponent::TryUpdateRigidBodyComponent()
{
    if (!m_ParentObject)
        return;

    RigidBodyComponent* rigid_body_component = m_ParentObject->tryGetComponent(RigidBodyComponent);
    if (rigid_body_component)
    {
        rigid_body_component->UpdateGlobalTransform(m_TransformBuffer[m_CurrentIndex], m_IsScaleDirty);
        m_IsScaleDirty = false;
    }
}

// =====================================================================================
// Hierarchy / world-space (Phase 0 — required by the Prefab system to support nesting)
// =====================================================================================

Matrix4x4 TransformComponent::GetWorldMatrix() const
{
    // Walk from `this` up to the root, multiplying parent * child on the way down.
    // Local matrices are concatenated in parent-to-child order: world = P_root * ... * P_local.
    Matrix4x4 world = GetLocalMatrix();
    TransformComponent* parent = m_Parent;
    while (parent != nullptr)
    {
        world = parent->GetLocalMatrix() * world;
        parent = parent->GetParent();
    }
    return world;
}

Vector3 TransformComponent::GetWorldPosition() const
{
    return GetWorldMatrix().GetTrans();
}

Quaternion TransformComponent::GetWorldRotation() const
{
    Vector3 pos, scale;
    Quaternion rot;
    GetWorldMatrix().Decomposition(pos, scale, rot);
    return rot;
}

Vector3 TransformComponent::GetWorldScale() const
{
    Vector3 pos, scale;
    Quaternion rot;
    GetWorldMatrix().Decomposition(pos, scale, rot);
    return scale;
}

void TransformComponent::DetachFromParentList()
{
    TransformComponent* parent = m_Parent;
    if (parent == nullptr)
    {
        return;
    }
    auto& siblings = parent->m_Children;
    siblings.erase(std::remove_if(siblings.begin(), siblings.end(), [this](const PPtr<TransformComponent>& p) {
                       const TransformComponent* casted = p;
                       return casted == this;
                   }),
                   siblings.end());
}

void TransformComponent::AttachToParentList()
{
    TransformComponent* parent = m_Parent;
    if (parent == nullptr)
    {
        return;
    }
    auto& siblings = parent->m_Children;
    for (const PPtr<TransformComponent>& p : siblings)
    {
        const TransformComponent* casted = p;
        if (casted == this)
        {
            return;  // already a child
        }
    }
    siblings.emplace_back(this);
}

void TransformComponent::SetParent(TransformComponent* new_parent, bool worldPositionStays)
{
    // Self-assignment / cycle guard: walk up new_parent's chain; if we hit `this`,
    // the operation would create a cycle — refuse silently (Unity logs an error here).
    {
        TransformComponent* probe = new_parent;
        while (probe != nullptr)
        {
            if (probe == this)
            {
                // would create a cycle; ignore
                return;
            }
            probe = probe->GetParent();
        }
    }

    if (static_cast<TransformComponent*>(m_Parent) == new_parent)
    {
        return;
    }

    // Capture world pose before the re-parent if the caller wants it preserved.
    Matrix4x4 world_before;
    if (worldPositionStays)
    {
        world_before = GetWorldMatrix();
    }

    // Detach from previous parent's children list, then assign new parent.
    DetachFromParentList();
    m_Parent = new_parent;
    AttachToParentList();

    if (worldPositionStays)
    {
        // Recompute local pose so that world pose is unchanged: localM = inv(parentWorld) * worldBefore.
        Matrix4x4 local_after;
        if (new_parent != nullptr)
        {
            local_after = new_parent->GetWorldMatrix().InverseAffine() * world_before;
        }
        else
        {
            local_after = world_before;
        }

        Vector3 new_pos, new_scale;
        Quaternion new_rot;
        local_after.Decomposition(new_pos, new_scale, new_rot);
        SetPosition(new_pos);
        SetScale(new_scale);
        SetRotation(new_rot);
    }
    // else: keep existing local pose verbatim.
}