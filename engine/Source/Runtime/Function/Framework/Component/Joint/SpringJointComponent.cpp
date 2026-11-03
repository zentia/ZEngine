#include "SpringJointComponent.h"

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Function/Physics/PhysicsScene.h"

PhysicsJointType SpringJointComponent::GetJointType() const
{
    return PhysicsJointType::distance;
}

void SpringJointComponent::SetSpring(float spring)
{
    m_JointRes.m_Spring = spring < 0.f ? 0.f : spring;

    std::shared_ptr<PhysicsScene> physics_scene = GetActivePhysicsScene();
    if (physics_scene && m_JointId != kInvalidJointId)
    {
        physics_scene->SetJointSpring(m_JointId, m_JointRes.m_Spring, m_JointRes.m_Damper);
    }
}

void SpringJointComponent::SetDamper(float damper)
{
    m_JointRes.m_Damper = damper < 0.f ? 0.f : damper;

    std::shared_ptr<PhysicsScene> physics_scene = GetActivePhysicsScene();
    if (physics_scene && m_JointId != kInvalidJointId)
    {
        physics_scene->SetJointSpring(m_JointId, m_JointRes.m_Spring, m_JointRes.m_Damper);
    }
}

float SpringJointComponent::GetMinDistance() const
{
    if (m_JointRes.m_MinDistance >= 0.f)
    {
        return m_JointRes.m_MinDistance;
    }

    // 还没设过、又是"自己推"：把引擎推出来的那个值读回来，这样脚本读到的永远
    // 是一个真在生效的数，而不是一个 -1。
    std::shared_ptr<PhysicsScene> physics_scene = GetActivePhysicsScene();
    if (!physics_scene || m_JointId == kInvalidJointId)
    {
        return m_JointRes.m_MinDistance;
    }

    float min_distance = 0.f;
    float max_distance = 0.f;
    return physics_scene->GetJointDistance(m_JointId, min_distance, max_distance)
               ? min_distance
               : m_JointRes.m_MinDistance;
}

void SpringJointComponent::SetMinDistance(float min_distance)
{
    m_JointRes.m_MinDistance = min_distance;

    std::shared_ptr<PhysicsScene> physics_scene = GetActivePhysicsScene();
    if (physics_scene && m_JointId != kInvalidJointId)
    {
        physics_scene->SetJointDistance(m_JointId, m_JointRes.m_MinDistance, m_JointRes.m_MaxDistance);
    }
}

float SpringJointComponent::GetMaxDistance() const
{
    if (m_JointRes.m_MaxDistance >= 0.f)
    {
        return m_JointRes.m_MaxDistance;
    }

    std::shared_ptr<PhysicsScene> physics_scene = GetActivePhysicsScene();
    if (!physics_scene || m_JointId == kInvalidJointId)
    {
        return m_JointRes.m_MaxDistance;
    }

    float min_distance = 0.f;
    float max_distance = 0.f;
    return physics_scene->GetJointDistance(m_JointId, min_distance, max_distance)
               ? max_distance
               : m_JointRes.m_MaxDistance;
}

void SpringJointComponent::SetMaxDistance(float max_distance)
{
    m_JointRes.m_MaxDistance = max_distance;

    std::shared_ptr<PhysicsScene> physics_scene = GetActivePhysicsScene();
    if (physics_scene && m_JointId != kInvalidJointId)
    {
        physics_scene->SetJointDistance(m_JointId, m_JointRes.m_MinDistance, m_JointRes.m_MaxDistance);
    }
}

void SpringJointComponent::ApplyJointSettings()
{
    std::shared_ptr<PhysicsScene> physics_scene = GetActivePhysicsScene();
    if (!physics_scene || m_JointId == kInvalidJointId)
    {
        return;
    }

    // 只在明确设过时才推距离：负值交给 `CreateJoint` 已经按当前姿态推好了，
    // 这里再推一次会传 -1 进去，把"保持当前距离"变成"距离是 -1 米"。
    if (m_JointRes.m_MinDistance >= 0.f && m_JointRes.m_MaxDistance >= 0.f)
    {
        physics_scene->SetJointDistance(m_JointId, m_JointRes.m_MinDistance, m_JointRes.m_MaxDistance);
    }

    physics_scene->SetJointSpring(m_JointId, m_JointRes.m_Spring, m_JointRes.m_Damper);
}
