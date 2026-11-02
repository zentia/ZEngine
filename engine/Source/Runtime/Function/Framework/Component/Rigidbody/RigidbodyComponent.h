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

    // --- surface material (Unity: `collider.material`) ----------------------
    //
    // Unity hangs these off the **collider**; Jolt has no per-shape material at
    // all -- one friction and one restitution per body -- so they live on the
    // Rigidbody here. That is a genuine deviation, documented in
    // doc/PHYSIC_MATERIAL_SCRIPT_API.md; the alternative (store a per-shape
    // material and never read it) is worse than an honest difference.
    //
    // Unity also splits friction into static and dynamic and picks by whether
    // the contact is sliding. With one coefficient per body there is nowhere to
    // put the second, so there is a single `friction` rather than a field that
    // is written, serialized and quietly ignored.

    ZFUNCTION()
    float GetFriction() const { return m_RigidbodyRes.m_Friction; }

    /// 同时写回 res 与活体：只改活体的话下一次重新建体（关卡重载、pending flush）
    /// 就静默回到作者设的值。
    ZFUNCTION()
    void SetFriction(float friction);

    ZFUNCTION()
    float GetBounciness() const { return m_RigidbodyRes.m_Bounciness; }

    ZFUNCTION()
    void SetBounciness(float bounciness);

    ZFUNCTION()
    PhysicMaterialCombine GetFrictionCombine() const { return m_RigidbodyRes.GetFrictionCombine(); }

    ZFUNCTION()
    void SetFrictionCombine(PhysicMaterialCombine combine_mode);

    ZFUNCTION()
    PhysicMaterialCombine GetBounceCombine() const { return m_RigidbodyRes.GetBounceCombine(); }

    ZFUNCTION()
    void SetBounceCombine(PhysicMaterialCombine combine_mode);

    // =========================================================================
    // 自由度约束（Unity 的 `rigidbody.constraints` / `freezeRotation`）
    // =========================================================================
    //
    // Unity 的 `constraints` 是一个位标志集，"冻住"的是**世界轴**（不是物体局部
    // 轴）—— `FreezeRotationY` 是"绕世界 Y 轴不转"，物体怎么摆都不影响。Jolt 的
    // `EAllowedDOFs` 同样是**世界轴**掩码，所以两边语义对得上，只是互为补集且
    // 位序不同（映射见 `RigidBody.h` 的 `ToJoltAllowedDOFs`）。
    //
    // ⚠️ 与 Unity 的一处差别，写在注释里：Jolt 的自由度是**求解器级**的（冻住的
    // 轴上速度恒为 0，撞上去也推不动），Unity 也是。所以"冻住 Y 位移"的箱子不会
    // 掉下去 —— 这正是想要的。

    /// 当前生效的约束。有刚体时以刚体为准（模拟出来的才是真在用的），没有时
    /// 返回脚本上次设的值 —— 与 `GetMass` 同一条"活体优先"的约定。
    ZFUNCTION()
    RigidbodyConstraints GetConstraints() const;

    /// 设置约束。同时写活体与组件自己的记录：只写活体的话，关卡重载（body 重建）
    /// 之后约束就静默没了。
    ZFUNCTION()
    void SetConstraints(RigidbodyConstraints constraints);

    /// Unity 的 `rigidbody.freezeRotation`：等价于一次设上 / 撤掉三个旋转位。
    ///
    /// ⚠️ **保留其它位**：`freezeRotation = true` 只动旋转位，不动已经设好的位移
    /// 冻结。把它写成"整体赋 FreezeRotation"会顺手清掉位移约束 —— 而"我明明冻
    /// 住了 Y 位移"这种 bug 要往回查很久。
    ZFUNCTION()
    bool GetFreezeRotation() const;
    ZFUNCTION()
    void SetFreezeRotation(bool freeze_rotation);

    // =========================================================================
    // 质心与惯性（Unity 的 `centerOfMass` / `inertiaTensor`）
    // =========================================================================
    //
    // 这两组是"推一下会怎么转"的唯一判据。`AddForceAtPosition` 有了之后缺的正是
    // 读回这一半：不知道质心就没法解释"为什么踢角上它没转"。

    /// **对象空间**质心（Unity 的 `rigidbody.centerOfMass`），只读。
    ZFUNCTION()
    Vector3 GetCenterOfMass() const;

    /// **世界空间**质心（Unity 的 `rigidbody.worldCenterOfMass`），只读。
    ZFUNCTION()
    Vector3 GetWorldCenterOfMass() const;

    /// 惯性张量的对角（Unity 的 `rigidbody.inertiaTensor`），只读。越大越难转。
    ZFUNCTION()
    Vector3 GetInertiaTensor() const;

    /// 把上面那个对角旋到对象空间的那次旋转（Unity 的
    /// `rigidbody.inertiaTensorRotation`），只读。
    ZFUNCTION()
    Quaternion GetInertiaTensorRotation() const;

    // =========================================================================
    // 连续碰撞检测（Unity 的 `rigidbody.collisionDetectionMode`）
    // =========================================================================

    /// 高速物体穿过薄墙的解药。**代价不小**（LinearCast 每步多一次扫掠），所以
    /// 只给子弹 / 快速移动平台开，别全场开。
    ///
    /// ⚠️ 读回来的粒度比写进去的粗：Jolt 只有 Discrete / LinearCast 两态，写
    /// `ContinuousDynamic` 读回 `Continuous`。理由见 `RigidBody.h` 的枚举注释。
    ZFUNCTION()
    CollisionDetectionMode GetCollisionDetectionMode() const;
    ZFUNCTION()
    void SetCollisionDetectionMode(CollisionDetectionMode mode);

    /// Unity 的 `rigidbody.maxAngularVelocity`（弧度/秒），挡"加了一次冲量之后
    /// 疯转"。默认取 Jolt 的初值（约 47 rad/s）。
    ZFUNCTION()
    float GetMaxAngularVelocity() const;
    ZFUNCTION()
    void SetMaxAngularVelocity(float max_angular_velocity);

    // =========================================================================
    // 点速度与相对力
    // =========================================================================

    /// **世界空间某一点**的速度（Unity 的 `rigidbody.GetPointVelocity`）。
    /// 与 `velocity` 的差别是它含旋转分量：转着的杆子，端点比质心快。
    ZFUNCTION()
    Vector3 GetPointVelocity(const Vector3& world_point) const;

    /// **对象空间某一点**的速度（Unity 的 `rigidbody.GetRelativePointVelocity`），
    /// 参数是相对**物体原点**（transform 位置）的局部偏移 —— 不是相对质心。
    ZFUNCTION()
    Vector3 GetRelativePointVelocity(const Vector3& local_point) const;

    /// 沿物体**自身**轴施加的力（Unity 的 `AddRelativeForce`）。推力器 / 载具
    /// 用这个：`AddForce(Vector3.forward)` 是"往世界前方"，`AddRelativeForce`
    /// 才是"往车头前方"。
    ZFUNCTION()
    void AddRelativeForce(const Vector3& force);

    /// 沿物体自身轴的力矩（Unity 的 `AddRelativeTorque`）。
    ZFUNCTION()
    void AddRelativeTorque(const Vector3& torque);

    // =========================================================================
    // 密度（Unity 的 `rigidbody.SetDensity`）
    // =========================================================================

    /// 按密度反推质量：`mass = density * volume`。
    ///
    /// Unity 的 `SetDensity` 是"给一个材质密度，让引擎算质量"，比手写质量稳 ——
    /// 换模型尺寸时不用同步改质量。密度为 0 / 负时夹到极小正数（与 `SetMass`
    /// 同一条理由：0 质量是 NaN 的入口）。
    ZFUNCTION()
    void SetDensity(float density);

    /// 当前密度（质量 / 体积）。拿不到体积时返回 0。
    ZFUNCTION()
    float GetDensity() const;

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
    ZSCRIPTPROPERTY(Name="friction", Getter="GetFriction", Setter="SetFriction")
    ZSCRIPTPROPERTY(Name="bounciness", Getter="GetBounciness", Setter="SetBounciness")
    ZSCRIPTPROPERTY(Name="constraints", Getter="GetConstraints", Setter="SetConstraints")
    ZSCRIPTPROPERTY(Name="freezeRotation", Getter="GetFreezeRotation", Setter="SetFreezeRotation")
    ZSCRIPTPROPERTY(Name="centerOfMass", Getter="GetCenterOfMass")
    ZSCRIPTPROPERTY(Name="worldCenterOfMass", Getter="GetWorldCenterOfMass")
    ZSCRIPTPROPERTY(Name="inertiaTensor", Getter="GetInertiaTensor")
    ZSCRIPTPROPERTY(Name="inertiaTensorRotation", Getter="GetInertiaTensorRotation")
    ZSCRIPTPROPERTY(Name="collisionDetectionMode", Getter="GetCollisionDetectionMode", Setter="SetCollisionDetectionMode")
    ZSCRIPTPROPERTY(Name="maxAngularVelocity", Getter="GetMaxAngularVelocity", Setter="SetMaxAngularVelocity")
    ZSCRIPTPROPERTY(Name="density", Getter="GetDensity", Setter="SetDensity")

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

    /// body 刚建好 / 刚 flush 出来之后把组件记的约束与 CCD 模式推下去。
    ///
    /// 不做这一步的话"脚本在 Awake 里设了 freezeRotation"会丢：设的时候 body 还
    /// 没建（关卡加载期物理场景通常还没发布），body 建出来时是`All`。
    void ApplyRuntimeBodySettings();

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

    /// 脚本设过的约束与 CCD 模式。
    ///
    /// ⚠️ **刻意不序列化**（与 `CameraComponent` 的 near/far clip 同一条取舍）：
    /// 往 `RigidBodyComponentRes` 里加字段要动它的 `Transfer()` 顺序，而现有
    /// .zasset 的字段顺序是资产生命线。运行时可写、能读回，只是不存盘。
    RigidbodyConstraints   m_Constraints {RigidbodyConstraints::None};
    CollisionDetectionMode m_CollisionDetectionMode {CollisionDetectionMode::Discrete};
};
