#include "CharacterJointComponent.h"

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Function/Physics/PhysicsScene.h"

PhysicsJointType CharacterJointComponent::GetJointType() const
{
    return PhysicsJointType::swing_twist;
}

// -----------------------------------------------------------------------------
// 限位
// -----------------------------------------------------------------------------

bool CharacterJointComponent::SetSwing1Limit(float half_angle)
{
    return SetTwistSwingLimits(half_angle,
                               m_JointRes.m_Swing2Limit,
                               m_JointRes.m_LowTwistLimit,
                               m_JointRes.m_HighTwistLimit);
}

bool CharacterJointComponent::SetSwing2Limit(float half_angle)
{
    return SetTwistSwingLimits(m_JointRes.m_Swing1Limit,
                               half_angle,
                               m_JointRes.m_LowTwistLimit,
                               m_JointRes.m_HighTwistLimit);
}

bool CharacterJointComponent::SetLowTwistLimit(float angle)
{
    return SetTwistSwingLimits(m_JointRes.m_Swing1Limit,
                               m_JointRes.m_Swing2Limit,
                               angle,
                               m_JointRes.m_HighTwistLimit);
}

bool CharacterJointComponent::SetHighTwistLimit(float angle)
{
    return SetTwistSwingLimits(m_JointRes.m_Swing1Limit,
                               m_JointRes.m_Swing2Limit,
                               m_JointRes.m_LowTwistLimit,
                               angle);
}

bool CharacterJointComponent::SetTwistSwingLimits(float swing1, float swing2, float low_twist, float high_twist)
{
    // 先判再存：存一半再判会把 res 留在"脚本没写过的"状态上，而 getter 读到的
    // 又是那个旧值 —— 于是脚本看到"我设了，读回来还是原来的"，却没有任何报错。
    if (swing1 < 0.f || swing2 < 0.f)
    {
        LOG_ERROR(ZPhysics, "CharacterJoint: swing half cone angles must be >= 0 ({} / {})", swing1, swing2);
        return false;
    }
    // 扭转区间必须跨 0：Jolt 的 twist 与铰链的 limit 同义（"相对建立姿态左右各
    // 多少"），不是一个任意区间。
    if (low_twist > 0.f || high_twist < 0.f)
    {
        LOG_ERROR(ZPhysics, "CharacterJoint: twist range must straddle zero ({} .. {})", low_twist, high_twist);
        return false;
    }

    m_JointRes.m_Swing1Limit = swing1;
    m_JointRes.m_Swing2Limit = swing2;
    m_JointRes.m_LowTwistLimit = low_twist;
    m_JointRes.m_HighTwistLimit = high_twist;

    PushLimits();
    return true;
}

// -----------------------------------------------------------------------------
// 摆动参考轴
// -----------------------------------------------------------------------------

void CharacterJointComponent::SetSwingAxis(const Vector3& swing_axis)
{
    if (m_JointRes.m_SwingAxis == swing_axis)
    {
        return;
    }
    m_JointRes.m_SwingAxis = swing_axis;
    // 与 `JointComponent::SetAxis` 同理：轴烘在约束的参考系里，改了只能重建。
    RebuildJoint();
}

// -----------------------------------------------------------------------------
// 限位弹簧
// -----------------------------------------------------------------------------

SoftJointLimitSpring CharacterJointComponent::GetTwistLimitSpring() const
{
    SoftJointLimitSpring spring;
    spring.spring = m_JointRes.m_TwistSpring;
    spring.damper = m_JointRes.m_TwistDamper;
    return spring;
}

bool CharacterJointComponent::SetTwistLimitSpring(const SoftJointLimitSpring& spring)
{
    if (spring.spring < 0.f || spring.damper < 0.f)
    {
        return false;
    }
    m_JointRes.m_TwistSpring = spring.spring;
    m_JointRes.m_TwistDamper = spring.damper;
    PushLimits();
    return true;
}

SoftJointLimitSpring CharacterJointComponent::GetSwingLimitSpring() const
{
    SoftJointLimitSpring spring;
    spring.spring = m_JointRes.m_SwingSpring;
    spring.damper = m_JointRes.m_SwingDamper;
    return spring;
}

bool CharacterJointComponent::SetSwingLimitSpring(const SoftJointLimitSpring& spring)
{
    if (spring.spring < 0.f || spring.damper < 0.f)
    {
        return false;
    }
    m_JointRes.m_SwingSpring = spring.spring;
    m_JointRes.m_SwingDamper = spring.damper;
    PushLimits();
    return true;
}

// -----------------------------------------------------------------------------

void CharacterJointComponent::ApplyJointSettings()
{
    PushLimits();
}

void CharacterJointComponent::FillJointLimits(PhysicsJointDesc& out_desc)
{
    out_desc.m_Swing1HalfAngle = m_JointRes.m_Swing1Limit;
    out_desc.m_Swing2HalfAngle = m_JointRes.m_Swing2Limit;
    out_desc.m_TwistMinAngle = m_JointRes.m_LowTwistLimit;
    out_desc.m_TwistMaxAngle = m_JointRes.m_HighTwistLimit;
    out_desc.m_SwingSpringFrequency = m_JointRes.m_SwingSpring;
    out_desc.m_SwingSpringDamping = m_JointRes.m_SwingDamper;
    out_desc.m_TwistSpringFrequency = m_JointRes.m_TwistSpring;
    out_desc.m_TwistSpringDamping = m_JointRes.m_TwistDamper;
}

void CharacterJointComponent::PushLimits()
{
    std::shared_ptr<PhysicsScene> physics_scene = GetActivePhysicsScene();
    if (!physics_scene || m_JointId == kInvalidJointId)
    {
        return;
    }

    physics_scene->SetJointSwingTwistLimits(m_JointId,
                                            m_JointRes.m_Swing1Limit,
                                            m_JointRes.m_Swing2Limit,
                                            m_JointRes.m_LowTwistLimit,
                                            m_JointRes.m_HighTwistLimit);
    physics_scene->SetJointSwingTwistSpring(m_JointId,
                                            m_JointRes.m_SwingSpring,
                                            m_JointRes.m_SwingDamper,
                                            m_JointRes.m_TwistSpring,
                                            m_JointRes.m_TwistDamper);
}
