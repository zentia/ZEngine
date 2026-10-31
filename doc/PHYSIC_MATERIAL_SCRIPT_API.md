# 物理材质（Unity `PhysicMaterial`）脚本接口

> 对应 Unity：`PhysicMaterial` 资源 + `Collider.material` + `Physics.defaultMaterial`。
> 落地位置：`Runtime/Resource/ResType/Components/RigidBody.h`（枚举 / 组合规则 / 序列化字段）、
> `Runtime/Function/Physics/PhysicsScene.{h,cpp}`（Jolt 侧落地）、
> `Runtime/Function/Framework/Component/Rigidbody/RigidbodyComponent.{h,cpp}`（脚本面）。
> 验收：`UnityParitySmokeTest` 的 AO / AP / AQ 三组；13 个变异全被捕获。

## 1. 之前缺什么

全仓库 grep `Friction` / `Restitution` 在 Runtime 里**零命中**。也就是说：

- 每个刚体用的是 Jolt 自己那套默认（摩擦 0.2、弹性 0），
- 作者**看不到也改不了**，
- 于是"冰面"、"橡胶球"这类最基础的手感差别做不出来 —— 不是不好做，是根本没有入口。

Unity 这边对应的是一个 `PhysicMaterial` 资源：`dynamicFriction` / `staticFriction` /
`bounciness` / `frictionCombine` / `bounceCombine`，挂在 Collider 上。

## 2. 现在有什么

### 脚本面（`RigidBodyComponent`）

| TS | C++ | 说明 |
| --- | --- | --- |
| `rigidbody.friction` | `GetFriction()` / `SetFriction(float)` | Unity 默认 **0.6** |
| `rigidbody.bounciness` | `GetBounciness()` / `SetBounciness(float)` | Unity 默认 **0**，夹到 `[0,1]` |
| `rigidbody.GetFrictionCombine()` | `GetFrictionCombine()` | 返回 `PhysicMaterialCombine` |
| `rigidbody.SetFrictionCombine(m)` | `SetFrictionCombine(PhysicMaterialCombine)` | |
| `rigidbody.GetBounceCombine()` | `GetBounceCombine()` | |
| `rigidbody.SetBounceCombine(m)` | `SetBounceCombine(PhysicMaterialCombine)` | |

`friction` / `bounciness` 是 `ZSCRIPTPROPERTY`（所以 TS 里是属性赋值）；
两个组合模式只有方法 —— `ZSCRIPTPROPERTY` 的类型表里没有枚举，而 Unity 那边
它们同样更常被当作"设一次就不管"的配置。

`PhysicMaterialCombine` 是 `ZENUM`，导出成 `declare enum PhysicMaterialCombine
{ Average, Minimum, Multiply, Maximum }`。

### 组合规则

两个面接触时的**有效模式**按 Unity 文档的优先级取高的那个：

```
Average < Minimum < Multiply < Maximum
```

即 `effective = max(modeA, modeB)`，然后按该模式合成：

| 模式 | 结果 |
| --- | --- |
| Average | `(a + b) / 2` |
| Minimum | `min(a, b)` |
| Multiply | `a * b` |
| Maximum | `max(a, b)` |

规则实现在 `CombineSurfaceValues()`（`RigidBody.h`，**纯函数、不碰 Jolt**）。
这是有意的：Jolt 的组合回调只在"两个特定材质真的接触"时才跑一次，靠真世界
复现四种组合 × 优先级基本不可能；做成纯函数之后可以在没有物理世界的情况下
把规则测干净。

## 3. 与 Unity 的两处偏差（不是 bug，是 Jolt 的模型不同）

### 3.1 材质挂在 Rigidbody 上，不在 Collider 上

Unity 的 `PhysicMaterial` 按 **Collider** 存，所以同一个刚体的两个碰撞体可以有
不同摩擦。Jolt 只有**每个 body 一个**摩擦、一个弹性（`Body::mFriction` /
`Body::mRestitution`），没有 per-shape 材质。

所以这里把字段放在 `RigidBodyComponent`（= body）上。一个刚体上挂多个碰撞体时，
它们共享同一套表面参数。

> 想改成 per-shape 的话，Jolt 的路子是 `Shape::SetMaterial` + 自定义
> `PhysicsMaterial` 子类，但 Jolt 的 `PhysicsMaterial` 基类**不带**摩擦/弹性字段
> —— 值仍然要从别处来。这属于"换一套合成机制"，不是补个字段。

### 3.2 只有一个 `friction`，没有 static / dynamic 之分

Unity 按"接触是否在滑动"在 `staticFriction` / `dynamicFriction` 之间选。Jolt
的组合回调签名是

```cpp
float (*)(const Body&, const SubShapeID&, const Body&, const SubShapeID&)
```

**拿不到接触点的相对速度**，所以没有判据去选。与其存一个"写得进、序列化得出来、
但永远没人读"的 `staticFriction`，这里只暴露一个 `friction`。

## 4. 实现要点

### 4.1 组合模式搭在 Jolt body 的 user data 上

Jolt 没有"每个 body 的组合模式"这种槽位，而组合回调在 `PhysicsSystem::Update`
**内部、可能并发地**被调用 —— 所以它不能查任何可变容器（`PhysicsManager` 那张
`body id -> GameObject` 表正在别处被写就炸了）。

于是把两个模式打包进 `Body::mUserData`（各占一段 bit）：

```cpp
PackCombineModes(fc, bc)          // 写入
FrictionCombineFromUserData(ud)   // 回调里读
BounceCombineFromUserData(ud)
```

摩擦 / 弹性的**值**不打包：Jolt 自己就存在 Body 上，再存一份就是第二个事实来源，
第一次有人改值就分叉。

user data 全 0 必须解出 `Average` / `Average` —— 一个从来没写过 user data 的 body
（测试里直接 `SpawnBox` 造的、或者别的创建路径）不能被当成野值。

### 4.2 组合回调必须**装到 Jolt 上**

`PhysicsScene` 的构造函数里（`PhysicsSystem` 刚 `Init` 完、还没放任何 body 进去）：

```cpp
m_Physics.m_JoltPhysicsSystem->SetCombineFriction(&JoltCombineFriction);
m_Physics.m_JoltPhysicsSystem->SetCombineRestitution(&JoltCombineRestitution);
```

不装的话 Jolt 用自己那套（摩擦 `sqrt(f1*f2)`、弹性 `max(r1,r2)`），跟 Unity 四种
模式都不一样 —— 而症状只有"手感不太对"，没有任何日志。这是最容易漏的一步，
所以 AQ7 / AQ8 是源码级门禁。

### 4.3 夹紧

- `friction` 负值夹到 0：Jolt 明确要求**合成后**的摩擦不得为负。
- `bounciness` 夹到 `[0,1]`：>1 会真的往系统里加能量（弹一次比一次高）。

夹而不是断言：脚本里写 `friction = -0.1` 几乎肯定是笔误，一个被夹住的手感比
"Debug 下炸、Release 下静默"好诊断。

### 4.4 默认值从 0.2 换成 0.6

Jolt 默认摩擦是 0.2；Unity 的 `PhysicMaterial.dynamicFriction` 默认是 **0.6**。
采用 Unity 的值，让新作者建的物体按 Unity 作者预期的方式表现。这是一次可观察的
物理行为变化（已有关卡里"贴着地面滑"的东西会比以前更涩）。

## 5. 为什么这样测

| 组 | 测什么 |
| --- | --- |
| AO | `CombineSurfaceValues` 四种模式的算术、交换律、优先级、越界枚举、user data 打包双射。纯函数，不需要物理世界。 |
| AP | 真 Jolt body 上的读写往返、两个量互相不干扰、无效 body id 返回 false 而不是读已释放内存、组件层的默认值与夹紧、**以及 res -> body 这条接线**（`CreateRigidBody` 造出来的 body 上真的有作者写的材质）。 |
| AQ | 源码级接线门：材质在建体时被读、组合回调被装上、字段进了 `Transfer()`、slim 构建有空实现。 |

AP21–AP23 那条是特意补的：前半段读写的是 `SpawnBox` 直接造的 Jolt body，而脚本改的是
**res**。只测前者的话，"res 从来没被 CreateRigidBody 读过"这种接线缺失会全绿通过。

> ⚠️ 造 res 时必须用 `om->NewObject<Box>()`，不能裸 `new Box()`：
> `PPtr<Geometry>` 按 InstanceID 解析，裸 new 出来的对象 id 为 0，`toShape()` 里
> 那次转换拿到 null，建体静默失败（只有一条 LOG_ERROR，测试表现为"body id 无效"）。
