# 角色控制器脚本 API（Unity `CharacterController` 对齐）

切片 10。这一篇讲 `MotorComponent` 上新增的那批脚本方法，以及为什么它们长这样。

## 背景：一个"实现了但碰不到"的组件

`MotorComponent` 在此前是**一整套完整的角色移动实现** —— 加速度曲线、跳跃状态机、
AI 意图驱动、胶囊碰撞求解 —— 但**一个 ZFUNCTION 都没有**。脚本想让角色动起来，
唯一能做的是直接写 `transform.position`，而那绕过了碰撞：角色会穿墙、穿地板。

这个切片把这套实现开给脚本，命名按 Unity 的 `CharacterController`，因为那是脚本
作者手上的肌肉记忆。

## 轴向约定提醒

ZEngine 是 **Z-up，forward = -Y**。`CharacterController` 的胶囊沿 **+Z** 立着，
`GameObject` 原点在**脚底**（不是胶囊中心）。因此：

| Unity 说法 | 本引擎 |
| --- | --- |
| `speed.y`（竖直分量） | `speed.z` |
| `center.y == height / 2` | `center.z == height / 2` |
| 向下 = -Y | 向下 = **-Z** |

## API

### 移动

```ts
const flags: number = motor.Move(motion);   // CollisionFlags 位域
motor.SimpleMove(speed);                    // m/s，自动乘 dt
```

**`Move` 与 `SimpleMove` 的差别**（这是最容易混错的一处）：

| | `Move` | `SimpleMove` |
| --- | --- | --- |
| 单位 | 本步的**位移**（米） | **米/秒** |
| 乘 deltaTime | 调用方自己乘 | 引擎乘，**调用方不要再乘** |
| 竖直分量 | 照用 | **忽略** |
| 重力 | 不管 | 自动施加 |

给 `SimpleMove` 再乘一次 `dt`，角色速度会随帧率**平方**衰减。

### `Move` 逐轴推进

`Move` 不是"一次合并的 sweep"，而是**先竖直、再两个水平轴**各推一次：

1. **碰撞标志才可能是真的。** 合并判定被挡住时无从知道是哪一轴挡的，只能把"所有
   非零轴"都报上去 —— 那是猜。分开推进则每一轴都能单独判定。
2. **贴墙滑行。** 合并判定下撞到墙就是整段位移取消，角色被墙"粘住"；分开推进则
   被挡的那轴归零、另一轴照走，正是 Unity 的手感。

### `CollisionFlags`

```ts
declare enum CollisionFlags { none = 0, sides = 1, above = 2, below = 4 }
```

值必须与 Unity 一致 —— 否则从 Unity 工程贴过来的每一处 `flags & Below` 都会静默
变成"永远不成立"。它是**位域**，判定要写 `(flags & CollisionFlags.below) != 0`，
不能写 `flags == below`（贴着地面的撞墙同时是 `Sides | Below`）。

`Move` 的 C++ 返回类型是 `int32_t` 而不是这个枚举：ZScript 绑定表里没有枚举的
Converter（`RigidBodyActorType` 同样只发布枚举、不进签名）。ZENUM 仍然把名字导给
TS，所以脚本写 `flags & CollisionFlags.below` 是可读的。

### 着地

`isGrounded` 走**向下射线**（`CharacterController::ProbeGrounded`），不是读跳跃状态机。

为什么不能用 `JumpState::idle`：它只表示"没按跳跃键"。一个从平台走出去的角色在
空中仍然是 `idle`，拿它当着地判据会让**二段跳免费拿到**。`CalculateTargetPosition`
里那个 `final_position.z <= 0` 也不行 —— 它把"地面"写死成了 z=0 平面，有二楼的
场景里是错的。

### 胶囊尺寸

```ts
motor.height = 1.5;   // 蹲下
motor.radius = 0.3;
motor.center;         // 只读语义上是可写的：写入会反解半高
```

**`height` 是含两端半球帽的总高**，与 `Capsule::m_HalfHeight`（圆柱段半高）不是
同一个量：

```
height = 2 * (halfHeight + radius)
```

所以 `SetHeight` 必须**保持半径**去反解半高。直接把 `height` 当半高写，角色会凭空
长高一整个半径，而且撞到天花板的时机全错。

`height` 小于 `2 * radius` 时夹到 `2 * radius`（退化成球）；`radius` 夹到 0.001
（Jolt 的 `CapsuleShape` 用非正的 convex radius 会直接断言）。

### 求解参数

| 属性 | 默认 | 说明 |
| --- | --- | --- |
| `minMoveDistance` | 0 | 比这更短的位移请求**整段丢弃**（消抖用） |
| `skinWidth` | 0.08 | 只用于着地探测容差 |
| `detectCollisions` | true | false 时 `Move` 穿墙（传送 / 过场用） |

### 本组件自带、Unity 同名类没有的

Unity 把移动参数放在角色自己的脚本里；本引擎的移动实现就在这个组件上，不导出的话
脚本连"跑多快"都改不了：`moveSpeed`、`jumpHeight`、`isMoving`。

## 刻意没做的两个 Unity 属性

**`slopeLimit` 与 `stepOffset` 没有导出。** 它们在本引擎里没有对应实现 —— 现在的
`CharacterController::move` 只做"目标位置重不重叠"的一次判定，没有任何法线信息，
也没有抬升-重试。导出出去就是一个"存了但不生效"的谎言：脚本设了 `slopeLimit = 45`
却照样爬上 80° 的墙，而且不报错。

真要支持，先得改 `CharacterController` 的求解（多级 sweep + depenetration），那是
功能开发，不是补绑定。这两条在 `MotorComponent.h` 的注释里有明确说明，接线门
（`AB12`）守着这段说明不被删掉。

## 架构：为什么逻辑在 `CharacterController` 而不在组件里

`MotorComponent` 的方法要靠 `WorldManager::GetCurrentActivePhysicsScene()` 找场景，
而活动关卡只能由加载流程设置（`m_CurrentActiveLevel` 没有 setter）。于是组件层在
headless 下**永远**走"没有场景"那条路 —— 把逻辑写在那里等于一行都测不到。

所以真实逻辑都在显式接收 `PhysicsScene*` 的方法上：

```
MotorComponent::Move          -> CharacterController::MoveWithFlags(scene, ...)
MotorComponent::SimpleMove    -> CharacterController::ComputeSimpleMoveMotion(...)   // 纯函数
MotorComponent::IsGrounded    -> CharacterController::ProbeGrounded(scene, ...)
```

组件层只剩"找 Transform / 找场景 / 写回位置"这点薄壳，由接线门守着。与 Rigidbody
切片（逻辑在 `PhysicsScene`，组件是转发）同一个取舍。

## 降级契约

没有 `WorldManager`、没有活动场景、没配 controller config 时：

* `Move` —— 无碰撞直移，返回 `none`（**不会**把角色钉死在原地，那看起来像引擎挂了）
* `IsGrounded` —— 返回 **false**（明确说"不知道"，好过猜一个 true 让脚本以为能跳）
* 尺寸 getter —— 返回 0；setter —— 空操作

## 测试

`UnityParitySmokeTest` 的 **AA 组**（40 项，行为）与 **AB 组**（21 项，接线门）。
AA 组直接驱动 `CharacterController` 与一台真实 `PhysicsScene`（地板 + 墙 + 矮天花板）。

16 个变异全部验证为红，其中三个最初漏网、后来补上的盲区值得记：

* **M4「改尺寸忘了刷新形状局部变换」** —— 最初拿"改半径后能不能挤过去"当判据，
  抓不到：半径变小确实改变了 Jolt 形状，而抬高量在水平推进里不起作用。换成**蹲下
  钻矮天花板**（抬高量直接决定撞不撞头）才抓住。
* **M12「落地不归零下落速度」** —— 断言用的 `fall` 初值是 0，"归零"对任何实现都
  成立。必须带着一个非零值进场。
* **M14「方法忘了标 ZFUNCTION」** —— 接线门只数 `void SimpleMove(`，实现还在就全
  绿。必须连着 `ZFUNCTION()` 一起数（`ZFUNCTION()\n    void SimpleMove(`）。

总计 754 checks / 0 failures。
