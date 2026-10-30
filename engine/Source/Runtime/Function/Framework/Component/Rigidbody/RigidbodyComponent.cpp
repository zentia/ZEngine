#include "RigidbodyComponent.h"

#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Function/Framework/Component/Transform/Transform.h"
#include "Runtime/Function/Framework/World/WorldManager.h"
#include "Runtime/Function/Physics/PhysicsScene.h"

namespace
{
    /// The active physics scene, or null when there is none OR no WorldManager at all.
    /// Every caller already handles "no scene yet" (deferred body, level teardown); a
    /// process without a WorldManager -- an editor tool or headless test instantiating a
    /// prefab that holds a body -- is the same situation and must take the same path
    /// instead of dereferencing GET_SYSTEM(WorldManager).
    std::shared_ptr<PhysicsScene> CurrentPhysicsScene()
    {
        WorldManager* world = GET_SYSTEM(WorldManager);
        return world != nullptr ? world->GetCurrentActivePhysicsScene().lock() : nullptr;
    }
}  // namespace



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
    std::shared_ptr<PhysicsScene> physics_scene = CurrentPhysicsScene();
    if (!physics_scene)
    {
        m_HasPendingBody = true;
        return;
    }

    m_RigidbodyId = physics_scene->CreateRigidBody(parent_transform->BuildLocalTRS(),
                                                   m_RigidbodyRes,
                                                   CurrentGameObjectLayer());
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

    std::shared_ptr<PhysicsScene> physics_scene = CurrentPhysicsScene();
    if (!physics_scene)
    {
        return false;
    }

    const Transform* parent_transform = m_ParentObject->tryGetComponentConst(Transform);
    if (parent_transform == nullptr)
    {
        return false;
    }

    const uint32_t body_id =
        physics_scene->CreateRigidBody(parent_transform->BuildLocalTRS(), m_RigidbodyRes, CurrentGameObjectLayer());
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

    std::shared_ptr<PhysicsScene> physics_scene = CurrentPhysicsScene();
    if (!physics_scene)
    {
        return;
    }

    physics_scene->RemoveRigidBody(m_RigidbodyId);
}

void RigidBodyComponent::CreateRigidBody(const TransformTRS& global_transform)
{
    std::shared_ptr<PhysicsScene> physics_scene = CurrentPhysicsScene();
    ASSERT(physics_scene);

    m_RigidbodyId = physics_scene->CreateRigidBody(global_transform, m_RigidbodyRes, CurrentGameObjectLayer());
}

int32_t RigidBodyComponent::CurrentGameObjectLayer() const
{
    // A body created outside a GameObject (imported resource, unit test) has no
    // layer to inherit; 0 is "Default", which is what the old hardcoded
    // NON_MOVING object layer effectively was.
    return m_ParentObject != nullptr ? m_ParentObject->GetLayer() : 0;
}

void RigidBodyComponent::RefreshBodyLayer()
{
    std::shared_ptr<PhysicsScene> physics_scene = GetActivePhysicsScene();
    if (physics_scene)
    {
        physics_scene->SetBodyLayer(m_RigidbodyId, CurrentGameObjectLayer());
    }
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

    std::shared_ptr<PhysicsScene> physics_scene = CurrentPhysicsScene();
    if (!physics_scene)
    {
        return;
    }

    physics_scene->RemoveRigidBody(m_RigidbodyId);
}

// A helper every call below shares: the scene is allowed to be missing (level
// teardown destroys it before the components), and a pending / already-removed
// body simply has no id to talk about.
std::shared_ptr<PhysicsScene> RigidBodyComponent::GetActivePhysicsScene() const
{
    if (m_RigidbodyId == k_invalid_rigidbody_id)
    {
        return nullptr;
    }

    return CurrentPhysicsScene();
}

void RigidBodyComponent::SetActorType(RigidBodyActorType actor_type)
{
    m_RigidbodyRes.SetActorType(actor_type);

    std::shared_ptr<PhysicsScene> physics_scene = GetActivePhysicsScene();
    if (physics_scene)
    {
        physics_scene->SetRigidBodyMotionType(m_RigidbodyId, actor_type);
    }
}

Vector3 RigidBodyComponent::GetLinearVelocity() const
{
    std::shared_ptr<PhysicsScene> physics_scene = GetActivePhysicsScene();
    if (!physics_scene)
    {
        return Vector3::ZERO;
    }

    Vector3 velocity;
    return physics_scene->GetLinearVelocity(m_RigidbodyId, velocity) ? velocity : Vector3::ZERO;
}

void RigidBodyComponent::SetLinearVelocity(const Vector3& velocity)
{
    std::shared_ptr<PhysicsScene> physics_scene = GetActivePhysicsScene();
    if (physics_scene)
    {
        physics_scene->SetLinearVelocity(m_RigidbodyId, velocity);
    }
}

Vector3 RigidBodyComponent::GetAngularVelocity() const
{
    std::shared_ptr<PhysicsScene> physics_scene = GetActivePhysicsScene();
    if (!physics_scene)
    {
        return Vector3::ZERO;
    }

    Vector3 velocity;
    return physics_scene->GetAngularVelocity(m_RigidbodyId, velocity) ? velocity : Vector3::ZERO;
}

void RigidBodyComponent::SetAngularVelocity(const Vector3& velocity)
{
    std::shared_ptr<PhysicsScene> physics_scene = GetActivePhysicsScene();
    if (physics_scene)
    {
        physics_scene->SetAngularVelocity(m_RigidbodyId, velocity);
    }
}

void RigidBodyComponent::AddForce(const Vector3& force)
{
    std::shared_ptr<PhysicsScene> physics_scene = GetActivePhysicsScene();
    if (physics_scene)
    {
        physics_scene->AddForce(m_RigidbodyId, force);
    }
}

void RigidBodyComponent::AddImpulse(const Vector3& impulse)
{
    std::shared_ptr<PhysicsScene> physics_scene = GetActivePhysicsScene();
    if (physics_scene)
    {
        physics_scene->AddImpulse(m_RigidbodyId, impulse);
    }
}

float RigidBodyComponent::GetMass() const
{
    // Prefer the live body: it is what actually resists a force. Fall back to
    // the authored value while the body is still pending.
    std::shared_ptr<PhysicsScene> physics_scene = GetActivePhysicsScene();
    if (physics_scene)
    {
        float inverse_mass = 0.f;
        if (physics_scene->GetInverseMass(m_RigidbodyId, inverse_mass) && inverse_mass > 0.f)
        {
            return 1.f / inverse_mass;
        }
    }

    const float authored_inverse_mass = m_RigidbodyRes.m_InverseMass;
    return authored_inverse_mass > 0.f ? 1.f / authored_inverse_mass : 0.f;
}

Vector3 RigidBodyComponent::GetBodyPosition() const
{
    std::shared_ptr<PhysicsScene> physics_scene = GetActivePhysicsScene();
    if (!physics_scene)
    {
        return Vector3::ZERO;
    }

    Vector3 position;
    return physics_scene->GetRigidBodyPosition(m_RigidbodyId, position) ? position : Vector3::ZERO;
}

Quaternion RigidBodyComponent::GetBodyRotation() const
{
    std::shared_ptr<PhysicsScene> physics_scene = GetActivePhysicsScene();
    if (!physics_scene)
    {
        return Quaternion::IDENTITY;
    }

    Quaternion rotation;
    return physics_scene->GetRigidBodyRotation(m_RigidbodyId, rotation) ? rotation : Quaternion::IDENTITY;
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
        std::shared_ptr<PhysicsScene> physics_scene = CurrentPhysicsScene();
        ASSERT(physics_scene);

        physics_scene->UpdateRigidBodyGlobalTransform(m_RigidbodyId, transform);
    }
}

void RigidBodyComponent::GetShapeBoundingBoxes(std::vector<AxisAlignedBox>& out_bounding_boxes) const
{
    std::shared_ptr<PhysicsScene> physics_scene = CurrentPhysicsScene();
    ASSERT(physics_scene);

    physics_scene->GetShapeBoundingBoxes(m_RigidbodyId, out_bounding_boxes);
}