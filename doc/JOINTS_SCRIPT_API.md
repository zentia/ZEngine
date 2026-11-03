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
| `CharacterJoint` | `CharacterJointComponent`（锥形球窝） |
| `ConfigurableJoint` | `ConfigurableJointComponent`（六自由度，第 6b 节） |

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

## 4. 四个值类型（比 Unity 少字段，是故意的）

```typescript
declare interface JointLimits { min: number; max: number; }
declare interface JointMotor  { targetVelocity: number; force: number; }
declare interface JointSpring { spring: number; damper: number; targetPosition: number; }
declare interface SoftJointLimitSpring { spring: number; damper: number; }
```

Unity 那边还多了：`JointLimits.bounciness` / `.bounceMinVelocity` /
`.contactDistance`、`JointMotor.freeSpin`。

**少声明是故意的**，理由写在 `PhysicsJoint.h`：Jolt 的限位只有"区间 + 一个软限位
弹簧"，没有弹性的位置；`freeSpin` 要用就得把力矩限幅设成单边，而该留哪一侧取决于
目标速度的符号 —— 做错就是"电机往反方向使劲"，一个只在仿真里看得出来、且不报错的
错。

于是脚本写 `limits.bounciness = 1` 会**编译不过**。编译不过比"能跑、改了个没人读
的字段"好诊断得多。

## 5. CharacterJointComponent（锥形球窝）

Unity 的 `CharacterJoint` 是"球窝 + 一个摆动锥 + 一段扭转区间"：布娃娃的脖子、
腰、肩。与球窝（`PhysicsJointType::point`）的差别是它**限制摆动的锥角**和**绕
自身轴的扭转区间**；与铰链的差别是它不限一根轴、而是给一个锥。

```typescript
declare interface SoftJointLimitSpring { spring: number; damper: number; }
```

⚠️ `SoftJointLimitSpring` 与 `JointSpring` 是**两个**类型：这个**没有**
`targetPosition` —— 限位弹簧拉回的是"限位面上"，目标位置由约束自己算，脚本给
不了。复用 `JointSpring` 会让那个字段变成"能写、没人读"。

### ⚠️ 与铰链相反：锥的限位**可以**运行时改

`HingeJointComponent` 的 `useLimits` 背后是一次 `RebuildJoint()`（Jolt 的
`HingeConstraint` 关不掉限位），而 `SwingTwistConstraint` 有
`SetNormalHalfConeAngle` / `SetPlaneHalfConeAngle` / `SetTwistMinAngle` /
`SetTwistMaxAngle` 四个 setter —— 所以配锥的限位**不重建**关节。

只有 `axis`（扭转轴）和 `swingAxis`（摆动参考轴）是烘进约束参考系的，改它们才
要重建。写脚本时这是两条相反的规则，别照抄铰链那条。

### ⚠️ 摆动轴与扭转轴平行 = NaN

`SwingTwistConstraintSettings` 的 `mPlaneAxis` 只被正交化**一次**：
`cross(parallel, parallel)` 是零向量，后面 `.Normalized()` 就是 NaN，一步之后
整个场景的位置都是 NaN —— 而 `CreateJoint` 照样返回一个合法的 id。所以
`PhysicsScene::PlaneAxisFor` 自己先把"沿扭转轴的那一份"减掉，减完太短就换一条
垂直轴。判据是 `CJ0f`：步进之后位置还是有限值。

### 拒绝而不是夹紧

`swing1Limit` / `swing2Limit` 是**半锥角**（>= 0），`lowTwistLimit` /
`highTwistLimit` 必须**跨 0**（与铰链同一条规则）。越界的值一律**拒掉**（返回
`false`），且**先判后存** —— 存一半再判会让 getter 读到"脚本从来没成功设过"的
值，而脚本那头看不到任何报错。

两层校验都要有：组件层挡住脚本，`PhysicsScene` 层挡住"直接投 Jolt"的调用者
（程序化布娃娃只走后者）。

### 角冲量：必须让重力真的拧出绕扭转轴的力矩

`GetJointImpulse` 在锥上取 `max(|twist|, |swingY|, |swingZ|)` 三股 lambda。这三股
**全是"限位型" part**，只在真的撞上锥面／扭到头的那一步才激活（`Fixed` 和 `Hinge`
读得到角冲量是因为它们另有一股恒激活的 `RotationConstraintPart`）。所以夹具必须
满足两个条件，否则读出来就是 0：

1. 关节点**偏离** B 的质心 —— 落在质心上时重力对枢轴的力矩是 0，B 压根不转，
   自然撞不上限位；
2. 力矩要绕**扭转轴** —— 力臂与重力不能平行。

另外别指望 `SetAngularVelocity` 当载荷：它不唤醒 body，且求解一结束角速度就被清
零，读到的 lambda 永远是"已经解完"的 0。让重力去拧才是持续载荷。

门禁：`CJ17g`（行为：角冲量非 0）+ `CJ17l`（源头：三股都被读进来了 —— 少读任何
一股，剩下两股照样能把它喂成非 0，行为上看不出来）。

## 6. SliderJointComponent（Unity 没有这个类型）

Unity 里"沿一根轴滑动"要用 `ConfigurableJoint` 把 5 个自由度锁掉来表达
（`xMotion = Locked` 那一套）。本引擎单独做一个组件：后端本来就有 slider 约束，
而"活塞 / 抽屉 / 电梯"这种需求用它比用 `ConfigurableJoint` 少写十行、也少十个能
写错的地方。等 `ConfigurableJoint` 补上之后它会保留（它是那份 API 的一个预设）。

## 6b. ConfigurableJointComponent（六自由度）

Unity 的 `ConfigurableJoint` 是"其他关节的超集"：六个自由度**逐个**配模式、逐轴
配限位与驱动。后端是 Jolt 的 `SixDOFConstraint`，两者的轴模型一致 ——
`axis` = 关节参考系 X、`secondaryAxis` = Y、第三条取叉乘（Unity 文档也是这么说
的）。

```typescript
declare enum ConfigurableJointMotion { Free = 0, Limited = 1, Locked = 2 }
declare interface JointDrive {
    positionSpring: number;   // 刚度（牛/米，旋转轴是 牛·米/弧度）
    positionDamper: number;
    maximumForce: number;     // 默认 Number.MAX_VALUE = 不限
}
```

| Unity | 本引擎 | 备注 |
| --- | --- | --- |
| `xMotion` / `yMotion` / `zMotion` | 同名 | 六个 setter，不是 `SetMotion(axis, mode)` —— 按轴编号写的版本在 TS 里要传 number，拼错也能过编译 |
| `angularXMotion` / `angularYMotion` / `angularZMotion` | 同名 | |
| `secondaryAxis` | 同名 | **改它要重建**（参考系是构造时烘进去的） |
| `linearLimit.limit` | `linearLimit`（米，>= 0） | 三个平移轴**共用**且对称 —— Unity 也是"离原点最大距离，每轴各自算" |
| `lowAngularXLimit` / `highAngularXLimit` | 同名 | 只有 X 能不对称；必须跨 0，用 `SetAngularXLimits(low, high)` 一次设 |
| `angularYLimit` / `angularZLimit` | 同名 | 只能对称，所以只收半角 |
| `linearLimitSpring` | 同名 | 只有**平移**限位有弹簧（见下） |
| `xDrive` / `yDrive` / `zDrive` | 同名 | |
| `angularXDrive` / `angularYZDrive` | 同名 | `angularYZDrive` 一份管 Y、Z 两轴 |
| `targetPosition` / `targetVelocity` / `targetAngularVelocity` / `targetRotation` | 同名 | |

### ⚠️ 六个轴的模式**都可以**运行时改，参考系不行

`SixDOFConstraint` 有 `SetTranslationLimits` / `SetRotationLimits` /
`SetMotorState`，所以改 `xMotion` 立即生效、不重建。只有 `axis` 与
`secondaryAxis` 是烘进 `mConstraintToBody1/2` 的，改它们走 `RebuildJoint()`。

### ⚠️ "Locked" 在 Jolt 里是一个**反序**区间

`MakeFixedAxis` 写的是 `min = FLT_MAX / max = -FLT_MAX`，判据是
`IsFixedAxis() { return min >= max; }`。所以：

* `GetJointAxisMotion` **先判 Free / Fixed，再看数值** —— 倒过来会把 Locked 读成
  Limited；
* `CreateJoint` 拒掉 `min > max` 的 Limited 区间：那和 Locked 是两种写法却一个
  意思，正是"我明明写了区间却被当成焊死"这类最难查的错。

### ⚠️ 驱动"开不开"由 `positionSpring > 0` 决定

没有单独的开关，与 Unity 一致（`spring = 0` 就是没有驱动）。Jolt 那侧对应
`EMotorState::Off` vs `PositionAndVelocity`：

```
F = k * (target - cur) + c * (targetVelocity - cur)
```

`targetVelocity` 为 0 时正好退化成 Unity 的 spring/damper 语义，所以 `targetVelocity`
不是"另一套电机"，而是同一条公式里的前馈项。

⚠️ 电机关着时**也把弹簧参数写下去**：否则"设了 spring=0"和"从没设过"在
`GetJointAxisDrive` 上分不出来。建关节时六个电机一律初始化成
`StiffnessAndDamping(0, 0)` —— Jolt 默认是 `{FrequencyAndDamping, 2 Hz, 1}`，那是
一组**看着像开着**的参数，不清零的话读回来会报出一个脚本从没写过的 2.0。

### ⚠️ 摆动型用 Pyramid，不是 Jolt 默认的 Cone

`SixDOFConstraintSettings::mSwingType` 默认是 `Cone`，而 `UpdateRotationLimits` 在
Cone 下会**强行**把 Y / Z 两轴改成对称并且只留 max（`mLimitMin[Y] = -mLimitMax[Y]`、
`mLimitMax = max(0, ...)`）—— 锥本来就只有一个半径。Unity 的 `angularYLimit` /
`angularZLimit` 却是各自独立的两个限位：写 Y=0.5 / Z=0.2 是完全正常的用法，Cone
会把它悄悄改成 Y=Z=0.5。Pyramid 才是"两轴各自上下限"的模型。

### ⚠️ 线性限位弹簧只有平移那三个轴有

Jolt 的 `mLimitsSpringSettings` 只有 `NumTranslation` 个，源码注释写得很直白：
*"Only soft translation limits are supported, soft rotation limits are not currently
supported"*。所以 Unity 的 `angularXLimitSpring` / `angularYZLimitSpring` 在本引擎
**不发布** —— 给了会静默无效，那正是这个引擎一贯避免的一类坑。

### ⚠️ `targetRotation` 存的是 body space，Jolt 存的是 constraint space

`SetTargetOrientationBS(q)` 内部存 `CS = B1⁻¹ * q * B2`。读回来必须反解成
`q = B1 * CS * B2⁻¹`，否则"设进去再读出来"拿到另一个数 —— 而两个都是合法的四元
数，看不出错。`mConstraintToBody*` 是 protected，公开侧只有
`GetConstraintToBody*Matrix()`。

### 刻意没有发布的 Unity 成员

* `slerpDrive` —— Jolt 没有"整体球面插值驱动"。最接近的是三个旋转轴各一个位置
  电机（`angularXDrive` / `angularYZDrive` 已覆盖），而 Unity 的 slerpDrive 启用时
  还会**覆盖**那两个驱动；这种优先级关系猜出来的版本只在仿真里看得出错。
* `angularXLimitSpring` / `angularYZLimitSpring` —— 见上。
* `projectionMode` / `projectionDistance` / `projectionAngle` —— Jolt 用 Baumgarte
  做位置求解，没有"投影"这套参数。
* `configuredInWorldSpace` / `swapBodies` —— 都会让"已建好的关节"和"组件字段"分叉
  （换了前者，之前设的 `axis` 全变成另一个意思），本引擎一律按宿主局部空间建。

## 7. 序列化

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

## 8. 已知缺口

| Unity | 状态 | 原因 |
| --- | --- | --- |
| `Joint.enableCollision` | **已实现**（第 10.4 节） | `GroupFilterTable` + 64 槽 sub-group 池。已知限制：一个 body 只有一个 sub-group，被第二个关节管时会顶掉第一个 |
| `Joint.massScale` / `connectedMassScale` | 未实现 | Jolt 没有 per-constraint 质量缩放 |
| `Joint.currentForce`（Vector3） | **已实现**（第 10.4 节） | 线性 lambda 各类型都能可靠拼成世界矢量；`dt` 取**固定步长**不是 `Tick` 形参 |
| `Joint.currentTorque`（Vector3） | 不发布 | 旋转 lambda 的 `Vector<2>` 分量用的垂直基是 Jolt 内部临时算的，公开 API 拿不到 ⇒ 方向只能猜（第 10.2 节） |
| `ConfigurableJoint` | **已实现**（第 6b 节） | — |
| `CharacterJoint.enableProjection` / `projectionDistance` / `projectionAngle` | 未实现 | Jolt 没有"投影纠错"；布娃娃拉伸要靠缩短步长 / 加求解步数 |
| `SoftJointLimit.bounciness` / `contactDistance` | 未实现 | 与 `JointLimits` 同一条取舍（第 4 节）：只给了裸的 `limit` |
| `JointLimits.bounciness` 等 | 见第 4 节 | — |

## 9. 测试

`UnityParitySmokeTest::TestJointComponents()`（`JN*` 段）三段：

1. **后端**（真场景、直投 Jolt 的刚体）：每种类型的读改写 + 类型拒绝 + 拆掉之后
   id 死透 + 世界锚点 + 冲量非零的对照组（静态 + 动态焊在一起）。
2. **组件**（真 GameObject + `RigidBodyComponent`）：四个子类都真的在后端建出了
   对应类型的约束；限位要重建、电机不要；断裂阈值默认无限；零向量轴被拒。
3. **接线门**（源码级）：d.ts 里四个 `declare class ... extends JointComponent`、
   三个 `declare interface`、四个 `declare type`；`ScriptBinding.h` 里三个
   Converter；`WorldManager.cpp` 里 `FlushPendingJoints` 在
   `FlushPendingRigidBodies` **之后**。

后两个切片在同一个 exe 里加了两段，编号沿用 `JN*` 的三段结构：

- **`CJ*` 段**（CharacterJoint，slice 57）：锥形摆动的**两个**半角各自独立、
  扭转区间的读写、摆动/扭转两根弹簧、断裂阈值在锥上也通。
- **`CF*` 段**（ConfigurableJoint，slice 58）：六轴模式/限位/驱动、四个目标量、
  线性限位弹簧；其中三条是**行为门**（见下）。

三条最值得留下的夹具设计（别的切片要照抄这三条思路）：

| 门 | 夹具为什么这么搭 |
| --- | --- |
| `CF7c` targetRotation 往返 | 参考系**刻意取非单位阵**（axis=+Z、secondaryAxis=+Y）。两端都是单位四元数时 `CS = B1⁻¹qB2` 退化成 `CS = q`，反解公式写错也读回原值 ⇒ 断言恒真 |
| `CF10a` Limited 轴真拦住 body | 区间取**只贴一侧**的 `-0.5 .. 0`，且三个旋转轴也锁掉（否则 body 绕枢轴摆，COM 位置就不只是平移了） |
| `CF11a` drive 真在驱动 | 看 body 动没动，而不是读弹簧参数 —— 参数不管电机开不开都会写下去（刻意的，见后端注释） |

### ⚠️ "0 failures" 也可能来自旧 exe

读测试输出先看**总数**：源码里 `grep -c "Check("` 应该≈运行时打印的总数。加了一
段新门而总数没变 ⇒ 那段没进 exe（跑的是旧产物）。二次确认用
`grep -a "<新门编号>" <exe>`（和查增量链接丢字符串同一套办法）。

`ScriptComponentSmokeTest` 的 `ZA8` + `Mover.ts` 的 `jointProbe` 从 TS 侧再查一遍
（值类型的字段名只有 `tsc` 抓得到）。

### 变异（`.mutate_joints.py`，33 个变异体，全 killed）

三条**活过一轮**的变异体，以及它们暴露的门禁问题 —— 这三条比"测过了"更有价值：

| 变异体 | 为什么当初活着 | 补的门禁 |
| --- | --- | --- |
| `GetJointVelocity` 恒返回 0 | 只读了"调用成功"，没比数值 | 直接 `SetAngularVelocity` 给一个已知 3rad/s 再比（比靠电机推稳） |
| `SetConnectedAnchor` 只改字段不重建 | 基线是 autoConfigure **还开着**时读的，两种实现结果一样 | 基线在状态切换**之后**取 |
| `FixedTick` 每步都判断裂 | 两个阈值都是 Infinity 时有一条提前 return，比较根本没执行 | 补一条"有限但极大的阈值"，并让铰链真吃上力（A 转静态） |

### 变异（`.mutate_config.py`，slice 58，28 个变异体，28/28 killed）

26 个被测试抓住，2 个编不过（`CFM02` 把 `SixDOFConstraintSettings` 换成
`PointConstraintSettings`、`CFM32` 删掉 `Converter<JointDrive>` —— 两者都是
"少了它根本编译不出来"）。

**两条只被源码级门抓住的变异体，以及为什么没有补行为门**：

| 变异体 | 抓它的 | 为什么没有行为门 |
| --- | --- | --- |
| `CFM12` 摆动型退回 Cone | `CF48a`（grep `ESwingType::Pyramid`） | Jolt 的 Cone 是在**每步 `Setup()` 里**才把 Y/Z 强行改成对称的；只改 settings 再读回来仍是不对称的值 ⇒ `CF27b` 那条"Y=0.3 / Z=0.15 各自独立"读不出差别。要补就得让 body 斜着摆出去撞 Y/Z 限位，而**限位型约束只在撞上那一步才激活**（与 slice 56 铰链同一条不稳），夹具很脆 ⇒ 维持源码级门 |
| `CFM08` 建关节时不清零 MotorSettings | `CF48b`（grep 清零那一行） | 没被驱动的轴根本不会开电机，"幽灵 2 Hz 弹簧"在行为上没有可观察后果 —— 它只会在**有人给这个轴加驱动**时变成默认值，而那正好是 `GetJointAxisDrive` 读得出来的。这条本质上只能查源头 |

（这是明确的选择，不是遗漏：两条都有对应的源码级门，且判据是唯一的。）

### ⚠️ 跑这一族变异体的两个前置条件

1. **d.ts 必须重新生成**（`ScriptTsChainSmokeTest --dump-dts`）。d.ts 是生成物、
   不入 git，只有跑一次 dump 才更新 —— 否则"把 prelude 里一行删掉"是看不见的。
   `.mutate_joints.py` 每次 build 之后自己刷三处；`UnityParitySmokeTest` 里那几条
   d.ts 门禁另配了**源码级**的孪生断言（直接在 `ScriptBinding.cpp` 上查）。
2. **动到 `ScriptBinding.cpp` 的模板必须整链**。Debug 用 `/INCREMENTAL`（`.ilk`），
   改 `kZEngineDtsTemplate` 那种 89KB 的只读字符串时增量链接**不搬数据**：
   .obj / .lib 里有那一行，exe 里没有。`.mutate_joints.py` 为此有 `force_relink()`。

## 10. 收尾方案（slice 59）

第 8 节那张表里有 6 项缺口。逐个查过 Jolt 之后，**只有 2 项真能做**，另外 4 项
属于"发布了也只能静默无效"，按引擎一贯的取舍（宁可不发布）维持不发布 —— 但要把
理由写在这里，免得下一个人再查一遍。

### 10.1 `Joint.enableCollision`（可做）

Unity 的默认值是 `false`：被同一个关节连起来的两个刚体**互不碰撞**（不然布娃娃
的相邻两段会互相弹开）。Jolt 的约束完全不参与接触生成，所以只能走过滤：

- `body.SetCollisionGroup(CollisionGroup(filter, groupID, subGroupID))`，改的时候
  要持 `BodyLockWrite`。body 的 `CollisionGroup` 现在只装 layer，所以建关节时
  **事后**改，而不是改 `CreateBody` 的路径 —— 后者会动到**所有**刚体的创建语义，
  风险面大得多。
- sub-group id 从一个固定大小的池里取（比如 64 个），关节拆掉时还回去。
- filter 是**裸指针**，Jolt 不持有 ⇒ `GroupFilterTable` 由 `PhysicsScene` 拥有，
  生命周期必须长过所有 body。

⚠️ `GroupFilterTable::CanCollide` 的实际判定顺序（`Jolt/Physics/Collision/
GroupFilterTable.h`，照抄下来免得下次再猜）：

```cpp
if (inGroup1.GetGroupID() == cInvalidGroup)   return true;    // 没入组 ⇒ 正常碰
if (inGroup1.GetGroupID() != inGroup2.GetGroupID()) return true;
if (inGroup1.GetGroupFilter() != inGroup2.GetGroupFilter()) return false;  // ← 反直觉
if (inGroup1.GetSubGroupID() == inGroup2.GetSubGroupID()) return false;    // ← 不查表
return IsCollisionEnabled(sg1, sg2);
```

三条推论，每条都会改设计：

1. **"filter 指针不同 ⇒ 不碰撞"**。所以"只给 A 设 filter"不是"没生效"，而是
   **A 与该 group 内所有 body 全都不碰撞** —— 一个比没生效更坏的静默错误。
2. **同一个 sub-group 直接返回 false，根本不查表** ⇒ `DisableCollision(sg, sg)`
   是**冗余**的。由此：**「忘了调 DisableCollision」是等价变异体**，不能拿它当
   变异体（会假活）。要变异就变异"两侧的 sub-group 取了不同的 id"。
3. 没被任何关节连的 body 保持默认 `cInvalidGroup` ⇒ 第一个分支就返回 true，
   行为完全不变。
- ⚠️ **已知限制**：一条链（A—B—C 两个关节串起来）里，B 同时属于两个关节，而
  Jolt 的 sub-group 一个 body 只能有一个 ⇒ 本方案只保证"直接相连的两个 body"
  互不碰撞。要做到 Unity 那种 island 级别，得在关节增删时维护并查集合并
  island，代价大得多 ⇒ 单开一片，这里先做直接相连这一层，并在文档与
  `LOG_WARNING` 里说清（池满 / 成链时**报出来**，不静默）。

**门（行为门，必须看 body 动没动）**：两个动态 body **重叠**放在一起（同位置），
用一个 fixed joint 连起来并 `enableCollision = false` ⇒ 步进之后它们**仍然重叠**；
改成 `true` ⇒ 被接触推开、距离变大。只读"设置成功"是白测：setter 永远返回 true。

**变异体**：① 从不设 filter（两 body 保持默认 `cInvalidGroup` ⇒ 永远碰撞）；
② 两侧取**不同**的 sub-group id（命中"查表"分支，默认全碰 ⇒ 不生效）；
③ 只给 A 设不给 B 设（那条反直觉分支 ⇒ 静默地把 A 与该 group 内所有 body 都
关掉碰撞 —— 门要能分辨"没生效"和"过头了"：拿第三个无关的 body 作对照，它必须
**仍然**和 A 碰）；④ 拆关节时不还回 sub-group id（池泄漏 ⇒ 反复建拆 N 次之后
拿不到 id —— 要有一条门在建拆 N > 池容量之后仍能成功）。

⚠️ 不要用"忘了 `DisableCollision(sg, sg)`"当变异体（见上面第 2 条推论：等价变异体）。

### 10.2 `Joint.currentForce`（可做，Vector3）—— 但 `currentTorque` **改判不发布**

⚠️ 上一版这里写的是"两个都能做"。查到 `RotationEulerConstraintPart` 那一层之后
改判：**`currentTorque` 拿不到可靠的方向**。

- Hinge / Slider / SwingTwist 的旋转（或平移）lambda 里有 `Vector<2>` 这种"两条
  垂直轴上的两个分量"。那两条轴是 Jolt **内部临时**由主轴推出来的
  （`RotationEulerConstraintPart` 拿到世界铰链轴之后自己算的基），公开 API 里没有。
- 拿 `GetConstraintToBody1Matrix()` 的 Y / Z 列去凑**是错的**：Hinge 的约束空间是
  `col0 = 铰链轴、col1 = normal、col2 = 铰链轴 × normal`，而 Jolt 内部那组基一般不
  等于这两列。凑出来的 Vector3 大小碰巧对、**方向是猜的** —— 正是第 8 节
  "看着像世界矢量、其实不是的东西"那句话。
- 只有 Fixed（Vec3）和 SixDOF（Vec3，约束空间）能给出方向，其余类型不行 ⇒
  与其"部分类型有方向、部分没有"，维持只给模长（`GetJointImpulse` 的角冲量）。

线性那一路**照做**（各类型的拼法见下面第 1~3 点，都是可靠的）。角的那一路维持
`GetJointImpulse` 的模长。

要把线性 lambda 变成 Unity 的 `currentForce`（世界空间 Vector3），三个关键：

1. `totalLambda` 是**冲量**（N·s），不是力 ⇒ 要除以步长。⚠️ 除的是**物理步长**
   `1 / m_Config.m_UpdateFrequency`，**不是** `Tick(float delta_time)` 的形参 ——
   引擎是固定步长，那个 `delta_time` 在 `Tick` 里压根没被用（就是 `PhysicsScene.cpp`
   那条 C4100 warning 的来源）。拿 `delta_time` 去除会在变帧率下给出错的数，
   而它还在"看起来合理"的量级上。
2. **每个约束类型给出来的东西都不一样**，必须逐个类型拼（这就是"只给模长"那个
   取舍的来源）。照 Jolt 头文件抄下来的返回类型：

| 约束 | 线性 lambda | 角 lambda |
| --- | --- | --- |
| Fixed | `Vec3` | `Vec3` |
| Point | `Vec3` | — |
| Hinge | `Vec3` | **`Vector<2>`** + `GetTotalLambdaRotationLimits()`（标量，沿铰链轴） |
| Slider | **`Vector<2>`** + `GetTotalLambdaPositionLimits()`（标量） | `Vec3` |
| Distance | **标量**（沿两点的轴） | — |
| SwingTwist | `Vec3` | `GetTotalLambdaTwist/SwingY/SwingZ()` 三个标量 |
| SixDOF | `Vec3`（**约束空间**） | `Vec3`（**约束空间**） |

   注意 hinge / slider 的第三个分量来自**另一个约束部件**（限位那条），不是从
   `Vector<2>` 里补的 —— 顺手补个 0 就会给出一个"看着像世界矢量、其实少了一维"
   的数。distance 只有一个标量，方向要自己取 `(p2 - p1).Normalized()`。
   SixDOF 的两个 Vec3 活在**约束空间**，要乘 `GetConstraintToBody1Matrix()` 的旋转
   换出去（与 `CF7c` 那条 `targetRotation` 的 BS/CS 换算是同一类坑）。
3. **空间：只有 SixDOF 需要换算，而且它是"看情况"的。**
   `PointConstraintPart` 的有效质量矩阵是用世界空间的 `mR1/mR2` 和世界空间逆惯量
   张量建的，`lambda` 直接以 `invM * lambda` 加到线速度上 ⇒ Fixed / Point /
   SwingTwist / Hinge 的线性项**就是世界空间冲量**，不要换算。
   但 `SixDOFConstraint::GetTotalLambdaPosition()` 是**二选一**：

   ```cpp
   IsTranslationFullyConstrained() ? mPointConstraintPart.GetTotalLambda()   // 世界空间
                                   : Vec3(mTranslationConstraintPart[0..2])  // 约束空间
   ```

   ⇒ "忘了判 `IsTranslationFullyConstrained`" 这条变异体**只在两种夹具下才分叉**：
   三个平移轴全约束（走世界空间那条）与只有一两个轴约束（走约束空间那条）。
   两条夹具都要有，否则它是等价变异体。

**门（已知输入，不要靠电机攒）**：静态 + 动态焊在一起，动态盒子 1 m³ / 密度 1000
⇒ m = 1000 kg，重力 -Z ⇒ `currentForce` 应该≈ `(0, 0, -9810)`，模长≈ 9810 N。
这是"读回来型"API 的标准夹具（见 `CF*` 那节的教训：靠积分攒出来的量会被
"数太小"打掉，诊断跟着跑偏）。

**变异体**：不除 `dt`（差 60 倍，但方向对 ⇒ 只比模长会漏）；不换坐标空间（约束
空间恰好是单位阵时恒真 ⇒ 夹具的参考系必须**非单位**，与 `CF7c` 同理）；符号取反
（拉 vs 推）。

### 10.3 判定为"不发布"的 4 项

| Unity | 结论 | 查证 |
| --- | --- | --- |
| `massScale` / `connectedMassScale` | 不发布 | Jolt 没有 per-constraint 质量缩放。只能在每个约束求解前后临时改 body 的 inverse mass，而 Jolt 没有 per-constraint 的 step 钩子；且同一 body 被两个约束共享时会互相打架 |
| `CharacterJoint.enableProjection` / `projectionDistance` / `projectionAngle` | 不发布 | Jolt 没有"投影纠错"。布娃娃拉伸要靠缩短步长 / 加求解迭代数，不是这三个数 |
| `SoftJointLimit.bounciness` / `contactDistance` | 不发布 | 限位弹簧只有 frequency / damping，没有 restitution；`contactDistance`（距限位多远开始生效）在 Jolt 里没有对应物 |
| `ConfigurableJoint.slerpDrive` | 不发布 | Jolt 的旋转驱动是**逐轴**三个电机，没有球形插值驱动。用欧拉角拆目标姿态去喂三个电机与 slerp **不等价**（有奇异、有万向锁），给了会静默不同 |

这四条的共同点是"给了也只能静默无效"，而静默无效正是这个引擎一贯避免的一类坑
（对比第 8 节 `angularXLimitSpring`：同样是"Jolt 只有平移限位支持软限位"，同样
不发布）。

### 10.4 已实现：门与变异体（`.mutate_ec.py`）

门都在 `UnityParitySmokeTest` 一个 exe 里，**2d 段 `EC0`~`EC22`** + 接线门
**`EC40`~`EC46`**。设计要点：

| 门 | 夹具 | 挡的是 |
| --- | --- | --- |
| `EC3a` | A 静态、B 动态，沿 X **重叠** 0.1 m（中心距 0.9）；六轴全 Locked 但 `TranslationX` 留 Free | "setter 返回了 true"这种白测。留自由轴是必须的 —— 否则约束自己就把它们按住了，碰不碰看不出来 |
| `EC4a` | 同上但碰撞**开着** | 对照组：证明 0.9 → ~1.0 的变化**就是** `enableCollision` 关掉的那件事 |
| `EC5` | 第三个**无关** body 与被过滤的那个重叠 | 挡"filter 扩散"。这条分支是反直觉的：`CanCollide` 里 **filter 指针不同 ⇒ 不碰**，所以"只给一端设 filter"不是没生效，是把那个 body 与同组所有 body 的碰撞全关了 |
| `EC6` | 开关 140 次 | 开启路径忘了还回 sub-group（池子 64 槽，140 次 = 70 次租用） |
| `EC7` | 80 个"建了就拆"的关节 | `RemoveJoint` 忘了还回 —— **与 EC6 互补**，两条泄漏路径各要一条门 |
| `EC10a` | 静态 + 动态焊一起，1000 kg，重力 -Z | 已知载荷 ≈ -9810 N。挡不除步长 / 符号取反 |
| `EC11a` | SixDOF 全约束 + **非单位**参考系 | 全约束走世界空间 ⇒ **不该**再旋转一次 |
| `EC14a` | SixDOF **非全约束**（`TranslationX` = Free）+ 非单位参考系 | ⚠️ 与 `EC11a` 互补：只有一组夹具时，"一律换算"和"一律不换算"必有一个活着 |
| `EC15a` | distance 关节，两锚点连线与重力同向 | 标量 lambda 自己取方向；少归一化会给出 2 倍的力 |
| `EC22` | 组件层与后端**比一致，且后端非 0** | "恒返回零"的 getter 能过"两边相等"，所以必须额外断言夹具真的在承重 |
| `EC46` | d.ts 里 `currentTorque:` 出现 **0** 次 | 否定门。它的变异体是一次**添加**（`ECM17`） |

**关于 `EC14a` 的那组坐标**：参考系取 `axis = +Y`、`secondaryAxis = +Z` ⇒ 约束
X = 世界 +Y、约束 Y = 世界 +Z、约束 Z = X×Y = 世界 +X。于是 -Z 的重力载荷落在
**约束 Y** 上 —— 少换算一次，那个数会报到世界 Y 上去，`z` 变 0。（若载荷恰好落在
约束 Z 上，"不换算"反而给出正确的 `z`，门就形同虚设 —— 所以自由轴/参考系必须
一起挑。）

**没有行为门的一条**：sub-group 池**满**时 `SetJointEnableCollision` 返回 false
并打一条 `LOG_WARNING`。这条路只有"池子够大"和"池子太小"两种写法，而 64 在任何
现有夹具下都不会被同时占满，所以"池子改成 8"这类变异体是等价变异体。留着警告
是为了让真出问题时有痕迹（静默退化成"不管"是要避免的那类坑）。

**`currentForce` 的符号**（改过一次，写在实现注释里）：`PointConstraintPart::
ApplyVelocityStep` 给 body1 **减** lambda、给 body2 **加** lambda，而 `m_BodyA`
是**宿主自己**。Unity 的 `Joint.currentForce` 是"关节施加到它自己刚体上的力"，
所以最终要取负。`AxisConstraintPart`（distance / SixDOF 的逐轴部件）是同一个约定。
拿错方向会得到一对"大小全对、方向全反"的力 —— 这种错在"能跑"上看不出来。
