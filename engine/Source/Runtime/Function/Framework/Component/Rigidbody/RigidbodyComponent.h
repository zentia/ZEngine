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

    void Tick(float delta_time) override {}
    void UpdateGlobalTransform(const TransformTRS& transform, bool is_scale_dirty);
    void GetShapeBoundingBoxes(std::vector<AxisAlignedBox>& out_boudning_boxes) const;

    /// Read-only access to the serialized shape list. RigidBodyShape derives
    /// from Object, so each shape needs its own slot in a written object
    /// graph; serialization drivers (PrefabUtility's flattener) walk this to
    /// collect them. Without that the shapes serialize as null references.
    const RigidBodyComponentRes& GetRigidbodyRes() const { return m_RigidbodyRes; }

protected:
    void CreateRigidBody(const TransformTRS& global_transform);
    void RemoveRigidBody();

    RigidBodyComponentRes m_RigidbodyRes;

    uint32_t m_RigidbodyId {0xffffffff};
};
