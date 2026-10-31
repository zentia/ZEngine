#include "RigidbodyComponent.h"

#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Function/Framework/Component/Transform/Transform.h"
#include "Runtime/Function/Framework/World/WorldManager.h"
#include "Runtime/Function/Physics/PhysicsManager.h"
#include "Runtime/Function/Physics/PhysicsScene.h"

namespace
{
    /// MovePosition / MoveRotation 用位移 / delta_time 反推速度，所以需要一个
    /// 步长。取 1/60 —— 与物理的固定步长一致：传一个和真实步长不符的值会让
    /// 运动学物体每帧走得或快或慢（Jolt 按这个速度积分一整步）。
    constexpr float kMoveStepSeconds = 1.0f / 60.0f;

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

    // 注销**先于**场景检查：关卡拆除时场景可能已经没了，但归属表里那条
    // 记录会留着指向这个刚析构的组件所属的 GameObject。留着它的后果是
    // 下一次有 body 复用这个 id 时，接触事件派发给一个已销毁的对象。
    UnregisterOwner();

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
    RegisterOwner();
}

void RigidBodyComponent::RegisterOwner()
{
    // 没有归属表，接触事件里的 body id 就翻译不成 GameObject，脚本侧的
    // `OnCollisionEnter(other)` 里的 other 无从填起。
    auto* manager = GET_SYSTEM(PhysicsManager);
    if (manager == nullptr || m_RigidbodyId == k_invalid_rigidbody_id)
    {
        return;
    }
    manager->RegisterBodyOwner(m_RigidbodyId, m_ParentObject);
}

void RigidBodyComponent::UnregisterOwner()
{
    if (auto* manager = GET_SYSTEM(PhysicsManager))
    {
        manager->UnregisterBodyOwner(m_RigidbodyId);
    }
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

    UnregisterOwner();
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

void RigidBodyComponent::SetMass(float mass)
{
    std::shared_ptr<PhysicsScene> physics_scene = GetActivePhysicsScene();
    if (physics_scene)
    {
        physics_scene->SetMass(m_RigidbodyId, mass);
    }
    // 同时写回序列化字段：只改刚体的话，存盘再读回来质量就变回作者设的那个
    // 值，而没有任何警告。活体优先（模拟出来的才是真实使用的），但存档必须跟上。
    m_RigidbodyRes.m_InverseMass = (mass > 0.f) ? (1.f / mass) : 0.f;
}

float RigidBodyComponent::GetDrag() const
{
    std::shared_ptr<PhysicsScene> physics_scene = GetActivePhysicsScene();
    if (!physics_scene)
    {
        return 0.f;
    }
    float damping = 0.f;
    return physics_scene->GetLinearDamping(m_RigidbodyId, damping) ? damping : 0.f;
}

void RigidBodyComponent::SetDrag(float drag)
{
    std::shared_ptr<PhysicsScene> physics_scene = GetActivePhysicsScene();
    if (physics_scene)
    {
        physics_scene->SetLinearDamping(m_RigidbodyId, drag);
    }
}

float RigidBodyComponent::GetAngularDrag() const
{
    std::shared_ptr<PhysicsScene> physics_scene = GetActivePhysicsScene();
    if (!physics_scene)
    {
        return 0.f;
    }
    float damping = 0.f;
    return physics_scene->GetAngularDamping(m_RigidbodyId, damping) ? damping : 0.f;
}

void RigidBodyComponent::SetAngularDrag(float angular_drag)
{
    std::shared_ptr<PhysicsScene> physics_scene = GetActivePhysicsScene();
    if (physics_scene)
    {
        physics_scene->SetAngularDamping(m_RigidbodyId, angular_drag);
    }
}

void RigidBodyComponent::SetFriction(float friction)
{
    // 负值没有物理意义，且 Jolt 明确要求"合成后的摩擦不得为负"。夹到 0 而不是
    // 断言：脚本里 `friction = -0.1` 更可能是笔误，而一个被夹住的手感比一个在
    // Debug 下炸掉、Release 下静默的断言好诊断。
    m_RigidbodyRes.m_Friction = (friction > 0.f) ? friction : 0.f;

    std::shared_ptr<PhysicsScene> physics_scene = GetActivePhysicsScene();
    if (physics_scene)
    {
        physics_scene->SetBodyFriction(m_RigidbodyId, m_RigidbodyRes.m_Friction);
    }
}

void RigidBodyComponent::SetBounciness(float bounciness)
{
    // Unity 的 bounciness 是 0..1（1 已经有能量近似误差了，再大会真的加能量）。
    m_RigidbodyRes.m_Bounciness = (bounciness > 0.f) ? ((bounciness < 1.f) ? bounciness : 1.f) : 0.f;

    std::shared_ptr<PhysicsScene> physics_scene = GetActivePhysicsScene();
    if (physics_scene)
    {
        physics_scene->SetBodyRestitution(m_RigidbodyId, m_RigidbodyRes.m_Bounciness);
    }
}

void RigidBodyComponent::SetFrictionCombine(PhysicMaterialCombine combine_mode)
{
    m_RigidbodyRes.SetFrictionCombine(combine_mode);

    std::shared_ptr<PhysicsScene> physics_scene = GetActivePhysicsScene();
    if (physics_scene)
    {
        // 两个模式一起写：user data 里两个都存，分开写会让"改了摩擦组合、弹性
        // 组合被悄悄重置"这种事发生。
        physics_scene->SetBodyCombineModes(m_RigidbodyId,
                                           m_RigidbodyRes.GetFrictionCombine(),
                                           m_RigidbodyRes.GetBounceCombine());
    }
}

void RigidBodyComponent::SetBounceCombine(PhysicMaterialCombine combine_mode)
{
    m_RigidbodyRes.SetBounceCombine(combine_mode);

    std::shared_ptr<PhysicsScene> physics_scene = GetActivePhysicsScene();
    if (physics_scene)
    {
        physics_scene->SetBodyCombineModes(m_RigidbodyId,
                                           m_RigidbodyRes.GetFrictionCombine(),
                                           m_RigidbodyRes.GetBounceCombine());
    }
}

bool RigidBodyComponent::GetUseGravity() const
{
    std::shared_ptr<PhysicsScene> physics_scene = GetActivePhysicsScene();
    if (!physics_scene)
    {
        return true;
    }
    float factor = 1.f;
    return physics_scene->GetGravityFactor(m_RigidbodyId, factor) ? (factor > 0.f) : true;
}

void RigidBodyComponent::SetUseGravity(bool use_gravity)
{
    std::shared_ptr<PhysicsScene> physics_scene = GetActivePhysicsScene();
    if (physics_scene)
    {
        physics_scene->SetGravityFactor(m_RigidbodyId, use_gravity ? 1.f : 0.f);
    }
}

bool RigidBodyComponent::GetIsKinematic() const
{
    return GetActorType() == RigidBodyActorType::kinematic;
}

void RigidBodyComponent::SetIsKinematic(bool is_kinematic)
{
    SetActorType(is_kinematic ? RigidBodyActorType::kinematic : RigidBodyActorType::dynamic);
}

void RigidBodyComponent::AddTorque(const Vector3& torque)
{
    std::shared_ptr<PhysicsScene> physics_scene = GetActivePhysicsScene();
    if (physics_scene)
    {
        physics_scene->AddTorque(m_RigidbodyId, torque);
    }
}

void RigidBodyComponent::AddAngularImpulse(const Vector3& angular_impulse)
{
    std::shared_ptr<PhysicsScene> physics_scene = GetActivePhysicsScene();
    if (physics_scene)
    {
        physics_scene->AddAngularImpulse(m_RigidbodyId, angular_impulse);
    }
}

void RigidBodyComponent::AddForceAtPosition(const Vector3& force, const Vector3& position)
{
    std::shared_ptr<PhysicsScene> physics_scene = GetActivePhysicsScene();
    if (physics_scene)
    {
        physics_scene->AddForceAtPosition(m_RigidbodyId, force, position);
    }
}

void RigidBodyComponent::AddImpulseAtPosition(const Vector3& impulse, const Vector3& position)
{
    std::shared_ptr<PhysicsScene> physics_scene = GetActivePhysicsScene();
    if (physics_scene)
    {
        physics_scene->AddImpulseAtPosition(m_RigidbodyId, impulse, position);
    }
}

void RigidBodyComponent::AddExplosionForce(float explosion_force,
                                           const Vector3& explosion_position,
                                           float explosion_radius)
{
    if (explosion_radius <= 0.f)
    {
        // 半径为 0 / 负的爆炸没有任何物体可作用。直接返回而不是照着公式算出
        // inf —— 除零会从这里一路传成 NaN 位置。
        return;
    }

    const Vector3 body_position = GetPosition();
    Vector3 direction = body_position - explosion_position;
    const float distance = direction.length();
    if (distance >= explosion_radius)
    {
        // 半径外不受力（Unity 同）：这是"手雷在墙那边"和"手雷在脚下"的分界。
        return;
    }
    if (distance < 1.0e-5f)
    {
        // 正好炸在质心上：方向没有定义。给一个确定的方向（引擎的 up）而不是
        // 归一化零向量（那会得到 NaN 并污染整个刚体的速度）。
        direction = Vector3::UNIT_Z;
    }
    else
    {
        direction /= distance;
    }

    const float falloff = 1.f - (distance / explosion_radius);
    AddForceAtPosition(direction * (explosion_force * falloff), body_position);
}

Vector3 RigidBodyComponent::GetPosition() const
{
    return GetBodyPosition();
}

Quaternion RigidBodyComponent::GetRotation() const
{
    return GetBodyRotation();
}

void RigidBodyComponent::SetPosition(const Vector3& position)
{
    std::shared_ptr<PhysicsScene> physics_scene = GetActivePhysicsScene();
    if (physics_scene)
    {
        physics_scene->SetRigidBodyPosition(m_RigidbodyId, position);
    }
}

void RigidBodyComponent::SetRotation(const Quaternion& rotation)
{
    std::shared_ptr<PhysicsScene> physics_scene = GetActivePhysicsScene();
    if (physics_scene)
    {
        physics_scene->SetRigidBodyRotation(m_RigidbodyId, rotation);
    }
}

void RigidBodyComponent::MovePosition(const Vector3& position)
{
    std::shared_ptr<PhysicsScene> physics_scene = GetActivePhysicsScene();
    if (physics_scene)
    {
        // 只动位置、保持当前朝向：Unity 的 MovePosition 也是这个语义（"把物体
        // 移到这"，不是"把物体摆成这个位姿"）。
        physics_scene->MoveKinematic(m_RigidbodyId, position, GetRotation(), kMoveStepSeconds);
    }
}

void RigidBodyComponent::MoveRotation(const Quaternion& rotation)
{
    std::shared_ptr<PhysicsScene> physics_scene = GetActivePhysicsScene();
    if (physics_scene)
    {
        physics_scene->MoveKinematic(m_RigidbodyId, GetPosition(), rotation, kMoveStepSeconds);
    }
}

bool RigidBodyComponent::IsSleeping() const
{
    std::shared_ptr<PhysicsScene> physics_scene = GetActivePhysicsScene();
    if (!physics_scene)
    {
        // 没有刚体时"没在模拟"是真的 —— 它没有在动。返回 true 而不是 false，
        // 这样 "如果它睡着就叫醒它" 的逻辑不会漏。
        return true;
    }
    bool is_sleeping = false;
    return physics_scene->IsSleeping(m_RigidbodyId, is_sleeping) ? is_sleeping : true;
}

void RigidBodyComponent::Sleep()
{
    std::shared_ptr<PhysicsScene> physics_scene = GetActivePhysicsScene();
    if (physics_scene)
    {
        physics_scene->SleepRigidBody(m_RigidbodyId);
    }
}

void RigidBodyComponent::WakeUp()
{
    std::shared_ptr<PhysicsScene> physics_scene = GetActivePhysicsScene();
    if (physics_scene)
    {
        physics_scene->WakeRigidBody(m_RigidbodyId);
    }
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