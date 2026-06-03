// =============================================================================
// PhysicsStubs.cpp (Web / Emscripten only)
// -----------------------------------------------------------------------------
// JoltPhysics is intentionally not built for the Web target — its SIMD/PCH
// layout doesn't fly cleanly under emcc out of the box, and `engine/3rdparty/
// CMakeLists.txt` exposes only an INTERFACE `Jolt` placeholder there.
//
// Several runtime translation units (`level.cpp`, `rigidbody_component.cpp`,
// `motor_component.cpp`, ...) still call into `PhysicsManager` /
// `PhysicsScene`. To keep the link step happy without dragging Jolt into the
// Web build, the corresponding `runtime/function/physics/*.cpp` files are
// excluded from the Emscripten configuration (see `engine/source/runtime/
// CMakeLists.txt`) and replaced by the no-op definitions below.
//
// Behaviour: every method either returns a sentinel "no hit / invalid body"
// value or is a no-op. This is enough for the gameplay layer to execute
// (rigid bodies just don't simulate), and matches what we want for the
// initial WebGL2 bring-up. Bringing real physics to Web is tracked separately
// (likely via `-msimd128` + disabling Jolt's PCH).
// =============================================================================

#if defined(__EMSCRIPTEN__)

    #include "Runtime/Function/Physics/PhysicsManager.h"
    #include "Runtime/Function/Physics/PhysicsScene.h"

    #include <algorithm>
    #include <memory>

// -----------------------------------------------------------------------------
// Concrete (empty) definitions for the two filter classes that `physics_scene.h`
// only forward-declares on Web. We need a *complete* type at the point where
// `std::unique_ptr<...>`'s destructor is instantiated; the destructors of
// `PhysicsScene` (defined below in this TU) qualify since these definitions
// are visible there.
// -----------------------------------------------------------------------------
class ObjectVsBroadPhaseLayerFilterImpl
{
};

class ObjectLayerPairFilterImpl
{
};

// -----------------------------------------------------------------------------
// PhysicsManager
// -----------------------------------------------------------------------------
bool PhysicsManager::Initialize()
{
    return true;
}

void PhysicsManager::Shutdown()
{
    m_Scenes.clear();
}

std::weak_ptr<PhysicsScene> PhysicsManager::CreatePhysicsScene(const Vector3& gravity)
{
    std::shared_ptr<PhysicsScene> physics_scene = std::make_shared<PhysicsScene>(gravity);
    m_Scenes.push_back(physics_scene);
    return physics_scene;
}

void PhysicsManager::DeletePhysicsScene(std::weak_ptr<PhysicsScene> physics_scene)
{
    std::shared_ptr<PhysicsScene> deleted_scene = physics_scene.lock();
    auto iter = std::find(m_Scenes.begin(), m_Scenes.end(), deleted_scene);
    if (iter != m_Scenes.end())
    {
        m_Scenes.erase(iter);
    }
}

// -----------------------------------------------------------------------------
// PhysicsScene
// -----------------------------------------------------------------------------
PhysicsScene::PhysicsScene(const Vector3& gravity)
{
    // m_Physics members default to nullptr; filters stay empty unique_ptrs on Web.
    m_Config.m_Gravity = gravity;
}

PhysicsScene::~PhysicsScene() = default;

uint32_t PhysicsScene::CreateRigidBody(const Transform& /*global_transform*/,
                                       const RigidBodyComponentRes& /*rigidbody_actor_res*/)
{
    return s_InvalidRigidbodyId;
}

void PhysicsScene::RemoveRigidBody(uint32_t /*body_id*/) {}

void PhysicsScene::UpdateRigidBodyGlobalTransform(uint32_t /*body_id*/, const Transform& /*global_transform*/) {}

void PhysicsScene::Tick(float /*delta_time*/) {}

bool PhysicsScene::Raycast(Vector3 /*ray_origin*/,
                           Vector3 /*ray_direction*/,
                           float /*ray_length*/,
                           std::vector<PhysicsHitInfo>& out_hits)
{
    out_hits.clear();
    return false;
}

bool PhysicsScene::Sweep(const RigidBodyShape& /*shape*/,
                         const Matrix4x4& /*shape_transform*/,
                         Vector3 /*sweep_direction*/,
                         float /*sweep_length*/,
                         std::vector<PhysicsHitInfo>& out_hits)
{
    out_hits.clear();
    return false;
}

bool PhysicsScene::IsOverlap(const RigidBodyShape& /*shape*/, const Matrix4x4& /*global_transform*/)
{
    return false;
}

void PhysicsScene::GetShapeBoundingBoxes(uint32_t /*body_id*/,
                                         std::vector<AxisAlignedBox>& out_bounding_boxes) const
{
    out_bounding_boxes.clear();
}

#endif  // __EMSCRIPTEN__
