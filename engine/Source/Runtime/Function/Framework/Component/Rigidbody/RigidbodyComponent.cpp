#include "RigidbodyComponent.h"

#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Function/Framework/Component/Transform/Transform.h"
#include "Runtime/Function/Framework/World/WorldManager.h"
#include "Runtime/Function/Physics/PhysicsScene.h"

IMPLEMENT_REGISTER_CLASS(RigidBodyComponent)
IMPLEMENT_OBJECT_SERIALIZE(RigidBodyComponent)

template<typename TransferFunction>
void RigidBodyComponent::Transfer(TransferFunction& transfer)
{
    Super::Transfer(transfer);
    transfer.Transfer(m_RigidbodyRes, "rigidbody_res");
}

INSTANTIATE_TEMPLATE_TRANSFER_EXPORTED(RigidBodyComponent)

void RigidBodyComponent::PostLoadResource(GameObject* parent_object)
{
    m_ParentObject = parent_object;

    const Transform* parent_transform = m_ParentObject->tryGetComponentConst(Transform);
    if (parent_transform == nullptr)
    {
        LOG_ERROR(ZRigidBody, "No transform component in the object");
        return;
    }

    // Level::load() creates objects before WorldManager publishes the level as
    // active, so there is no current physics scene yet. A prefab-expanded body
    // hits this on every level load. Defer instead of asserting: FlushPendingBody
    // creates it once the scene exists.
    std::shared_ptr<PhysicsScene> physics_scene = GET_SYSTEM(WorldManager)->GetCurrentActivePhysicsScene().lock();
    if (!physics_scene)
    {
        m_HasPendingBody = true;
        return;
    }

    m_RigidbodyId = physics_scene->CreateRigidBody(parent_transform->BuildLocalTRS(), m_RigidbodyRes);
    m_HasPendingBody = false;
}

bool RigidBodyComponent::HasPendingBody() const
{
    return m_HasPendingBody;
}

bool RigidBodyComponent::FlushPendingBody()
{
    if (!m_HasPendingBody)
    {
        return false;
    }

    std::shared_ptr<PhysicsScene> physics_scene = GET_SYSTEM(WorldManager)->GetCurrentActivePhysicsScene().lock();
    if (!physics_scene)
    {
        return false;
    }

    const Transform* parent_transform = m_ParentObject->tryGetComponentConst(Transform);
    if (parent_transform == nullptr)
    {
        return false;
    }

    const uint32_t body_id = physics_scene->CreateRigidBody(parent_transform->BuildLocalTRS(), m_RigidbodyRes);
    if (body_id == k_invalid_rigidbody_id)
    {
        // CreateRigidBody logs the reason (e.g. no shapes resolved). Stay
        // pending so a later flush can retry, and report failure so the caller
        // does not count this as a created collider.
        return false;
    }

    m_RigidbodyId = body_id;
    m_HasPendingBody = false;
    return true;
}

RigidBodyComponent::~RigidBodyComponent()
{
    // A body that was never created (no physics scene at PostLoadResource time,
    // or no pending flush) has no id to remove -- and destroying a level tears
    // its scene down first, so the scene may be gone here too.
    if (m_RigidbodyId == k_invalid_rigidbody_id)
    {
        return;
    }

    std::shared_ptr<PhysicsScene> physics_scene = GET_SYSTEM(WorldManager)->GetCurrentActivePhysicsScene().lock();
    if (!physics_scene)
    {
        return;
    }

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
    // 与析构路径同样的判据：body 从未创建成功时（PostLoadResource 那会儿
    // 还没有物理场景，或延迟构建还没 flush）m_RigidbodyId 仍是哨兵值。
    // 把哨兵推进 PhysicsScene 的待销毁队列会让 Jolt 去销毁一个不存在的
    // body，直接断言崩溃——PIE 跑起来后第一帧就会踩到。
    if (m_RigidbodyId == k_invalid_rigidbody_id)
    {
        return;
    }

    std::shared_ptr<PhysicsScene> physics_scene = GET_SYSTEM(WorldManager)->GetCurrentActivePhysicsScene().lock();
    if (!physics_scene)
    {
        return;
    }

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