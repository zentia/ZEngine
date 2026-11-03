#pragma once

#include "JointComponent.h"

#include "SliderJointComponent.generated.h"

/// 滑轨关节：沿一根轴滑动，另外两轴与朝向都锁死。
///
/// ⚠️ **不是** Unity 的类型 —— Unity 里这个效果要用 `ConfigurableJoint` 把
/// 5 个自由度锁掉来表达（`xMotion = Locked` 那一套）。本引擎把它单独做成一个
/// 组件：后端本来就有 slider 约束，而"活塞 / 抽屉 / 电梯"这种需求用一个
/// `SliderJoint` 比用 `ConfigurableJoint` 少写十行、也少十个能写错的地方。
/// 若哪天补了 `ConfigurableJoint`，这个组件会保留（它是那份 API 的一个预设，
/// 不是竞争者）。
ZCLASS()
class SliderJointComponent : public JointComponent
{
    ZGENERATED_BODY(SliderJointComponent)

public:
    SliderJointComponent() = default;

    PhysicsJointType GetJointType() const override;

    // --- 限位（米，相对建立时的姿态）----------------------------------------
    //
    // 与铰链同一条规则：限位烘在约束里，`useLimits` 切换要重建。

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

    /// 电机要走到哪个位置（米，相对建立时的姿态）。
    ZFUNCTION()
    float GetMotorTargetPosition() const { return m_JointRes.m_MotorTargetPosition; }
    ZFUNCTION()
    void SetMotorTargetPosition(float target_position);

    /// 电机最大推力（牛）。
    ZFUNCTION()
    float GetMotorForce() const { return m_JointRes.m_MotorForce; }
    ZFUNCTION()
    void SetMotorForce(float force);

    ZSCRIPTPROPERTY(Name="useLimits", Getter="GetUseLimits", Setter="SetUseLimits")
    ZSCRIPTPROPERTY(Name="limits", Getter="GetLimits", Setter="SetLimits")
    ZSCRIPTPROPERTY(Name="useMotor", Getter="GetUseMotor", Setter="SetUseMotor")
    ZSCRIPTPROPERTY(Name="motorTargetPosition", Getter="GetMotorTargetPosition", Setter="SetMotorTargetPosition")
    ZSCRIPTPROPERTY(Name="motorForce", Getter="GetMotorForce", Setter="SetMotorForce")

protected:
    void ApplyJointSettings() override;
    void FillJointLimits(PhysicsJointDesc& out_desc) override;

    void PushMotor();
};
