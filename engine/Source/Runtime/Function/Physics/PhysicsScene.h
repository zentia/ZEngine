#pragma once

// On Web (Emscripten) we don't link Jolt — `jolt/utils.h` itself pulls
// `Jolt/Jolt.h` which is not on the include path in that build. Forward-declare
// the two filter implementations and use unique_ptr members so the header is
// self-contained on Web. Native builds keep the value-typed filter members
// exactly as before to preserve ABI/initialisation order.
#if defined(__EMSCRIPTEN__)
    #include <memory>
class ObjectVsBroadPhaseLayerFilterImpl;
class ObjectLayerPairFilterImpl;
#else
    #include "jolt/utils.h"
#endif
#include "Runtime/Core/Math/AxisAligned.h"
#include "Runtime/Core/Math/Quaternion.h"
#include "Runtime/Function/Physics/PhysicsConfig.h"

namespace JPH
{
    class PhysicsSystem;
    class JobSystem;
    class TempAllocator;
    class BroadPhaseLayerInterface;
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

struct PhysicsHitInfo
{
    Vector3 hit_position;
    Vector3 hit_normal;
    float hit_distance {0.f};
    uint32_t body_id {s_InvalidRigidbodyId};
};

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

    uint32_t CreateRigidBody(const TransformTRS& global_transform, const RigidBodyComponentRes& rigidbody_actor_res);
    void RemoveRigidBody(uint32_t body_id);

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

    void Tick(float delta_time);

    /// cast a ray and find the hits
    /// @ray_origin: origin of ray
    /// @ray_direction: ray direction
    /// @ray_length: ray length, anything beyond this length will not be reported as a hit
    /// @out_hits: the found hits, sorted by distance
    /// @return: true if any hits found, else false
    bool Raycast(Vector3 ray_origin, Vector3 ray_direction, float ray_length, std::vector<PhysicsHitInfo>& out_hits);

    /// cast a shape and find the hits
    /// @shape: the casted rigidbody shape
    /// @shape_transform: the initial global transform of the casted shape
    /// @sweep_direction: sweep direction
    /// @sweep_length: sweep length, anything beyond this length will not be reported as a hit
    /// @out_hits: the found hits, sorted by distance
    /// @return: true if any hits found, else false
    bool Sweep(const RigidBodyShape& shape,
               const Matrix4x4& shape_transform,
               Vector3 sweep_direction,
               float sweep_length,
               std::vector<PhysicsHitInfo>& out_hits);

    /// overlap test
    /// @shape: rigidbody shape
    /// @return: true if overlapped with any rigidbodies
    bool IsOverlap(const RigidBodyShape& shape, const Matrix4x4& global_transform);

    void GetShapeBoundingBoxes(uint32_t body_id, std::vector<AxisAlignedBox>& out_bounding_boxes) const;

#ifdef ENABLE_PHYSICS_DEBUG_RENDERER
    void DrawPhysicsScene(JPH::DebugRenderer* debug_renderer);
#endif

protected:
    /// Shared implementation of AddForce / AddImpulse: both take a body write
    /// lock and do nothing on a non-dynamic body.
    bool ApplyForceOrImpulse(uint32_t body_id, const Vector3& value, bool is_force);

    // we use single Jolt physics system for each scene
    JoltPhysics m_Physics;
#if defined(__EMSCRIPTEN__)
    // Web stub: forward-declared so the header doesn't need Jolt at all.
    std::unique_ptr<ObjectVsBroadPhaseLayerFilterImpl> m_ObjectVsBroadPhaseLayerFilter;
    std::unique_ptr<ObjectLayerPairFilterImpl> m_ObjectLayerPairFilter;
#else
    ObjectVsBroadPhaseLayerFilterImpl m_ObjectVsBroadPhaseLayerFilter;
    ObjectLayerPairFilterImpl m_ObjectLayerPairFilter;
#endif
    PhysicsConfig m_Config;

    std::vector<uint32_t> m_PendingRemoveBodies;
};