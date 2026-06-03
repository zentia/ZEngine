#pragma once

#include "Runtime/Function/Framework/Component/Component.h"
#include "Runtime/Resource/ResType/Components/RigidBody.h"

class RigidBodyComponent : public Component
{
public:
    RigidBodyComponent() = default;
    ~RigidBodyComponent() override;

    void PostLoadResource(GameObject* parent_object) override;

    void Tick(float delta_time) override {}
    void UpdateGlobalTransform(const Transform& transform, bool is_scale_dirty);
    void GetShapeBoundingBoxes(std::vector<AxisAlignedBox>& out_boudning_boxes) const;

protected:
    void CreateRigidBody(const Transform& global_transform);
    void RemoveRigidBody();

    RigidBodyComponentRes m_RigidbodyRes;

    uint32_t m_RigidbodyId {0xffffffff};
};