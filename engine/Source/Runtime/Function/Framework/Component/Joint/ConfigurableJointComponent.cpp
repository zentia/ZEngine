#include "ConfigurableJointComponent.h"

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Function/Physics/PhysicsScene.h"

namespace
{
    /// 枚举是从 TS 的 number 来的，可以乱给 —— 越界值在 C++ 侧是一个合法的
    /// `ConfigurableJointMotion`，但没有任何一个 case 认它。
    bool IsValidMotion(ConfigurableJointMotion motion)
    {
        return motion == ConfigurableJointMotion::Free ||
               motion == ConfigurableJointMotion::Limited ||
               motion == ConfigurableJointMotion::Locked;
    }

    ConfigurableJointMotion MotionFromRes(uint8_t value)
    {
        return static_cast<ConfigurableJointMotion>(value);
    }
} // namespace

PhysicsJointType ConfigurableJointComponent::GetJointType() const
{
    return PhysicsJointType::configurable;
}

// -----------------------------------------------------------------------------
// 参考系
// -----------------------------------------------------------------------------

void ConfigurableJointComponent::SetSecondaryAxis(const Vector3& secondary_axis)
{
    if (m_JointRes.m_SecondaryAxis == secondary_axis)
    {
        return;
    }
    m_JointRes.m_SecondaryAxis = secondary_axis;
    // 与 `JointComponent::SetAxis` 同理：参考系是构造时烘进约束的，改了只能重建。
    RebuildJoint();
}

// -----------------------------------------------------------------------------
// 六个轴的运动模式
// -----------------------------------------------------------------------------

ConfigurableJointMotion ConfigurableJointComponent::GetXMotion() const
{
    return MotionFromRes(m_JointRes.m_MotionX);
}

bool ConfigurableJointComponent::SetXMotion(ConfigurableJointMotion motion)
{
    if (!IsValidMotion(motion))
    {
        return false;
    }
    float min = 0.f;
    float max = 0.f;
    TranslationLimits(min, max);
    if (!PushAxisMotion(ConfigurableJointAxis::TranslationX, motion, min, max))
    {
        return false;
    }
    m_JointRes.m_MotionX = static_cast<uint8_t>(motion);
    return true;
}

ConfigurableJointMotion ConfigurableJointComponent::GetYMotion() const
{
    return MotionFromRes(m_JointRes.m_MotionY);
}

bool ConfigurableJointComponent::SetYMotion(ConfigurableJointMotion motion)
{
    if (!IsValidMotion(motion))
    {
        return false;
    }
    float min = 0.f;
    float max = 0.f;
    TranslationLimits(min, max);
    if (!PushAxisMotion(ConfigurableJointAxis::TranslationY, motion, min, max))
    {
        return false;
    }
    m_JointRes.m_MotionY = static_cast<uint8_t>(motion);
    return true;
}

ConfigurableJointMotion ConfigurableJointComponent::GetZMotion() const
{
    return MotionFromRes(m_JointRes.m_MotionZ);
}

bool ConfigurableJointComponent::SetZMotion(ConfigurableJointMotion motion)
{
    if (!IsValidMotion(motion))
    {
        return false;
    }
    float min = 0.f;
    float max = 0.f;
    TranslationLimits(min, max);
    if (!PushAxisMotion(ConfigurableJointAxis::TranslationZ, motion, min, max))
    {
        return false;
    }
    m_JointRes.m_MotionZ = static_cast<uint8_t>(motion);
    return true;
}

ConfigurableJointMotion ConfigurableJointComponent::GetAngularXMotion() const
{
    return MotionFromRes(m_JointRes.m_AngularMotionX);
}

bool ConfigurableJointComponent::SetAngularXMotion(ConfigurableJointMotion motion)
{
    if (!IsValidMotion(motion))
    {
        return false;
    }
    float min = 0.f;
    float max = 0.f;
    RotationLimitFor(ConfigurableJointAxis::RotationX, min, max);
    if (!PushAxisMotion(ConfigurableJointAxis::RotationX, motion, min, max))
    {
        return false;
    }
    m_JointRes.m_AngularMotionX = static_cast<uint8_t>(motion);
    return true;
}

ConfigurableJointMotion ConfigurableJointComponent::GetAngularYMotion() const
{
    return MotionFromRes(m_JointRes.m_AngularMotionY);
}

bool ConfigurableJointComponent::SetAngularYMotion(ConfigurableJointMotion motion)
{
    if (!IsValidMotion(motion))
    {
        return false;
    }
    float min = 0.f;
    float max = 0.f;
    RotationLimitFor(ConfigurableJointAxis::RotationY, min, max);
    if (!PushAxisMotion(ConfigurableJointAxis::RotationY, motion, min, max))
    {
        return false;
    }
    m_JointRes.m_AngularMotionY = static_cast<uint8_t>(motion);
    return true;
}

ConfigurableJointMotion ConfigurableJointComponent::GetAngularZMotion() const
{
    return MotionFromRes(m_JointRes.m_AngularMotionZ);
}

bool ConfigurableJointComponent::SetAngularZMotion(ConfigurableJointMotion motion)
{
    if (!IsValidMotion(motion))
    {
        return false;
    }
    float min = 0.f;
    float max = 0.f;
    RotationLimitFor(ConfigurableJointAxis::RotationZ, min, max);
    if (!PushAxisMotion(ConfigurableJointAxis::RotationZ, motion, min, max))
    {
        return false;
    }
    m_JointRes.m_AngularMotionZ = static_cast<uint8_t>(motion);
    return true;
}

// -----------------------------------------------------------------------------
// 限位
// -----------------------------------------------------------------------------

bool ConfigurableJointComponent::SetLinearLimit(float limit)
{
    if (limit < 0.f)
    {
        LOG_ERROR(ZPhysics, "ConfigurableJoint: linearLimit must be >= 0 ({})", limit);
        return false;
    }
    m_JointRes.m_LinearLimit = limit;
    // ⚠️ 必须把三个平移轴一起重推：`linearLimit` 是三轴共用的，只推当前这一个
    // 的话另外两个轴还留着旧区间 —— 而它们读回来还是对的（res 只有一份），于是
    // "改了但只有一轴生效"完全看不出来。
    PushAllMotions();
    return true;
}

bool ConfigurableJointComponent::SetAngularXLimits(float low, float high)
{
    // 与 CharacterJoint 的扭转区间同一条规则：X 的区间必须跨 0（"相对建立姿态
    // 左右各多少"），不是一个任意区间。
    if (low > 0.f || high < 0.f)
    {
        LOG_ERROR(ZPhysics, "ConfigurableJoint: angular X limit must straddle zero ({} .. {})", low, high);
        return false;
    }
    m_JointRes.m_LowAngularXLimit = low;
    m_JointRes.m_HighAngularXLimit = high;
    PushAllMotions();
    return true;
}

bool ConfigurableJointComponent::SetAngularYLimit(float angle)
{
    if (angle < 0.f)
    {
        LOG_ERROR(ZPhysics, "ConfigurableJoint: angular Y limit must be >= 0 ({})", angle);
        return false;
    }
    m_JointRes.m_AngularYLimit = angle;
    PushAllMotions();
    return true;
}

bool ConfigurableJointComponent::SetAngularZLimit(float angle)
{
    if (angle < 0.f)
    {
        LOG_ERROR(ZPhysics, "ConfigurableJoint: angular Z limit must be >= 0 ({})", angle);
        return false;
    }
    m_JointRes.m_AngularZLimit = angle;
    PushAllMotions();
    return true;
}

// -----------------------------------------------------------------------------
// 线性限位弹簧
// -----------------------------------------------------------------------------

SoftJointLimitSpring ConfigurableJointComponent::GetLinearLimitSpring() const
{
    SoftJointLimitSpring spring;
    spring.spring = m_JointRes.m_LinearLimitSpring;
    spring.damper = m_JointRes.m_LinearLimitDamper;
    return spring;
}

bool ConfigurableJointComponent::SetLinearLimitSpring(const SoftJointLimitSpring& spring)
{
    if (spring.spring < 0.f || spring.damper < 0.f)
    {
        return false;
    }
    m_JointRes.m_LinearLimitSpring = spring.spring;
    m_JointRes.m_LinearLimitDamper = spring.damper;
    PushLinearLimitSpring();
    return true;
}

// -----------------------------------------------------------------------------
// 驱动
// -----------------------------------------------------------------------------

JointDrive ConfigurableJointComponent::GetXDrive() const
{
    JointDrive drive;
    drive.positionSpring = m_JointRes.m_XDriveSpring;
    drive.positionDamper = m_JointRes.m_XDriveDamper;
    drive.maximumForce = m_JointRes.m_XDriveForce;
    return drive;
}

bool ConfigurableJointComponent::SetXDrive(const JointDrive& drive)
{
    return CommitDrive(ConfigurableJointAxis::TranslationX, drive);
}

JointDrive ConfigurableJointComponent::GetYDrive() const
{
    JointDrive drive;
    drive.positionSpring = m_JointRes.m_YDriveSpring;
    drive.positionDamper = m_JointRes.m_YDriveDamper;
    drive.maximumForce = m_JointRes.m_YDriveForce;
    return drive;
}

bool ConfigurableJointComponent::SetYDrive(const JointDrive& drive)
{
    return CommitDrive(ConfigurableJointAxis::TranslationY, drive);
}

JointDrive ConfigurableJointComponent::GetZDrive() const
{
    JointDrive drive;
    drive.positionSpring = m_JointRes.m_ZDriveSpring;
    drive.positionDamper = m_JointRes.m_ZDriveDamper;
    drive.maximumForce = m_JointRes.m_ZDriveForce;
    return drive;
}

bool ConfigurableJointComponent::SetZDrive(const JointDrive& drive)
{
    return CommitDrive(ConfigurableJointAxis::TranslationZ, drive);
}

JointDrive ConfigurableJointComponent::GetAngularXDrive() const
{
    JointDrive drive;
    drive.positionSpring = m_JointRes.m_AngularXDriveSpring;
    drive.positionDamper = m_JointRes.m_AngularXDriveDamper;
    drive.maximumForce = m_JointRes.m_AngularXDriveForce;
    return drive;
}

bool ConfigurableJointComponent::SetAngularXDrive(const JointDrive& drive)
{
    return CommitDrive(ConfigurableJointAxis::RotationX, drive);
}

JointDrive ConfigurableJointComponent::GetAngularYZDrive() const
{
    JointDrive drive;
    drive.positionSpring = m_JointRes.m_AngularYZDriveSpring;
    drive.positionDamper = m_JointRes.m_AngularYZDriveDamper;
    drive.maximumForce = m_JointRes.m_AngularYZDriveForce;
    return drive;
}

bool ConfigurableJointComponent::SetAngularYZDrive(const JointDrive& drive)
{
    // Y 与 Z 共用一份（Unity 的 `angularYZDrive` 就是这样）。两个都推，不推的话
    // "改了驱动"只有一轴见效 —— 而读回来还是对的（res 只有一份），看不出来。
    if (!CommitDrive(ConfigurableJointAxis::RotationY, drive))
    {
        return false;
    }
    return CommitDrive(ConfigurableJointAxis::RotationZ, drive);
}

// -----------------------------------------------------------------------------
// 目标
// -----------------------------------------------------------------------------

void ConfigurableJointComponent::SetTargetPosition(const Vector3& target_position)
{
    m_JointRes.m_TargetPosition = target_position;
    PushTargets();
}

void ConfigurableJointComponent::SetTargetVelocity(const Vector3& target_velocity)
{
    m_JointRes.m_TargetVelocity = target_velocity;
    PushTargets();
}

void ConfigurableJointComponent::SetTargetAngularVelocity(const Vector3& target_angular_velocity)
{
    m_JointRes.m_TargetAngularVelocity = target_angular_velocity;
    PushTargets();
}

void ConfigurableJointComponent::SetTargetRotation(const Quaternion& target_rotation)
{
    m_JointRes.m_TargetRotation = target_rotation;
    PushTargets();
}

// -----------------------------------------------------------------------------
// 内部
// -----------------------------------------------------------------------------

void ConfigurableJointComponent::TranslationLimits(float& out_min, float& out_max) const
{
    out_min = -m_JointRes.m_LinearLimit;
    out_max = m_JointRes.m_LinearLimit;
}

void ConfigurableJointComponent::RotationLimitFor(ConfigurableJointAxis axis, float& out_min, float& out_max) const
{
    switch (axis)
    {
        case ConfigurableJointAxis::RotationX:
            out_min = m_JointRes.m_LowAngularXLimit;
            out_max = m_JointRes.m_HighAngularXLimit;
            break;
        case ConfigurableJointAxis::RotationY:
            out_min = -m_JointRes.m_AngularYLimit;
            out_max = m_JointRes.m_AngularYLimit;
            break;
        case ConfigurableJointAxis::RotationZ:
            out_min = -m_JointRes.m_AngularZLimit;
            out_max = m_JointRes.m_AngularZLimit;
            break;
        default:
            out_min = 0.f;
            out_max = 0.f;
            break;
    }
}

bool ConfigurableJointComponent::PushAxisMotion(ConfigurableJointAxis axis,
                                                ConfigurableJointMotion motion,
                                                float min,
                                                float max)
{
    std::shared_ptr<PhysicsScene> physics_scene = GetActivePhysicsScene();
    if (!physics_scene || m_JointId == kInvalidJointId)
    {
        // 还没建出来：`FillJointLimits` 会在建的那一步带上这些值，这里不算失败。
        return true;
    }
    return physics_scene->SetJointAxisMotion(m_JointId, axis, motion, min, max);
}

void ConfigurableJointComponent::PushAllMotions()
{
    float translation_min = 0.f;
    float translation_max = 0.f;
    TranslationLimits(translation_min, translation_max);

    const ConfigurableJointAxis translation_axes[3] = {ConfigurableJointAxis::TranslationX,
                                                       ConfigurableJointAxis::TranslationY,
                                                       ConfigurableJointAxis::TranslationZ};
    const uint8_t translation_motions[3] = {m_JointRes.m_MotionX, m_JointRes.m_MotionY, m_JointRes.m_MotionZ};
    for (int i = 0; i < 3; ++i)
    {
        PushAxisMotion(translation_axes[i], MotionFromRes(translation_motions[i]), translation_min, translation_max);
    }

    const ConfigurableJointAxis rotation_axes[3] = {ConfigurableJointAxis::RotationX,
                                                    ConfigurableJointAxis::RotationY,
                                                    ConfigurableJointAxis::RotationZ};
    const uint8_t rotation_motions[3] = {m_JointRes.m_AngularMotionX,
                                         m_JointRes.m_AngularMotionY,
                                         m_JointRes.m_AngularMotionZ};
    for (int i = 0; i < 3; ++i)
    {
        float min = 0.f;
        float max = 0.f;
        RotationLimitFor(rotation_axes[i], min, max);
        PushAxisMotion(rotation_axes[i], MotionFromRes(rotation_motions[i]), min, max);
    }
}

void ConfigurableJointComponent::PushDrive(ConfigurableJointAxis axis, const JointDrive& drive)
{
    std::shared_ptr<PhysicsScene> physics_scene = GetActivePhysicsScene();
    if (!physics_scene || m_JointId == kInvalidJointId)
    {
        return;
    }
    physics_scene->SetJointAxisDrive(m_JointId, axis, drive);
}

bool ConfigurableJointComponent::CommitDrive(ConfigurableJointAxis axis, const JointDrive& drive)
{
    if (drive.positionSpring < 0.f || drive.positionDamper < 0.f || drive.maximumForce < 0.f)
    {
        LOG_ERROR(ZPhysics,
                  "ConfigurableJoint: drive needs spring / damper / maximumForce >= 0 ({} / {} / {})",
                  drive.positionSpring,
                  drive.positionDamper,
                  drive.maximumForce);
        return false;
    }

    PushDrive(axis, drive);

    switch (axis)
    {
        case ConfigurableJointAxis::TranslationX:
            m_JointRes.m_XDriveSpring = drive.positionSpring;
            m_JointRes.m_XDriveDamper = drive.positionDamper;
            m_JointRes.m_XDriveForce = drive.maximumForce;
            break;
        case ConfigurableJointAxis::TranslationY:
            m_JointRes.m_YDriveSpring = drive.positionSpring;
            m_JointRes.m_YDriveDamper = drive.positionDamper;
            m_JointRes.m_YDriveForce = drive.maximumForce;
            break;
        case ConfigurableJointAxis::TranslationZ:
            m_JointRes.m_ZDriveSpring = drive.positionSpring;
            m_JointRes.m_ZDriveDamper = drive.positionDamper;
            m_JointRes.m_ZDriveForce = drive.maximumForce;
            break;
        case ConfigurableJointAxis::RotationX:
            m_JointRes.m_AngularXDriveSpring = drive.positionSpring;
            m_JointRes.m_AngularXDriveDamper = drive.positionDamper;
            m_JointRes.m_AngularXDriveForce = drive.maximumForce;
            break;
        case ConfigurableJointAxis::RotationY:
        case ConfigurableJointAxis::RotationZ:
            m_JointRes.m_AngularYZDriveSpring = drive.positionSpring;
            m_JointRes.m_AngularYZDriveDamper = drive.positionDamper;
            m_JointRes.m_AngularYZDriveForce = drive.maximumForce;
            break;
        default:
            break;
    }
    return true;
}

void ConfigurableJointComponent::PushLinearLimitSpring()
{
    std::shared_ptr<PhysicsScene> physics_scene = GetActivePhysicsScene();
    if (!physics_scene || m_JointId == kInvalidJointId)
    {
        return;
    }
    physics_scene->SetJointLinearLimitSpring(m_JointId, m_JointRes.m_LinearLimitSpring, m_JointRes.m_LinearLimitDamper);
}

void ConfigurableJointComponent::PushTargets()
{
    std::shared_ptr<PhysicsScene> physics_scene = GetActivePhysicsScene();
    if (!physics_scene || m_JointId == kInvalidJointId)
    {
        return;
    }
    physics_scene->SetJointTarget(m_JointId,
                                  m_JointRes.m_TargetPosition,
                                  m_JointRes.m_TargetVelocity,
                                  m_JointRes.m_TargetAngularVelocity,
                                  m_JointRes.m_TargetRotation);
}

void ConfigurableJointComponent::ApplyJointSettings()
{
    PushAllMotions();
    PushDrive(ConfigurableJointAxis::TranslationX, GetXDrive());
    PushDrive(ConfigurableJointAxis::TranslationY, GetYDrive());
    PushDrive(ConfigurableJointAxis::TranslationZ, GetZDrive());
    PushDrive(ConfigurableJointAxis::RotationX, GetAngularXDrive());
    PushDrive(ConfigurableJointAxis::RotationY, GetAngularYZDrive());
    PushDrive(ConfigurableJointAxis::RotationZ, GetAngularYZDrive());
    PushLinearLimitSpring();
    PushTargets();
}

void ConfigurableJointComponent::FillJointLimits(PhysicsJointDesc& out_desc)
{
    float translation_min = 0.f;
    float translation_max = 0.f;
    TranslationLimits(translation_min, translation_max);

    const ConfigurableJointAxis translation_axes[3] = {ConfigurableJointAxis::TranslationX,
                                                       ConfigurableJointAxis::TranslationY,
                                                       ConfigurableJointAxis::TranslationZ};
    const uint8_t translation_motions[3] = {m_JointRes.m_MotionX, m_JointRes.m_MotionY, m_JointRes.m_MotionZ};
    for (int i = 0; i < 3; ++i)
    {
        const int index = static_cast<int>(translation_axes[i]);
        out_desc.m_Motion[index] = MotionFromRes(translation_motions[i]);
        out_desc.m_AxisLimitMin[index] = translation_min;
        out_desc.m_AxisLimitMax[index] = translation_max;
    }

    const ConfigurableJointAxis rotation_axes[3] = {ConfigurableJointAxis::RotationX,
                                                    ConfigurableJointAxis::RotationY,
                                                    ConfigurableJointAxis::RotationZ};
    const uint8_t rotation_motions[3] = {m_JointRes.m_AngularMotionX,
                                         m_JointRes.m_AngularMotionY,
                                         m_JointRes.m_AngularMotionZ};
    for (int i = 0; i < 3; ++i)
    {
        float min = 0.f;
        float max = 0.f;
        RotationLimitFor(rotation_axes[i], min, max);
        const int index = static_cast<int>(rotation_axes[i]);
        out_desc.m_Motion[index] = MotionFromRes(rotation_motions[i]);
        out_desc.m_AxisLimitMin[index] = min;
        out_desc.m_AxisLimitMax[index] = max;
    }

    out_desc.m_AxisDrive[static_cast<int>(ConfigurableJointAxis::TranslationX)] = GetXDrive();
    out_desc.m_AxisDrive[static_cast<int>(ConfigurableJointAxis::TranslationY)] = GetYDrive();
    out_desc.m_AxisDrive[static_cast<int>(ConfigurableJointAxis::TranslationZ)] = GetZDrive();
    out_desc.m_AxisDrive[static_cast<int>(ConfigurableJointAxis::RotationX)] = GetAngularXDrive();
    // Y 与 Z 共用 `angularYZDrive`。
    out_desc.m_AxisDrive[static_cast<int>(ConfigurableJointAxis::RotationY)] = GetAngularYZDrive();
    out_desc.m_AxisDrive[static_cast<int>(ConfigurableJointAxis::RotationZ)] = GetAngularYZDrive();

    out_desc.m_LinearLimitSpring = m_JointRes.m_LinearLimitSpring;
    out_desc.m_LinearLimitDamper = m_JointRes.m_LinearLimitDamper;

    out_desc.m_TargetPosition = m_JointRes.m_TargetPosition;
    out_desc.m_TargetVelocity = m_JointRes.m_TargetVelocity;
    out_desc.m_TargetAngularVelocity = m_JointRes.m_TargetAngularVelocity;
    out_desc.m_TargetRotation = m_JointRes.m_TargetRotation;
}
