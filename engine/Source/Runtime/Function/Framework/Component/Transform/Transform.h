#pragma once

#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/BaseClasses/PPtr.h"
#include "Runtime/Core/Math/LocalTransform.h"
#include "Runtime/Core/Math/Matrix4.h"
#include "Runtime/Core/Math/Quaternion.h"
#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Core/Math/Vector3d.h"
#include "Runtime/Function/Framework/Component/Component.h"
#include "Runtime/Function/Framework/Component/Transform/TransformAccess.h"
#include "Runtime/Function/Framework/Component/Transform/TransformChangeSystemMask.h"

#include <cstdint>
#include <vector>

struct TransformHierarchy;
class TransformSceneRoots;

/// Scene-graph transform (UnityEngine.Transform + internal hierarchy access).
class Transform : public Component
{
    REGISTER_CLASS(Transform);
    DECLARE_OBJECT_SERIALIZE();

    friend class TransformSceneRoots;
    friend void UpdateTransformAccessors(TransformHierarchy& hierarchy, uint32_t index);

public:
    enum SetParentOptions : uint32_t
    {
        kWorldPositionStays = 1u << 0,
        kLocalPositionStays = 1u << 1,
    };

    Transform() = default;

    void PostLoadResource(GameObject* parent_object) override;
    void OnSerializedFieldsUpdated() override;

    // ---- local space ----
    Vector3d GetLocalPositionD() const;
    Vector3 GetLocalPosition() const;
    Quaternion GetLocalRotation() const;
    Vector3 GetLocalScale() const;

    void SetLocalPosition(const Vector3d& local_position);
    void SetLocalPosition(const Vector3& local_position) { SetLocalPosition(Vector3d(local_position)); }
    void SetLocalRotation(const Quaternion& local_rotation);
    void SetLocalScale(const Vector3& local_scale);
    void SetLocalPositionAndRotation(const Vector3& local_position, const Quaternion& local_rotation);

    // ---- world space ----
    Vector3d GetWorldPositionD() const;
    Vector3 GetPosition() const;
    Quaternion GetRotation() const;
    Vector3 GetLossyScale() const;

    void SetPosition(const Vector3d& world_position);
    void SetPosition(const Vector3& world_position) { SetPosition(Vector3d(world_position)); }
    void SetRotation(const Quaternion& world_rotation);
    void SetPositionAndRotation(const Vector3& world_position, const Quaternion& world_rotation);

    void GetPositionAndRotation(Vector3& world_position, Quaternion& world_rotation) const;
    void GetLocalPositionAndRotation(Vector3& local_position, Quaternion& local_rotation) const;

    // ---- matrices ----
    Matrix4x4 GetLocalMatrix() const;
    Matrix4x4 GetLocalToWorldMatrix() const;
    Matrix4x4 GetWorldToLocalMatrix() const;

    // ---- space conversion ----
    Vector3 TransformPoint(const Vector3& local_point) const;
    Vector3 TransformDirection(const Vector3& local_direction) const;
    Vector3 TransformVector(const Vector3& local_vector) const;
    Vector3 InverseTransformPoint(const Vector3& world_point) const;
    Vector3 InverseTransformDirection(const Vector3& world_direction) const;
    Vector3 InverseTransformVector(const Vector3& world_vector) const;

    // ---- hierarchy ----
    Transform* GetParent() const { return m_Father; }
    Transform* GetRoot();
    const Transform* GetRoot() const;

    bool SetParent(Transform* new_parent, SetParentOptions options = kWorldPositionStays);
    bool SetParent(Transform* new_parent, bool world_position_stays)
    {
        return SetParent(new_parent,
                         world_position_stays ? kWorldPositionStays : kLocalPositionStays);
    }

    int GetChildrenCount() const { return static_cast<int>(m_Children.size()); }
    Transform* GetChild(int index) const;
    size_t GetChildCount() const { return m_Children.size(); }
    const std::vector<PPtr<Transform>>& GetChildren() const { return m_Children; }

    int GetSiblingIndex() const;
    void SetSiblingIndex(int new_index);
    void SetAsFirstSibling() { SetSiblingIndex(0); }
    void SetAsLastSibling();

    void Tick(float delta_time) override;

    void TryUpdateRigidBodyComponent();

    LocalTransform GetLocalTransform() const;
    const LocalTransform& GetLocalTransformConst() const { return m_LocalTransformBuffer[m_CurrentIndex]; }

    // ---- Unity internal: TransformAccess / hierarchy ----
    bool IsTransformHierarchyInitialized() const { return m_TransformData.hierarchy != nullptr; }
    TransformAccessReadOnly GetTransformAccessReadOnly() const { return m_TransformData; }
    TransformAccess GetTransformAccess();

    void EnsureTransformHierarchyExists();
    void RebuildTransformHierarchy();

    void SetDispatchInterested(TransformChangeSystemHandle system, bool interested);
    void SetDispatchInterestedAll(bool interested);
    void QueueChanges();

    static TransformChangeSystemHandle GetRendererChangeSystem();
    static TransformChangeSystemHandle GetPhysicsChangeSystem();

    Transform* GetNextSceneRoot() const { return m_NextSceneRoot; }
    Transform* GetPrevSceneRoot() const { return m_PrevSceneRoot; }

protected:
    Vector3 m_LocalPositionLegacy {Vector3::ZERO};
    Vector3d m_LocalPosition {Vector3d::ZERO};
    Quaternion m_LocalRotation {Quaternion::IDENTITY};
    Vector3 m_LocalScale {Vector3::UNIT_SCALE};

    LocalTransform m_LegacyTransformBlob;
    LocalTransform m_LocalTransformBuffer[2];
    size_t m_CurrentIndex {0};
    size_t m_NextIndex {1};

    PPtr<Transform> m_Father;
    std::vector<PPtr<Transform>> m_Children;

    TransformAccess m_TransformData;

private:
    void SetSceneRootLinks(Transform* prev, Transform* next);

    void ApplySerializedLocalToBuffers();
    void WriteLocalToSerializedFields();
    void SyncSerializedToHierarchy();
    void SyncHierarchyToSerialized();
    void MarkTransformDirty(bool scale_changed = false);
    void RegisterDefaultChangeInterests();
    void UpdateSceneRootRegistration(Transform* old_father);

    uint32_t CountNodesDeep() const;
    uint32_t InitializeTransformHierarchyRecursive(TransformHierarchy& hierarchy, int& index, int32_t parent_index);

    void DetachFromParentList();
    void AttachToParentList();

    uint32_t FindLastChildIndex() const;
    void EnsureCapacityIncrease(uint32_t node_count_increase);
    bool ApplySetParentHierarchy(Transform* previous_father, Transform* new_father);

    Transform* m_NextSceneRoot {nullptr};
    Transform* m_PrevSceneRoot {nullptr};
};
