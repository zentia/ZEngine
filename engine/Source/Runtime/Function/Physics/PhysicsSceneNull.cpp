// ---------------------------------------------------------------------------
// Null PhysicsScene -- compiled ONLY into the slim iOS shared runtime
// (ZENGINE_IOS_SHARED_SLIM_MODULES / ZENGINE_SHARED_NO_PHYSICS).
//
// Why this file exists: the 104 symbols ZRuntimeShared exports are the
// MemoryProfiler C API + pesapi; there is no physics entry point. Jolt was
// still ~0.18 MB of the dylib because PhysicsScene.cpp / PhysicsCharacter.cpp /
// Jolt/Utils.cpp are part of the Runtime source set and static registrars keep
// them reachable.
//
// Those three TUs are excluded from this target, so *something* still has to
// define the ~20 PhysicsScene methods the rest of the engine calls
// (RigidbodyComponent, Level, NavigationSystem, AIPerceptionComponent,
// CharacterController, MotorComponent, PhysicsManager). This is that
// something: every query reports "no such body" and every mutator does
// nothing, which is exactly the documented contract for an unknown body id
// (see the comment on the body-state section in PhysicsScene.h) -- callers
// already treat false as "nothing happened".
//
// It must define the virtual destructor: that is the key function, so it is
// what emits the vtable and typeinfo for PhysicsScene.
//
// The guard rail: if this file ever misses a method the rest of the engine
// calls, the link still succeeds (-undefined dynamic_lookup) but the symbol is
// bound to 0 and the first call is EXC_BAD_ACCESS. That is why
// `nm -m | grep -c "dynamically looked up"` has to stay 0 -- check it after
// touching anything here.
// ---------------------------------------------------------------------------

// Wrapped as a whole, not `#error`ed: Runtime/CMakeLists.txt globs *.cpp, so
// this file is in EVERY target's source list (ZRuntime, host, Editor, web).
// Outside the slim build it has to compile to an empty TU.
#if defined(ZENGINE_SHARED_NO_PHYSICS)

#include "Runtime/Function/Physics/PhysicsScene.h"

#include "Runtime/Core/Log/LogSystem.h"

namespace
{
    // One line per scene, not per call: the slim build is a profiler-only
    // plugin, so a per-frame warning would bury everything else.
    void LogOnce()
    {
        LOG_WARNING(ZPhysics,
                    "PhysicsScene: physics is disabled in this build "
                    "(ZENGINE_SHARED_NO_PHYSICS) -- all queries report 'no body'");
    }
}  // namespace

PhysicsScene::PhysicsScene(const Vector3& gravity)
{
    LogOnce();
    m_Config.m_Gravity = gravity;
}

PhysicsScene::~PhysicsScene() = default;

void PhysicsScene::setGravity(const Vector3& gravity)
{
    m_Config.m_Gravity = gravity;
}

uint32_t PhysicsScene::CreateRigidBody(const TransformTRS&, const RigidBodyComponentRes&, int32_t)
{
    return s_InvalidRigidbodyId;
}

void PhysicsScene::RemoveRigidBody(uint32_t) {}

uint32_t PhysicsScene::CreateStaticAnchorBody(const Vector3&)
{
    // 没有 Jolt 就没有 body，"拴在世界某个点上"这件事也就没有落点。返回
    // 无效 id，让调用方走"关节建不出来"那条路（而不是返回一个假的 id，
    // 之后每个关节方法都拿它去查、全部静默失败）。
    return s_InvalidRigidbodyId;
}

bool PhysicsScene::GetBodyLayer(uint32_t, int32_t&) const
{
    return false;
}

bool PhysicsScene::SetBodyLayer(uint32_t, int32_t)
{
    return false;
}

void PhysicsScene::UpdateRigidBodyGlobalTransform(uint32_t, const TransformTRS&) {}

bool PhysicsScene::GetRigidBodyMotionType(uint32_t, RigidBodyActorType&) const
{
    return false;
}

bool PhysicsScene::SetRigidBodyMotionType(uint32_t, RigidBodyActorType)
{
    return false;
}

bool PhysicsScene::GetRigidBodyPosition(uint32_t, Vector3&) const
{
    return false;
}

bool PhysicsScene::GetRigidBodyRotation(uint32_t, Quaternion&) const
{
    return false;
}

bool PhysicsScene::GetLinearVelocity(uint32_t, Vector3&) const
{
    return false;
}

bool PhysicsScene::SetLinearVelocity(uint32_t, const Vector3&)
{
    return false;
}

bool PhysicsScene::GetAngularVelocity(uint32_t, Vector3&) const
{
    return false;
}

bool PhysicsScene::SetAngularVelocity(uint32_t, const Vector3&)
{
    return false;
}

bool PhysicsScene::AddForce(uint32_t, const Vector3&)
{
    return false;
}

bool PhysicsScene::AddImpulse(uint32_t, const Vector3&)
{
    return false;
}

bool PhysicsScene::GetInverseMass(uint32_t, float&) const
{
    return false;
}

void PhysicsScene::Tick(float) {}

bool PhysicsScene::SetRigidBodySensor(uint32_t, bool)
{
    return false;
}

bool PhysicsScene::IsRigidBodySensor(uint32_t, bool&) const
{
    return false;
}

bool PhysicsScene::GetBodySurface(uint32_t, float&, float&) const
{
    return false;
}

bool PhysicsScene::SetBodyFriction(uint32_t, float)
{
    return false;
}

bool PhysicsScene::SetBodyRestitution(uint32_t, float)
{
    return false;
}

bool PhysicsScene::SetBodyCombineModes(uint32_t, PhysicMaterialCombine, PhysicMaterialCombine)
{
    return false;
}

void PhysicsScene::PollTriggerEvents(std::vector<PhysicsTriggerEvent>& out_events)
{
    out_events.clear();
}

void PhysicsScene::PollCollisionEvents(std::vector<PhysicsCollisionEvent>& out_events)
{
    out_events.clear();
}

uint32_t PhysicsScene::CreateJoint(const PhysicsJointDesc&)
{
    return s_InvalidJointId;
}

bool PhysicsScene::RemoveJoint(uint32_t)
{
    return false;
}

bool PhysicsScene::Raycast(Vector3, Vector3, float, std::vector<PhysicsHitInfo>& out_hits, uint32_t)
{
    out_hits.clear();
    return false;
}

bool PhysicsScene::Sweep(const RigidBodyShape&,
                         const Matrix4x4&,
                         Vector3,
                         float,
                         std::vector<PhysicsHitInfo>& out_hits,
                         uint32_t)
{
    out_hits.clear();
    return false;
}

bool PhysicsScene::IsOverlap(const RigidBodyShape&, const Matrix4x4&, uint32_t)
{
    return false;
}

bool PhysicsScene::OverlapAll(const RigidBodyShape&, const Matrix4x4&, std::vector<PhysicsHitInfo>& out_hits, uint32_t)
{
    out_hits.clear();
    return false;
}

bool PhysicsScene::CastPrimitive(const PhysicsPrimitiveDesc&,
                                 const Matrix4x4&,
                                 Vector3,
                                 float,
                                 std::vector<PhysicsHitInfo>& out_hits,
                                 uint32_t)
{
    out_hits.clear();
    return false;
}

bool PhysicsScene::OverlapPrimitive(const PhysicsPrimitiveDesc&,
                                    const Matrix4x4&,
                                    std::vector<PhysicsHitInfo>& out_hits,
                                    uint32_t)
{
    out_hits.clear();
    return false;
}

void PhysicsScene::FilterHitsByLayer(std::vector<PhysicsHitInfo>& in_out_hits, uint32_t)
{
    in_out_hits.clear();
}

void PhysicsScene::GetShapeBoundingBoxes(uint32_t, std::vector<AxisAlignedBox>& out_bounding_boxes) const
{
    out_bounding_boxes.clear();
}

bool PhysicsScene::ApplyForceOrImpulse(uint32_t, const Vector3&, bool)
{
    return false;
}

// --- Rigidbody 的其余面（constraints / 质心 / 惯性 / CCD / 点速度）----------
//
// 与上面同一条契约：没有物理就没有 body，一律"查不到" / "什么都没做"。

bool PhysicsScene::GetBodyAllowedDOFs(uint32_t, uint32_t& out_dof_mask) const
{
    out_dof_mask = 0;
    return false;
}

bool PhysicsScene::SetBodyAllowedDOFs(uint32_t, uint32_t, float)
{
    return false;
}

bool PhysicsScene::GetBodyCenterOfMass(uint32_t, Vector3& out_local_center_of_mass) const
{
    out_local_center_of_mass = Vector3::ZERO;
    return false;
}

bool PhysicsScene::GetBodyWorldCenterOfMass(uint32_t, Vector3& out_world_center_of_mass) const
{
    out_world_center_of_mass = Vector3::ZERO;
    return false;
}

bool PhysicsScene::GetBodyInertiaTensor(uint32_t, Vector3& out_inertia, Quaternion& out_rotation) const
{
    out_inertia = Vector3::ZERO;
    out_rotation = Quaternion::IDENTITY;
    return false;
}

bool PhysicsScene::GetBodyVolume(uint32_t, float& out_volume) const
{
    out_volume = 0.f;
    return false;
}

bool PhysicsScene::GetBodyMotionQuality(uint32_t, CollisionDetectionMode& out_mode) const
{
    out_mode = CollisionDetectionMode::Discrete;
    return false;
}

bool PhysicsScene::SetBodyMotionQuality(uint32_t, CollisionDetectionMode)
{
    return false;
}

bool PhysicsScene::GetBodyMaxAngularVelocity(uint32_t, float& out_max_angular_velocity) const
{
    out_max_angular_velocity = 0.f;
    return false;
}

bool PhysicsScene::SetBodyMaxAngularVelocity(uint32_t, float)
{
    return false;
}

bool PhysicsScene::GetBodyPointVelocity(uint32_t, const Vector3&, Vector3& out_velocity) const
{
    out_velocity = Vector3::ZERO;
    return false;
}

// --- 关节运行时控制 --------------------------------------------------------

bool PhysicsScene::GetJointType(uint32_t, PhysicsJointType& out_type) const
{
    out_type = PhysicsJointType::fixed;
    return false;
}

bool PhysicsScene::SetJointEnabled(uint32_t, bool)
{
    return false;
}

bool PhysicsScene::IsJointEnabled(uint32_t, bool& out_enabled) const
{
    out_enabled = false;
    return false;
}

bool PhysicsScene::GetJointAngle(uint32_t, float& out_angle_radians) const
{
    out_angle_radians = 0.f;
    return false;
}

bool PhysicsScene::GetJointVelocity(uint32_t, float& out_radians_per_second) const
{
    out_radians_per_second = 0.f;
    return false;
}

bool PhysicsScene::SetJointLimits(uint32_t, float, float)
{
    return false;
}

bool PhysicsScene::GetJointLimits(uint32_t, float& out_min_limit, float& out_max_limit) const
{
    out_min_limit = 0.f;
    out_max_limit = 0.f;
    return false;
}

bool PhysicsScene::SetJointMotor(uint32_t, bool, float, float)
{
    return false;
}

bool PhysicsScene::SetJointPositionMotor(uint32_t, bool, float, float, float, float)
{
    return false;
}

bool PhysicsScene::SetJointDistance(uint32_t, float, float)
{
    return false;
}

bool PhysicsScene::GetJointDistance(uint32_t, float& out_min_distance, float& out_max_distance) const
{
    out_min_distance = 0.f;
    out_max_distance = 0.f;
    return false;
}

bool PhysicsScene::SetJointSpring(uint32_t, float, float)
{
    return false;
}

bool PhysicsScene::GetJointImpulse(uint32_t, float& out_linear_impulse, float& out_angular_impulse) const
{
    out_linear_impulse = 0.f;
    out_angular_impulse = 0.f;
    return false;
}

bool PhysicsScene::SetJointSwingTwistLimits(uint32_t, float, float, float, float)
{
    return false;
}

bool PhysicsScene::GetJointSwingTwistLimits(uint32_t, float&, float&, float&, float&) const
{
    return false;
}

bool PhysicsScene::SetJointSwingTwistSpring(uint32_t, float, float, float, float)
{
    return false;
}

bool PhysicsScene::GetJointSwingTwistSpring(uint32_t, float&, float&, float&, float&) const
{
    return false;
}

#endif  // ZENGINE_SHARED_NO_PHYSICS
