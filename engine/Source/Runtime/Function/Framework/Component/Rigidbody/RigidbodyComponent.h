#pragma once

#include "Runtime/Function/Framework/Component/Component.h"
#include "Runtime/Resource/ResType/Components/RigidBody.h"

class RigidBodyComponent : public Component
{
    REGISTER_CLASS(RigidBodyComponent);
    DECLARE_OBJECT_SERIALIZE();

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

    RigidBodyComponentRes m_RigidbodyRes;

    /// Sentinel for "no body created yet". Matches the value the field was
    /// already default-initialized to, so an unflushed body is detectable.
    static constexpr uint32_t k_invalid_rigidbody_id = 0xffffffff;

    uint32_t m_RigidbodyId {k_invalid_rigidbody_id};

    /// Set by PostLoadResource when no physics scene was active yet; cleared
    /// when the body is actually created.
    bool m_HasPendingBody {false};
};
