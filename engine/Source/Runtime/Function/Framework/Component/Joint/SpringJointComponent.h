#pragma once

#include "JointComponent.h"

#include "SpringJointComponent.generated.h"

/// Unity 的 `SpringJoint`：把两个刚体拉在一段距离上，像一根弹簧。
///
/// 吊灯、拖车挂钩、软绳的第一段 —— 都是"别离太远，也别贴太近"。
///
/// ⚠️ 距离区间**随时可改**（不需要重建）：Jolt 的 `DistanceConstraint` 有
/// `SetDistance`。这与铰链的限位不同，别照抄那边的"改了要重建"。
ZCLASS()
class SpringJointComponent : public JointComponent
{
    ZGENERATED_BODY(SpringJointComponent)

public:
    SpringJointComponent() = default;

    PhysicsJointType GetJointType() const override;

    /// 刚度。Jolt 里是频率（Hz）；0 = 硬限位（"拉不动"，不是"没有弹簧"）。
    ZFUNCTION()
    float GetSpring() const { return m_JointRes.m_Spring; }
    ZFUNCTION()
    void SetSpring(float spring);

    /// 阻尼。越大越"发闷"。
    ZFUNCTION()
    float GetDamper() const { return m_JointRes.m_Damper; }
    ZFUNCTION()
    void SetDamper(float damper);

    /// 最小距离（米）。见 `GetMinDistance` 关于负值的说明。
    ZFUNCTION()
    float GetMinDistance() const;
    ZFUNCTION()
    void SetMinDistance(float min_distance);

    /// 最大距离（米）。
    ZFUNCTION()
    float GetMaxDistance() const;
    ZFUNCTION()
    void SetMaxDistance(float max_distance);

    ZSCRIPTPROPERTY(Name="spring", Getter="GetSpring", Setter="SetSpring")
    ZSCRIPTPROPERTY(Name="damper", Getter="GetDamper", Setter="SetDamper")
    ZSCRIPTPROPERTY(Name="minDistance", Getter="GetMinDistance", Setter="SetMinDistance")
    ZSCRIPTPROPERTY(Name="maxDistance", Getter="GetMaxDistance", Setter="SetMaxDistance")

protected:
    void ApplyJointSettings() override;
};
