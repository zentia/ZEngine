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

    // Unity 里这几个是**属性**（`transform.forward = v`），不是方法。ZSCRIPTPROPERTY
    // 把已有的 ZFUNCTION getter/setter 对发布成 pesapi 访问器属性，所以脚本里
    // 两种拼写法都能用：`t.forward = v` 和 `t.SetForward(v)`。
    //
    // 没有 setter 的（`childCount` / `root` / `parent`）与 Unity 一致是只读；
    // `parent` 是唯一的例外 —— Unity 可写，这里只能读：`SetParent` 的脚本拼写
    // 是 `SetParent(parent, worldPositionStays)`（两个参数），而属性 setter 只
    // 接受一个。要改父级就用 `SetParent`。
    ZSCRIPTPROPERTY(Name="forward", Getter="GetForward", Setter="SetForward")
    ZSCRIPTPROPERTY(Name="up", Getter="GetUp", Setter="SetUp")
    ZSCRIPTPROPERTY(Name="right", Getter="GetRight", Setter="SetRight")
    ZSCRIPTPROPERTY(Name="eulerAngles", Getter="GetEulerAngles", Setter="SetEulerAngles")
    ZSCRIPTPROPERTY(Name="localEulerAngles", Getter="GetLocalEulerAngles", Setter="SetLocalEulerAngles")
    ZSCRIPTPROPERTY(Name="childCount", Getter="GetChildrenCount")
    ZSCRIPTPROPERTY(Name="root", Getter="GetRoot")
    ZSCRIPTPROPERTY(Name="parent", Getter="GetParent")

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

    // ---- 方向向量 ---------------------------------------------------------
    //
    // ⚠️ ZEngine 是 **Z-up、forward = -Y**（与 Unity 的 Y-up、forward = +Z 不同）:
    //
    //     ZEngine   X = 左      Y = -前     Z = 上
    //     Unity     X = 右      Y = 上      Z = 前
    //
    // 这套基是右手系（X × Y = Z 两边都成立），只是"哪个轴朝前/朝上"不同。
    // `GetForward()` 返回的是**这个物体在本引擎里真正朝向的方向** —— 也就是
    // MotorComponent 用来算位移的那个（`rotation * NEGATIVE_UNIT_Y`）。
    // 脚本写 `transform.GetForward() * speed * dt` 因此和引擎内部一致；如果按
    // Unity 的 +Z 去算，角色会朝着侧面走。
    //
    // 名字统一用 Unity 的拼写（forward/up/right），但**语义是 ZEngine 的轴**。

    /// 本物体朝向（local -Y 转到世界空间）。
    ZFUNCTION()
    Vector3 GetForward() const { return TransformDirection(Vector3::NEGATIVE_UNIT_Y); }
    ZFUNCTION()
    Vector3 GetBack() const { return TransformDirection(Vector3::UNIT_Y); }
    /// local +Z 转到世界空间。
    ZFUNCTION()
    Vector3 GetUp() const { return TransformDirection(Vector3::UNIT_Z); }
    ZFUNCTION()
    Vector3 GetDown() const { return TransformDirection(Vector3::NEGATIVE_UNIT_Z); }
    /// local -X（因为 local +X 是左）。
    ZFUNCTION()
    Vector3 GetRight() const { return TransformDirection(Vector3::NEGATIVE_UNIT_X); }
    ZFUNCTION()
    Vector3 GetLeft() const { return TransformDirection(Vector3::UNIT_X); }

    /// 把朝向转到 `direction`，up 尽量贴近 `up_hint`。
    ///
    /// `direction` 与 `up_hint` 平行时（正上方 / 正下方）没有唯一解，此时换一
    /// 个参考轴而不是产生 NaN —— Unity 的 `transform.forward = v` 在这种输入下
    /// 也是"给某个合法解"。
    ZFUNCTION()
    void SetForward(const Vector3& direction);

    /// 同上，对准 up。用于"贴地时把脚下方向对齐地面法线"。
    ZFUNCTION()
    void SetUp(const Vector3& direction);

    /// 对准 right（local -X）。与 SetForward / SetUp 同一套基：只给一个方向时
    /// 剩下两个自由度不唯一，up 尽量贴近当前 up，退化时换参考轴。
    ZFUNCTION()
    void SetRight(const Vector3& direction);

    // ---- 欧拉角 -----------------------------------------------------------
    //
    // 单位**度**，合成顺序是"先绕 X、再绕 Y、再绕 Z"（等价于矩阵 Rz·Ry·Rx）。
    // 与 Unity 的 `transform.eulerAngles` 一样是"三个角"，但 Unity 的合成顺序是
    // Z·X·Y（Y-up 那套）；这里按 ZEngine 的 Z-up 选了 X→Y→Z，两者**不可互换**。
    //
    // 精度提示：欧拉角有万向锁。`GetEulerAngles(SetEulerAngles(v))` 只在非退化
    // 区域等于 v；Y 分量接近 ±90° 时 x/z 会互相吞并。需要精确存朝向就用
    // `GetRotation()` / `SetRotation()` 传四元数。

    ZFUNCTION()
    Vector3 GetEulerAngles() const;
    ZFUNCTION()
    void SetEulerAngles(const Vector3& euler_angles);
    ZFUNCTION()
    Vector3 GetLocalEulerAngles() const;
    ZFUNCTION()
    void SetLocalEulerAngles(const Vector3& euler_angles);

    // ---- 移动 / 旋转 ------------------------------------------------------
    //
    // 命名规则：ZHeaderTool 里**只有一个重载能占用一个脚本名**（见上面
    // `SetLocalPosition` 的注释），所以 Unity 靠 `Space` 枚举区分的两个版本在这
    // 里是两个名字。

    /// 沿**自身**坐标轴平移（Unity 的 `Translate(v, Space.Self)`）。
    /// 这是 `Translate` 的默认语义：给角色"向前走"用 `Translate(0, s, 0)`… 不，
    /// 因为 forward = -Y，向前是 `Translate(Vector3(0, -s, 0))`。
    ZFUNCTION()
    void Translate(const Vector3& translation);

    /// 沿**世界**坐标轴平移（Unity 的 `Translate(v, Space.World)`）。
    ZFUNCTION()
    void TranslateWorld(const Vector3& translation);

    /// 绕自身轴转（欧拉角，度）。局部空间，所以连续调用会累积成"自转"。
    ZFUNCTION()
    void Rotate(const Vector3& euler_angles);

    /// 绕**世界**空间的一条轴转 `angle` 度（右手定则）。公转 / 绕柱子摆动用它，
    /// 不要用 `Rotate` —— 那个是绕自身轴。
    ZFUNCTION()
    void RotateAxis(const Vector3& axis, float angle);

    /// 绕世界空间的一个**点**转（Unity 的 `RotateAround`）：既改朝向也改位置。
    /// 相机绕角色转用它最省事。
    ZFUNCTION()
    void RotateAround(const Vector3& point, const Vector3& axis, float angle);

    /// 让朝向指向世界空间的某个点（Unity 的 `LookAt`）。up 默认世界 +Z。
    ///
    /// 目标是自己的位置时是**空操作**，不是 NaN —— 工程里出现这种输入通常是
    /// "相机跟随目标还没更新"，静默跳过比把整个 transform 写成 NaN 好（NaN 会
    /// 一路污染到渲染矩阵，而且极难回溯来源）。
    ZFUNCTION()
    void LookAt(const Vector3& world_position);
    ZFUNCTION()
    void LookAtWithUp(const Vector3& world_position, const Vector3& world_up);

    // ---- 层级 -------------------------------------------------------------

    /// 自己或任一祖先是 `other`。Unity 的 `transform.IsChildOf`。
    ZFUNCTION()
    bool IsChildOf(Transform* other) const;

    /// 解除所有子节点（挂到场景根）。Unity 的 `transform.DetachChildren`。
    ///
    /// 循环里**先取下一个**再解绑：SetParent 会改动 m_Children，正序遍历一边
    /// 改一边读会跳元素（经典的"删了一半"）。
    ZFUNCTION()
    void DetachChildren();

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
