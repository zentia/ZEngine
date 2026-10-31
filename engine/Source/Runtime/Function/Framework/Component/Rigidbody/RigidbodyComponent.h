#pragma once

#include "Runtime/Core/Math/Quaternion.h"
#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Function/Framework/Component/Component.h"
#include "Runtime/Resource/ResType/Components/RigidBody.h"

#include <memory>

class PhysicsScene;

#include "RigidbodyComponent.generated.h"

ZCLASS()
class RigidBodyComponent : public Component
{
    ZGENERATED_BODY(RigidBodyComponent)

public:
    RigidBodyComponent() = default;
    ~RigidBodyComponent() override;

    void PostLoadResource(GameObject* parent_object) override;

    /// True when PostLoadResource could not create the body because no physics
    /// scene was active yet. The body stays uncreated until FlushPendingBody
    /// succeeds, so a level whose bodies are all pending has no colliders.
    bool HasPendingBody() const;

    /// Create the body deferred by PostLoadResource. Returns true if it was
    /// created; false if nothing was pending or the scene is still unavailable
    /// (which leaves it pending, safe to retry).
    bool FlushPendingBody();

    // --- public physics API -------------------------------------------------
    //
    // Everything below reads or writes the live Jolt body through the current
    // physics scene, and every one of them is a no-op (or a zero / default
    // value) when there is no body: a body can be pending creation or already
    // torn down while a script still holds the component, and a script must not
    // be able to take the frame down by touching it.

    /// False when no body exists yet (pending creation, creation failed, or the
    /// level is being torn down). Every other call below is a no-op then.
    ZFUNCTION()
    bool HasBody() const { return m_RigidbodyId != k_invalid_rigidbody_id; }

    /// Whether the body simulates: gravity, forces, impulses and collisions.
    /// Reading it returns the *authored* value, so it stays meaningful while the
    /// body is still pending.
    ZFUNCTION()
    RigidBodyActorType GetActorType() const { return m_RigidbodyRes.GetActorType(); }

    /// Change the body type. Writes the serialized field (so it survives a
    /// save/reload) and applies it to the live body if there is one.
    ZFUNCTION()
    void SetActorType(RigidBodyActorType actor_type);

    /// Metres per second. Zero for a static body.
    ZFUNCTION()
    Vector3 GetLinearVelocity() const;
    ZFUNCTION()
    void SetLinearVelocity(const Vector3& velocity);

    /// Radians per second. Zero for a static body.
    ZFUNCTION()
    Vector3 GetAngularVelocity() const;
    ZFUNCTION()
    void SetAngularVelocity(const Vector3& velocity);

    /// Continuous force (Newtons), accumulated until the next physics step and
    /// cleared by it. Ignored on a non-dynamic body.
    ZFUNCTION()
    void AddForce(const Vector3& force);

    /// Instantaneous momentum change -- the "kick it" primitive. Ignored on a
    /// non-dynamic body.
    ZFUNCTION()
    void AddImpulse(const Vector3& impulse);

    /// Kilograms. 0 when the body is immovable (static / kinematic, or an
    /// authored inverse mass of 0).
    ZFUNCTION()
    float GetMass() const;

    /// Unity's `rigidbody.mass = m`。
    ///
    /// 质量 <= 0 时夹到极小正数：0 质量的倒数质量是 inf，一步积分后就是 NaN
    /// 位置，而一个 NaN 物体会把整个 broadphase 拖垮（表现是"物理突然全不
    /// 动了"，而且不报错）。
    ZFUNCTION()
    void SetMass(float mass);

    /// Unity 的 `rigidbody.drag`：每秒保留 (1 - drag) 的线速度。
    ZFUNCTION()
    float GetDrag() const;
    ZFUNCTION()
    void SetDrag(float drag);

    /// Unity 的 `rigidbody.angularDrag`，同上但作用于角速度。
    ZFUNCTION()
    float GetAngularDrag() const;
    ZFUNCTION()
    void SetAngularDrag(float angular_drag);

    /// Unity 的 `rigidbody.useGravity`。
    ZFUNCTION()
    bool GetUseGravity() const;
    ZFUNCTION()
    void SetUseGravity(bool use_gravity);

    /// Unity 的 `rigidbody.isKinematic`：运动学物体不受力和碰撞推动，但**会**
    /// 推别人 —— 这正是"移动平台"和"玩家推杆"要的。
    ZFUNCTION()
    bool GetIsKinematic() const;
    ZFUNCTION()
    void SetIsKinematic(bool is_kinematic);

    /// 连续力矩（牛·米），累加到下一步、被下一步清掉。
    ZFUNCTION()
    void AddTorque(const Vector3& torque);

    /// 瞬时角动量变化 —— "让它转起来"的原语。
    ZFUNCTION()
    void AddAngularImpulse(const Vector3& angular_impulse);

    /// 作用在**世界坐标某一点**上的力。和 `AddForce` 的差别是它会带出力矩：
    /// "踢箱子的角"会让它一边飞一边转，而 `AddForce` 只会让它平移。
    ZFUNCTION()
    void AddForceAtPosition(const Vector3& force, const Vector3& position);

    /// 作用在**世界坐标某一点**上的冲量（子弹命中、爆炸的手感）。
    ZFUNCTION()
    void AddImpulseAtPosition(const Vector3& impulse, const Vector3& position);

    /// Unity 的 `AddExplosionForce`：模拟一次爆炸对本刚体的作用。
    ///
    /// 和 Unity 一样只作用于**这一个**刚体（不是"把周围都炸飞"；那是自己在
    /// `Physics.OverlapSphere` 上循环）。衰减按距离线性降到 0，半径外的物体
    /// 不受力 —— 半径内的物体则一定被推动，这是"手雷在脚下"和"手雷在墙那
    /// 边"的分界。
    ZFUNCTION()
    void AddExplosionForce(float explosion_force, const Vector3& explosion_position, float explosion_radius);

    /// Where the body actually is. For a simulated body this drifts away from
    /// the Transform component, because nothing writes the body back yet.
    ZFUNCTION()
    Vector3 GetBodyPosition() const;
    ZFUNCTION()
    Quaternion GetBodyRotation() const;

    /// Unity 的 `rigidbody.position` / `rigidbody.rotation`：直接搬运，**不**
    /// 产生扫掠（路上的人不会被推开）。要"推着别人走"用 `MovePosition`。
    ZFUNCTION()
    Vector3 GetPosition() const;
    ZFUNCTION()
    void SetPosition(const Vector3& position);
    ZFUNCTION()
    Quaternion GetRotation() const;
    ZFUNCTION()
    void SetRotation(const Quaternion& rotation);

    /// Unity 的 `MovePosition` / `MoveRotation`：由物理步按 delta_time 反推速度
    /// 走过去，所以**会**推开路上的东西。运动学物体的正确移动方式。
    ZFUNCTION()
    void MovePosition(const Vector3& position);
    ZFUNCTION()
    void MoveRotation(const Quaternion& rotation);

    /// 是否已经睡着。睡着的物体不再模拟也不再上报接触 —— "该动没动"有一半
    /// 是这个原因。
    ZFUNCTION()
    bool IsSleeping() const;
    ZFUNCTION()
    void Sleep();
    ZFUNCTION()
    void WakeUp();

    ZSCRIPTPROPERTY(Name="mass", Getter="GetMass", Setter="SetMass")
    ZSCRIPTPROPERTY(Name="drag", Getter="GetDrag", Setter="SetDrag")
    ZSCRIPTPROPERTY(Name="angularDrag", Getter="GetAngularDrag", Setter="SetAngularDrag")
    ZSCRIPTPROPERTY(Name="useGravity", Getter="GetUseGravity", Setter="SetUseGravity")
    ZSCRIPTPROPERTY(Name="isKinematic", Getter="GetIsKinematic", Setter="SetIsKinematic")
    ZSCRIPTPROPERTY(Name="velocity", Getter="GetLinearVelocity", Setter="SetLinearVelocity")
    ZSCRIPTPROPERTY(Name="angularVelocity", Getter="GetAngularVelocity", Setter="SetAngularVelocity")
    ZSCRIPTPROPERTY(Name="position", Getter="GetPosition", Setter="SetPosition")
    ZSCRIPTPROPERTY(Name="rotation", Getter="GetRotation", Setter="SetRotation")

    void Tick(float delta_time) override {}
    void UpdateGlobalTransform(const TransformTRS& transform, bool is_scale_dirty);
    void GetShapeBoundingBoxes(std::vector<AxisAlignedBox>& out_boudning_boxes) const;

    /// Read-only access to the serialized shape list. RigidBodyShape derives
    /// from Object, so each shape needs its own slot in a written object
    /// graph; serialization drivers (PrefabUtility's flattener) walk this to
    /// collect them. Without that the shapes serialize as null references.
    const RigidBodyComponentRes& GetRigidbodyRes() const { return m_RigidbodyRes; }

    /// Mutable access for importers/migrators that must patch the shape list
    /// after construction (legacy JSON stores shapes inline, while the runtime
    /// holds them by reference).
    RigidBodyComponentRes& GetRigidbodyResMutable() { return m_RigidbodyRes; }

    /// The Jolt body id, or 0xffffffff when there is no body. Scripts compare
    /// this against `RaycastHit.bodyId` to recognise their own object.
    ZFUNCTION()
    uint32_t GetBodyId() const { return m_RigidbodyId; }

    /// Re-read the parent GameObject's layer and push it into the live body.
    ///
    /// Called by `GameObject::SetLayerIndex`: Unity applies a layer change to
    /// physics immediately, and a body that kept its old Jolt object layer would
    /// keep colliding through a matrix the author had just changed.
    void RefreshBodyLayer();

    /// The Unity layer the body should be created on.
    int32_t CurrentGameObjectLayer() const;

protected:
    void CreateRigidBody(const TransformTRS& global_transform);
    void RemoveRigidBody();

    /// 在 PhysicsManager 的 body -> GameObject 表里登记 / 注销自己。
    /// 没有这张表，接触事件里的 body id 翻译不成 GameObject。
    void RegisterOwner();
    void UnregisterOwner();

    /// The scene the live body lives in, or null when there is no body (pending
    /// creation) or no active scene (level teardown destroys it first).
    std::shared_ptr<PhysicsScene> GetActivePhysicsScene() const;

    ZPROPERTY()
    RigidBodyComponentRes m_RigidbodyRes;

    /// Sentinel for "no body created yet". Matches the value the field was
    /// already default-initialized to, so an unflushed body is detectable.
    static constexpr uint32_t k_invalid_rigidbody_id = 0xffffffff;

    uint32_t m_RigidbodyId {k_invalid_rigidbody_id};

    /// Set by PostLoadResource when no physics scene was active yet; cleared
    /// when the body is actually created.
    bool m_HasPendingBody {false};
};
