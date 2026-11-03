#pragma once

#include "JointComponent.h"

#include "CharacterJointComponent.generated.h"

/// Unity 的 `CharacterJoint`：一个**锥形的球窝**，布娃娃的脖子 / 腰 / 肩都靠它。
///
/// 与球窝（`PhysicsJointType::point`）的差别是它**限制摆动的锥角**和**绕自身轴的
/// 扭转区间**；与铰链的差别是它不限一根轴、而是给一个锥。
///
/// ⚠️ 与 `HingeJointComponent` 相反：这里的**限位可以运行时改**（Jolt 的
/// `SwingTwistConstraint` 有 `SetNormalHalfConeAngle` 等四个 setter），所以配限位
/// 不需要重建关节。只有改 `axis` / `swingAxis` / 连接点才要重建 —— 轴是烘进约束
/// 参考系的。
///
/// ⚠️ 本类**没有**自己的 `ZPROPERTY`：状态全在基类的 `JointComponentRes` 里（那里
/// 写了为什么）。
ZCLASS()
class CharacterJointComponent : public JointComponent
{
    ZGENERATED_BODY(CharacterJointComponent)

public:
    CharacterJointComponent() = default;

    PhysicsJointType GetJointType() const override;

    // --- 摆动 / 扭转限位（弧度）---------------------------------------------
    //
    // Unity 的 `swing1Limit` / `swing2Limit` 是**半锥角**，所以只给一个非负数；
    // `lowTwistLimit` / `highTwistLimit` 是跨 0 的区间（low <= 0 <= high），与
    // 铰链同一条规则。
    //
    // 越界的值被**拒掉**（返回 false）而不是夹住：夹出来的区间和脚本写的不是一回
    // 事，而"我明明写了 -0.5 却被当成 0"是最难查的一类。

    ZFUNCTION()
    float GetSwing1Limit() const { return m_JointRes.m_Swing1Limit; }
    ZFUNCTION()
    bool SetSwing1Limit(float half_angle);

    ZFUNCTION()
    float GetSwing2Limit() const { return m_JointRes.m_Swing2Limit; }
    ZFUNCTION()
    bool SetSwing2Limit(float half_angle);

    ZFUNCTION()
    float GetLowTwistLimit() const { return m_JointRes.m_LowTwistLimit; }
    ZFUNCTION()
    bool SetLowTwistLimit(float angle);

    ZFUNCTION()
    float GetHighTwistLimit() const { return m_JointRes.m_HighTwistLimit; }
    ZFUNCTION()
    bool SetHighTwistLimit(float angle);

    /// 一次设四个。脚本里"配一组限位"是常见写法，分开四次写会中间经过三个
    /// 非法中间态（`low` 设成 +0.5 而 `high` 还是 0 ⇒ 区间不跨 0 ⇒ 被拒）。
    ZFUNCTION()
    bool SetTwistSwingLimits(float swing1, float swing2, float low_twist, float high_twist);

    // --- 摆动参考轴 ---------------------------------------------------------

    /// Unity 的 `CharacterJoint.swingAxis`：与 `axis`（扭转轴）垂直、定出摆动两个
    /// 方向的那条轴。**改变它要重建关节**（它烘在约束的参考系里）。
    ZFUNCTION()
    Vector3 GetSwingAxis() const { return m_JointRes.m_SwingAxis; }
    ZFUNCTION()
    void SetSwingAxis(const Vector3& swing_axis);

    // --- 限位弹簧 -----------------------------------------------------------

    ZFUNCTION()
    SoftJointLimitSpring GetTwistLimitSpring() const;
    ZFUNCTION()
    bool SetTwistLimitSpring(const SoftJointLimitSpring& spring);

    ZFUNCTION()
    SoftJointLimitSpring GetSwingLimitSpring() const;
    ZFUNCTION()
    bool SetSwingLimitSpring(const SoftJointLimitSpring& spring);

    ZSCRIPTPROPERTY(Name="swing1Limit", Getter="GetSwing1Limit", Setter="SetSwing1Limit")
    ZSCRIPTPROPERTY(Name="swing2Limit", Getter="GetSwing2Limit", Setter="SetSwing2Limit")
    ZSCRIPTPROPERTY(Name="lowTwistLimit", Getter="GetLowTwistLimit", Setter="SetLowTwistLimit")
    ZSCRIPTPROPERTY(Name="highTwistLimit", Getter="GetHighTwistLimit", Setter="SetHighTwistLimit")
    ZSCRIPTPROPERTY(Name="swingAxis", Getter="GetSwingAxis", Setter="SetSwingAxis")
    ZSCRIPTPROPERTY(Name="twistLimitSpring", Getter="GetTwistLimitSpring", Setter="SetTwistLimitSpring")
    ZSCRIPTPROPERTY(Name="swingLimitSpring", Getter="GetSwingLimitSpring", Setter="SetSwingLimitSpring")

protected:
    void ApplyJointSettings() override;
    void FillJointLimits(PhysicsJointDesc& out_desc) override;

private:
    /// 把四个限位 + 两条弹簧一起推给后端。
    void PushLimits();
};
