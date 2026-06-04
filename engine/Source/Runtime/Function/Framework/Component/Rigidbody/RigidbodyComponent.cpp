#include "RigidbodyComponent.h"

#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Function/Framework/Component/Transform/Transform.h"
#include "Runtime/Function/Framework/World/WorldManager.h"
#include "Runtime/Function/Physics/PhysicsScene.h"

void RigidBodyComponent::PostLoadResource(GameObject* parent_object)
{
    m_ParentObject = parent_object;

    const Transform* parent_transform = m_ParentObject->tryGetComponentConst(Transform);
    if (parent_transform == nullptr)
    {
        LOG_ERROR(ZRigidBody, "No transform component in the object");
        return;
    }

    std::shared_ptr<PhysicsScene> physics_scene = GET_SYSTEM(WorldManager)->GetCurrentActivePhysicsScene().lock();
    ASSERT(physics_scene);

    m_RigidbodyId = physics_scene->CreateRigidBody(parent_transform->BuildLocalTRS(), m_RigidbodyRes);
}

RigidBodyComponent::~RigidBodyComponent()
{
    std::shared_ptr<PhysicsScene> physics_scene = GET_SYSTEM(WorldManager)->GetCurrentActivePhysicsScene().lock();
    ASSERT(physics_scene);

    physics_scene->RemoveRigidBody(m_RigidbodyId);
}

void RigidBodyComponent::CreateRigidBody(const TransformTRS& global_transform)
{
    std::shared_ptr<PhysicsScene> physics_scene = GET_SYSTEM(WorldManager)->GetCurrentActivePhysicsScene().lock();
    ASSERT(physics_scene);

    m_RigidbodyId = physics_scene->CreateRigidBody(global_transform, m_RigidbodyRes);
}

void RigidBodyComponent::RemoveRigidBody()
{
    std::shared_ptr<PhysicsScene> physics_scene = GET_SYSTEM(WorldManager)->GetCurrentActivePhysicsScene().lock();
    ASSERT(physics_scene);

    physics_scene->RemoveRigidBody(m_RigidbodyId);
}

void RigidBodyComponent::UpdateGlobalTransform(const TransformTRS& transform, bool is_scale_dirty)
{
    if (is_scale_dirty)
    {
        RemoveRigidBody();

        CreateRigidBody(transform);
    }
    else
    {
        std::shared_ptr<PhysicsScene> physics_scene = GET_SYSTEM(WorldManager)->GetCurrentActivePhysicsScene().lock();
        ASSERT(physics_scene);

        physics_scene->UpdateRigidBodyGlobalTransform(m_RigidbodyId, transform);
    }
}

void RigidBodyComponent::GetShapeBoundingBoxes(std::vector<AxisAlignedBox>& out_bounding_boxes) const
{
    std::shared_ptr<PhysicsScene> physics_scene = GET_SYSTEM(WorldManager)->GetCurrentActivePhysicsScene().lock();
    ASSERT(physics_scene);

    physics_scene->GetShapeBoundingBoxes(m_RigidbodyId, out_bounding_boxes);
}