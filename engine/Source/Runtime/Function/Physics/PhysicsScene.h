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

class Transform;
class RigidBodyComponentRes;
class RigidBodyShape;

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

    uint32_t CreateRigidBody(const Transform& global_transform, const RigidBodyComponentRes& rigidbody_actor_res);
    void RemoveRigidBody(uint32_t body_id);

    void UpdateRigidBodyGlobalTransform(uint32_t body_id, const Transform& global_transform);

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