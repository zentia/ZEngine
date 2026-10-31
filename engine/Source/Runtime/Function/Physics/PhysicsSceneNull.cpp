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

#endif  // ZENGINE_SHARED_NO_PHYSICS
