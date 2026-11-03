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

#include <cfloat>

#include "Runtime/BaseClasses/ObjectMacros.h"

/// 六自由度约束的**轴**，顺序与 Jolt 的 `SixDOFConstraintSettings::EAxis` 一致。
///
/// 先三个平移、再三个旋转 —— 这个顺序不是随手排的：`TranslationX` 到
/// `TranslationZ` 正好是一段连续下标，Jolt 的"限位只有平移那三个有弹簧"就是按
/// 这段下标切的（`mLimitsSpringSettings[NumTranslation]`）。
enum class ConfigurableJointAxis : unsigned char
{
    TranslationX,
    TranslationY,
    TranslationZ,

    RotationX,
    RotationY,
    RotationZ,

    Count
};

/// 六个轴的数量，用来声明 `PhysicsJointDesc` 里那几组平行数组。
static constexpr int kConfigurableAxisCount = static_cast<int>(ConfigurableJointAxis::Count);

/// Unity 的 `ConfigurableJointMotion`（`ConfigurableJoint.xMotion` / ...）。
///
/// ⚠️ 取值顺序与 Unity 的 `ConfigurableJointMotion` 一致（Free / Limited /
/// Locked），所以脚本里写 `ConfigurableJointMotion.Limited` 与 C++ 里写
/// `ConfigurableJointMotion::Limited` 是同一个数。
///
/// ZENUM：这个枚举要出现在脚本里（`joint.xMotion = ConfigurableJointMotion.Limited`
/// 得能过 tsc），所以必须标 —— 不标的 `enum class` 会被 codegen 直接跳过，而跳过
/// 的表现是"能编译、能运行、赋值静默无效"。
ZENUM()
enum class ConfigurableJointMotion : unsigned char
{
    Free,     ///< 该轴完全不受约束（Jolt：区间写成 [-FLT_MAX, FLT_MAX]）。
    Limited,  ///< 该轴被限位约束（Jolt：区间写成 [min, max]）。
    Locked    ///< 该轴完全锁死（Jolt：区间**写反**成 [FLT_MAX, -FLT_MAX]，
              ///< 于是这个自由度被驱动回 0）。
};

/// Unity 的 `JointDrive`（`ConfigurableJoint.xDrive` / `.angularXDrive` / ...）。
///
/// ⚠️ 与 `JointMotor` / `JointSpring` 是**三个**类型，因为追的是三个不同的量：
/// `JointMotor` 追**速度**、`JointSpring` 追**角度**、`JointDrive` 追**位置 / 姿态**
/// 并且带一个"最大出力"。合成一个就得再加一个"模式"参数，而"两个只有一个参数表
/// 不同的重载"是最容易传错的一种。
struct JointDrive
{
    /// 位置弹簧的刚度（牛/米，旋转轴是牛·米/弧度）。Jolt 那边是
    /// `ESpringMode::StiffnessAndDamping` 的 stiffness，可以直接照搬 —— 不像
    /// `JointSpring.spring` 那样要当成频率解释。
    float positionSpring {0.f};

    /// 阻尼。Jolt 里是 `F = -k*x - c*v` 的 c。
    float positionDamper {0.f};

    /// 电机能使出的最大力（牛）/ 力矩（牛·米）。`FLT_MAX` 就是 Unity 的
    /// `float.PositiveInfinity`（那边 JointDrive 的默认值）。
    float maximumForce {FLT_MAX};
};

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

/// Unity 的 `SoftJointLimitSpring`（`CharacterJoint.twistLimitSpring` /
/// `.swingLimitSpring`）。
///
/// ⚠️ 与 `JointSpring` 是**两个**类型：这个没有 `targetPosition` —— 限位弹簧拉回
/// 的是"限位面上"，目标位置由约束自己算，脚本给不了。复用 `JointSpring` 的话那
/// 个字段就成了"能写、没人读"，正是这文件开头说的那种最难的坑。
struct SoftJointLimitSpring
{
    /// 刚度。Jolt 里是限位弹簧的**频率**（Hz）。0 = 硬限位。
    float spring {0.f};
    /// 阻尼。
    float damper {0.f};
};
