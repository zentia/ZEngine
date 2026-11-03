#pragma once

#include "JointComponent.h"

#include "ConfigurableJointComponent.generated.h"

/// Unity 的 `ConfigurableJoint`：六个自由度**逐个**配模式（Free / Limited /
/// Locked）、逐轴配限位与驱动。Unity 那边的定位就是"其他关节的超集"，本引擎靠
/// Jolt 的 `SixDOFConstraint` 对应 —— 两者的轴模型一致：
/// `axis` = 关节参考系 X、`secondaryAxis` = Y、第三条取叉乘。
///
/// 与铰链的差别（也是本类最大的一点）：**六个轴的模式都可以在运行时改**，因为
/// Jolt 的 `SixDOFConstraint` 有 `SetTranslationLimits` / `SetRotationLimits` /
/// `SetMotorState`。只有 `axis` / `secondaryAxis` 改了要重建 —— 参考系是构造时烘
/// 进去的。
///
/// ⚠️ 本类**没有**自己的 `ZPROPERTY`：状态全在基类的 `JointComponentRes` 里（那里
/// 写了为什么）。
///
/// ⚠️ 刻意没有发布的 Unity 成员：
///   * `slerpDrive` —— Jolt 没有"整体球面插值驱动"这一路。最接近的是三个旋转轴
///     各一个位置电机（`angularXDrive` / `angularYZDrive` 已经覆盖），而 Unity 的
///     slerpDrive 在启用时还会**覆盖**那两个驱动 —— 这种"优先级"关系猜出来的版本
///     只在仿真里看得出错，宁可不给。
///   * `angularXLimitSpring` / `angularYZLimitSpring` —— Jolt 的注释写得很直白：
///     "Only soft translation limits are supported"。给了会静默无效。
///   * `projectionMode` / `projectionDistance` / `projectionAngle` —— Jolt 的
///     位置求解用的是 Baumgarte，没有"投影"这套参数。
///   * `configuredInWorldSpace` / `swapBodies` —— 前者改变 `axis` 的解释方式（换
///     了它，之前设的 axis 全变成另一个意思），后者交换约束两端；两者都会让"已经
///     建好的关节"和"组件字段"分叉，本引擎一律按宿主局部空间建。
ZCLASS()
class ConfigurableJointComponent : public JointComponent
{
    ZGENERATED_BODY(ConfigurableJointComponent)

public:
    ConfigurableJointComponent() = default;

    PhysicsJointType GetJointType() const override;

    // --- 参考系 -------------------------------------------------------------

    /// Unity 的 `ConfigurableJoint.secondaryAxis`：与 `axis` 一起定出关节参考系
    ///（X = axis，Y = secondaryAxis，Z = 叉乘）。**改变它要重建关节**。
    ZFUNCTION()
    Vector3 GetSecondaryAxis() const { return m_JointRes.m_SecondaryAxis; }
    ZFUNCTION()
    void SetSecondaryAxis(const Vector3& secondary_axis);

    // --- 六个轴的运动模式 ---------------------------------------------------
    //
    // 六个 getter / setter 而不是一个 `SetMotion(axis, mode)`：脚本要的是
    // `joint.xMotion = ConfigurableJointMotion.Limited`，按轴编号写的版本在 TS
    // 里要传一个 number，拼错了也能过编译。
    //
    // ⚠️ 改模式**不会**重建关节（后端有 setter），改完立即生效。

    ZFUNCTION()
    ConfigurableJointMotion GetXMotion() const;
    ZFUNCTION()
    bool SetXMotion(ConfigurableJointMotion motion);

    ZFUNCTION()
    ConfigurableJointMotion GetYMotion() const;
    ZFUNCTION()
    bool SetYMotion(ConfigurableJointMotion motion);

    ZFUNCTION()
    ConfigurableJointMotion GetZMotion() const;
    ZFUNCTION()
    bool SetZMotion(ConfigurableJointMotion motion);

    ZFUNCTION()
    ConfigurableJointMotion GetAngularXMotion() const;
    ZFUNCTION()
    bool SetAngularXMotion(ConfigurableJointMotion motion);

    ZFUNCTION()
    ConfigurableJointMotion GetAngularYMotion() const;
    ZFUNCTION()
    bool SetAngularYMotion(ConfigurableJointMotion motion);

    ZFUNCTION()
    ConfigurableJointMotion GetAngularZMotion() const;
    ZFUNCTION()
    bool SetAngularZMotion(ConfigurableJointMotion motion);

    // --- 限位 ---------------------------------------------------------------

    /// Unity 的 `linearLimit.limit`（米，>= 0）。三个平移轴**共用**它 —— Unity
    /// 也是"离原点的最大距离，每个轴各自算"。
    ZFUNCTION()
    float GetLinearLimit() const { return m_JointRes.m_LinearLimit; }
    ZFUNCTION()
    bool SetLinearLimit(float limit);

    /// 绕 X 的旋转限位（弧度）。Unity 只有 X 这一轴能上下不对称，所以给两个。
    /// `low <= 0 <= high`。
    ZFUNCTION()
    float GetLowAngularXLimit() const { return m_JointRes.m_LowAngularXLimit; }
    ZFUNCTION()
    float GetHighAngularXLimit() const { return m_JointRes.m_HighAngularXLimit; }

    /// 一次设 X 的上下限。分开设会经过一个"区间不跨 0"的非法中间态
    ///（先把 low 设成 +0.5，此时 high 还是 0 ⇒ 被拒）。
    ZFUNCTION()
    bool SetAngularXLimits(float low, float high);

    /// 绕 Y / Z 的旋转限位（弧度，>= 0）。Unity 这两轴只能对称，所以只收半角。
    ZFUNCTION()
    float GetAngularYLimit() const { return m_JointRes.m_AngularYLimit; }
    ZFUNCTION()
    bool SetAngularYLimit(float angle);

    ZFUNCTION()
    float GetAngularZLimit() const { return m_JointRes.m_AngularZLimit; }
    ZFUNCTION()
    bool SetAngularZLimit(float angle);

    // --- 线性限位弹簧 -------------------------------------------------------

    ZFUNCTION()
    SoftJointLimitSpring GetLinearLimitSpring() const;
    ZFUNCTION()
    bool SetLinearLimitSpring(const SoftJointLimitSpring& spring);

    // --- 驱动 ---------------------------------------------------------------

    ZFUNCTION()
    JointDrive GetXDrive() const;
    ZFUNCTION()
    bool SetXDrive(const JointDrive& drive);

    ZFUNCTION()
    JointDrive GetYDrive() const;
    ZFUNCTION()
    bool SetYDrive(const JointDrive& drive);

    ZFUNCTION()
    JointDrive GetZDrive() const;
    ZFUNCTION()
    bool SetZDrive(const JointDrive& drive);

    /// 绕 X 的那一轴。
    ZFUNCTION()
    JointDrive GetAngularXDrive() const;
    ZFUNCTION()
    bool SetAngularXDrive(const JointDrive& drive);

    /// 绕 Y 与 Z 两轴**共用**一份（Unity 的 `angularYZDrive` 就是一份管两轴）。
    ZFUNCTION()
    JointDrive GetAngularYZDrive() const;
    ZFUNCTION()
    bool SetAngularYZDrive(const JointDrive& drive);

    // --- 目标 ---------------------------------------------------------------

    ZFUNCTION()
    Vector3 GetTargetPosition() const { return m_JointRes.m_TargetPosition; }
    ZFUNCTION()
    void SetTargetPosition(const Vector3& target_position);

    ZFUNCTION()
    Vector3 GetTargetVelocity() const { return m_JointRes.m_TargetVelocity; }
    ZFUNCTION()
    void SetTargetVelocity(const Vector3& target_velocity);

    ZFUNCTION()
    Vector3 GetTargetAngularVelocity() const { return m_JointRes.m_TargetAngularVelocity; }
    ZFUNCTION()
    void SetTargetAngularVelocity(const Vector3& target_angular_velocity);

    ZFUNCTION()
    Quaternion GetTargetRotation() const { return m_JointRes.m_TargetRotation; }
    ZFUNCTION()
    void SetTargetRotation(const Quaternion& target_rotation);

    ZSCRIPTPROPERTY(Name="secondaryAxis", Getter="GetSecondaryAxis", Setter="SetSecondaryAxis")
    ZSCRIPTPROPERTY(Name="xMotion", Getter="GetXMotion", Setter="SetXMotion")
    ZSCRIPTPROPERTY(Name="yMotion", Getter="GetYMotion", Setter="SetYMotion")
    ZSCRIPTPROPERTY(Name="zMotion", Getter="GetZMotion", Setter="SetZMotion")
    ZSCRIPTPROPERTY(Name="angularXMotion", Getter="GetAngularXMotion", Setter="SetAngularXMotion")
    ZSCRIPTPROPERTY(Name="angularYMotion", Getter="GetAngularYMotion", Setter="SetAngularYMotion")
    ZSCRIPTPROPERTY(Name="angularZMotion", Getter="GetAngularZMotion", Setter="SetAngularZMotion")
    ZSCRIPTPROPERTY(Name="linearLimit", Getter="GetLinearLimit", Setter="SetLinearLimit")
    ZSCRIPTPROPERTY(Name="lowAngularXLimit", Getter="GetLowAngularXLimit")
    ZSCRIPTPROPERTY(Name="highAngularXLimit", Getter="GetHighAngularXLimit")
    ZSCRIPTPROPERTY(Name="angularYLimit", Getter="GetAngularYLimit", Setter="SetAngularYLimit")
    ZSCRIPTPROPERTY(Name="angularZLimit", Getter="GetAngularZLimit", Setter="SetAngularZLimit")
    ZSCRIPTPROPERTY(Name="linearLimitSpring", Getter="GetLinearLimitSpring", Setter="SetLinearLimitSpring")
    ZSCRIPTPROPERTY(Name="xDrive", Getter="GetXDrive", Setter="SetXDrive")
    ZSCRIPTPROPERTY(Name="yDrive", Getter="GetYDrive", Setter="SetYDrive")
    ZSCRIPTPROPERTY(Name="zDrive", Getter="GetZDrive", Setter="SetZDrive")
    ZSCRIPTPROPERTY(Name="angularXDrive", Getter="GetAngularXDrive", Setter="SetAngularXDrive")
    ZSCRIPTPROPERTY(Name="angularYZDrive", Getter="GetAngularYZDrive", Setter="SetAngularYZDrive")
    ZSCRIPTPROPERTY(Name="targetPosition", Getter="GetTargetPosition", Setter="SetTargetPosition")
    ZSCRIPTPROPERTY(Name="targetVelocity", Getter="GetTargetVelocity", Setter="SetTargetVelocity")
    ZSCRIPTPROPERTY(Name="targetAngularVelocity",
                    Getter="GetTargetAngularVelocity",
                    Setter="SetTargetAngularVelocity")
    ZSCRIPTPROPERTY(Name="targetRotation", Getter="GetTargetRotation", Setter="SetTargetRotation")

protected:
    void ApplyJointSettings() override;
    void FillJointLimits(PhysicsJointDesc& out_desc) override;

private:
    /// 把某个轴的模式 + 限位推给后端，成功才落到 res 上（先判后存）。
    bool PushAxisMotion(ConfigurableJointAxis axis, ConfigurableJointMotion motion, float min, float max);

    /// 六个轴一起推（建好关节时、改了共用限位时）。
    void PushAllMotions();

    void PushDrive(ConfigurableJointAxis axis, const JointDrive& drive);

    /// 校验 + 推给后端 + 落到 res。被拒时 res **不动**（先判后存，与
    /// `CharacterJointComponent::SetTwistSwingLimits` 同一条规则）。
    ///
    /// ⚠️ `RotationY` / `RotationZ` 两个轴都落到 `angularYZDrive` 那三个字段上
    /// —— Unity 的 `angularYZDrive` 就是一份管两轴。
    bool CommitDrive(ConfigurableJointAxis axis, const JointDrive& drive);

    void PushLinearLimitSpring();

    void PushTargets();

    /// 平移轴当前该用的限位区间。Unity 的 `linearLimit` 三个轴**共用**一份且
    /// 对称，所以这里不收轴参数 —— 收了就是个没人读的形参。
    void TranslationLimits(float& out_min, float& out_max) const;

    /// 某个旋转轴当前该用的限位区间（X 不对称，Y / Z 对称）。
    void RotationLimitFor(ConfigurableJointAxis axis, float& out_min, float& out_max) const;
};
