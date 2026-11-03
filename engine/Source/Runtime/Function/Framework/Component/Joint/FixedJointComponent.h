#pragma once

#include "JointComponent.h"

#include "FixedJointComponent.generated.h"

/// Unity 的 `FixedJoint`：把两个刚体**焊死**（相对位置与朝向都不许变）。
///
/// 布娃娃的相邻骨骼、可以打碎的"整块"道具，都用它。
///
/// ⚠️ 下面两条 breakForce / breakTorque 在基类 `JointComponent` 上**也有**，
/// 这里是**重复声明**，不是笔误。理由很实在：ZHeaderTool 对"既没有 ZFUNCTION
/// 也没有 ZSCRIPTPROPERTY 的类"不生成脚本绑定表，而 Unity 的 `FixedJoint`
/// 恰好就没有任何自己的成员（只有继承来的 breakForce / breakTorque）—— 不重
/// 复声明一次，`FixedJointComponent` 就根本不会出现在 zengine.d.ts 里，脚本
/// 里 `AddComponent("FixedJointComponent")` 拿不到一个能标注的类型。
/// TypeScript 允许子类用完全相同的类型重新声明继承来的属性，所以这一条既不
/// 改语义、也不产生运行期第二份状态。
ZCLASS()
class FixedJointComponent : public JointComponent
{
    ZGENERATED_BODY(FixedJointComponent)

public:
    FixedJointComponent() = default;

    PhysicsJointType GetJointType() const override;

    ZFUNCTION()
    float GetBreakForce() const { return m_JointRes.m_BreakForce; }
    ZFUNCTION()
    void SetBreakForce(float break_force) { m_JointRes.m_BreakForce = break_force; }

    ZFUNCTION()
    float GetBreakTorque() const { return m_JointRes.m_BreakTorque; }
    ZFUNCTION()
    void SetBreakTorque(float break_torque) { m_JointRes.m_BreakTorque = break_torque; }

    ZSCRIPTPROPERTY(Name="breakForce", Getter="GetBreakForce", Setter="SetBreakForce")
    ZSCRIPTPROPERTY(Name="breakTorque", Getter="GetBreakTorque", Setter="SetBreakTorque")
};
