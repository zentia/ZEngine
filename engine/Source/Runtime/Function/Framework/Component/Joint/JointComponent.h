#pragma once

#include "Runtime/BaseClasses/PPtr.h"
#include "Runtime/Core/Math/Quaternion.h"
#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Core/Serialize/SerializeUtility.h"
#include "Runtime/Function/Framework/Component/Component.h"
#include "Runtime/Function/Framework/Component/Rigidbody/RigidbodyComponent.h"
#include "Runtime/Function/Physics/PhysicsJoint.h"

#include <cfloat>
#include <memory>

class PhysicsScene;

// 两个"只要声明就够"的类型（与 PhysicsScene.h 顶上那一批同一招）：固定底层
// 类型的枚举一经声明就是完整类型，可以按值返回；`PhysicsJointDesc` 只按引用
// 传。这样本头文件**不** include PhysicsScene.h —— 那边会拖进 jolt/utils.h，
// 而这个头文件要能被 ZHeaderTool 生成的 .gen.cpp 和 Web / slim-iOS 构建吃下。
enum class PhysicsJointType : unsigned char;
struct PhysicsJointDesc;

/// "没有关节"。与 `PhysicsScene::s_InvalidJointId` 同一个值（0）：关节 id 是
/// 1 基的，0 天然空出来当哨兵。
static constexpr uint32_t kInvalidJointId = 0;

/// Unity 的 `Mathf.Infinity`：`Joint.breakForce` / `breakTorque` 的默认值
///（"永不断"）。用 `FLT_MAX` 而不是真的 `INFINITY`，是为了让 YAML / 二进制
/// 序列化能来回 —— inf 在 YAML 里写得出来，读回来各家实现不一致。
static constexpr float kInfiniteBreak = FLT_MAX;

/// 关节的**作者态**参数，一个结构装完（与 `RigidBodyComponentRes` 同一套路）。
///
/// ⚠️ 为什么是"一个 res"而不是每个字段一条 `ZPROPERTY()`：ZHeaderTool 会把
/// 每条 ZPROPERTY 自动发布成同名的脚本属性（`m_Anchor` -> `anchor`），而脚本
/// 那边的 `anchor` 必须是**带副作用的 setter**（改了连接点要重建约束）。两条
/// 同名属性会让工具直接报错（"collides with the script-exposed ZPROPERTY
/// field"），而一个 res 的类型不在脚本可编组类型表里，因此不会自动发布 ——
/// Unity 的拼写留给下面的 `ZSCRIPTPROPERTY`。
///
/// 字段是**全关节的并集**：滑轨用不到 `m_Spring`，弹簧关节用不到 `m_UseLimits`。
/// 一份 res 比四份更省事，代价是每个关节多序列化几个用不到的数 —— 而分开的
/// 话 `HingeJointComponent` 与 `SliderJointComponent` 各要一份 res、各要在
/// 生成代码里多一条 `ZPROPERTY`，收益只是磁盘上少几个字节。
class JointComponentRes
{
public:
    DECLARE_SERIALIZE(JointComponentRes)

    PPtr<RigidBodyComponent> m_ConnectedBody;

    /// 宿主刚体局部空间里的连接点。
    Vector3 m_Anchor {Vector3::ZERO};

    /// 见 `GetConnectedAnchor` —— 有连接体时是局部、没有时是世界。
    Vector3 m_ConnectedAnchor {Vector3::ZERO};

    Vector3 m_Axis {Vector3::UNIT_X};

    bool m_AutoConfigureConnectedAnchor {true};

    /// 断裂阈值（牛 / 牛·米）。`kInfiniteBreak` = Unity 的默认（永不断）。
    float m_BreakForce {kInfiniteBreak};
    float m_BreakTorque {kInfiniteBreak};

    // --- 限位（铰链：弧度；滑轨：米）--------------------------------------
    bool m_UseLimits {false};
    float m_MinLimit {0.f};
    float m_MaxLimit {0.f};

    // --- 电机 --------------------------------------------------------------
    bool m_UseMotor {false};
    /// 铰链：目标角速度（弧度/秒）。
    float m_MotorTargetVelocity {0.f};
    /// 滑轨：目标位置（米）。
    float m_MotorTargetPosition {0.f};
    /// 铰链：最大力矩（牛·米）；滑轨：最大推力（牛）。
    float m_MotorForce {0.f};

    // --- 铰链弹簧 ----------------------------------------------------------
    bool m_UseSpring {false};
    float m_Spring {0.f};
    float m_Damper {0.f};
    float m_SpringTargetPosition {0.f};

    // --- 弹簧关节的距离区间（米）-------------------------------------------
    float m_MinDistance {-1.f};
    float m_MaxDistance {-1.f};

    // --- CharacterJoint 的摆动 / 扭转（弧度）-------------------------------
    //
    // Unity 的 `swing1Limit.limit` / `swing2Limit.limit` 是半锥角（>= 0），
    // `lowTwistLimit.limit` / `highTwistLimit.limit` 是跨 0 的区间。这里存的是
    // 那一层 `SoftJointLimit.limit` 的**裸值**（bounciness / contactDistance 在
    // Jolt 里没有对应物，见 `PhysicsJoint.h` 的取舍）。
    float m_Swing1Limit {0.f};
    float m_Swing2Limit {0.f};
    float m_LowTwistLimit {0.f};
    float m_HighTwistLimit {0.f};

    /// 与扭转轴垂直、定出摆动两个方向的那条轴（宿主局部空间）。
    Vector3 m_SwingAxis {Vector3::UNIT_Y};

    /// 限位弹簧（频率 Hz / 阻尼比）。0 = 硬限位。
    float m_SwingSpring {0.f};
    float m_SwingDamper {0.f};
    float m_TwistSpring {0.f};
    float m_TwistDamper {0.f};

    // --- ConfigurableJoint --------------------------------------------------
    //
    // ⚠️ 运动模式存成 `uint8_t` 而不是 `ConfigurableJointMotion`：序列化只认识
    // 基础类型（泛型的 `SerializeTraits<T>` 会去找 `T::Transfer`，枚举没有），
    // 所以这里存整数、由组件那层 static_cast。sizeof 与枚举一致（都是
    // `unsigned char`），磁盘上没有区别。

    /// 与 `m_Axis` 一起定出关节参考系的那条轴（宿主局部空间）。Unity 的
    /// `secondaryAxis`：X = axis、Y = secondaryAxis、Z 取叉乘。
    Vector3 m_SecondaryAxis {Vector3::UNIT_Y};

    uint8_t m_MotionX {0};
    uint8_t m_MotionY {0};
    uint8_t m_MotionZ {0};
    uint8_t m_AngularMotionX {0};
    uint8_t m_AngularMotionY {0};
    uint8_t m_AngularMotionZ {0};

    /// 线性限位（米，>= 0）。三个平移轴**共用**它 —— Unity 的 `Linear Limit`
    /// 也是"离原点的最大距离，每个轴各自算"。
    float m_LinearLimit {0.f};

    /// 绕 X 的旋转限位（弧度）：Unity 的 X 轴**可以**上下不对称。
    /// low <= 0 <= high。
    float m_LowAngularXLimit {0.f};
    float m_HighAngularXLimit {0.f};

    /// 绕 Y / Z 的旋转限位（弧度，>= 0）：Unity 这两轴只能对称，所以只存半角。
    float m_AngularYLimit {0.f};
    float m_AngularZLimit {0.f};

    /// 线性限位弹簧（刚度 / 阻尼）。Jolt 只有**平移**限位支持弹簧。
    float m_LinearLimitSpring {0.f};
    float m_LinearLimitDamper {0.f};

    /// 五个驱动（Unity 的 `xDrive` / `yDrive` / `zDrive` / `angularXDrive` /
    /// `angularYZDrive`），各三个数。
    ///
    /// ⚠️ 拆成裸 float 而不是直接存 `JointDrive`：`JointDrive` 是**脚本**的值
    /// 类型（和 `JointLimits` 一样没有 `Transfer`），为了让它可序列化就把序列化
    /// 宏挂进 `PhysicsJoint.h`，会让那个"必须保持 Jolt-free"的头文件开始依赖
    /// 序列化那一套 —— 而收益只是少写 10 行。
    float m_XDriveSpring {0.f};
    float m_XDriveDamper {0.f};
    float m_XDriveForce {FLT_MAX};
    float m_YDriveSpring {0.f};
    float m_YDriveDamper {0.f};
    float m_YDriveForce {FLT_MAX};
    float m_ZDriveSpring {0.f};
    float m_ZDriveDamper {0.f};
    float m_ZDriveForce {FLT_MAX};
    float m_AngularXDriveSpring {0.f};
    float m_AngularXDriveDamper {0.f};
    float m_AngularXDriveForce {FLT_MAX};
    float m_AngularYZDriveSpring {0.f};
    float m_AngularYZDriveDamper {0.f};
    float m_AngularYZDriveForce {FLT_MAX};

    /// 四个目标量。Unity 的 `targetPosition` / `targetVelocity` /
    /// `targetAngularVelocity` / `targetRotation`。
    Vector3 m_TargetPosition {Vector3::ZERO};
    Vector3 m_TargetVelocity {Vector3::ZERO};
    Vector3 m_TargetAngularVelocity {Vector3::ZERO};
    Quaternion m_TargetRotation {Quaternion::IDENTITY};
};

template<typename TransferFunction>
void JointComponentRes::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(m_ConnectedBody, "connected_body");
    transfer.Transfer(m_Anchor, "anchor");
    transfer.Transfer(m_ConnectedAnchor, "connected_anchor");
    transfer.Transfer(m_Axis, "axis");
    transfer.Transfer(m_AutoConfigureConnectedAnchor, "auto_configure_connected_anchor");
    transfer.Transfer(m_BreakForce, "break_force");
    transfer.Transfer(m_BreakTorque, "break_torque");
    transfer.Transfer(m_UseLimits, "use_limits");
    transfer.Transfer(m_MinLimit, "min_limit");
    transfer.Transfer(m_MaxLimit, "max_limit");
    transfer.Transfer(m_UseMotor, "use_motor");
    transfer.Transfer(m_MotorTargetVelocity, "motor_target_velocity");
    transfer.Transfer(m_MotorTargetPosition, "motor_target_position");
    transfer.Transfer(m_MotorForce, "motor_force");
    transfer.Transfer(m_UseSpring, "use_spring");
    transfer.Transfer(m_Spring, "spring");
    transfer.Transfer(m_Damper, "damper");
    transfer.Transfer(m_SpringTargetPosition, "spring_target_position");
    transfer.Transfer(m_MinDistance, "min_distance");
    transfer.Transfer(m_MaxDistance, "max_distance");
    transfer.Transfer(m_Swing1Limit, "swing1_limit");
    transfer.Transfer(m_Swing2Limit, "swing2_limit");
    transfer.Transfer(m_LowTwistLimit, "low_twist_limit");
    transfer.Transfer(m_HighTwistLimit, "high_twist_limit");
    transfer.Transfer(m_SwingAxis, "swing_axis");
    transfer.Transfer(m_SwingSpring, "swing_spring");
    transfer.Transfer(m_SwingDamper, "swing_damper");
    transfer.Transfer(m_TwistSpring, "twist_spring");
    transfer.Transfer(m_TwistDamper, "twist_damper");
    // ConfigurableJoint（追加在末尾 —— 动既有键的顺序 = 动资产生命线）
    transfer.Transfer(m_SecondaryAxis, "secondary_axis");
    transfer.Transfer(m_MotionX, "motion_x");
    transfer.Transfer(m_MotionY, "motion_y");
    transfer.Transfer(m_MotionZ, "motion_z");
    transfer.Transfer(m_AngularMotionX, "angular_motion_x");
    transfer.Transfer(m_AngularMotionY, "angular_motion_y");
    transfer.Transfer(m_AngularMotionZ, "angular_motion_z");
    transfer.Transfer(m_LinearLimit, "linear_limit");
    transfer.Transfer(m_LowAngularXLimit, "low_angular_x_limit");
    transfer.Transfer(m_HighAngularXLimit, "high_angular_x_limit");
    transfer.Transfer(m_AngularYLimit, "angular_y_limit");
    transfer.Transfer(m_AngularZLimit, "angular_z_limit");
    transfer.Transfer(m_LinearLimitSpring, "linear_limit_spring");
    transfer.Transfer(m_LinearLimitDamper, "linear_limit_damper");
    transfer.Transfer(m_XDriveSpring, "x_drive_spring");
    transfer.Transfer(m_XDriveDamper, "x_drive_damper");
    transfer.Transfer(m_XDriveForce, "x_drive_force");
    transfer.Transfer(m_YDriveSpring, "y_drive_spring");
    transfer.Transfer(m_YDriveDamper, "y_drive_damper");
    transfer.Transfer(m_YDriveForce, "y_drive_force");
    transfer.Transfer(m_ZDriveSpring, "z_drive_spring");
    transfer.Transfer(m_ZDriveDamper, "z_drive_damper");
    transfer.Transfer(m_ZDriveForce, "z_drive_force");
    transfer.Transfer(m_AngularXDriveSpring, "angular_x_drive_spring");
    transfer.Transfer(m_AngularXDriveDamper, "angular_x_drive_damper");
    transfer.Transfer(m_AngularXDriveForce, "angular_x_drive_force");
    transfer.Transfer(m_AngularYZDriveSpring, "angular_yz_drive_spring");
    transfer.Transfer(m_AngularYZDriveDamper, "angular_yz_drive_damper");
    transfer.Transfer(m_AngularYZDriveForce, "angular_yz_drive_force");
    transfer.Transfer(m_TargetPosition, "target_position");
    transfer.Transfer(m_TargetVelocity, "target_velocity");
    transfer.Transfer(m_TargetAngularVelocity, "target_angular_velocity");
    transfer.Transfer(m_TargetRotation, "target_rotation");
}

INSTANTIATE_TEMPLATE_TRANSFER_WITH_DECL(JointComponentRes, , Transfer, void)

#include "JointComponent.generated.h"

/// Unity 的 `Joint`。
///
/// 与 Unity 一样是**抽象**的：脚本 `AddComponent("JointComponent")` 拿不到东西
///（`GameObject::AddComponentByTypeName` 会因为"没有 factory"拒绝），能加的是
/// 下面那几个具体子类。抽象在这里不是洁癖 —— `Joint` 自己不决定后端用哪种
/// 约束，而没有类型的关节建不出来。
///
/// ⚠️ 类型由一个**纯虚函数**给，不是一个字段。存成字段的话，"改了字段但约束
/// 已经建好"这两个状态就会分叉，表现是"我调的是 hinge 的方法，改的其实是个
/// distance 约束"。子类固定返回常量，分叉不可能发生。
ZCLASS(Abstract)
class JointComponent : public Component
{
    ZGENERATED_BODY(JointComponent)

public:
    JointComponent() = default;
    ~JointComponent() override;

    void PostLoadResource(GameObject* parent_object) override;

    /// 本子类对应哪个后端约束类型。
    virtual PhysicsJointType GetJointType() const = 0;

    /// 关节还在等（宿主或连接体的 body 还没建出来）。
    bool HasPendingJoint() const { return m_HasPendingJoint; }

    /// 把等着的关节建出来。返回 false 表示还建不了（保持 pending，可以重试）。
    ///
    /// 由 `WorldManager::FlushPendingJoints` 在关卡发布为 active **之后**调
    /// —— 与 `RigidBodyComponent::FlushPendingBody` 同一条时序，且必须在它
    /// 之后：关节要的两个 body 可能这一刻才刚补建出来。
    bool FlushPendingJoint();

    ZFUNCTION()
    bool HasJoint() const { return m_JointId != kInvalidJointId; }

    /// 活着的关节 id。脚本拿它去和日志比对；关节拆掉后它是 `kInvalidJointId`。
    ZFUNCTION()
    uint32_t GetJointId() const { return m_JointId; }

    // --- Unity 的 Joint 门面 ------------------------------------------------

    /// Unity 的 `Joint.connectedBody`。nullptr = 拴在**世界**上（Unity 同义）。
    ZFUNCTION()
    RigidBodyComponent* GetConnectedBody() const;

    /// 换连接体。会**重建**关节（Jolt 的约束建好之后改不了端点）。
    ZFUNCTION()
    void SetConnectedBody(RigidBodyComponent* connected_body);

    /// Unity 的 `Joint.anchor`：宿主刚体**局部空间**里的连接点。
    ZFUNCTION()
    Vector3 GetAnchor() const { return m_JointRes.m_Anchor; }
    ZFUNCTION()
    void SetAnchor(const Vector3& anchor);

    /// Unity 的 `Joint.connectedAnchor`。
    ///
    /// ⚠️ 含义随 `connectedBody` 变，与 Unity 完全一致：
    ///   * 有连接体 —— 连接体**局部空间**里的点；
    ///   * 没有连接体（拴在世界）—— **世界空间**坐标。
    ZFUNCTION()
    Vector3 GetConnectedAnchor() const { return m_JointRes.m_ConnectedAnchor; }
    ZFUNCTION()
    void SetConnectedAnchor(const Vector3& connected_anchor);

    /// Unity 的 `Joint.axis`：宿主刚体**局部空间**里的铰链 / 滑轨轴。
    ZFUNCTION()
    Vector3 GetAxis() const { return m_JointRes.m_Axis; }
    ZFUNCTION()
    void SetAxis(const Vector3& axis);

    /// Unity 的 `Joint.autoConfigureConnectedAnchor`（默认 true）。
    ZFUNCTION()
    bool GetAutoConfigureConnectedAnchor() const { return m_JointRes.m_AutoConfigureConnectedAnchor; }
    ZFUNCTION()
    void SetAutoConfigureConnectedAnchor(bool auto_configure);

    /// 拆掉现有关节并按当前参数重建。
    ///
    /// 连 `axis` 都要走这里：Jolt 的约束在建好之后**没有**"改轴"这条路（轴在
    /// 构造时就被烘进约束的坐标系了）。只改组件字段不重建的话，读回来的角度
    /// / 限制都还是旧轴上的，而组件字段已经变了 —— 一个只在物理表现上出错、
    /// 不报错的错位。
    ZFUNCTION()
    bool RebuildJoint();

    // --- 断裂（Unity 的 `Joint.breakForce` / `breakTorque`）------------------
    //
    // 每个固定步把约束上一步施加的冲量换算成力 / 力矩，超过阈值就把关节关掉。
    //
    // ⚠️ 与 Unity 的一处**刻意**差别：Unity 是 `Destroy(joint)` 把组件销毁掉，
    // 这里是"关掉组件 + 关掉 Jolt 那一侧"。在 FixedTick 中间自销毁会把正在
    // 遍历的组件列表抽掉一块，不是这里能安全做的事。脚本侧可观测的结果一样
    //（关节不再起作用，`joint.enabled == false`），而且能读回"断过"。

    ZFUNCTION()
    float GetBreakForce() const { return m_JointRes.m_BreakForce; }
    ZFUNCTION()
    void SetBreakForce(float break_force) { m_JointRes.m_BreakForce = break_force; }

    ZFUNCTION()
    float GetBreakTorque() const { return m_JointRes.m_BreakTorque; }
    ZFUNCTION()
    void SetBreakTorque(float break_torque) { m_JointRes.m_BreakTorque = break_torque; }

    /// 这个关节已经断过（被 `breakForce` / `breakTorque` 关掉）。
    ZFUNCTION()
    bool HasBroken() const { return m_HasBroken; }

    /// Unity 没有这个：断裂之后重新装回去（重建约束）。
    ZFUNCTION()
    bool RepairJoint();

    void FixedTick(float fixed_delta_time) override;

    /// Unity 里停用关节组件会真的把关节拆掉，这里同：顺手把 Jolt 那一侧也
    /// `SetEnabled` 了（而不是只记一个字段等重建时才生效）。
    void SetEnabled(bool enabled) override;

    ZSCRIPTPROPERTY(Name="connectedBody", Getter="GetConnectedBody", Setter="SetConnectedBody")
    ZSCRIPTPROPERTY(Name="anchor", Getter="GetAnchor", Setter="SetAnchor")
    ZSCRIPTPROPERTY(Name="connectedAnchor", Getter="GetConnectedAnchor", Setter="SetConnectedAnchor")
    ZSCRIPTPROPERTY(Name="axis", Getter="GetAxis", Setter="SetAxis")
    ZSCRIPTPROPERTY(Name="autoConfigureConnectedAnchor",
                    Getter="GetAutoConfigureConnectedAnchor",
                    Setter="SetAutoConfigureConnectedAnchor")
    ZSCRIPTPROPERTY(Name="breakForce", Getter="GetBreakForce", Setter="SetBreakForce")
    ZSCRIPTPROPERTY(Name="breakTorque", Getter="GetBreakTorque", Setter="SetBreakTorque")
    ZSCRIPTPROPERTY(Name="hasBroken", Getter="HasBroken")

protected:
    /// 建关节。成功时把 id 记下来并调 `ApplyJointSettings()`。
    bool BuildJoint();

    /// 拆关节，并且 —— 如果当初为它造过世界锚点 body —— 把锚点也拆掉。
    void DestroyJoint();

    /// 关节**刚建好**时把子类那批参数（电机 / 弹簧 / 距离）推下去。
    ///
    /// 不推的话"脚本在 Awake 里设了 limits"会丢：设的时候 body / 关节都还没
    /// 建，等建出来时 Jolt 那侧是默认区间。
    virtual void ApplyJointSettings() {}

    /// 按当前组件字段填一份 `PhysicsJointDesc`。返回 false 表示信息不全
    ///（宿主没刚体、连接体没有 body 等）。
    bool BuildJointDesc(PhysicsJointDesc& out_desc, uint32_t& out_other_body_id);

    /// 子类在建约束**之前**把限位填进 desc（铰链 / 滑轨 / 弹簧）。
    ///
    /// ⚠️ 限位必须烘在建约束那一步，不能事后改：Jolt 的
    /// `HingeConstraint::SetLimits` 断言区间跨 0 且把 `mHasLimits` 直接置真，
    /// **没有**"关掉限位"这条路。所以 `useLimits = false` 要生效只能重建。
    virtual void FillJointLimits(PhysicsJointDesc& out_desc) {}

    std::shared_ptr<PhysicsScene> GetActivePhysicsScene() const;

    ZPROPERTY()
    JointComponentRes m_JointRes;

    uint32_t m_JointId {kInvalidJointId};

    bool m_HasBroken {false};

    /// `connectedBody == null` 时造出来的那个"世界锚点"body。
    ///
    /// ⚠️ 必须和关节**一起**拆：锚点是 `EmptyShape` 的静态 body，不参与碰撞
    /// 也不渲染，漏掉它不会崩、不会有任何告警，只是关卡里一直多一个 body。
    uint32_t m_AnchorBodyId {0xffffffff};

    bool m_HasPendingJoint {false};
};
