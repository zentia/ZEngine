# Unity 的 Rigidbody 其余面（constraints / 质心 / 惯性 / CCD / 点速度）

> slice 55。相关的既有文档：`doc/RIGIDBODY_SCRIPT_API.md`（播放控制那一半）、
> `doc/PHYSIC_MATERIAL_SCRIPT_API.md`（摩擦 / 弹性为什么挂在刚体上而不是
> collider 上）、`doc/PHYSICS_SCRIPT_API.md`（`Physics` 静态查询门面）。

## 1. 这一片补的是什么

`RigidBodyComponent` 之前有 mass / drag / 力 / 冲量 / 位姿 / 摩擦，缺的是"**约束
与转动惯量**"那一半 —— 也就是 Unity 里这几样：

| Unity | ZEngine | 落点 |
| --- | --- | --- |
| `rigidbody.constraints` | `GetConstraints` / `SetConstraints` | Jolt `EAllowedDOFs` |
| `rigidbody.freezeRotation` | `GetFreezeRotation` / `SetFreezeRotation` | 上面那三个旋转位 |
| `rigidbody.centerOfMass` | `GetCenterOfMass` | `Shape::GetCenterOfMass()` |
| `rigidbody.worldCenterOfMass` | `GetWorldCenterOfMass` | `Body::GetCenterOfMassPosition()` |
| `rigidbody.inertiaTensor` | `GetInertiaTensor` | 逆惯性取倒数 |
| `rigidbody.inertiaTensorRotation` | `GetInertiaTensorRotation` | `MotionProperties::GetInertiaRotation()` |
| `rigidbody.collisionDetectionMode` | `GetCollisionDetectionMode` / `Set...` | Jolt `EMotionQuality` |
| `rigidbody.maxAngularVelocity` | `GetMaxAngularVelocity` / `Set...` | `MotionProperties` |
| `rigidbody.GetPointVelocity` | `GetPointVelocity` | `Body::GetPointVelocity` |
| `rigidbody.GetRelativePointVelocity` | `GetRelativePointVelocity` | 上一个 + 局部点换算 |
| `rigidbody.AddRelativeForce` / `AddRelativeTorque` | 同名 | 按 body 朝向旋过去 |
| `rigidbody.SetDensity` / `density` | `SetDensity` / `GetDensity` | 密度 × 体积 = 质量 |

类型别名：`declare type Rigidbody = RigidBodyComponent;`（与 `Material` /
`Light` 同一套做法，见 `ScriptBinding.cpp`）。反射名仍然是 `RigidBodyComponent`
—— `AddComponent` / `GetComponent` 收的是字符串，与 d.ts 的类型名无关。

## 2. 约束：两套位序，互为补集

这是本片**唯一**真正容易写错的地方，单独说。

- Unity 的 `RigidbodyConstraints` 表达"**冻住**哪些轴"：
  `FreezePositionX=2, Y=4, Z=8, FreezeRotationX=16, Y=32, Z=64`。
- Jolt 的 `EAllowedDOFs` 表达"**允许**哪些轴"：
  `TranslationX=1, Y=2, Z=4, RotationX=8, Y=16, Z=32, All=63`。

两者**互为补集**，而且轴序不同（Unity 的位移从 bit1 起、旋转从 bit4 起；Jolt 的
位移从 bit0 起、旋转从 bit3 起）。所以只能逐位搬，整体移位是错的。

映射只活在一处：`RigidBody.h` 的 `ToJoltAllowedDOFs` / `ToRigidbodyConstraints`
（与既有的 `ToRigidBodyActorType` 同一条约定）。这两个函数是纯位运算、不碰任何
Jolt 类型 —— 于是能被一个**不起物理世界**的测试直接断言。

> **为什么值得这样**：映射写反一位的表现是"物体在某个轴上悄悄不动了"，而"冻住
> 没生效"和"映射写反了"长得一模一样。先把映射钉死，再测它有没有被用上，两件事
> 才分得开。测试里的 RB2 / RB3 / RB4 分别钉 X / Z / rotZ 三个位，RB10 钉的是
> "FreezePosition**Y** 不是 X"。

### 2.1 `SetMass` 不许清掉约束

`MotionProperties::SetMassProperties(allowed_dofs, mass_properties)` 把**掩码和
质量一起收**。原来的 `SetMass` 写死 `EAllowedDOFs::All`，于是：

```
rigidbody.constraints = FreezeRotation;
rigidbody.mass = 30;          // <- 约束静默消失
```

现在的写法是 `motion_properties->SetMassProperties(motion_properties->GetAllowedDOFs(), mp)`
—— 沿用当前掩码。行为级断言是 RB25 / RB26，源码级是 RB72。

### 2.2 约束要在 body 重建之后补回去

`SetConstraints` 只在有活体时写 Jolt。但脚本在 `OnAwake` 里设约束时，body 往往
还没建出来（关卡加载期物理场景尚未发布）。所以组件自己记一份
`m_Constraints`，三条建 body 的路径（`PostLoadResource` / `FlushPendingBody` /
缩放变化时的 `CreateRigidBody`）都要调一次 `ApplyRuntimeBodySettings()`。

⚠️ **刻意不序列化**：往 `RigidBodyComponentRes` 里加字段要动它的 `Transfer()`
顺序，而现有 .zasset 的字段顺序是资产生命线（与 `CameraComponent` 的
near/far clip 同一条取舍）。运行时可写、能读回，只是不存盘。

## 3. 连续碰撞检测：四个模式落到两态上

Unity 有四个模式（`Discrete` / `Continuous` / `ContinuousDynamic` /
`ContinuousSpeculative`），Jolt 只有两个运动质量（`Discrete` / `LinearCast`）。

处理方式是**保留四个名字、承认窄化**：

- 三个连续模式都走 `LinearCast`；
- **读回来一律是 `Continuous`**（写 `ContinuousDynamic` 读回 `Continuous`）。

理由是"宁可窄而诚实"：要在 body 旁边另存一份作者值才能做到"原样读回"，而那份
值和 Jolt 的真实状态迟早会分叉 —— 分叉之后 `Get` 报的是"我设的那个"，
`Simulate` 用的是 Jolt 里那个，比现在更难查。

⚠️ 实现细节：换运动质量必须走 `BodyInterface::SetMotionQuality`（`MotionProperties`
上**只有 getter**）。而且它内部会拿写锁，所以**不能**在任何 `BodyLockWrite`
的作用域里调用 —— 那会撞 Jolt 的 "lock of same or higher priority" 断言。
（第一次写的时候就是这么踩的。）

## 4. 惯性张量：Jolt 存的是**逆**惯性

`MotionProperties::GetInverseInertiaDiagonal()` 给的是**逆**惯性的对角，所以
`GetInertiaTensor` 要取倒数。

某个旋转轴被冻住时该分量是 0 —— 倒数就是 inf。这时返回 **0** 而不是 inf：
脚本拿它只能做"大不大"的粗略比较，而 inf 会一路传播成 NaN 角速度。

`GetInertiaTensorRotation` 返回把对角旋到对象空间的那次旋转，Jolt 保证它是单位
四元数（RB31 顺手钉了这件事）。

## 5. 质心：两个"位置"不是同一个量

- `centerOfMass`（对象空间）用 `Shape::GetCenterOfMass()`，不含 body 旋转。
- `worldCenterOfMass`（世界空间）用 `Body::GetCenterOfMassPosition()`。

⚠️ **Jolt 的 `Body::GetPosition()` 不是质心**，两条差一个旋转后的局部质心：

```
Body::GetPosition()            = mPosition - rotation * shape->GetCenterOfMass()   // 物体原点
Body::GetCenterOfMassPosition() = mPosition                                        // 质心
```

形状质心在原点时（单个 box / sphere / capsule）两者相等 —— 所以这件事在只玩对称
形状的测试里**永远看不出来**。复合碰撞体上它们差 `rotation * localCOM`。

Unity 里这同样是两个量：`rigidbody.position`（transform 位置 == 物体原点）与
`rigidbody.worldCenterOfMass`。所以：

- `GetWorldCenterOfMass()` **不能**图省事用 `GetRigidBodyPosition()` 顶替；
- `GetRelativePointVelocity(local)` 的 `local` 是刚体**局部空间**里的点，局部空间
  原点是 transform == **物体原点**，所以换算用 `GetPosition()` 而不是质心。

测试里为此专门造了一块形状偏离原点 0.25 米的盒子（`SpawnOffsetComBox`）：只用
对称盒子的话，"返回质心"和"返回零"、"返回质心"和"返回原点"全都相等，那两条断言
是**白测**。

## 6. 点速度

`GetPointVelocity(world_point)` 与 `velocity` 的差别是它含旋转分量：线速度为 0、
绕 Z 以 5 rad/s 自转的刚体，X 方向偏 1 米那个点的速度是 ω × r = `(0, 5, 0)`。
RB41 钉方向（不只是大小），RB43 钉"平移与旋转是叠加而不是替换"。

`GetRelativePointVelocity(local_point)` 的定义是
`GetPointVelocity(worldCenterOfMass + rotation * local_point)`。

## 7. 密度

`SetDensity(density)` = `SetMass(density * volume)`，`volume` 来自
`Shape::GetVolume()`。拿不到体积（body 不在 / 形状退化）时静默返回，而不是照着
`volume = 0` 算出 `mass = 0` —— 0 质量是 NaN 的入口（见 `SetMass` 的注释）。

## 8. 刻意没做的

- **`rigidbody.detectCollisions`**：Unity 的语义是"不参与碰撞但仍在世界里"。
  Jolt 没有对应的每体开关（`SetIsSensor` 是"报告但不产生响应"，语义不同），
  用 object layer 模拟会顺手改掉层碰撞矩阵。不做一个看起来对的假开关。
- **`rigidbody.solverIterations` / `solverVelocityIterations`**：Jolt 里这两个是
  **全局** `PhysicsSettings`，不是每体。发一个每体的 setter 只能是被忽略的字段。
- **`rigidbody.interpolation`**：渲染侧的插值，本引擎没有那条链路。
- **`rigidbody.sleepThreshold`**：Jolt 没有每体的睡眠阈值（只有全局
  `PhysicsSettings::mTimeBeforeSleep`）。
- **`rigidbody.SweepTest`**：`PhysicsScene::Sweep` 已经能扫，但 `SweepTest` 要
  用**自己**的形状去扫并且回一个 `RaycastHit`，得在门面层把形状取出来再喂回去。
  留在下一片。

## 9. 测试

`UnityParitySmokeTest::TestRigidbodyExtras()`，check ID 前缀 `RB`（0..78）。

四段，顺序刻意：

1. **纯映射**（RB0–RB12）：不起物理世界，钉死 Unity 位 ↔ Jolt 掩码。
2. **真场景行为**（RB13–RB43）：每个语义都配一个**对照组** ——
   "冻住 Z 不掉了"必须配"没冻时确实掉了"（RB16），"冻住 Rz 不转了"必须配
   "解冻后同一个力矩确实让它转"（RB22），"上限挡住了"必须配"抬高上限后同一个
   冲量转得更快"（RB40）。没有对照组的话，一个"场景根本没在模拟"的实现是全绿的。
3. **组件降级**（RB46–RB62）：没有活体时的读值与安全空操作。
4. **接线门**（RB63–RB78）：声明 + 发布名 + 真的走到场景 + null 构建也定义了符号。

TS 侧：`Mover.ts` 的 `rigidbodyProbe`（11 段）+ `ScriptComponentSmokeTest` 的 `ZA7`。
除了"方法在不在"，还钉了两件 tsc 抓不到的事：`Rigidbody` 类型别名存在，以及
`RigidbodyConstraints.FreezeRotation === 112` / `FreezeAll === 126`（组合常量是
按位或出来的，codegen 改个命名规则就会让它们悄悄错位）。

## 10. 这轮踩到的

- **`MotionProperties` 没有 `SetMotionQuality`**：只有 getter。换运动质量必须走
  `BodyInterface`，而它内部拿写锁 ⇒ 不能在已有的 `BodyLockWrite` 作用域里调。
- **`SetBodyAllowedDOFs` 必须额外收一个质量**：`SetMassProperties` 收完整的
  `MassProperties`，而"位移被全冻住"的 body 逆质量是 0，读不回真实质量。不传就
  会在"冻住全部位移"之后把质量丢掉。
- **改质量的路径要沿用当前 DOF 掩码**（见 §2.1）。
