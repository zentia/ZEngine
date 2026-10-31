#pragma once

#include "Runtime/BaseClasses/Object.h"
#include "Runtime/Core/Math/AxisAligned.h"
#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Function/Framework/Component/Rigidbody/RigidbodyComponent.h"

#include "ScriptCollider.generated.h"

// Unity 的 `Collider` 在 ZEngine 里**不是独立组件**。
//
// 本引擎的碰撞形状（`RigidBodyShape`）是 `RigidBodyComponent` 资源的一部分 ——
// 一个对象要么有刚体（连带它的形状），要么什么碰撞体都没有。Unity 是"Collider
// 独立挂、有没有 Rigidbody 都行"，静态碰撞体就是"只有 Collider"。
//
// 所以这里沿用 `Camera` 门面的做法（见 ScriptCamera.h）：`Collider` 是一个**绑
// 在 GameObject 上的视图对象**，不是新组件。脚本写：
//
//     const col = Collider.FromGameObject(go);
//
// 而不是 `GetComponent("Collider")` —— 后者会返回 null，因为根本没有这个组件
// 类型，而"返回 null"看起来和"这个对象没有碰撞体"一模一样。
//
// 为什么不干脆造一个 ColliderComponent
// ------------------------------------
// 那要动序列化、prefab、编辑器、导入器四条链，还会产生"同一个碰撞体两份真值
// 源"（res 里的形状 vs 组件上的字段）。在已有实现上加一层薄视图，成本差两个数
// 量级，且没有双真值源。

/// 碰撞形状的三种。值与 `RigidBodyShapeType`（Resource/ResType/Components/
/// RigidBody.h）一一对应 —— 那边是资源层的枚举，不能直接拿来当脚本 API 的名字。
ZENUM()
enum class ColliderShapeType : unsigned char
{
    box = 0,
    sphere = 1,
    capsule = 2,
    invalid = 3
};

ZCLASS()
class Collider : public Object
{
    ZGENERATED_BODY(Collider)

public:
    Collider() = default;

    /// 取到 `owner` 的碰撞体视图。
    ///
    /// **同一个 GameObject 永远返回同一个 Collider 实例**（内部按 GameObject*
    /// 缓存，同 Camera 门面）：脚本可以放心缓存 `const col = Collider.FromGameObject(go)`。
    ///
    /// 返回 nullptr 的情况：`owner` 为空。
    /// **不**返回 nullptr 的情况：`owner` 身上没有 RigidBodyComponent —— 那是一
    /// 个很常见的合法状态（静态装饰物、纯逻辑对象），此时拿到的是一个
    /// `IsValid() == false` 的 Collider。区分这两者是有意的：调用方不用在每次
    /// 调用前判断 "是不是 null"，而 `IsValid()` 明确回答"有没有碰撞体"。
    ZFUNCTION(Name="FromGameObject")
    static Collider* FromGameObject(GameObject* owner);

    /// 从一次物理查询的 `bodyId` 反查碰撞体（`RaycastHit.bodyId`）。
    ///
    /// `Physics.*` 的命中记录里已经有 `gameObject`，所以这个入口主要是给"只记
    /// 了 body id"的场合（碰撞事件回调、自己缓存的 id）。
    ZFUNCTION(Name="FromBodyId")
    static Collider* FromBodyId(uint32_t body_id);

    /// 有没有碰撞体。没有 RigidBodyComponent 或刚体还没建成为 false。
    ///
    /// 注意 `HasBody()` 那一半：body 可能因为"加载时还没有物理场景"而处于
    /// pending 状态。此时形状是存在的（能读尺寸），但物理上还没参与模拟。
    /// 这里按"有没有碰撞体"回答，所以看的是形状而不是 body。
    ZFUNCTION()
    bool IsValid() const;

    ZFUNCTION()
    GameObject* GetGameObject() const { return m_Owner; }

    ZSCRIPTPROPERTY(Name="gameObject", Getter="GetGameObject")

    /// Unity 的 `collider.attachedRigidbody`：这个碰撞体挂在哪个刚体上。
    /// 本引擎里两者必然同一个组件，所以它不是 null 就是那个 RigidBodyComponent。
    ZFUNCTION(Name="GetAttachedRigidbody")
    RigidBodyComponent* GetAttachedRigidbody() const;

    ZSCRIPTPROPERTY(Name="attachedRigidbody", Getter="GetAttachedRigidbody")

    // ---- 形状 ------------------------------------------------------------

    /// 这个对象有几个碰撞形状。一个刚体可以有多个（Unity 同样：一个 GameObject
    /// 上可以挂 BoxCollider + SphereCollider）。
    ZFUNCTION()
    int32_t GetShapeCount() const;

    ZSCRIPTPROPERTY(Name="shapeCount", Getter="GetShapeCount")

    /// `index` 号形状的 `ColliderShapeType`。越界返回 invalid。
    ZFUNCTION()
    int32_t GetShapeType(int32_t index) const;

    /// 形状中心相对 GameObject 原点的偏移（Unity 的 `BoxCollider.center`）。
    /// 越界或没有碰撞体时返回零向量。
    ZFUNCTION()
    Vector3 GetShapeCenter(int32_t index) const;

    /// Unity 的 `BoxCollider.size`：**全尺寸**，不是半尺寸。非盒体返回零向量。
    ZFUNCTION()
    Vector3 GetShapeSize(int32_t index) const;

    /// Unity 的 `SphereCollider.radius` / `CapsuleCollider.radius`。非球/胶囊
    /// 返回 0。
    ZFUNCTION()
    float GetShapeRadius(int32_t index) const;

    /// Unity 的 `CapsuleCollider.height`：含两端半球帽的总高（同
    /// `CharacterController.height`）。非胶囊返回 0。
    ZFUNCTION()
    float GetShapeHeight(int32_t index) const;

    // ---- 触发 / 包围盒 ---------------------------------------------------

    /// Unity 的 `collider.isTrigger`：只报告碰撞、不产生物理响应。
    ///
    /// 走 `PhysicsScene::IsRigidBodySensor` / `SetRigidBodySensor`（Jolt 的
    /// sensor）。没有活动物理场景时：读返回 false，写是空操作 —— 与
    /// `RigidBodyComponent` 上那批方法的降级契约一致。
    ZFUNCTION()
    bool GetIsTrigger() const;

    ZFUNCTION()
    void SetIsTrigger(bool is_trigger);

    ZSCRIPTPROPERTY(Name="isTrigger", Getter="GetIsTrigger", Setter="SetIsTrigger")

    /// Unity 的 `collider.bounds.center`。
    ///
    /// **从资源算，不查物理场景**：有场景没场景答案一致，而且可测。代价是它反映
    /// 的是"按 Transform 摆好之后"的包围盒，不包含 Jolt 那一步的膨胀（Unity 的
    /// bounds 也不包含）。
    ///
    /// 没有碰撞体时返回零向量。
    ZFUNCTION()
    Vector3 GetBoundsCenter() const;

    /// Unity 的 `collider.bounds.size`（全尺寸）。
    ZFUNCTION()
    Vector3 GetBoundsSize() const;

    ZSCRIPTPROPERTY(Name="boundsCenter", Getter="GetBoundsCenter")
    ZSCRIPTPROPERTY(Name="boundsSize", Getter="GetBoundsSize")

    /// Unity 的 `collider.bounds.Contains(point)`。
    ZFUNCTION()
    bool Contains(const Vector3& point) const;

    /// 包围盒上离 `point` 最近的一点（Unity 的 `collider.ClosestPoint` 是**精确
    /// 几何**上的最近点，这里是 AABB 上的最近点）。
    ///
    /// 差别在盒体的角上最明显：AABB 的最近点会在角上，而真实盒体表面的最近点
    /// 在棱上。对"选中一个碰撞体"这类用途够用；需要精确解的场合用
    /// `Physics.Raycast` 从 point 往各个方向打。
    ZFUNCTION()
    Vector3 ClosestPoint(const Vector3& point) const;

public:
    /// 只由缓存装配，脚本无需（也不应该）自己改绑的对象。刻意**不**标
    /// ZFUNCTION：改绑一个视图等于让它指向别的东西，脚本拿到会是最难查的一类
    /// bug。
    void SetOwner(GameObject* owner) { m_Owner = owner; }

private:
    /// 缓存键。用裸指针而不是 PPtr：Collider 是**视图**，不拥有对象，也不该让
    /// 一次 PPtr 解析把"对象已销毁"变成"静默返回空视图"。生命周期由调用方保证
    /// （同 Camera 门面对 RenderCamera* 的处理）。
    GameObject* m_Owner {nullptr};

    /// 每次调用都去 GameObject 上查，不缓存 —— 组件可以在运行期被
    /// `AddComponent` / 编辑器删掉，缓存下来就是野指针。
    RigidBodyComponent* ResolveRigidBody() const;

    /// 世界空间 AABB。没有碰撞体时返回 `IsValid() == false` 的盒子。
    AxisAlignedBox ComputeWorldBounds() const;
};
