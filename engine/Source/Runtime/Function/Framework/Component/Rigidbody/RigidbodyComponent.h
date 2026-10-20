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

    /// Where the body actually is. For a simulated body this drifts away from
    /// the Transform component, because nothing writes the body back yet.
    ZFUNCTION()
    Vector3 GetBodyPosition() const;
    ZFUNCTION()
    Quaternion GetBodyRotation() const;

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

protected:
    void CreateRigidBody(const TransformTRS& global_transform);
    void RemoveRigidBody();

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
};
