#include "SliderJointComponent.h"

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Function/Physics/PhysicsScene.h"

PhysicsJointType SliderJointComponent::GetJointType() const
{
    return PhysicsJointType::slider;
}

void SliderJointComponent::SetUseLimits(bool use_limits)
{
    if (m_JointRes.m_UseLimits == use_limits)
    {
        return;
    }
    m_JointRes.m_UseLimits = use_limits;
    RebuildJoint();
}

JointLimits SliderJointComponent::GetLimits() const
{
    JointLimits limits;
    limits.min = m_JointRes.m_MinLimit;
    limits.max = m_JointRes.m_MaxLimit;
    return limits;
}

void SliderJointComponent::SetLimits(const JointLimits& limits)
{
    // 与铰链同一条：`CreateJoint` 的 slider 分支要求 `min < 0 < max`（滑轨的
    // 限位是"相对建立姿态能左右滑多少"，不是任意区间）。脚本写
    // `{min: 0, max: 5}` 想说的是"往正方向最多 5 米"，原样收下即可。
    m_JointRes.m_MinLimit = limits.min > 0.f ? 0.f : limits.min;
    m_JointRes.m_MaxLimit = limits.max < 0.f ? 0.f : limits.max;

    if (m_JointRes.m_UseLimits)
    {
        RebuildJoint();
    }
}

void SliderJointComponent::SetUseMotor(bool use_motor)
{
    m_JointRes.m_UseMotor = use_motor;
    PushMotor();
}

void SliderJointComponent::SetMotorTargetPosition(float target_position)
{
    m_JointRes.m_MotorTargetPosition = target_position;
    PushMotor();
}

void SliderJointComponent::SetMotorForce(float force)
{
    m_JointRes.m_MotorForce = force < 0.f ? 0.f : force;
    PushMotor();
}

void SliderJointComponent::ApplyJointSettings()
{
    PushMotor();
}

void SliderJointComponent::FillJointLimits(PhysicsJointDesc& out_desc)
{
    if (!m_JointRes.m_UseLimits)
    {
        return;
    }
    out_desc.m_MinLimit = m_JointRes.m_MinLimit;
    out_desc.m_MaxLimit = m_JointRes.m_MaxLimit;
}

void SliderJointComponent::PushMotor()
{
    std::shared_ptr<PhysicsScene> physics_scene = GetActivePhysicsScene();
    if (!physics_scene || m_JointId == kInvalidJointId)
    {
        return;
    }

    // 滑轨的电机天然是**位置**电机（"走到这个位置"），所以直接走
    // `SetJointMotor` —— 它给 slider 就是 Position 状态。
    physics_scene->SetJointMotor(m_JointId,
                                 m_JointRes.m_UseMotor,
                                 m_JointRes.m_MotorTargetPosition,
                                 m_JointRes.m_MotorForce);
}
