#pragma once

#include "Runtime/Function/Controller/CharacterController.h"
#include "Runtime/Function/AI/AIMoveIntent.h"
#include "Runtime/Function/Framework/Component/Component.h"
#include "Runtime/Resource/ResType/Components/Motor.h"
#include "MotorComponent.generated.h"

// 只用在下面几个私有辅助函数的签名里（指针就够），真实定义由 .cpp 引入 ——
// CharacterController.h 对 PhysicsScene 也是这个处理。
class PhysicsScene;

enum class MotorState : unsigned char
{
    moving,
    jumping
};

enum class JumpState : unsigned char
{
    idle,
    rising,
    falling
};

/// Unity 的 `CollisionFlags`：`CharacterController.Move` 的返回值。
///
/// **位域**，不是互斥枚举 —— 一次贴着地面的撞墙同时是 Sides|Below。所以
/// 判定要写 `(flags & CollisionFlags.below) != 0`，不能写 `flags == below`。
///
/// 值必须与 Unity 一致（Sides=1, Above=2, Below=4），否则从 Unity 工程贴过来
/// 的每一处 `if (flags == CollisionFlags.Below)` 都会静默变成"永远不成立"。
///
/// `Move` 的 C++ 返回类型是 `int32_t` 而不是这个枚举：ZScript 绑定表里没有
/// 枚举的 Converter（`RigidBodyActorType` 同样只发布枚举、不进签名）。ZENUM
/// 仍然把名字导给 TS，脚本写 `flags & CollisionFlags.below`。
ZENUM()
enum class CollisionFlags : unsigned char
{
    none = 0,
    sides = 1,
    above = 2,
    below = 4
};

ZCLASS()
class MotorComponent : public Component
{
    ZGENERATED_BODY(MotorComponent)

public:
    MotorComponent() = default;

    void PostLoadResource(GameObject* parent_object) override;

    ~MotorComponent() override;

    void Tick(float delta_time) override;
    void TickPlayerMotor(float delta_time);
    void TickAIMotor(float delta_time);

    /// Framework-level entry point for the AI layer.
    ///
    /// AIControllerComponent calls this once per frame with the intent its
    /// behaviour tree produced. This is deliberately a framework-level seam:
    /// scripts configure AI (profile, ranges) the same way a blueprint wires a
    /// behaviour tree, but they never drive movement themselves.
    void ApplyAIMoveIntent(const AIMoveIntent& intent, float delta_time);

    /// Yaw-only turn toward a point, rate limited by m_AITurnSpeedDegrees.
    /// Returns true once the agent is facing the target within tolerance.
    bool TurnToward(const Vector3& world_position, float delta_time);

    const Vector3& getTargetPosition() const { return m_TargetPosition; }

    float getSpeedRatio() const { return m_MoveSpeedRatio; }
    bool getIsMoving() const { return m_IsMoving; }
    bool IsAIDriven() const { return m_IsAIDriven; }

    /// Hand control back to player input. Called when the AIController is
    /// disabled or removed so a possessed pawn does not keep drifting.
    void ClearAIControl();

    void SetAITurnSpeedDegrees(float degrees) { m_AITurnSpeedDegrees = degrees; }
    float GetAITurnSpeedDegrees() const { return m_AITurnSpeedDegrees; }

    void SetAIArriveRadius(float radius) { m_AIArriveRadius = radius; }
    float GetAIArriveRadius() const { return m_AIArriveRadius; }

    /// Mutable access for importers/migrators. The controller config is stored
    /// inline in legacy JSON but held by PPtr at runtime, so it has to be
    /// patched in after the component's own Transfer has run.
    MotorComponentRes& GetMotorResMutable() { return m_MotorRes; }

    void GetOffStuckDead();

    // =====================================================================
    // Unity 的 CharacterController 面
    // =====================================================================
    //
    // 这个组件此前**一个 ZFUNCTION 都没有**：底下的 `MotorComponent` 是一套
    // 完整的角色移动实现（加速度 / 跳跃状态机 / AI 意图 / 胶囊碰撞），但脚本
    // 一点都碰不到 —— 想让 TS 控制角色只能去改 `Transform.position`，那就绕
    // 过了碰撞，角色会穿墙。
    //
    // 下面这批方法把这个实现开给脚本。命名按 Unity 的 `CharacterController`
    // 来，因为那是脚本作者手上的肌肉记忆；语义有偏差的地方都写在各自的注释里。
    //
    // 刻意**没有**做的两个 Unity 属性，因为它们在本引擎里没有对应实现，导出
    // 出去就是一个"存了但不生效"的谎言：
    //   * `slopeLimit` —— 需要沿法线做坡角判定并拒绝上坡。现在的控制器只做
    //     "目标位置重不重叠"的一次判定，没有任何法线信息。
    //   * `stepOffset` —— 需要抬升-重试的上台阶逻辑。同上，没有。
    // 真要支持先得改 `CharacterController::move` 的求解（多级 sweep +
    // depenetration），那是功能开发，不是补绑定。

    /// Unity 的 `CharacterController.Move(motion)`。
    ///
    /// 逐轴推进：先竖直、再水平两轴。两个理由，缺一不可：
    ///   1. **拿到真实的碰撞标志**。一次合并的位移被挡住时无从知道是哪一轴挡的，
    ///      只能把"所有非零轴"都报上去 —— 那是猜。分开推进则每一轴都能单独判定。
    ///   2. **贴墙滑行**。合并判定下撞到墙就是整段位移取消，角色会被墙"粘住"；
    ///      分开推进则被挡的那一轴归零、另一轴照走，正是 Unity 的手感。
    ///
    /// 返回 `CollisionFlags` 位域（见枚举的注释）。
    ///
    /// 没有 Transform、没有物理场景、或者 `detectCollisions=false` 时退化为
    /// 无碰撞直移并返回 none —— 但**不会**把角色钉死在原地：那看起来像引擎挂了。
    ZFUNCTION()
    int32_t Move(const Vector3& motion);

    /// Unity 的 `CharacterController.SimpleMove(speed)`。
    ///
    /// 与 `Move` 的两处差别，都是 Unity 的语义：
    ///   * `speed` 的**竖直分量被忽略**（Unity 原话）；竖直方向由这里自己算。
    ///   * 单位是 **m/s 且自动乘 delta**，所以调用方不需要（也不应该）自己乘 dt
    ///     —— `Move` 要乘、`SimpleMove` 不乘，混用就是"角色速度随帧率变化"。
    ///   * 重力自动施加：不在地面上时竖直速度按 -g*dt 累积。
    ZFUNCTION()
    void SimpleMove(const Vector3& speed);

    /// Unity 的 `CharacterController.velocity`：**只读**，且是"上一次 Move 实际
    /// 走出的位移 / 时间"，不是"你请求的速度"。
    ///
    /// 这个区别是刻意的：撞墙之后请求速度和实际速度不一样，而脚本想知道的是
    /// "我到底动没动、多快"（撞墙检测、移动动画的 blend 参数都读它）。
    ZFUNCTION()
    const Vector3& GetVelocity() const { return m_Velocity; }

    ZSCRIPTPROPERTY(Name="velocity", Getter="GetVelocity")

    /// Unity 的 `CharacterController.isGrounded`。
    ///
    /// 走 `CharacterController::ProbeGrounded`（向下射线），**不是**读跳跃状态机：
    /// `JumpState::idle` 只表示"没按跳跃键"，一个从平台走出去的角色在空中仍然是
    /// idle，拿它当着地判据会让二段跳免费拿到。
    ///
    /// 没有物理场景时返回 false —— 明确说"不知道"，好过猜一个 true 让脚本以为
    /// 可以跳。
    ZFUNCTION()
    bool IsGrounded() const;

    ZSCRIPTPROPERTY(Name="isGrounded", Getter="IsGrounded")

    // ---- 胶囊尺寸 --------------------------------------------------------

    /// Unity 的 `CharacterController.height`：**含两端半球帽**的总高。
    /// 与 `Capsule::m_HalfHeight`（圆柱段的半高）不是同一个量，换算是
    /// `height = 2 * (half_height + radius)`。@see CharacterController::SetCapsuleHeight
    ZFUNCTION()
    float GetHeight() const;

    ZFUNCTION()
    void SetHeight(float height);

    ZSCRIPTPROPERTY(Name="height", Getter="GetHeight", Setter="SetHeight")

    ZFUNCTION()
    float GetRadius() const;

    ZFUNCTION()
    void SetRadius(float radius);

    ZSCRIPTPROPERTY(Name="radius", Getter="GetRadius", Setter="SetRadius")

    /// Unity 的 `CharacterController.center`：胶囊中心相对 GameObject 原点的
    /// 偏移。本引擎把原点放在**脚底**，所以 X/Y 恒为 0，Z 恒为 `height / 2`。
    ZFUNCTION()
    Vector3 GetCenter() const;

    ZFUNCTION()
    void SetCenter(const Vector3& center);

    ZSCRIPTPROPERTY(Name="center", Getter="GetCenter", Setter="SetCenter")

    // ---- 求解参数 --------------------------------------------------------

    /// Unity 的 `minMoveDistance`：比这更短的位移请求直接丢弃。
    ///
    /// 默认 0（Unity 同）。调它只为消抖：贴着墙推进时每帧的微小位移会不停地
    /// 触发碰撞回调，设一个 0.001 就能压掉。
    ZFUNCTION()
    float GetMinMoveDistance() const { return m_MinMoveDistance; }

    ZFUNCTION()
    void SetMinMoveDistance(float distance) { m_MinMoveDistance = (distance > 0.f) ? distance : 0.f; }

    ZSCRIPTPROPERTY(Name="minMoveDistance", Getter="GetMinMoveDistance", Setter="SetMinMoveDistance")

    /// Unity 的 `skinWidth`：**唯一**被本引擎真实使用的地方是着地探测的容差
    /// （脚底离地不超过它就仍算贴地）。
    ///
    /// 与 Unity 的差别：Unity 的 skin 还是碰撞求解里的一层"留白"，本引擎的
    /// `CharacterController::move` 没有这层，所以设大它**不会**让角色更容易挤过
    /// 缝隙。默认 0.08（Unity 同）。
    ZFUNCTION()
    float GetSkinWidth() const { return m_SkinWidth; }

    ZFUNCTION()
    void SetSkinWidth(float width) { m_SkinWidth = (width > 0.f) ? width : 0.f; }

    ZSCRIPTPROPERTY(Name="skinWidth", Getter="GetSkinWidth", Setter="SetSkinWidth")

    /// Unity 的 `detectCollisions`：false 时 `Move` 无视一切几何（穿墙）。
    /// 传送 / 过场运镜 / 调试用。**不是**性能优化 —— 求解成本不变。
    ZFUNCTION()
    bool GetDetectCollisions() const { return m_DetectCollisions; }

    ZFUNCTION()
    void SetDetectCollisions(bool detect) { m_DetectCollisions = detect; }

    ZSCRIPTPROPERTY(Name="detectCollisions", Getter="GetDetectCollisions", Setter="SetDetectCollisions")

    // ---- 本组件自带、Unity 同名类没有的几个量 ----------------------------
    //
    // 它们不在 Unity 的 CharacterController 上（Unity 把移动参数放在角色自己的
    // 脚本里）。但本引擎的移动实现就在这个组件上，不导出的话脚本连"跑多快"
    // 都改不了。

    ZFUNCTION()
    float GetMoveSpeed() const { return m_MotorRes.m_MoveSpeed; }

    ZFUNCTION()
    void SetMoveSpeed(float speed) { m_MotorRes.m_MoveSpeed = speed; }

    ZSCRIPTPROPERTY(Name="moveSpeed", Getter="GetMoveSpeed", Setter="SetMoveSpeed")

    ZFUNCTION()
    float GetJumpHeight() const { return m_MotorRes.m_JumpHeight; }

    ZFUNCTION()
    void SetJumpHeight(float height) { m_MotorRes.m_JumpHeight = (height > 0.f) ? height : 0.f; }

    ZSCRIPTPROPERTY(Name="jumpHeight", Getter="GetJumpHeight", Setter="SetJumpHeight")

    ZFUNCTION()
    bool IsMoving() const { return m_IsMoving; }

    ZSCRIPTPROPERTY(Name="isMoving", Getter="IsMoving")

private:
    /// 当前活动的物理场景；没有 WorldManager / 没有活动关卡时返回 nullptr。
    /// 调用方必须对 nullptr 降级 —— 这不是"不可能发生"的分支，headless 与
    /// 关卡切换的空档都会走到。
    PhysicsScene* ResolvePhysicsScene() const;

    /// 取到 CharacterController；没配 controller config 时返回 nullptr。
    CharacterController* ResolveCharacterController() const;
    void CalculatedDesiredHorizontalMoveSpeed(unsigned int command, float delta_time);
    void CalculatedDesiredVerticalMoveSpeed(unsigned int command, float delta_time);
    void CalculatedDesiredMoveDirection(unsigned int command, const Quaternion& object_rotation);
    void CalculateDesiredDisplacement(float delta_time);
    void CalculateTargetPosition(const Vector3&& current_position);

    ZPROPERTY()
    MotorComponentRes m_MotorRes;

    float m_MoveSpeedRatio {0.f};
    float m_VerticalMoveSpeed {0.f};
    float m_JumpHorizontalSpeedRatio {0.f};

    Vector3 m_DesiredDisplacement;
    Vector3 m_DesiredHorizontalMoveDirection;
    Vector3 m_JumpInitialVelocity;
    Vector3 m_TargetPosition;

    MotorState m_MotorState {MotorState::moving};
    JumpState m_JumpState {JumpState::idle};

    ControllerType m_ControllerType = ControllerType::invalid;
    Controller* m_Controller = nullptr;

    bool m_IsMoving {false};

    // ---- CharacterController 面（Unity 同名类的可脚本状态）-----------------
    //
    // 这几个字段**不序列化**：它们和 `input` / 运行期标志属于同一类，是脚本在
    // Awake 里设一次的运行期配置，不是作者资产。加进 Transfer 会多出一份"资产
    // 里写了 0.08 但脚本在 Awake 里改成 0.2"的双真值源。

    /// 上一次 `Move` **实际**走出的速度（位移 / 时间）。@see GetVelocity
    Vector3 m_Velocity {Vector3::ZERO};
    float m_SkinWidth {0.08f};
    float m_MinMoveDistance {0.f};
    bool m_DetectCollisions {true};
    /// SimpleMove 自己维护的竖直速度（m/s，向下为负）。放在这里而不是复用
    /// `m_VerticalMoveSpeed`：后者是玩家输入路径的状态，脚本那条路不该互相踩。
    float m_SimpleMoveFallSpeed {0.f};

    // ---- AI driving state ---------------------------------------------------

    /// True while an AIControllerComponent is feeding this motor. Player input
    /// is ignored while set, so a possessed pawn cannot fight its own AI.
    bool m_IsAIDriven {false};
    AIMoveIntent m_AIIntent;

    float m_AITurnSpeedDegrees {360.f};
    /// Stop moving once within this distance of the target; prevents the
    /// jitter you get when a seek loop keeps overshooting a point.
    float m_AIArriveRadius {0.1f};
};
