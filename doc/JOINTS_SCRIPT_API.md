# JOINTS — 关节脚本面（Unity 的 Joint 一族）

引擎此前只有 `PhysicsScene::CreateJoint` / `RemoveJoint`：脚本**能造**一个关节，
之后再也没有接口碰它 —— 读不到铰链角度、开不了电机、调不了弹簧。这一片把
Unity 的 `Joint` 那一族补齐到"脚本能用"的程度。

三个层次：

| 层 | 文件 | 干什么 |
| --- | --- | --- |
| 值类型 | `Function/Physics/PhysicsJoint.h` | `JointLimits` / `JointMotor` / `JointSpring`（Jolt-free，手写 Converter） |
| 后端 | `Function/Physics/PhysicsScene.{h,cpp}` | 关节的运行时控制 + 世界锚点 + 冲量 |
| 组件 | `Function/Framework/Component/Joint/` | `JointComponent` + 四个子类 |

脚本里的名字：

```typescript
let hinge = gameObject.AddComponent("HingeJointComponent") as HingeJoint;
hinge.connectedBody = otherRigidbody;
hinge.useLimits = true;
hinge.limits = { min: -1, max: 0.5 };
hinge.useMotor = true;
hinge.motor = { targetVelocity: 3, force: 12 };
console.log(hinge.angle, hinge.velocity);
```

⚠️ `AddComponent` / `GetComponent` 收的是**反射名**（`HingeJointComponent`），
`HingeJoint` 只是 `declare type` 类型别名。与 `Rigidbody` / `Material` / `Light`
同一条例子。

## 1. 类族

| Unity | ZEngine |
| --- | --- |
| `Joint`（抽象） | `JointComponent`（`ZCLASS(Abstract)`） |
| `HingeJoint` | `HingeJointComponent` |
| `FixedJoint` | `FixedJointComponent` |
| `SpringJoint` | `SpringJointComponent` |
| —（Unity 用 `ConfigurableJoint` 表达） | `SliderJointComponent` |
| `CharacterJoint` | **未实现**（见第 7 节） |
| `ConfigurableJoint` | **未实现** |

## 2. PhysicsScene 的运行时控制

```
GetJointType(id, out)            类型**不记录**，一律问 Jolt（GetSubType）
SetJointEnabled / IsJointEnabled
GetJointAngle / GetJointVelocity         仅 hinge
SetJointLimits / GetJointLimits          hinge（弧度）/ slider（米）
SetJointMotor(id, on, target, max)       hinge = 角速度，slider = 位置
SetJointPositionMotor(id, on, target, max, hz, damping)
SetJointDistance / GetJointDistance      仅 distance
SetJointSpring(id, hz, damping)
GetJointImpulse(id, out_linear, out_angular)   上一步的冲量**模长**
CreateStaticAnchorBody(world_position)   "拴在世界"的那一端
```

两条贯穿全组的规则：

1. **类型不另存一份。** `CreateJoint` 返回 1 基 id，`RemoveJoint` 只把槽位置空、
   不收缩数组（id 永不复用）。另存一张 "id -> 类型" 的表会在拆掉之后和真实约束
   分叉，而分叉的表现是"我调的是 hinge 的方法，改的其实是个 distance 约束"。
2. **每个方法按类型拒绝。** 拿 hinge 的方法去动 distance 约束返回 `false`，不是
   "尽力而为"。静默干点别的是最难查的一类。

### 单位随类型（⚠️ 最容易传错的一处）

| 方法 | hinge | slider |
| --- | --- | --- |
| `SetJointMotor` 的 `target` | 目标角速度 rad/s | 目标位置 m |
| `SetJointMotor` 的 `max_force` | 最大力矩 N·m | 最大推力 N |
| `SetJointLimits` | 弧度 | 米 |

合成一个方法而不是 `SetHingeMotor` / `SetSliderMotor` 各一个，是因为两个名字只有
参数表不同 —— 那是最容易传错的重载。脚本可以从 `GetJointType` 知道自己手上是什么。

### 速度电机 vs 位置电机

`SetJointMotor` 是**速度**电机（Jolt 的 `EMotorState::Velocity`），
`SetJointPositionMotor` 是**位置**电机（`EMotorState::Position`）。
Jolt 里两者共用同一个电机槽，所以 Unity 的 `HingeJoint.useSpring` 与 `useMotor`
同时开时，**弹簧优先**（与 Unity 一致）。

## 3. JointComponent 门面（Unity 的 Joint）

| Unity | ZEngine |
| --- | --- |
| `connectedBody` | `connectedBody`（`RigidBodyComponent`，null = 拴在世界） |
| `anchor` | `anchor`（宿主刚体**局部**空间） |
| `connectedAnchor` | `connectedAnchor` ⚠️ 有连接体时是局部、**没有时是世界** |
| `axis` | `axis`（宿主刚体局部空间，默认 +X） |
| `autoConfigureConnectedAnchor` | 同名，默认 `true` |
| `breakForce` / `breakTorque` | 同名，默认 `FLT_MAX`（= Unity 的 `Mathf.Infinity`） |
| — | `hasBroken` / `RepairJoint()`（Unity 没有，见下） |
| — | `HasJoint()` / `GetJointId()` / `RebuildJoint()` |

**改了 `anchor` / `axis` / `connectedAnchor` / `connectedBody` 都会重建关节**：
Jolt 的约束在建好之后没有"改连接点"或"改轴"这条路（它们被烘进约束的坐标系）。
只改组件字段不重建的话，读回来的角度还是旧轴上的 —— 一个不报错的错位。

**断裂**：每个 `FixedTick` 把约束上一步的冲量除以步长得到力/力矩，超阈值就把
组件关掉（同时关掉 Jolt 那一侧）。与 Unity 的一处**刻意**差别：Unity 是
`Destroy(joint)`，这里不自销毁（在 `FixedTick` 中间自销毁会把正在遍历的组件
列表抽掉一块）。可观测结果一样，且 `hasBroken` 能读回来、`RepairJoint()` 能装回去。

**停用**：`enabled = false` 会真的把 Jolt 那一侧的约束关掉，不是只记一个字段。

### 限位必须烘在建约束那一步

Jolt 的 `HingeConstraint::SetLimits` 断言区间跨 0 且把 `mHasLimits` 直接置真，
**没有**"关掉限位"这条路。所以 `useLimits` 的切换背后是一次 `RebuildJoint()`。
电机与弹簧则可以随时改。

### 退化限位（min == max）＝ 锁死

脚本最容易写出的顺序是"先 `useLimits = true`，再配 `limits`"—— 中间那一步
`min` 和 `max` 都是 0。而 Jolt 在 `HingeConstraint` / `SliderConstraint` 的
构造函数里断言：

```
min != max || mLimitsSpringSettings.mFrequency > 0   // "Better use a fixed constraint"
```

也就是说 Jolt 认为"两个限位重合 ⇒ 你该用 fixed 约束"。可 Unity 的语义恰好相反：
`min == max == 0` 就是"锁死在这个角度"。所以 `CreateJoint` 在退化区间上自己补
一条限位弹簧（`kDegenerateLimitFrequency` / `kDegenerateLimitDamping`，见
`PhysicsScene.cpp`），**而不是**把限位静默丢掉（那会让 `useLimits = true`
看起来生效了却什么也没做）。滑块用同一套规则 —— 它的守卫从 `<`/`>` 放宽到
`<=`/`>=` 就是为这个。

## 4. 三个值类型（比 Unity 少字段，是故意的）

```typescript
declare interface JointLimits { min: number; max: number; }
declare interface JointMotor  { targetVelocity: number; force: number; }
declare interface JointSpring { spring: number; damper: number; targetPosition: number; }
```

Unity 那边还多了：`JointLimits.bounciness` / `.bounceMinVelocity` /
`.contactDistance`、`JointMotor.freeSpin`。

**少声明是故意的**，理由写在 `PhysicsJoint.h`：Jolt 的限位只有"区间 + 一个软限位
弹簧"，没有弹性的位置；`freeSpin` 要用就得把力矩限幅设成单边，而该留哪一侧取决于
目标速度的符号 —— 做错就是"电机往反方向使劲"，一个只在仿真里看得出来、且不报错的
错。

于是脚本写 `limits.bounciness = 1` 会**编译不过**。编译不过比"能跑、改了个没人读
的字段"好诊断得多。

## 5. SliderJointComponent（Unity 没有这个类型）

Unity 里"沿一根轴滑动"要用 `ConfigurableJoint` 把 5 个自由度锁掉来表达
（`xMotion = Locked` 那一套）。本引擎单独做一个组件：后端本来就有 slider 约束，
而"活塞 / 抽屉 / 电梯"这种需求用它比用 `ConfigurableJoint` 少写十行、也少十个能
写错的地方。等 `ConfigurableJoint` 补上之后它会保留（它是那份 API 的一个预设）。

## 6. 序列化

所有作者态参数装在一个 `JointComponentRes` 里（`ZPROPERTY()`，基类持有，四个子类
共用）。

⚠️ 这是**必须的**，不是风格选择：ZHeaderTool 会把每条 `ZPROPERTY()` 自动发布成
同名脚本属性（`m_Anchor` → `anchor`），而脚本那边的 `anchor` 必须是**带副作用的
setter**（改了要重建约束）。两条同名属性会让 ZHeaderTool 直接报
`ZSCRIPTPROPERTY name 'anchor' collides with the script-exposed ZPROPERTY field`。
一个 res 的类型不在"脚本可编组类型"表里，因此不会自动发布 —— Unity 的拼写留给
`ZSCRIPTPROPERTY`。

字段是四个子类的**并集**：`SliderJoint` 用不到 `m_Spring`，`SpringJoint` 用不到
`m_UseLimits`。一份 res 比四份省事，代价是每个关节多序列化几个用不到的数。

## 7. 已知缺口

| Unity | 状态 | 原因 |
| --- | --- | --- |
| `Joint.enableCollision` | 未实现 | Jolt 的约束不影响接触生成；要"两个刚体互不碰撞"得走 `GroupFilterTable` + 每个 body 一个 sub-group id，会动现有 body 的创建语义 |
| `Joint.massScale` / `connectedMassScale` | 未实现 | Jolt 没有 per-constraint 质量缩放 |
| `Joint.currentForce` / `currentTorque`（Vector3） | 未实现 | 只给了**模长**（`GetJointImpulse`）。每个约束的 lambda 活在自己的坐标系里，拼成 Vector3 会给出一个看着像世界矢量、其实不是的东西 |
| `CharacterJoint` | 未实现 | 需要后端补 Jolt 的 `SwingTwistConstraint` |
| `ConfigurableJoint` | 未实现 | 需要后端补 `SixDOFConstraint` |
| `JointLimits.bounciness` 等 | 见第 4 节 | — |

## 8. 测试

`UnityParitySmokeTest::TestJointComponents()`（`JN*` 段）三段：

1. **后端**（真场景、直投 Jolt 的刚体）：每种类型的读改写 + 类型拒绝 + 拆掉之后
   id 死透 + 世界锚点 + 冲量非零的对照组（静态 + 动态焊在一起）。
2. **组件**（真 GameObject + `RigidBodyComponent`）：四个子类都真的在后端建出了
   对应类型的约束；限位要重建、电机不要；断裂阈值默认无限；零向量轴被拒。
3. **接线门**（源码级）：d.ts 里四个 `declare class ... extends JointComponent`、
   三个 `declare interface`、四个 `declare type`；`ScriptBinding.h` 里三个
   Converter；`WorldManager.cpp` 里 `FlushPendingJoints` 在
   `FlushPendingRigidBodies` **之后**。

`ScriptComponentSmokeTest` 的 `ZA8` + `Mover.ts` 的 `jointProbe` 从 TS 侧再查一遍
（值类型的字段名只有 `tsc` 抓得到）。

### 变异（`.mutate_joints.py`，33 个变异体，全 killed）

三条**活过一轮**的变异体，以及它们暴露的门禁问题 —— 这三条比"测过了"更有价值：

| 变异体 | 为什么当初活着 | 补的门禁 |
| --- | --- | --- |
| `GetJointVelocity` 恒返回 0 | 只读了"调用成功"，没比数值 | 直接 `SetAngularVelocity` 给一个已知 3rad/s 再比（比靠电机推稳） |
| `SetConnectedAnchor` 只改字段不重建 | 基线是 autoConfigure **还开着**时读的，两种实现结果一样 | 基线在状态切换**之后**取 |
| `FixedTick` 每步都判断裂 | 两个阈值都是 Infinity 时有一条提前 return，比较根本没执行 | 补一条"有限但极大的阈值"，并让铰链真吃上力（A 转静态） |

### ⚠️ 跑这一族变异体的两个前置条件

1. **d.ts 必须重新生成**（`ScriptTsChainSmokeTest --dump-dts`）。d.ts 是生成物、
   不入 git，只有跑一次 dump 才更新 —— 否则"把 prelude 里一行删掉"是看不见的。
   `.mutate_joints.py` 每次 build 之后自己刷三处；`UnityParitySmokeTest` 里那几条
   d.ts 门禁另配了**源码级**的孪生断言（直接在 `ScriptBinding.cpp` 上查）。
2. **动到 `ScriptBinding.cpp` 的模板必须整链**。Debug 用 `/INCREMENTAL`（`.ilk`），
   改 `kZEngineDtsTemplate` 那种 89KB 的只读字符串时增量链接**不搬数据**：
   .obj / .lib 里有那一行，exe 里没有。`.mutate_joints.py` 为此有 `force_relink()`。
