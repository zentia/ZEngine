#pragma once

// On Web (Emscripten) we don't link Jolt — `jolt/utils.h` itself pulls
// `Jolt/Jolt.h` which is not on the include path in that build. Forward-declare
// the two filter implementations and use unique_ptr members so the header is
// self-contained on Web. Native builds keep the value-typed filter members
// exactly as before to preserve ABI/initialisation order.
#include <memory>
#include <mutex>
#include <vector>

// ZENGINE_SHARED_NO_PHYSICS: the slim iOS shared runtime drops the Jolt-backed
// TUs (PhysicsScene.cpp / PhysicsCharacter.cpp / Jolt/Utils.cpp) and links
// PhysicsSceneNull.cpp instead, so this header must not pull jolt/utils.h and
// must not declare members whose construction needs a symbol from those TUs
// (the two filter impls, whose vtables live in Jolt/Utils.cpp). Everything else
// is pointer-only and stays.
#if defined(__EMSCRIPTEN__) || defined(ZENGINE_SHARED_NO_PHYSICS)
class ObjectVsBroadPhaseLayerFilterImpl;
class ObjectLayerPairFilterImpl;
#else
    #include "jolt/utils.h"
#endif
#include "Runtime/Core/Math/AxisAligned.h"
#include "Runtime/Core/Math/Quaternion.h"
#include "Runtime/Function/Physics/PhysicsConfig.h"
#include "Runtime/Function/Physics/PhysicsJoint.h"
#include "Runtime/Function/Physics/PhysicsQuery.h"

namespace JPH
{
    class PhysicsSystem;
    class JobSystem;
    class TempAllocator;
    class BroadPhaseLayerInterface;
    class Constraint;
#ifdef ENABLE_PHYSICS_DEBUG_RENDERER
    class DebugRenderer;
#endif
}  // namespace JPH

class TransformTRS;
class RigidBodyComponentRes;
class RigidBodyShape;
// Opaque-enum declaration: an enum with a fixed underlying type is a complete
// type once declared, so PhysicsScene.h stays free of the RigidBody.h include
// (which pulls in PPtr / Geometry / the generated header).
enum class RigidBodyActorType : unsigned char;
// Same trick, same reason: the combine-mode enum is only ever passed through
// here, and its underlying type is fixed so a declaration is a complete type.
enum class PhysicMaterialCombine : unsigned char;
// Unity's RigidbodyConstraints / CollisionDetectionMode. Both come from
// RigidBody.h; the scene only needs them as complete, opaque enum types.
enum class RigidbodyConstraints : int;
enum class CollisionDetectionMode : unsigned char;

static constexpr uint32_t s_InvalidRigidbodyId = 0xffffffff;
static constexpr uint32_t s_InvalidJointId = 0;

/// Which of Unity's three callbacks a contact corresponds to.
///
/// `Stay` 不是 Jolt 的一个回调 —— `OnContactPersisted` 每个子步都会来一次
/// （60Hz 模拟 + 一个接触跨 3 个子步就是 3 次），而 Unity 的
/// `OnCollisionStay` / `OnTriggerStay` 是**每帧一次**。所以 Stay 由 poll 时
/// "上一帧就在、这一帧还在"推导，而不是由 Jolt 直接喂。
enum class PhysicsContactPhase : unsigned char
{
    enter = 0,
    stay  = 1,
    exit  = 2,
};

/// One trigger (sensor) boundary crossing for this frame.
struct PhysicsTriggerEvent
{
    /// The body flagged as a sensor.
    uint32_t m_TriggerBodyId {s_InvalidRigidbodyId};
    /// The body that entered or left it.
    uint32_t m_OtherBodyId {s_InvalidRigidbodyId};
    /// true = OnTriggerEnter, false = OnTriggerExit **或 Stay**
    /// —— 三态请看 `m_Phase`。这个字段留着是因为它是 2026-10 之前的唯一判
    /// 据，`PhysicsCharacterJointSmokeTest` 还在用它；新代码一律用 m_Phase。
    bool m_IsEnter {true};
    PhysicsContactPhase m_Phase {PhysicsContactPhase::enter};
};

/// One collision (non-sensor) contact for this frame.
///
/// 与 `PhysicsTriggerEvent` 分开：触发器只关心"谁进谁出"，碰撞还要把接触点、
/// 法线、相对速度交给脚本（`OnCollisionEnter(collision)` 里那些）。
struct PhysicsCollisionEvent
{
    uint32_t m_BodyA {s_InvalidRigidbodyId};
    uint32_t m_BodyB {s_InvalidRigidbodyId};
    PhysicsContactPhase m_Phase {PhysicsContactPhase::enter};

    /// 世界空间接触点。两个物体多个接触点时取 Jolt 给的**第一个**
    /// （`ContactManifold::GetWorldSpaceContactPointOn1(0)`）—— Unity 的
    /// `collision.contacts[0]` 同样是"第一个"，不保证是哪个。
    Vector3 m_Point;
    /// 接触法线，从 A 指向 B。
    Vector3 m_Normal;
    /// 沿法线的相对速度大小（m/s）。Unity 的 `collision.relativeVelocity`
    /// 是矢量，这里给模长 —— 判断"撞得重不重"够用，且不用把 Vector3 编进
    /// 事件再搬一次。
    float m_RelativeSpeed {0.f};
};

/// The constraint types exposed to gameplay. They map 1:1 onto Jolt's
/// two-body constraints; everything else (gears, pulleys, paths) can be added
/// later without changing the API shape.
enum class PhysicsJointType : unsigned char
{
    fixed,       ///< Welds two bodies together (Unity FixedJoint).
    hinge,       ///< Rotation about one axis, with optional limits (HingeJoint).
    point,       ///< Keeps two points coincident (ball socket).
    slider,      ///< Translation along one axis, with optional limits.
    distance,    ///< Keeps two points within a distance range (SpringJoint).
    swing_twist, ///< Cone-limited ball socket (Unity CharacterJoint).
    configurable ///< Six DOF: per-axis limits + per-axis drives (Unity ConfigurableJoint).
};

/// Description of a joint. All pivots/axes are in world space: they are
/// converted to each body's local frame when the constraint is created.
struct PhysicsJointDesc
{
    PhysicsJointType m_Type {PhysicsJointType::fixed};
    uint32_t m_BodyA {s_InvalidRigidbodyId};
    uint32_t m_BodyB {s_InvalidRigidbodyId};

    /// Attachment points. Ignored for fixed/slider when m_AutoDetectPoint is
    /// true — Jolt then freezes the current relative pose.
    Vector3 m_PivotA;
    Vector3 m_PivotB;

    /// Hinge/slider axis, in world space. For a hinge both bodies usually share
    /// the same axis; m_AxisB exists for the case where they do not.
    Vector3 m_AxisA {0.f, 1.f, 0.f};
    Vector3 m_AxisB {0.f, 1.f, 0.f};

    /// hinge: [-pi, 0] / [0, pi] radians. slider: [-inf, 0] / [0, inf] units.
    /// distance: [min, max] distance. Negative means "derive from the pivots".
    float m_MinLimit {-1.f};
    float m_MaxLimit {-1.f};

    // ---- swing_twist（Unity 的 CharacterJoint）----------------------------
    //
    // `m_AxisA` / `m_AxisB` 是**扭转轴**；摆动还要一条与它垂直的"平面轴"来定
    // 出摆动的两个方向（Jolt 的 mTwistAxis / mPlaneAxis 就是这样一对）。

    /// 与扭转轴垂直的那条轴（世界空间）。会被投影成垂直分量，不必严格正交。
    Vector3 m_PlaneAxisA {0.f, 1.f, 0.f};
    Vector3 m_PlaneAxisB {0.f, 1.f, 0.f};

    /// 摆动半锥角（弧度，>= 0）。Unity 的 `swing1Limit.limit` / `swing2Limit.limit`：
    /// swing1 绕平面轴，swing2 绕另一条。
    float m_Swing1HalfAngle {0.f};
    float m_Swing2HalfAngle {0.f};

    /// 扭转区间（弧度，[-pi, pi]）。Unity 的 `lowTwistLimit` / `highTwistLimit`
    /// 同样是"相对建立姿态"，与 Jolt 的 `mTwistMinAngle` / `mTwistMaxAngle` 同义。
    float m_TwistMinAngle {0.f};
    float m_TwistMaxAngle {0.f};

    /// 限位弹簧（频率 Hz / 阻尼比）。0 表示"硬限位"（Jolt 的默认，也是 Unity
    /// 里 `spring = 0` 的意思 —— 但**不是**"没有弹簧"）。
    float m_SwingSpringFrequency {0.f};
    float m_SwingSpringDamping {0.f};
    float m_TwistSpringFrequency {0.f};
    float m_TwistSpringDamping {0.f};

    // ---- configurable（Unity 的 ConfigurableJoint）--------------------------
    //
    // `m_AxisA` / `m_SecondaryAxisA` 是关节参考系的 X / Y，第三条由叉乘得出 ——
    // 与 Unity 的 `axis` / `secondaryAxis` 是同一件事（Unity 的文档：Axis 和
    // Secondary Axis 一起表达关节的局部坐标系，第三条自动取叉乘）。
    //
    // 下面三组平行数组都按 `ConfigurableJointAxis` 下标（0..5）。

    /// 与 `m_AxisA` 一起定出关节参考系的那条轴（世界空间）。会被投影成垂直分量，
    /// 不必严格正交；**平行**时后端自己换一条（与 `PlaneAxisFor` 同一条路）。
    Vector3 m_SecondaryAxisA {0.f, 1.f, 0.f};
    Vector3 m_SecondaryAxisB {0.f, 1.f, 0.f};

    /// 每个轴的运动模式。**默认全 Free**：与 Jolt 的
    /// `SixDOFConstraintSettings` 初值（min = -FLT_MAX / max = FLT_MAX）一致，
    /// 也和 Unity 里"刚 AddComponent 还没配"的 ConfigurableJoint 一样 —— 先什么
    /// 都不约束，等脚本来配。默认成 Locked 会让一个没配过的关节把物体焊死，
    /// 而"焊死"和"配错了"从外面看不出来。
    ConfigurableJointMotion m_Motion[kConfigurableAxisCount] {
        ConfigurableJointMotion::Free, ConfigurableJointMotion::Free, ConfigurableJointMotion::Free,
        ConfigurableJointMotion::Free, ConfigurableJointMotion::Free, ConfigurableJointMotion::Free};

    /// 限位区间。只有 `m_Motion[i] == Limited` 时才有意义 —— 单位随轴是弧度还是
    /// 米，与 Jolt 一致。
    float m_AxisLimitMin[kConfigurableAxisCount] {0.f, 0.f, 0.f, 0.f, 0.f, 0.f};
    float m_AxisLimitMax[kConfigurableAxisCount] {0.f, 0.f, 0.f, 0.f, 0.f, 0.f};

    /// 每个轴的驱动。`positionSpring <= 0` 视为"这个轴没有驱动"（Jolt 那侧就把
    /// 电机置成 Off —— 刚度为 0 的电机没有意义，而 Jolt 的 `SetMotorState` 会在
    /// 打开电机时断言设置合法）。
    JointDrive m_AxisDrive[kConfigurableAxisCount];

    /// 线性限位弹簧（刚度 / 阻尼）。**只有平移那三个轴有** —— Jolt 的
    /// `mLimitsSpringSettings` 只有 `NumTranslation` 个，旋转限位是硬的。
    /// 0 = 硬限位。
    float m_LinearLimitSpring {0.f};
    float m_LinearLimitDamper {0.f};

    /// 四个目标量（Unity 的 `targetPosition` / `targetVelocity` /
    /// `targetAngularVelocity` / `targetRotation`）。`targetRotation` 是**宿主
    /// 局部**的姿态，与 Unity 一致（`ConfigurableJoint.targetRotation` 是相对
    /// 关节建立时姿态的旋转）。
    Vector3 m_TargetPosition {Vector3::ZERO};
    Vector3 m_TargetVelocity {Vector3::ZERO};
    Vector3 m_TargetAngularVelocity {Vector3::ZERO};
    Quaternion m_TargetRotation {Quaternion::IDENTITY};

    /// Let Jolt compute the attachment points from the current body poses.
    bool m_AutoDetectPoint {true};
};

struct PhysicsHitInfo
{
    Vector3 hit_position;
    Vector3 hit_normal;
    float hit_distance {0.f};
    uint32_t body_id {s_InvalidRigidbodyId};
    /// Unity layer of the hit body. Filled by every query so the caller does
    /// not have to re-lock the body to find out whether it wanted the hit.
    int32_t layer {0};
};

class PhysicsContactListener;

class PhysicsScene
{
    struct JoltPhysics
    {
        JPH::PhysicsSystem* m_JoltPhysicsSystem {nullptr};
        JPH::JobSystem* m_JoltJobSystem {nullptr};
        JPH::TempAllocator* m_TempAllocator {nullptr};
        JPH::BroadPhaseLayerInterface* m_JoltBroadPhaseLayerInterface {nullptr};

        int m_CollisionSteps {1};
        int m_IntegrationSubsteps {1};
    };

public:
    PhysicsScene(const Vector3& gravity);
    virtual ~PhysicsScene();

    const Vector3& getGravity() const { return m_Config.m_Gravity; }
    /// Unity's `Physics.gravity`. Takes effect on the next step: Jolt reads the
    /// gravity out of its own body state, so this also has to re-apply it to
    /// every live body (a dynamic body keeps the gravity it was given at
    /// creation until something sets it again).
    void setGravity(const Vector3& gravity);

    /// @param unity_layer  the GameObject's layer, which becomes the Jolt
    /// object layer (see Jolt/Utils.h) and is therefore what the layer
    /// collision matrix and every query mask filter on.
    uint32_t CreateRigidBody(const TransformTRS& global_transform,
                             const RigidBodyComponentRes& rigidbody_actor_res,
                             int32_t unity_layer = 0);
    void RemoveRigidBody(uint32_t body_id);

    /// 一个不动、不参与碰撞的刚体，用来当关节的"世界那一头"（Unity 里
    /// `Joint.connectedBody == null` 的意思）。
    ///
    /// Jolt 没有"世界"这个约束端点：任何 `TwoBodyConstraint` 都要两个
    /// `Body`。所以"拴在世界某个点上"必须真的造一个 body 出来。它用 Jolt 的
    /// `EmptyShape` —— 没有体积、不与任何东西碰撞。换成任何有体积的形状，
    /// 这个锚点就会去推它本该拴住的那个物体，关节看起来像"自己会抖"。
    ///
    /// ⚠️ 调用方**拥有**它：关节没了要 `RemoveRigidBody` 掉，否则关卡里会
    /// 积攒一批看不见也撞不到的静态 body（不崩，但 `Physics` 的 body 计数
    /// 一直涨，是最难发现的一类泄漏）。
    ///
    /// @return 新 body 的 id，失败时 `s_InvalidRigidbodyId`。
    uint32_t CreateStaticAnchorBody(const Vector3& world_position);

    // --- layers -------------------------------------------------------------
    //
    // The Unity layer IS the Jolt object layer, so there is no side table to
    // keep in sync: the body is the single source of truth.

    /// @return false when the body does not exist; out_layer untouched.
    bool GetBodyLayer(uint32_t body_id, int32_t& out_layer) const;

    /// Re-tag a live body. Used when a script assigns `gameObject.layer`, which
    /// Unity applies to physics immediately rather than at the next rebuild.
    /// @return false when the body does not exist or the layer is out of range.
    bool SetBodyLayer(uint32_t body_id, int32_t unity_layer);

    void UpdateRigidBodyGlobalTransform(uint32_t body_id, const TransformTRS& global_transform);

    // --- body state (public physics API) -----------------------------------
    //
    // Every one of these takes a body id and returns false when the id is
    // invalid, the body has already been removed, or the query does not apply
    // to that body's motion type (a static body has no velocity and no mass
    // properties). Callers on the Component layer treat "false" as "nothing
    // happened" rather than asserting -- a body can legitimately be pending
    // creation (PostLoadResource ran before the scene existed) or already torn
    // down while a script still holds the component.

    /// Motion type of a live body. False when the body does not exist.
    bool GetRigidBodyMotionType(uint32_t body_id, RigidBodyActorType& out_motion_type) const;

    /// Change a live body's motion type. This is what makes a body start
    /// simulating: a body created as static has no motion properties at all.
    bool SetRigidBodyMotionType(uint32_t body_id, RigidBodyActorType motion_type);

    /// Where the body actually is, which for a simulated body drifts away from
    /// the Transform component (nothing writes the body back yet).
    bool GetRigidBodyPosition(uint32_t body_id, Vector3& out_position) const;
    bool GetRigidBodyRotation(uint32_t body_id, Quaternion& out_rotation) const;

    bool GetLinearVelocity(uint32_t body_id, Vector3& out_velocity) const;
    bool SetLinearVelocity(uint32_t body_id, const Vector3& velocity);
    bool GetAngularVelocity(uint32_t body_id, Vector3& out_velocity) const;
    bool SetAngularVelocity(uint32_t body_id, const Vector3& velocity);

    /// Continuous force, accumulated for the next step and cleared by it.
    /// No-op (false) on a non-dynamic body.
    bool AddForce(uint32_t body_id, const Vector3& force);

    /// Instantaneous change in momentum; the "kick something" primitive.
    /// No-op (false) on a non-dynamic body.
    bool AddImpulse(uint32_t body_id, const Vector3& impulse);

    /// 1 / mass. 0 means immovable, which is what a static body reports.
    bool GetInverseMass(uint32_t body_id, float& out_inverse_mass) const;

    /// Kilograms. 0 when the body is immovable (static / kinematic).
    bool GetMass(uint32_t body_id, float& out_mass) const;

    /// Unity's `rigidbody.mass`.
    ///
    /// 质量不是"改一个倒数"就完了：惯性张量必须按新质量重新标定，否则一个
    /// 10kg 的物体会带着 1kg 的转动惯量转 —— 推起来对了，转起来飞快。Jolt 的
    /// `MassProperties::ScaleToMass` 一次把两样都缩对，所以走它而不是只写
    /// `SetInverseMass`。
    ///
    /// 质量 <= 0 时**夹到极小正数**而不是照单全收：0 质量在除法里是 inf（倒数
    /// 质量），一旦进积分就是 NaN 位置，而 NaN 物体会把整个 broadphase 拖垮。
    /// Unity 也是这个态度（mass 必须为正）。
    bool SetMass(uint32_t body_id, float mass);

    // --- 自由度约束（Unity 的 `rigidbody.constraints`）----------------------
    //
    // Jolt 用 `EAllowedDOFs` 掩码表达"哪些轴还能动"，Unity 用 `RigidbodyConstraints`
    // 位标志表达"哪些轴被冻住" —— 两者互为补集且位序不同。这里的接口只流通
    // **Jolt 掩码**，映射（`ToJoltAllowedDOFs` / `ToRigidbodyConstraints`）活在
    // RigidBody.h 里，与 `ToRigidBodyActorType` 同一条约定：映射只有一份，且能
    // 被一个不起物理世界的单测断言。
    //
    // ⚠️ `SetBodyAllowedDOFs` 额外要一个 `mass`：Jolt 的
    // `MotionProperties::SetMassProperties` 收的是**完整的** MassProperties
    // （质量 + 惯性张量），而"位移被全冻住"的 body 倒数质量是 0 —— 从它读不回
    // 真实质量。不传就会在"冻住全部位移"之后把质量丢掉。

    /// @return false when the body does not exist or is not dynamic.
    bool GetBodyAllowedDOFs(uint32_t body_id, uint32_t& out_dof_mask) const;
    bool SetBodyAllowedDOFs(uint32_t body_id, uint32_t dof_mask, float mass);

    // --- 质心与惯性张量 ----------------------------------------------------
    //
    // Unity 把这三个都放在 `Rigidbody` 上（`centerOfMass` / `worldCenterOfMass` /
    // `inertiaTensor` / `inertiaTensorRotation`），它们是"推一下会怎么转"的唯一
    // 判据，缺了就只能靠试。

    /// **对象空间**质心（Unity 的 `rigidbody.centerOfMass`）。取自形状自己的
    /// `GetCenterOfMass()`，不含 body 的旋转。
    bool GetBodyCenterOfMass(uint32_t body_id, Vector3& out_local_center_of_mass) const;

    /// **世界空间**质心（Unity 的 `rigidbody.worldCenterOfMass`）。
    ///
    /// ⚠️ 与 `GetRigidBodyPosition()` **不是同一个量**：Jolt 的
    /// `Body::GetPosition()` 是物体**原点**（`mPosition - rotation * localCOM`），
    /// 而质心是 `mPosition`。形状质心不在原点时两者相差 `rotation * localCOM`。
    /// Unity 里同样是两个量（`rigidbody.position` vs `worldCenterOfMass`）。
    bool GetBodyWorldCenterOfMass(uint32_t body_id, Vector3& out_world_center_of_mass) const;

    /// 惯性张量的对角 + 把它旋到对象空间的那次旋转。
    ///
    /// ⚠️ Jolt 存的是**逆**惯性（`GetInverseInertiaDiagonal`），所以这里要取
    /// 倒数 —— 而某个轴被冻住时该分量是 0，倒数就是 inf。那种轴返回 0 而不是
    /// inf：脚本拿它只能做"大不大"的比较，而 inf 会一路传成 NaN 角速度。
    bool GetBodyInertiaTensor(uint32_t body_id, Vector3& out_inertia, Quaternion& out_rotation) const;

    /// 形状体积（立方米）。`SetDensity` 要用它把密度换算成质量。
    bool GetBodyVolume(uint32_t body_id, float& out_volume) const;

    // --- 连续碰撞检测 ------------------------------------------------------

    /// Unity 的 `rigidbody.collisionDetectionMode`。
    ///
    /// ⚠️ 读回来的粒度比写进去的粗：Jolt 只有 Discrete / LinearCast 两态，所以
    /// 写 `ContinuousDynamic` 读回 `Continuous`。这是**有意的**窄化 —— 在 body
    /// 旁边另存一份作者值就会多一个会分叉的真值来源。
    bool GetBodyMotionQuality(uint32_t body_id, CollisionDetectionMode& out_mode) const;
    bool SetBodyMotionQuality(uint32_t body_id, CollisionDetectionMode mode);

    // --- 速度上限与点速度 --------------------------------------------------

    /// 角速度上限（弧度/秒）。Unity 的 `rigidbody.maxAngularVelocity`，用来挡住
    /// "加了一次冲量之后疯转"。<= 0 时夹到极小正数。
    bool GetBodyMaxAngularVelocity(uint32_t body_id, float& out_max_angular_velocity) const;
    bool SetBodyMaxAngularVelocity(uint32_t body_id, float max_angular_velocity);

    /// **世界空间某一点**的速度（Unity 的 `rigidbody.GetPointVelocity`）。
    /// 与 `GetLinearVelocity` 的差别是它含旋转带来的分量："转着的杆子，端点比
    /// 中心快"。
    bool GetBodyPointVelocity(uint32_t body_id, const Vector3& world_point, Vector3& out_velocity) const;

    /// 1 = 受重力，0 = 不受（Unity 的 `rigidbody.useGravity`）。可以取中间值做
    /// "半重力"，Jolt 支持，Unity 不支持 —— 这里照 Unity 只暴露布尔。
    bool GetGravityFactor(uint32_t body_id, float& out_factor) const;
    bool SetGravityFactor(uint32_t body_id, float factor);

    /// Unity 的 `rigidbody.drag` / `angularDrag`。Jolt 叫 damping，语义相同：
    /// 每秒保留 (1 - damping) 的速度。
    bool GetLinearDamping(uint32_t body_id, float& out_damping) const;
    bool SetLinearDamping(uint32_t body_id, float damping);
    bool GetAngularDamping(uint32_t body_id, float& out_damping) const;
    bool SetAngularDamping(uint32_t body_id, float damping);

    /// 连续力矩（牛·米），累加到下一步、被下一步清掉。非动力学刚体上无操作。
    bool AddTorque(uint32_t body_id, const Vector3& torque);

    /// 瞬时角动量变化 —— "让它转起来"的原语。
    bool AddAngularImpulse(uint32_t body_id, const Vector3& angular_impulse);

    /// 作用在**世界坐标某一点**上的力。除了线性分量还会带出力矩，所以"踢箱子
    /// 的角"会让它一边飞一边转 —— 这正是 `AddForce` 做不到的。
    bool AddForceAtPosition(uint32_t body_id, const Vector3& force, const Vector3& position);

    /// 作用在**世界坐标某一点**上的冲量（爆炸、子弹命中的手感）。
    bool AddImpulseAtPosition(uint32_t body_id, const Vector3& impulse, const Vector3& position);

    /// 直接把物体搬过去（Unity 的 `rigidbody.position = p`）。**不**产生碰撞
    /// 扫掠 —— 想让 kinematic 物体推着别人走用 `MoveKinematic`。
    bool SetRigidBodyPosition(uint32_t body_id, const Vector3& position);

    bool SetRigidBodyRotation(uint32_t body_id, const Quaternion& rotation);

    /// Unity 的 `MovePosition` / `MoveRotation`：给一个目标位姿，由物理步按
    /// `delta_time` 反推速度走过去。和 `SetRigidBodyPosition` 的差别是它会
    /// **推**开路上的东西，而传送不会。
    bool MoveKinematic(uint32_t body_id,
                       const Vector3& target_position,
                       const Quaternion& target_rotation,
                       float delta_time);

    /// 是否已经睡着。睡着的物体不再模拟，也不再上报接触 —— 一个"该动没动"的
    /// bug 有一半是这个。
    bool IsSleeping(uint32_t body_id, bool& out_is_sleeping) const;
    bool SleepRigidBody(uint32_t body_id);
    bool WakeRigidBody(uint32_t body_id);

    // --- surface material (Unity's PhysicMaterial) --------------------------
    //
    // Jolt stores one friction and one restitution per **body**, not per shape,
    // so these are body-wide here where Unity would put them on the collider
    // (see doc/PHYSIC_MATERIAL_SCRIPT_API.md). They are readable and writable
    // on a live body so `rigidbody.friction = 0.1` takes effect on the next
    // contact instead of waiting for the body to be rebuilt.

    bool GetBodySurface(uint32_t body_id, float& out_friction, float& out_restitution) const;
    bool SetBodyFriction(uint32_t body_id, float friction);
    bool SetBodyRestitution(uint32_t body_id, float restitution);

    /// The combine modes live in the body's user data, so changing them is a
    /// user-data write, not a solver-state write.
    bool SetBodyCombineModes(uint32_t body_id,
                             PhysicMaterialCombine friction_combine,
                             PhysicMaterialCombine bounce_combine);

    void Tick(float delta_time);

    /// The underlying Jolt system. Exposed so the character controller and the
    /// joint layer can create Jolt objects that belong to this scene; nothing
    /// outside Function/Physics should need it.
    JPH::PhysicsSystem* GetJoltPhysicsSystem() { return m_Physics.m_JoltPhysicsSystem; }
    const JPH::PhysicsSystem* GetJoltPhysicsSystem() const { return m_Physics.m_JoltPhysicsSystem; }

    // --- triggers -----------------------------------------------------------
    //
    // A trigger is a rigid body flagged as a Jolt sensor: it reports contacts
    // but never produces a collision response, so things pass straight through
    // it. Note that Jolt only reports sensor contacts against *moving* bodies,
    // which matches Unity (a trigger needs something to walk into it).

    /// @return false when the body does not exist.
    bool SetRigidBodySensor(uint32_t body_id, bool is_sensor);

    /// @return false when the body does not exist; out_is_sensor untouched.
    bool IsRigidBodySensor(uint32_t body_id, bool& out_is_sensor) const;

    /// Drain this frame's trigger events. The queue is emptied by the call, so
    /// call it once per frame from a single place.
    void PollTriggerEvents(std::vector<PhysicsTriggerEvent>& out_events);

    /// Drain this frame's collision events, including the `stay` entries
    /// derived from pairs that were already in contact last poll. Same
    /// once-per-frame contract as `PollTriggerEvents`.
    ///
    /// 两个队列分开排空是刻意的：合并成一个"contact event"就会让触发器事件
    /// 背上它用不到的 point/normal，也会让已有的
    /// `PollTriggerEvents` 调用方签名全变。
    void PollCollisionEvents(std::vector<PhysicsCollisionEvent>& out_events);

    // --- joints -------------------------------------------------------------

    /// @return the new joint's id, or s_InvalidJointId when either body is
    /// missing, the two ids are the same, or the constraint could not be built.
    uint32_t CreateJoint(const PhysicsJointDesc& in_desc);

    /// Detach and destroy a joint. Returns false when the id is unknown.
    bool RemoveJoint(uint32_t joint_id);

    // --- joint runtime control ---------------------------------------------
    //
    // `CreateJoint` / `RemoveJoint` were the whole API, which meant a script
    // could make a joint but never touch it again: no way to read the hinge
    // angle, no way to drive a motor, no way to re-tune a spring. These are
    // the per-frame / per-event knobs Unity's joint classes expose.
    //
    // ⚠️ 类型**不做**记录，一律问 Jolt（`Constraint::GetSubType()`）。另存一份
    // "这个 id 是什么类型" 的表就会在 `RemoveJoint` 留空洞（id 不复用）之后和
    // 真实约束分叉 —— 而分叉的表现是"调用了一个 hinge 的方法，改的其实是个
    // distance 约束"。Jolt 自己知道，就让它自己说。
    //
    // ⚠️ 每个方法都**按类型拒绝**：拿 hinge 的方法去动一个 distance 约束返回
    // false 而不是"尽力而为"。静默干点别的是最难查的一类。

    /// @return false when the id is unknown or the joint was already removed.
    bool GetJointType(uint32_t joint_id, PhysicsJointType& out_type) const;

    /// 关掉的约束完全不参与求解（Unity 没有这个开关，但"临时拆掉门闩"要它）。
    bool SetJointEnabled(uint32_t joint_id, bool enabled);
    bool IsJointEnabled(uint32_t joint_id, bool& out_enabled) const;

    /// 铰链**当前**角度（弧度，带符号）。Unity 的 `HingeJoint.angle`。
    /// 非铰链返回 false。
    bool GetJointAngle(uint32_t joint_id, float& out_angle_radians) const;

    /// 铰链**当前**角速度（弧度/秒）：两个刚体的相对角速度在铰链轴上的投影。
    /// Unity 的 `HingeJoint.velocity`。非铰链返回 false。
    ///
    /// ⚠️ 从两个刚体现算而不是读约束：Jolt 的 `HingeConstraint` 没有
    /// "当前角速度" 这个访问器，只有目标角速度（`mTargetAngularVelocity`，那是
    /// **电机**的输入）。把目标当成当前值读回来，会让"门关上了吗"这种判断在
    /// 电机开着时永远是"到了"。
    bool GetJointVelocity(uint32_t joint_id, float& out_radians_per_second) const;

    /// 铰链 / 滑轨的限制区间。单位随类型：**弧度**（铰链）/ **米**（滑轨）。
    /// 传 min > max 表示"不要限制"（Jolt 的初值是 [-inf, inf] 那一族）。
    bool SetJointLimits(uint32_t joint_id, float min_limit, float max_limit);
    bool GetJointLimits(uint32_t joint_id, float& out_min_limit, float& out_max_limit) const;

    /// 铰链 / 滑轨的电机。
    ///
    /// ⚠️ `target` 与 `max_force` 的**单位随类型**：
    ///   * hinge  -- target 是角速度（弧度/秒），max_force 是最大力矩（牛·米）；
    ///   * slider -- target 是位置（米），max_force 是最大推力（牛）。
    /// 收成一个方法而不是 `SetHingeMotor` / `SetSliderMotor` 各一个，是因为
    /// 脚本已经能从 `GetJointType` 知道自己手上是什么 —— 而两个名字只有一个
    /// 参数表不同，是最容易传错的一种重载。
    bool SetJointMotor(uint32_t joint_id, bool enabled, float target, float max_force);

    /// **位置**电机（Unity 的 `HingeJoint.useSpring` + `spring.targetPosition`，
    /// 以及滑轨上"走到某个位置"）。
    ///
    /// ⚠️ 与 `SetJointMotor` 是**两个不同的目标量**，不合成一个方法：`target`
    /// 那里是"速度"（铰链 rad/s、滑轨 m/s 意义上的目标速度），这里是"位置"
    ///（铰链 rad、滑轨 m）。合成一个就得再加一个"模式"参数，而"两个只有一个
    /// 参数表不同的重载"是最容易传错的一种。
    ///
    /// `frequency_hz` 是驱动弹簧的**频率**（Hz，> 0）：Jolt 的位置电机按
    /// `F = stiffness*(target - cur) - damping*cur_vel` 施力，刚度由频率推出。
    /// 传 0 会让 `MotorSettings::IsValid()` 为假，而 `SetMotorState` 会断言 ——
    /// 这里直接拒掉。
    bool SetJointPositionMotor(uint32_t joint_id,
                               bool enabled,
                               float target,
                               float max_force,
                               float frequency_hz,
                               float damping);

    /// 弹簧关节（`PhysicsJointType::distance`）的距离区间。**米**。
    bool SetJointDistance(uint32_t joint_id, float min_distance, float max_distance);
    bool GetJointDistance(uint32_t joint_id, float& out_min_distance, float& out_max_distance) const;

    /// 弹簧的软硬。`frequency_hz` 为 0 时 Jolt 走"硬限位"（多硬取决于时间步与
    /// 求解步数），这正是 Unity 里 `spring = 0` 的意思 —— 但**不是**"没有弹簧"。
    bool SetJointSpring(uint32_t joint_id, float frequency_hz, float damping);

    /// swing-twist 的四个限位（Unity 的 `CharacterJoint.swing1Limit` /
    /// `swing2Limit` / `lowTwistLimit` / `highTwistLimit`）。**弧度**。
    ///
    /// 半锥角为负没有意义（锥是对称的），扭转轴要求 `min <= max` —— 两个都拒掉而
    /// 不夹：夹出来的区间和脚本写的不是一回事，而"我明明写了 0.2 却被当成 0"最难查。
    bool SetJointSwingTwistLimits(uint32_t joint_id,
                                  float swing1_half_angle,
                                  float swing2_half_angle,
                                  float twist_min_angle,
                                  float twist_max_angle);
    bool GetJointSwingTwistLimits(uint32_t joint_id,
                                  float& out_swing1_half_angle,
                                  float& out_swing2_half_angle,
                                  float& out_twist_min_angle,
                                  float& out_twist_max_angle) const;

    /// swing-twist 的限位弹簧（频率 Hz / 阻尼比）。摆动与扭转各一份。
    /// 传 0 = 硬限位。
    bool SetJointSwingTwistSpring(uint32_t joint_id,
                                  float swing_frequency_hz,
                                  float swing_damping,
                                  float twist_frequency_hz,
                                  float twist_damping);
    bool GetJointSwingTwistSpring(uint32_t joint_id,
                                  float& out_swing_frequency_hz,
                                  float& out_swing_damping,
                                  float& out_twist_frequency_hz,
                                  float& out_twist_damping) const;

    /// 上一步这个约束实际施加的冲量**模长**（N·s / N·m·s），除以步长就是力与
    /// 力矩。Unity 的 `Joint.breakForce` / `breakTorque` 判的就是它。
    ///
    /// ⚠️ 只给**模长**，不给矢量：每个约束的 lambda 活在自己的坐标系里（铰链那
    /// 两个"旋转"分量是垂直于轴的那两维，滑轨那个"位置"分量同理），拼成一个
    /// Vector3 会得到一个看着像世界矢量、其实不是的东西。而"断不断"只需要大小。
    /// 这也是本引擎没有 Unity 的 `Joint.currentForce`（Vector3）的原因。
    bool GetJointImpulse(uint32_t joint_id, float& out_linear_impulse, float& out_angular_impulse) const;

    // --- configurable（Unity 的 ConfigurableJoint）---------------------------

    /// 单个轴的**运动模式**与限位。
    ///
    /// @param limit_min / limit_max  只有 `motion == Limited` 时被读；`Locked` 时
    /// 忽略（Jolt 那边是"区间写反"，没有数值可填）。
    ///
    /// ⚠️ `Locked` 在 Jolt 里是"把区间写成 [FLT_MAX, -FLT_MAX]"，读回来要认这个
    /// 反序 —— 所以 `GetJointAxisMotion` 给的 min/max 在 Locked 时是**反的**，
    /// 别拿它去判"区间合不合法"。
    bool SetJointAxisMotion(uint32_t joint_id,
                            ConfigurableJointAxis axis,
                            ConfigurableJointMotion motion,
                            float limit_min,
                            float limit_max);
    bool GetJointAxisMotion(uint32_t joint_id,
                            ConfigurableJointAxis axis,
                            ConfigurableJointMotion& out_motion,
                            float& out_limit_min,
                            float& out_limit_max) const;

    /// 单个轴的**驱动**。
    ///
    /// ⚠️ 电机开不开由 `drive.positionSpring > 0` 决定，没有一个单独的开关：
    /// Unity 的 JointDrive 也是这样（spring 为 0 就是没有驱动）。刚度为 0 的
    /// 位置电机在 Jolt 里会断言设置非法，所以这里直接拒掉而不是"设了但不生效"。
    ///
    /// ⚠️ 平移轴用 `maximumForce` 当**力**限幅，旋转轴当**力矩**限幅 —— 与 Jolt
    /// 的 `MotorSettings::SetForceLimit` / `SetTorqueLimit` 同一条分割线。
    bool SetJointAxisDrive(uint32_t joint_id, ConfigurableJointAxis axis, const JointDrive& drive);
    bool GetJointAxisDrive(uint32_t joint_id, ConfigurableJointAxis axis, JointDrive& out_drive) const;

    /// 四个目标量一起设：Jolt 那侧它们各自是一个 setter，但语义上是"同一个目标
    /// 姿态"的四个面，分开设会经过三个中间态（设了 targetPosition 但
    /// targetVelocity 还是旧的 ⇒ 驱动朝一个没人想要的方向使劲）。
    bool SetJointTarget(uint32_t joint_id,
                        const Vector3& target_position,
                        const Vector3& target_velocity,
                        const Vector3& target_angular_velocity,
                        const Quaternion& target_rotation);
    bool GetJointTarget(uint32_t joint_id,
                        Vector3& out_target_position,
                        Vector3& out_target_velocity,
                        Vector3& out_target_angular_velocity,
                        Quaternion& out_target_rotation) const;

    /// 线性限位的软硬。**只有平移那三个轴有**：Jolt 的注释写得很直白 ——
    /// "Only soft translation limits are supported, soft rotation limits are not
    /// currently supported"，所以 Unity 的 `angularXLimitSpring` /
    /// `angularYZLimitSpring` 在本引擎里没有对应物（写了会静默无效，那正是这
    /// 个引擎一贯避免的一类坑 ⇒ 干脆不发布这两个属性）。
    bool SetJointLinearLimitSpring(uint32_t joint_id, float spring, float damper);
    bool GetJointLinearLimitSpring(uint32_t joint_id, float& out_spring, float& out_damper) const;

    /// cast a ray and find the hits
    /// @ray_origin: origin of ray
    /// @ray_direction: ray direction
    /// @ray_length: ray length, anything beyond this length will not be reported as a hit
    /// @out_hits: the found hits, sorted by distance
    /// @layer_mask: only bodies on a layer selected by this mask are reported
    ///             (default: every layer).
    /// @return: true if any hits found, else false
    bool Raycast(Vector3 ray_origin,
                 Vector3 ray_direction,
                 float ray_length,
                 std::vector<PhysicsHitInfo>& out_hits,
                 uint32_t layer_mask = kPhysicsAllLayers);

    /// cast a shape and find the hits
    /// @shape: the casted rigidbody shape
    /// @shape_transform: the initial global transform of the casted shape
    /// @sweep_direction: sweep direction
    /// @sweep_length: sweep length, anything beyond this length will not be reported as a hit
    /// @out_hits: the found hits, sorted by distance
    /// @layer_mask: only bodies on a selected layer are reported.
    /// @return: true if any hits found, else false
    bool Sweep(const RigidBodyShape& shape,
               const Matrix4x4& shape_transform,
               Vector3 sweep_direction,
               float sweep_length,
               std::vector<PhysicsHitInfo>& out_hits,
               uint32_t layer_mask = kPhysicsAllLayers);

    /// overlap test
    /// @shape: rigidbody shape
    /// @layer_mask: only bodies on a selected layer count.
    /// @return: true if overlapped with any rigidbodies
    bool IsOverlap(const RigidBodyShape& shape,
                   const Matrix4x4& global_transform,
                   uint32_t layer_mask = kPhysicsAllLayers);

    /// Which bodies a shape overlaps. `IsOverlap` only answers "any", which is
    /// enough for a Check* but not for Unity's `Physics.OverlapSphere`, whose
    /// whole point is handing back the list.
    ///
    /// @out_hits: one entry per overlapping body. `hit_distance` is meaningless
    ///            for an overlap (there is no direction) and stays 0.
    /// @return: true if anything overlapped.
    bool OverlapAll(const RigidBodyShape& shape,
                    const Matrix4x4& global_transform,
                    std::vector<PhysicsHitInfo>& out_hits,
                    uint32_t layer_mask = kPhysicsAllLayers);

    // --- primitive queries (no engine Object needed) ------------------------
    //
    // Sweep / OverlapAll take a RigidBodyShape, which means an engine Object
    // (and ObjectManager) has to exist to describe "a sphere of radius 2".
    // Scripts do not have one. These take a plain descriptor instead, so
    // `Physics.SphereCast` costs no allocation.

    /// Cast a box / sphere / capsule. @see Sweep.
    bool CastPrimitive(const PhysicsPrimitiveDesc& desc,
                       const Matrix4x4& world_transform,
                       Vector3 sweep_direction,
                       float sweep_length,
                       std::vector<PhysicsHitInfo>& out_hits,
                       uint32_t layer_mask = kPhysicsAllLayers);

    /// Every body a box / sphere / capsule overlaps. @see OverlapAll.
    bool OverlapPrimitive(const PhysicsPrimitiveDesc& desc,
                          const Matrix4x4& world_transform,
                          std::vector<PhysicsHitInfo>& out_hits,
                          uint32_t layer_mask = kPhysicsAllLayers);

    void GetShapeBoundingBoxes(uint32_t body_id, std::vector<AxisAlignedBox>& out_bounding_boxes) const;

#ifdef ENABLE_PHYSICS_DEBUG_RENDERER
    void DrawPhysicsScene(JPH::DebugRenderer* debug_renderer);
#endif

protected:
    /// Shared implementation of AddForce / AddImpulse: both take a body write
    /// lock and do nothing on a non-dynamic body.
    bool ApplyForceOrImpulse(uint32_t body_id, const Vector3& value, bool is_force);

    /// Drop every hit whose body is on a layer `layer_mask` does not select,
    /// and stamp the surviving hits with their layer.
    ///
    /// Applied *after* the collector has run rather than inside it: Jolt's
    /// collector traits early-out on the first hit for AnyHit queries, and a
    /// mask test that ran there would have to replicate Jolt's body locking.
    /// Query result sets are small, so a second pass is the cheap option.
    void FilterHitsByLayer(std::vector<PhysicsHitInfo>& in_out_hits, uint32_t layer_mask);

    // we use single Jolt physics system for each scene
    JoltPhysics m_Physics;
#if defined(__EMSCRIPTEN__)
    // Web stub: forward-declared so the header doesn't need Jolt at all.
    std::unique_ptr<ObjectVsBroadPhaseLayerFilterImpl> m_ObjectVsBroadPhaseLayerFilter;
    std::unique_ptr<ObjectLayerPairFilterImpl> m_ObjectLayerPairFilter;
#elif !defined(ZENGINE_SHARED_NO_PHYSICS)
    ObjectVsBroadPhaseLayerFilterImpl m_ObjectVsBroadPhaseLayerFilter;
    ObjectLayerPairFilterImpl m_ObjectLayerPairFilter;
#endif
    // (slim build: no filter members at all -- see the note at the top of the file)
    PhysicsConfig m_Config;

    std::vector<uint32_t> m_PendingRemoveBodies;

#if !defined(ZENGINE_SHARED_NO_PHYSICS)
    /// Registered on the Jolt system so sensors can produce trigger events.
    /// Kept behind a unique_ptr because the header only forward-declares it
    /// (and because on Web the whole thing is replaced by a stub).
    ///
    /// Dropped on the slim build: PhysicsContactListener is only *defined* in
    /// PhysicsScene.cpp, so a TU that cannot see it cannot destroy the
    /// unique_ptr either -- and that TU is exactly the one that is excluded.
    std::unique_ptr<PhysicsContactListener> m_ContactListener;
#endif

    /// Constraints owned by this scene. Jolt's ConstraintManager does not take
    /// ownership, so entries are released by hand.
    std::vector<JPH::Constraint*> m_Joints;

    /// 关节 id -> 约束，越界 / 已拆除返回 nullptr。上面那一组运行时控制共用。
    JPH::Constraint* JointAt(uint32_t joint_id) const;
};