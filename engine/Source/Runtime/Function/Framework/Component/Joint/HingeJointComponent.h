#pragma once

#include "JointComponent.h"

#include "HingeJointComponent.generated.h"

/// Unity 的 `HingeJoint`。
///
/// 绕一根轴转，可以带限位、电机、弹簧 —— 门、车轮、秋千都是它。
///
/// ⚠️ 三种"驱动"里，**限位必须重建关节**才能关掉（见 `FillJointLimits` 的注
/// 释），电机与弹簧则可以随时改。这是 Jolt 的不对称，不是设计选择：写脚本时
/// `useLimits` 那一次切换背后是一次 `RebuildJoint()`。
///
/// ⚠️ 本类**没有**自己的 `ZPROPERTY`：状态全在基类的 `JointComponentRes` 里
///（那里写了为什么）。这也意味着 `HingeJointComponent` 的序列化字段就是基类
/// 那一份，`SliderJointComponent` 与它共用 —— 见 `JointComponentRes` 的取舍。
ZCLASS()
class HingeJointComponent : public JointComponent
{
    ZGENERATED_BODY(HingeJointComponent)

public:
    HingeJointComponent() = default;

    PhysicsJointType GetJointType() const override;

    /// 当前角度（弧度，带符号，相对建立时的姿态）。Unity 的 `HingeJoint.angle`。
    /// 关节还没建好 / 已经拆掉时返回 0。
    ZFUNCTION()
    float GetAngle() const;

    /// 当前角速度（弧度/秒）。Unity 的 `HingeJoint.velocity`。
    ZFUNCTION()
    float GetVelocity() const;

    // --- 限位 ---------------------------------------------------------------

    ZFUNCTION()
    bool GetUseLimits() const { return m_JointRes.m_UseLimits; }
    ZFUNCTION()
    void SetUseLimits(bool use_limits);

    ZFUNCTION()
    JointLimits GetLimits() const;
    ZFUNCTION()
    void SetLimits(const JointLimits& limits);

    // --- 电机 ---------------------------------------------------------------

    ZFUNCTION()
    bool GetUseMotor() const { return m_JointRes.m_UseMotor; }
    ZFUNCTION()
    void SetUseMotor(bool use_motor);

    ZFUNCTION()
    JointMotor GetMotor() const;
    ZFUNCTION()
    void SetMotor(const JointMotor& motor);

    // --- 弹簧 ---------------------------------------------------------------

    ZFUNCTION()
    bool GetUseSpring() const { return m_JointRes.m_UseSpring; }
    ZFUNCTION()
    void SetUseSpring(bool use_spring);

    ZFUNCTION()
    JointSpring GetSpring() const;
    ZFUNCTION()
    void SetSpring(const JointSpring& spring);

    ZSCRIPTPROPERTY(Name="angle", Getter="GetAngle")
    ZSCRIPTPROPERTY(Name="velocity", Getter="GetVelocity")
    ZSCRIPTPROPERTY(Name="useLimits", Getter="GetUseLimits", Setter="SetUseLimits")
    ZSCRIPTPROPERTY(Name="limits", Getter="GetLimits", Setter="SetLimits")
    ZSCRIPTPROPERTY(Name="useMotor", Getter="GetUseMotor", Setter="SetUseMotor")
    ZSCRIPTPROPERTY(Name="motor", Getter="GetMotor", Setter="SetMotor")
    ZSCRIPTPROPERTY(Name="useSpring", Getter="GetUseSpring", Setter="SetUseSpring")
    ZSCRIPTPROPERTY(Name="spring", Getter="GetSpring", Setter="SetSpring")

protected:
    void ApplyJointSettings() override;
    void FillJointLimits(PhysicsJointDesc& out_desc) override;

    /// 电机与弹簧共用 Jolt 的**同一个**电机槽：弹簧是位置电机、电机是速度电
    /// 机。优先级与 Unity 一致（弹簧优先）。
    void PushMotor();
};
