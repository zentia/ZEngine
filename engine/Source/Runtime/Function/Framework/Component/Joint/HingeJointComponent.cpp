#include "HingeJointComponent.h"

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Function/Physics/PhysicsScene.h"

#include <algorithm>

namespace
{
    /// 弹簧刚度的下限。`SetJointPositionMotor` 拒掉 frequency <= 0，而脚本写
    /// `spring = 0` 的意思是"我要弹簧，只是很软"，不是"关掉"—— 这里夹到一个
    /// 明显很软但仍能驱动的值。
    constexpr float kMinSpringFrequency = 0.05f;
}  // namespace

PhysicsJointType HingeJointComponent::GetJointType() const
{
    return PhysicsJointType::hinge;
}

float HingeJointComponent::GetAngle() const
{
    std::shared_ptr<PhysicsScene> physics_scene = GetActivePhysicsScene();
    if (!physics_scene || m_JointId == kInvalidJointId)
    {
        return 0.f;
    }

    float angle = 0.f;
    return physics_scene->GetJointAngle(m_JointId, angle) ? angle : 0.f;
}

float HingeJointComponent::GetVelocity() const
{
    std::shared_ptr<PhysicsScene> physics_scene = GetActivePhysicsScene();
    if (!physics_scene || m_JointId == kInvalidJointId)
    {
        return 0.f;
    }

    float velocity = 0.f;
    return physics_scene->GetJointVelocity(m_JointId, velocity) ? velocity : 0.f;
}

void HingeJointComponent::SetUseLimits(bool use_limits)
{
    if (m_JointRes.m_UseLimits == use_limits)
    {
        return;
    }
    m_JointRes.m_UseLimits = use_limits;
    // 限位烘在约束里，只能重建（见 `FillJointLimits` 的注释）。
    RebuildJoint();
}

JointLimits HingeJointComponent::GetLimits() const
{
    JointLimits limits;
    limits.min = m_JointRes.m_MinLimit;
    limits.max = m_JointRes.m_MaxLimit;
    return limits;
}

void HingeJointComponent::SetLimits(const JointLimits& limits)
{
    // Jolt 断言 `min <= 0 <= max`：铰链限位是"相对建立姿态的左右各多少"，不是
    // 一个任意区间。这里夹而不是拒 —— 脚本写 `{min: 10, max: 90}` 想说的是
    // "往正方向最多 90"，夹成 `{min: 0, max: 90}` 正是那个意思。
    m_JointRes.m_MinLimit = limits.min > 0.f ? 0.f : limits.min;
    m_JointRes.m_MaxLimit = limits.max < 0.f ? 0.f : limits.max;

    if (m_JointRes.m_UseLimits)
    {
        RebuildJoint();
    }
}

void HingeJointComponent::SetUseMotor(bool use_motor)
{
    m_JointRes.m_UseMotor = use_motor;
    PushMotor();
}

JointMotor HingeJointComponent::GetMotor() const
{
    JointMotor motor;
    motor.targetVelocity = m_JointRes.m_MotorTargetVelocity;
    motor.force = m_JointRes.m_MotorForce;
    return motor;
}

void HingeJointComponent::SetMotor(const JointMotor& motor)
{
    m_JointRes.m_MotorTargetVelocity = motor.targetVelocity;
    // 力矩为负没有意义（`SetTorqueLimit` 给的是对称区间），夹到 0 —— 那就是
    // "电机使不上劲"，而"关掉电机"是 `useMotor = false` 那件事。
    m_JointRes.m_MotorForce = motor.force < 0.f ? 0.f : motor.force;
    PushMotor();
}

void HingeJointComponent::SetUseSpring(bool use_spring)
{
    m_JointRes.m_UseSpring = use_spring;
    PushMotor();
}

JointSpring HingeJointComponent::GetSpring() const
{
    JointSpring spring;
    spring.spring = m_JointRes.m_Spring;
    spring.damper = m_JointRes.m_Damper;
    spring.targetPosition = m_JointRes.m_SpringTargetPosition;
    return spring;
}

void HingeJointComponent::SetSpring(const JointSpring& spring)
{
    m_JointRes.m_Spring = spring.spring;
    m_JointRes.m_Damper = spring.damper;
    m_JointRes.m_SpringTargetPosition = spring.targetPosition;
    PushMotor();
}

void HingeJointComponent::ApplyJointSettings()
{
    // 限位不在这里：它们已经在 `FillJointLimits` 里烘进约束了。
    PushMotor();
}

void HingeJointComponent::FillJointLimits(PhysicsJointDesc& out_desc)
{
    if (!m_JointRes.m_UseLimits)
    {
        return;
    }
    out_desc.m_MinLimit = m_JointRes.m_MinLimit;
    out_desc.m_MaxLimit = m_JointRes.m_MaxLimit;
}

void HingeJointComponent::PushMotor()
{
    std::shared_ptr<PhysicsScene> physics_scene = GetActivePhysicsScene();
    if (!physics_scene || m_JointId == kInvalidJointId)
    {
        return;
    }

    if (m_JointRes.m_UseSpring)
    {
        // 弹簧优先（Unity 同）：位置电机，朝 `targetPosition` 拉。
        physics_scene->SetJointPositionMotor(m_JointId,
                                            true,
                                            m_JointRes.m_SpringTargetPosition,
                                            m_JointRes.m_MotorForce,
                                            std::max(m_JointRes.m_Spring, kMinSpringFrequency),
                                            std::max(m_JointRes.m_Damper, 0.f));
        return;
    }

    if (m_JointRes.m_UseMotor)
    {
        physics_scene->SetJointMotor(m_JointId,
                                     true,
                                     m_JointRes.m_MotorTargetVelocity,
                                     m_JointRes.m_MotorForce);
        return;
    }

    // 两个都关：速度电机 Off。（弹簧那一侧没有独立的开关，位置电机关了之后
    // `mLimitsSpringSettings` 仍留着上次的值，但它只在有软限位时才起作用。）
    physics_scene->SetJointMotor(m_JointId, false, 0.f, 0.f);
}
