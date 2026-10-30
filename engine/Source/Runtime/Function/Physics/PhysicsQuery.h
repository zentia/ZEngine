#pragma once

// =============================================================================
// PhysicsQuery -- the Jolt-free vocabulary of a physics query.
//
// Why this file exists separately from PhysicsScene.h: the script binding
// table (Runtime/Scripting/Binding/ScriptBinding.h) needs a
// ZScript::Converter for RaycastHit, and PhysicsScene.h drags in
// Jolt/utils.h -> Jolt/Jolt.h. Pulling Jolt into ScriptBinding.h would put it
// into every generated .gen.cpp in the engine (and into the Web / slim-iOS
// builds that deliberately do not link Jolt at all).
//
// So: everything a *query* is made of -- the mask, the primitive descriptor
// and the hit record -- lives here and includes nothing but Vector3.
// =============================================================================

#include "Runtime/Core/Math/Vector3.h"

/// Body id handed back by a query when there was no hit. Same sentinel as
/// `s_InvalidRigidbodyId` in PhysicsScene.h, redeclared here so this header
/// stays free of the Jolt include.
static constexpr uint32_t kInvalidPhysicsBodyId = 0xffffffffu;

/// "Every layer". Unity spells this `Physics.AllLayers` (-1); as a uint32_t
/// mask it is all bits set.
static constexpr uint32_t kPhysicsAllLayers = 0xffffffffu;

/// Unity has exactly 32 layers and so do we (TagLayerManager::kLayerCount).
static constexpr int32_t kPhysicsLayerCount = 32;

/// Is `layer` selected by `layer_mask`?
///
/// A layer outside [0, 32) is never selected -- not even by kPhysicsAllLayers.
/// That is deliberate: it keeps a garbage layer index from matching "everything"
/// and silently turning a mis-authored collider into a hit.
inline bool PhysicsLayerMaskContains(uint32_t layer_mask, int32_t layer)
{
    if (layer < 0 || layer >= kPhysicsLayerCount)
    {
        return false;
    }
    return (layer_mask & (1u << static_cast<uint32_t>(layer))) != 0u;
}

/// Which primitive to cast / overlap, and how big it is.
///
/// Deliberately not a `RigidBodyShape`: building one needs an engine Object
/// (ObjectManager), and a physics query runs every frame -- allocating a
/// managed Box per OverlapSphere would leak. PhysicsScene turns this into a
/// JPH::Shape for the duration of the call.
struct PhysicsPrimitiveDesc
{
    enum class Kind : unsigned char
    {
        box,
        sphere,
        capsule
    };

    Kind kind {Kind::sphere};

    /// box: half extents on each axis.
    Vector3 halfExtents {0.5f, 0.5f, 0.5f};

    /// sphere / capsule: radius.
    float radius {0.5f};

    /// capsule: half height of the cylindrical part (excluding the caps).
    float halfHeight {0.5f};
};

/// The result of a query, mirroring Unity's `RaycastHit`.
///
/// `valid` is the one field Unity does not have: Unity returns a bool from
/// Physics.Raycast and fills an out-parameter. TypeScript has no out
/// parameters here, so the hit record carries its own validity and
/// `Physics.Raycast()` can return it directly.
struct RaycastHit
{
    /// false when the query hit nothing; every other field is then default.
    bool valid {false};
    /// World-space contact point.
    Vector3 point;
    /// World-space surface normal at the contact.
    Vector3 normal;
    /// Distance along the ray / sweep.
    float distance {0.f};
    /// The rigid body that was hit, or kInvalidPhysicsBodyId.
    uint32_t bodyId {kInvalidPhysicsBodyId};
    /// Unity layer of the body that was hit.
    int32_t layer {0};
};
