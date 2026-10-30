#pragma once

#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/BaseClasses/PPtr.h"
#include "Runtime/Core/Math/TransformTRS.h"
#include "Runtime/Core/Math/Matrix4.h"
#include "Runtime/Core/Math/Quaternion.h"
#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Core/Math/Vector3d.h"
#include "Runtime/Function/Framework/Component/Component.h"
#include "Runtime/Function/Framework/Component/Transform/TransformAccess.h"
#include "Runtime/Function/Framework/Component/Transform/TransformChangeSystemMask.h"

#include <cstdint>
#include <vector>
#include "Transform.generated.h"

struct TransformHierarchy;
class TransformSceneRoots;

/// Scene-graph transform (UnityEngine.Transform + internal hierarchy access).
ZCLASS(CustomTransfer)
class Transform : public Component
{
    ZGENERATED_BODY(Transform)

public:

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

    // ---- script properties -------------------------------------------------
    // ZSCRIPTPROPERTY publishes an existing ZFUNCTION getter/setter pair as a
    // pesapi accessor property, so a script writes `transform.position = v`
    // instead of `transform.SetPosition(v)`. The getter must be ZFUNCTION-
    // annotated (that is where ZHeaderTool reads the TypeScript type from) and
    // take no arguments; the setter takes exactly one. Omitting Setter makes
    // the property read-only.
    //
    // Transform deliberately has no ZPROPERTY fields -- its serialization is a
    // hand-written Transfer() -- which is why these are accessor-backed rather
    // than field-backed like LightComponent's `flux` / `radius`.
    ZSCRIPTPROPERTY(Name="position", Getter="GetPosition", Setter="SetPosition")
    ZSCRIPTPROPERTY(Name="localPosition", Getter="GetLocalPosition", Setter="SetLocalPosition")
    ZSCRIPTPROPERTY(Name="rotation", Getter="GetRotation", Setter="SetRotation")
    ZSCRIPTPROPERTY(Name="localRotation", Getter="GetLocalRotation", Setter="SetLocalRotation")
    ZSCRIPTPROPERTY(Name="localScale", Getter="GetLocalScale", Setter="SetLocalScale")
    ZSCRIPTPROPERTY(Name="lossyScale", Getter="GetLossyScale")

    // ---- local space ----
    // ZFUNCTION() markers are compiler no-ops picked up by ZHeaderTool, which
    // turns them into pesapi bindings and `declare class Transform` members.
    // Overloaded pairs (SetLocalPosition(Vector3d) / (Vector3)) can only bind
    // one spelling per script name, so only the Vector3 one is exported.
    Vector3d GetLocalPositionD() const;
    ZFUNCTION()
    Vector3 GetLocalPosition() const;
    ZFUNCTION()
    Quaternion GetLocalRotation() const;
    ZFUNCTION()
    Vector3 GetLocalScale() const;

    void SetLocalPosition(const Vector3d& local_position);
    ZFUNCTION()
    void SetLocalPosition(const Vector3& local_position) { SetLocalPosition(Vector3d(local_position)); }
    ZFUNCTION()
    void SetLocalRotation(const Quaternion& local_rotation);
    ZFUNCTION()
    void SetLocalScale(const Vector3& local_scale);
    void SetLocalPositionAndRotation(const Vector3& local_position, const Quaternion& local_rotation);

    // ---- world space ----
    // Same overload rule as above: only the Vector3 / Quaternion spellings are
    // exported. The Vector3d ones stay C++-only (TS has one number type).
    Vector3d GetWorldPositionD() const;
    ZFUNCTION()
    Vector3 GetPosition() const;
    ZFUNCTION()
    Quaternion GetRotation() const;
    ZFUNCTION()
    Vector3 GetLossyScale() const;

    void SetPosition(const Vector3d& world_position);
    ZFUNCTION()
    void SetPosition(const Vector3& world_position) { SetPosition(Vector3d(world_position)); }
    ZFUNCTION()
    void SetRotation(const Quaternion& world_rotation);
    void SetPositionAndRotation(const Vector3& world_position, const Quaternion& world_rotation);

    void GetPositionAndRotation(Vector3& world_position, Quaternion& world_rotation) const;
    void GetLocalPositionAndRotation(Vector3& local_position, Quaternion& local_rotation) const;

    // ---- matrices ----
    //
    // 这三个之前没导是因为 `Matrix4x4` 没有 Converter（批次 0 才补上），不是刻意不导。
    // 矩阵本身对玩法脚本用处不大（要的是位置 / 方向），但它是"自己算点变换"的兜底：
    // 有了它，脚本不必为了一次变换去引第二套数学库。
    ZFUNCTION()
    Matrix4x4 GetLocalMatrix() const;
    ZFUNCTION()
    Matrix4x4 GetLocalToWorldMatrix() const;
    ZFUNCTION()
    Matrix4x4 GetWorldToLocalMatrix() const;

    // ---- space conversion ----
    ZFUNCTION()
    Vector3 TransformPoint(const Vector3& local_point) const;
    ZFUNCTION()
    Vector3 TransformDirection(const Vector3& local_direction) const;
    /// 与 `TransformDirection` 的区别：**会带上缩放**。位移用 Point、朝向用 Direction、
    /// 长度有意义的向量（速度、偏移）用 Vector。
    ZFUNCTION()
    Vector3 TransformVector(const Vector3& local_vector) const;
    ZFUNCTION()
    Vector3 InverseTransformPoint(const Vector3& world_point) const;
    ZFUNCTION()
    Vector3 InverseTransformDirection(const Vector3& world_direction) const;
    ZFUNCTION()
    Vector3 InverseTransformVector(const Vector3& world_vector) const;

    // ---- hierarchy ----
    ZFUNCTION()
    Transform* GetParent() const { return m_Father; }
    /// 沿 `GetParent()` 一路向上到根。自己就是根时返回自己。
    /// 只有非 const 那个标了 ZFUNCTION —— 同名的 const 重载如果也标，两个拼写会抢
    /// 同一个脚本名（见上面 `SetLocalPosition` 的重载规则），而脚本拿不到 const 对象。
    ZFUNCTION()
    Transform* GetRoot();
    const Transform* GetRoot() const;

    bool SetParent(Transform* new_parent, SetParentOptions options = kWorldPositionStays);
    // The bool spelling is the script-facing one: SetParentOptions is a scoped
    // enum with no ZScript::Converter, and only one overload may claim a name.
    ZFUNCTION()
    bool SetParent(Transform* new_parent, bool world_position_stays)
    {
        return SetParent(new_parent,
                         world_position_stays ? kWorldPositionStays : kLocalPositionStays);
    }

    ZFUNCTION()
    int GetChildrenCount() const { return static_cast<int>(m_Children.size()); }
    ZFUNCTION()
    Transform* GetChild(int index) const;
    size_t GetChildCount() const { return m_Children.size(); }

    /// Unity 的 `transform.Find("Child/Grandchild")`：按**名字**找后代，支持用
    /// '/' 分隔的相对路径。与 `GameObject.Find` 的区别是它只看子树，而且名字
    /// 不需要全局唯一 —— 场景里十个 "Wheel" 也不会互相干扰。
    ///
    /// 任一段缺失就返回 nullptr（不抛、不部分返回）：半个路径的结果没有意义，
    /// 而调用方拿到一个非预期的中间节点会当成找到了再往下去用。
    ///
    /// 空串 / 只有 '/' 返回 nullptr，不返回自己 —— Unity 同。
    ZFUNCTION()
    Transform* Find(const eastl::string& path) const;

    /// Forget m_Father / m_Children on THIS transform only, without touching the
    /// transforms they point at. For an object whose serialized state was just copied
    /// from another Transform (EditorPrefabCloner): its links still name the ORIGINAL's
    /// parent and children, and SetParent() would append to them instead of replacing
    /// them. Must be called before the clone is linked into a hierarchy.
    void ClearCopiedHierarchyLinks()
    {
        m_Father = PPtr<Transform>();
        m_Children.clear();
    }
    const std::vector<PPtr<Transform>>& GetChildren() const { return m_Children; }

    ZFUNCTION()
    int GetSiblingIndex() const;
    ZFUNCTION()
    void SetSiblingIndex(int new_index);
    /// 排到父节点的第一个 / 最后一个。`SetSiblingIndex` 已经导出了，这两个只是它的
    /// 具名特例 —— 导出来的理由是脚本不该为了"置顶"去知道"最后一个下标是多少"
    /// （SetAsLastSibling 传的是 INT_MAX，不是 `GetChildrenCount() - 1`）。
    ZFUNCTION()
    void SetAsFirstSibling() { SetSiblingIndex(0); }
    ZFUNCTION()
    void SetAsLastSibling();

    void Tick(float delta_time) override;

    void TryUpdateRigidBodyComponent();

    TransformTRS BuildLocalTRS() const;

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
