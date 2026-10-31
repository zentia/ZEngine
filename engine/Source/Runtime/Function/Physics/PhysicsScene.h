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
    fixed,     ///< Welds two bodies together (Unity FixedJoint).
    hinge,     ///< Rotation about one axis, with optional limits (HingeJoint).
    point,     ///< Keeps two points coincident (ball socket).
    slider,    ///< Translation along one axis, with optional limits.
    distance   ///< Keeps two points within a distance range (SpringJoint).
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
};