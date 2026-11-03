#pragma once

// =============================================================================
// PhysicsJoint -- the Jolt-free vocabulary of Unity's joint value types.
//
// Same reason PhysicsQuery.h exists apart from PhysicsScene.h: the script
// binding table (ScriptBinding.h) needs a ZScript::Converter for each of these,
// and ScriptBinding.h must stay free of Jolt -- it is included by every
// generated .gen.cpp in the engine, including the Web / slim-iOS builds that
// deliberately do not link Jolt.
//
// These are Unity's `JointLimits` / `JointMotor` / `JointSpring`: plain value
// structs, copied in and out of the joint components.
// =============================================================================

/// Unity 的 `JointLimits`（`HingeJoint.limits`）。
///
/// ⚠️ 刻意**只有 min / max**。Unity 那一边还有 `bounciness` /
/// `bounceMinVelocity` / `contactDistance`，Jolt 的限位只有"区间 + 一个软限位
/// 弹簧"，没有"撞到限位弹回来"的位置。少声明字段是**故意**的：脚本写
/// `limits.bounciness = 1` 会**编译不过**，而编译不过比"能编译、能运行、改的
/// 是一个没人读的字段"好诊断得多 —— 后者要等手感不对了才被发现。
struct JointLimits
{
    /// 最小角度（铰链，弧度）/ 最小位置（滑轨，米）。
    float min {0.f};
    /// 最大角度（铰链，弧度）/ 最大位置（滑轨，米）。
    float max {0.f};
};

/// Unity 的 `JointMotor`（`HingeJoint.motor`）。
///
/// ⚠️ 同样刻意**没有** Unity 的 `freeSpin`。Jolt 的电机是"对称力矩限幅 + 目标
/// 速度"，没有"超速时不刹车"这一位；要用它是**可以**的（把力矩限幅设成单边），
/// 但单边该留哪一侧取决于目标速度的符号，做错就是"电机往反方向使劲"—— 一个
/// 只在仿真里才看得出来、且不报错的错。宁可不给，也不给一个猜的。
struct JointMotor
{
    /// 目标角速度（铰链，弧度/秒）。
    float targetVelocity {0.f};
    /// 电机能使出的最大力矩（铰链，牛·米）。
    float force {0.f};
};

/// Unity 的 `JointSpring`（`HingeJoint.spring`）。
struct JointSpring
{
    /// 刚度。Jolt 里是驱动弹簧的**频率**（Hz）。
    float spring {0.f};
    /// 阻尼。
    float damper {0.f};
    /// 弹簧要拉到的角度（铰链，弧度）。
    float targetPosition {0.f};
};
