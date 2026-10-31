# 导航代理脚本 API（`NavMeshAgentComponent` / `Navigation`）

配套源码：

| 文件 | 作用 |
| --- | --- |
| `engine/Source/Runtime/Function/AI/Navigation/NavMeshAgentComponent.h/.cpp` | 会走路的那个组件（Unity 的 `NavMeshAgent`） |
| `engine/Source/Runtime/Scripting/ScriptNavigation/ScriptNavMesh.h` | 静态查询门面 + `NavMeshPath` / `NavMeshHit` |
| `engine/Source/Runtime/Function/AI/Navigation/NavMesh.h` | 网格本身（A\* + string pulling，本次未改） |
| `engine/Source/Runtime/Function/AI/Navigation/NavigationSystem.h` | 系统持有网格；新增 `BuildWithSampler` |

## 0. 这次补的是什么

引擎此前有**网格**（`NavMesh`）和**系统**（`NavigationSystem`），但没有"会走路的东西"：
AI 层拿到 `NavPath` 的航点数组后，每个动作各自现写"怎么沿数组走"。结果是同一套
加速 / 限速 / 转向 / 到点判定被实现了三遍，且口径不同 —— 有的直接 `SetPosition`
瞬移，有的不减速冲过终点再急停。

Unity 里这个职责属于 `NavMeshAgent`，所以按 Unity 的契约补一层：脚本只设
`destination`，其余每帧由组件推进。

与 `MotorComponent` 的分工：

| | `MotorComponent` | `NavMeshAgentComponent` |
| --- | --- | --- |
| 是什么 | 角色控制器（胶囊 / 重力 / 台阶） | 导航代理 |
| 谁来驱动 | `AIMoveIntent`（方向 + 速度比例） | 自己寻路、自己积分 |
| 写 Transform 吗 | 写（经物理步进） | 写（除非 `updatePosition = false`） |
| 会用导航网格吗 | 不会 | 会 |

## 1. `NavMeshAgentComponent`

### 1.1 寻路

```ts
agent.SetDestination(new Vector3(10, 10, 0)): boolean;
agent.destination                              // 读写（写 = SetDestination）
agent.CalculatePath(target): boolean           // 只算路，不开始走
agent.ResetPath(): void                        // 清路径，**保留**目标
agent.GetWaypointCount(): number
agent.GetWaypoint(i): Vector3
```

`SetDestination` 返回 **是否找到路径**，但**失败时目标点仍然记下来了**。这是
Unity 的行为，也是必须的：否则"先设目标、再建导航网格"这种常见顺序会让代理
永远走不起来（`autoRepath` 也不会重试，因为目标已经被丢掉了）。

### 1.2 状态

```ts
agent.hasPath: boolean
agent.pathStatus: number        // 0 PathComplete / 1 PathPartial / 2 PathInvalid
agent.remainingDistance: number
agent.velocity: Vector3
agent.nextPosition: Vector3
agent.isOnNavMesh: boolean
```

`remainingDistance` **扣掉了 `stoppingDistance`**，进入停止半径后就是 0。Unity
同 —— 不扣的话永远差一点点，状态机切不过去。

### 1.3 停 / 走

```ts
agent.Stop(): void
agent.Resume(): void
agent.isStopped                 // 读写
agent.Warp(pos): boolean        // 瞬移 + 丢路径；返回目标点是否可站立
agent.Move(offset): void        // 相对位移，不走寻路
```

`Resume()` 会把重算计时器直接拨到阈值，所以**恢复的那一帧就重算一次**：停着的
时候网格可能已经重建过，旧航点是按旧网格算的。

### 1.4 参数

`speed` `angularSpeed` `acceleration` `stoppingDistance` `radius` `height`
`autoBraking` `autoRepath` `updatePosition` `updateRotation` —— 全部可读写，
且都是 `ZPROPERTY()`（进序列化、进 Inspector）。

默认值取 Unity 的：`speed 3.5`、`angularSpeed 120`、`acceleration 8`、
`stoppingDistance 0.5`、`radius 0.5`、`height 2`。

### 1.5 每帧在做什么（`Tick`）

1. 没路 / 已停 → 按 `acceleration` 把速度收掉（**不是**一帧归零：从全速瞬间
   归零视觉上是"撞墙"，`velocity` 曲线也不连续，根运动会跳）。
2. 跳过已走过的航点。中间航点的"算走过"半径是 **5cm**（不是 `radius`）：
   string pulling 的拐点落在障碍角上，用代理半径同量级的判定会把拐角切掉，
   代理贴着墙角斜插过去然后卡住。
   另外判"已经越过"（`(wp - pos)·velocity < 0`）:3.5 m/s @60fps 一帧走 5.8cm，
   比 5cm 还大，只用距离判定的话走直线时会周期性多停一帧。
3. 期望速度：`autoBraking` 开着时用 `v = sqrt(2·a·d)` 按**整条剩余路径**减速
   （只按当前这一段减速会在每个拐角先全速冲一下再急停）。
4. 速度朝期望值走，单步不超过 `acceleration · dt`，再夹到 `speed` 以内。
5. 写位置（若 `updatePosition`）、转向（若 `updateRotation`）。

### 1.6 朝向

本地 **-Y 为前**，与 `MotorComponent` 一致（`current_rotation * NEGATIVE_UNIT_Y`）。
转向只做偏航（z 分量抹掉，代理不该俯仰），用叉积 + 轴角而不是"算 yaw 再插值
角度"—— 后者在 ±180° 附近会绕远路。

## 2. `Navigation`（静态查询）

```ts
Navigation.IsReady(): boolean
Navigation.GetWalkableCellCount(): number
Navigation.GetCellSize(): number
Navigation.CalculatePath(source, target): NavMeshPath
Navigation.SamplePosition(source, maxDistance): NavMeshHit
Navigation.Raycast(source, target): NavMeshHit
```

### 2.1 为什么叫 `Navigation` 而不是 `NavMesh`

`NavMesh` 这个名字被引擎内部的网格类占了
（`Runtime/Function/AI/Navigation/NavMesh.h`），同一命名空间下再声明一个同名
class 是**编译错误**，不是二义性。沿用 `VectorMath`（Unity 的 `Vector3`）的
取舍：名字让位给引擎，偏差写进文档。

### 2.2 与 Unity 的偏差

| 点 | Unity | ZEngine | 原因 |
| --- | --- | --- | --- |
| 静态类名 | `NavMesh` | `Navigation` | 见 2.1 |
| `CalculatePath` | 填调用方传入的 `NavMeshPath` | 返回新对象 | TS 没有 out/ref 参数 |
| `pathStatus` | 枚举 | `int` | ZFUNCTION 返回不了枚举；`ZENUM NavMeshPathStatus` 只作名字表 |
| `NavMeshHit.normal` | 有 | **没有** | 网格导航（格子）不存表面法线；给一个假的 `+Z` 比不给更危险 |
| `NavMeshHit.mask` / area | 有 | 没有 | 本引擎没有导航区域 / areaMask |
| `SamplePosition` 搜索 | 由内向外在半径内搜 | 取全局最近点再判是否在半径内 | 网格上"最近就是最近"，行为等价 |

### 2.3 `hit` 在两个方法里意思相反

这是 Unity 自己的不一致，照搬而不是"修正"：

* `SamplePosition`：`hit == true` 表示**找到了**可站立点；
* `Raycast`：`hit == true` 表示**被挡住**，`position` 是挡住的点。

反过来"统一"会让从 Unity 搬过来的分支判断在两个方法里各错一次。

## 3. `NavigationSystem.BuildWithSampler`

新增。网格的 `Build` 本来就只吃一个函数指针（为了让网格能脱离物理单测），
这个口子原先只开在 `NavMesh` 上，系统层只有 `BuildFromActiveLevel` —— 它要求
`WorldManager` 已激活关卡。跑在编辑器之外的地方（服务器、烘焙工具、测试）没
有这个前提，所以把口子开到系统层。

```cpp
int BuildWithSampler(const NavMeshBuildSettings& settings, NavRaycastFunc raycast);
```

建出来的表对 `FindPath` / `IsStandable` 立刻可见（走的是同一个 `m_NavMesh`）。

## 4. 一个必须知道的既有行为：墙顶是"可走孤岛"

`NavMesh::Build` 对每个格子从高处往下打一条射线，取**第一个命中面**当落脚点。
所以一面 3 米高的墙，其顶上的格子 `walkable = true`、中心 `z = 3`。它不会减少
`GetWalkableCellCount()`，也不会让 `GetCells()[i].walkable` 变 false。

真正挡住通行的是 `IsConnected` 里的高度判据：墙顶那格与地面差 3 米，超过
`max_step_height`（默认 0.6），于是墙顶连成一座**孤岛**，谁也上不去、也下不来。

由此得到两条写测试 / 写判据时的规则：

1. **不要用"可走格数变少"判断墙生效了** —— 它没变。
2. **判断"这个点站得住吗"要用 `IsStandable()`，不要只看格子的 `walkable` 位**
   —— `IsStandable` 会额外查高度差。只看 walkable 位会把"目标点在墙里"误判成
   "能到"。

`PathPartial` 的判据就是按第 2 条写的（`RequestPath` 里用
`navigation_system->IsStandable(target)`），而不是"终点离目标多远"—— `FindPath`
会把最后一个航点直接写成 `goal`，那个距离恒为 0，量不出吸附。

## 5. 已知未做

* **OffMeshLink**（跳跃点、传送点）：网格模型里没有对应的概念。
* **导航区域 / areaMask / `SetAreaCost`**：本引擎的网格没有区域标记。
* **避让（RVO）**：多个代理会互相穿过。Unity 有 obstacle avoidance，这里只有
  `radius` 参与网格构建时的墙体排斥，运行时不避让。
* **`NavMeshAgent.Move` 不做碰撞**：它就是一段直接位移，穿墙是调用方的责任
  （Unity 同）。
