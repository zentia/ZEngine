#pragma once

// =============================================================================
// Physics -- the static script-facing physics facade (Unity's `Physics`).
//
// Why a class and not free functions: ZHeaderTool only binds ZFUNCTIONs that
// live on a ZCLASS, and the .d.ts it emits is `declare class Physics { static
// Raycast(...) }` -- which is also how Unity spells it. It is marked Abstract
// because Unity's Physics is a static class: REGISTER_CLASS_TRAITS(
// kTypeIsAbstract) stops a script from doing `new Physics()`.
//
// Everything here delegates to the active PhysicsScene. When there is none
// (editor with no level loaded, or a headless test), each query reports "no
// hit" rather than asserting -- same contract as RigidBodyComponent's body
// state accessors.
// =============================================================================

#include "Runtime/BaseClasses/Object.h"
#include "Runtime/Core/Math/Quaternion.h"
#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Function/Physics/PhysicsQuery.h"

#include <EASTL/string.h>

#include <memory>
#include <vector>

#include "Physics.generated.h"

class PhysicsScene;

ZCLASS(Abstract)
class Physics : public Object
{
    ZGENERATED_BODY(Physics)

public:
    // --- ray ---------------------------------------------------------------

    /// The nearest hit along a ray, or a `RaycastHit` with `valid === false`.
    ///
    /// `maxDistance` <= 0 means "infinite", matching Unity's default of
    /// Mathf.Infinity (Jolt has no infinity, so it is clamped to the scene's
    /// reachable range).
    ZFUNCTION()
    static RaycastHit Raycast(Vector3 origin, Vector3 direction, float maxDistance, int32_t layerMask);

    /// Every hit along the ray, nearest first. Unity's `Physics.RaycastAll`.
    ZFUNCTION()
    static std::vector<RaycastHit> RaycastAll(Vector3 origin, Vector3 direction, float maxDistance, int32_t layerMask);

    /// Is there anything between the two points? Unity's `Physics.Linecast`.
    ZFUNCTION()
    static bool Linecast(Vector3 start, Vector3 end, int32_t layerMask);

    // --- sweeps ------------------------------------------------------------

    /// Sweep a sphere. `RaycastHit.valid === false` when it hit nothing.
    ZFUNCTION()
    static RaycastHit SphereCast(Vector3 origin, float radius, Vector3 direction, float maxDistance, int32_t layerMask);

    ZFUNCTION()
    static std::vector<RaycastHit> SphereCastAll(Vector3 origin,
                                                 float radius,
                                                 Vector3 direction,
                                                 float maxDistance,
                                                 int32_t layerMask);

    /// Sweep an axis-aligned box of `halfExtents` about `center`.
    ZFUNCTION()
    static RaycastHit BoxCast(Vector3 center,
                              Vector3 halfExtents,
                              Vector3 direction,
                              float maxDistance,
                              int32_t layerMask);

    /// Sweep a capsule whose two cap centres are `point1` / `point2`.
    ZFUNCTION()
    static RaycastHit CapsuleCast(Vector3 point1,
                                  Vector3 point2,
                                  float radius,
                                  Vector3 direction,
                                  float maxDistance,
                                  int32_t layerMask);

    // --- boolean containment tests ----------------------------------------

    ZFUNCTION()
    static bool CheckSphere(Vector3 center, float radius, int32_t layerMask);

    ZFUNCTION()
    static bool CheckBox(Vector3 center, Vector3 halfExtents, int32_t layerMask);

    ZFUNCTION()
    static bool CheckCapsule(Vector3 point1, Vector3 point2, float radius, int32_t layerMask);

    // --- overlaps ----------------------------------------------------------

    /// Every body a sphere touches. Unity returns Collider[]; ZEngine returns
    /// hit records, so the caller also gets the contact point and the layer.
    ZFUNCTION()
    static std::vector<RaycastHit> OverlapSphere(Vector3 center, float radius, int32_t layerMask);

    ZFUNCTION()
    static std::vector<RaycastHit> OverlapBox(Vector3 center, Vector3 halfExtents, int32_t layerMask);

    ZFUNCTION()
    static std::vector<RaycastHit> OverlapCapsule(Vector3 point1, Vector3 point2, float radius, int32_t layerMask);

    // --- layers ------------------------------------------------------------

    /// Make (or stop making) two layers collide. This is the Layer Collision
    /// Matrix in the Unity editor, at runtime.
    ZFUNCTION()
    static void IgnoreLayerCollision(int32_t layer1, int32_t layer2, bool ignore);

    ZFUNCTION()
    static bool GetIgnoreLayerCollision(int32_t layer1, int32_t layer2);

    /// The set of layers `layer` collides with, as a bit mask. Feed it straight
    /// back into `layerMask` to ask "what can this object hit?".
    ZFUNCTION()
    static int32_t GetCollisionLayerMask(int32_t layer);

    ZFUNCTION()
    static eastl::string LayerToName(int32_t layer);

    /// @return the layer index, or -1 when the name is not defined (Unity
    /// returns -1 too).
    ZFUNCTION()
    static int32_t NameToLayer(const eastl::string& name);

    // --- world -------------------------------------------------------------

    /// Unity's `Physics.gravity`, as a getter/setter pair: ZSCRIPTPROPERTY
    /// cannot bind a static field.
    ZFUNCTION()
    static Vector3 GetGravity();

    ZFUNCTION()
    static void SetGravity(Vector3 gravity);

public:
    // Not script-facing: the scene every query above runs against.
    static std::shared_ptr<PhysicsScene> CurrentScene();
};
