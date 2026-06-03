#pragma once

#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/BaseClasses/PPtr.h"
#include "Runtime/Core/Math/Matrix4.h"
#include "Runtime/Core/Math/Quaternion.h"
#include "Runtime/Core/Math/Transform.h"
#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Function/Framework/Component/Component.h"

#include <vector>

class TransformComponent : public Component
{
    REGISTER_CLASS(TransformComponent);
    DECLARE_OBJECT_SERIALIZE();

public:
    TransformComponent() = default;

    void PostLoadResource(GameObject* parent_object) override;
    void OnSerializedFieldsUpdated() override;

    // ---- local-space accessors (existing API, unchanged) ----
    Vector3 GetPosition() const { return m_TransformBuffer[m_CurrentIndex].m_Position; }
    Vector3 GetScale() const { return m_TransformBuffer[m_CurrentIndex].m_Scale; }
    Quaternion getRotation() const { return m_TransformBuffer[m_CurrentIndex].m_Rotation; }

    void SetPosition(const Vector3& new_translation);
    void SetScale(const Vector3& new_scale);
    void SetRotation(const Quaternion& new_rotation);

    const Transform& getTransformConst() const { return m_TransformBuffer[m_CurrentIndex]; }
    Transform& getTransform() { return m_TransformBuffer[m_NextIndex]; }

    /// Local-to-parent matrix (alias: getMatrix). Same shape as before — backwards compatible.
    Matrix4x4 getMatrix() const { return m_TransformBuffer[m_CurrentIndex].getMatrix(); }
    Matrix4x4 GetLocalMatrix() const { return getMatrix(); }

    // ---- world-space accessors (new in Phase 0 — required by Prefab nesting) ----
    /// Walks up the parent chain and concatenates local matrices.
    /// Cost is O(depth); fine for editor tooling and prefab flattening.
    Matrix4x4 GetWorldMatrix() const;
    Vector3 GetWorldPosition() const;
    Quaternion GetWorldRotation() const;
    Vector3 GetWorldScale() const;

    // ---- parent / children hierarchy (new in Phase 0) ----
    TransformComponent* GetParent() const { return m_Parent; }

    /// Re-parents this transform.
    /// @param new_parent  new parent (nullptr to detach to scene root)
    /// @param worldPositionStays  if true, world-space pose is preserved across the re-parent
    ///        (Unity-equivalent semantics). Local pose is recomputed from the new parent.
    ///        if false, the existing local pose is kept verbatim under the new parent.
    void SetParent(TransformComponent* new_parent, bool worldPositionStays = true);

    size_t GetChildCount() const { return m_Children.size(); }
    TransformComponent* GetChild(size_t i) const { return (i < m_Children.size()) ? static_cast<TransformComponent*>(m_Children[i]) : nullptr; }
    const std::vector<PPtr<TransformComponent>>& GetChildren() const { return m_Children; }

    void Tick(float delta_time) override;

    void TryUpdateRigidBodyComponent();

protected:
    Transform m_Transform;

    Transform m_TransformBuffer[2];
    size_t m_CurrentIndex {0};
    size_t m_NextIndex {1};

    // Hierarchy (serialized via PPtr<TransformComponent>; resolved through ObjectManager / SerializedFile).
    PPtr<TransformComponent> m_Parent;
    std::vector<PPtr<TransformComponent>> m_Children;

private:
    // Internal helpers — do NOT participate in serialization.
    void DetachFromParentList();  // remove `this` from m_Parent->m_Children
    void AttachToParentList();    // append `this` to m_Parent->m_Children (if not already present)
};