# 物理查询脚本 API（Physics）

> 目标：Unity 的 `Physics` 静态类在 ZEngine 里的对应物，TypeScript 可调用。
> 本文只记**会踩的坑**和**为什么这么设计**；接口清单直接看
> `engine/Source/Runtime/Function/Physics/Physics.h`（每个方法都有注释）。

## 1. 最重要的一条：Jolt 的 object layer == Unity 的 layer

改之前 `Layers` 是个 8 值枚举（`NON_MOVING` / `MOVING` / `DEBRIS` / `SENSOR`），
和 Inspector 里选的层**毫无关系**。后果是：

* `Physics.Raycast(..., layerMask)` 没有层可过滤 —— 只能全收或全不收；
* `TagLayerManager` 的层碰撞矩阵写了位，但**没有任何代码读它**
  （典型的"实现了但没接"：能编译、能运行、零报错）。

现在 `PhysicsScene::CreateRigidBody()` 把 GameObject 的 Unity 层（0..31）直接
写进 Jolt 的 object layer：

| 位置 | 消费方式 |
| --- | --- |
| `Jolt/Utils.cpp` `ObjectLayerPairFilterImpl::ShouldCollide` | 碰撞裁决查 `TagLayerManager::GetIgnoreLayerCollision`（**矩阵唯一的消费点**） |
| `PhysicsScene::FilterHitsByLayer` | 每次查询按 `layerMask` 过滤命中，并回填 `hit.layer` |
| `PhysicsScene::GetBodyLayer` / `SetBodyLayer` | 运行时改层（`GameObject.layer = x` 立即生效） |

motion type（static / kinematic / dynamic）是 Jolt 的另一个概念，仍由
`RigidBodyActorType` 决定，与层无关。

**坑**：层号超范围时 `CreateRigidBody` 回落到 0 而不是夹取到别的值 —— Jolt 的
`BroadPhaseLayerInterface` 里 `ASSERT(inLayer < NUM_LAYERS)` 在 Debug 下是真的会
断的，传 47 进去是崩溃而不是"没打中"。

## 2. 为什么 `Physics` 是 `ZCLASS(Abstract)`

Unity 的 `Physics` 是静态类。这里用 `ZCLASS(Abstract)` 是为了拿到
`REGISTER_CLASS_TRAITS(kTypeIsAbstract)` —— 脚本 `new Physics()` 会被拒。
.d.ts 生成的是 `declare class Physics { static ... }`，拼写与 Unity 一致。

**不支持静态属性**（`Physics.gravity`）：`ZSCRIPTPROPERTY` 的 getter 不能是
static（`zht/parser.py` 会报错），所以拆成 `GetGravity()` / `SetGravity()`。

## 3. 为什么 `Raycast` 返回 `RaycastHit` 而不是 bool

Unity 是 `Physics.Raycast(ray, out hit)` 返回 bool。pesapi 这一侧的参数只有
`FromJs` 没有写回通道，out 参数做不出来，于是让记录自己带 `valid`：

```typescript
const hit = Physics.Raycast(origin, dir, 100, -1);
if (hit.valid) { /* hit.point / hit.normal / hit.distance / hit.bodyId / hit.layer */ }
```

`RaycastHit` 定义在 **`PhysicsQuery.h`**，不是 `PhysicsScene.h`。原因是
`ZScript::Converter<RaycastHit>` 要进 `ScriptBinding.h`，而 `PhysicsScene.h` 会
拖进 `Jolt/Jolt.h` —— 那会把它灌进引擎每一个 `.gen.cpp`，并且 Web / slim-iOS
这两个**故意不链 Jolt** 的构建会直接编不过。

同理，`Sweep` / `IsOverlap` 收的是 `RigidBodyShape`（需要 ObjectManager 造引擎
对象），脚本侧没有，所以另开了 `CastPrimitive` / `OverlapPrimitive`，吃一个
`PhysicsPrimitiveDesc` 值类型，**一次查询零分配**。

## 4. layerMask 的语义（与 Unity 逐条对齐）

| 写法 | 含义 |
| --- | --- |
| `-1` | 所有层（Unity 的 `Physics.AllLayers`） |
| `0` | **一层都不测**（Unity 也是这个语义，不是"全部"） |
| `1 << n` | 只测第 n 层 |
| 负数（非 -1） | 按位掩码解释；`GetCollisionLayerMask` 返回的就是这种 int |

`maxDistance <= 0` 视作 Infinity（内部夹到 1e6，Jolt 不接受无穷）。

## 5. 测试与门禁

`UnityParitySmokeTest` 的 H / I 两组：

* **H（行为）**：真起 `PhysicsScene` + 真 Jolt 刚体，验证命中排序、掩码过滤、
  重叠、胶囊朝向，以及**矩阵真的挡住了碰撞**（H41：排除 8/9 后动态刚体直接
  穿过静态刚体落到 y < 0）。
* **I（接线门）**：源码级，钉住"过滤函数被每个查询调用""Jolt 的 pair filter
  真的引了 TagLayerManager""CreateRigidBody 每条路径都传了层"。

变异验证过两组互补：

| 变异 | 结果 |
| --- | --- |
| `FilterHitsByLayer` 的掩码判断改成 `if (true)` | H9/H11/H12/H17/H21/H27/H48 红（7 条） |
| `ObjectLayerPairFilterImpl::ShouldCollide` 直接 `return true` | H41 + I9 红（2 条） |

## 6. 坑：别在持读锁时调 BodyInterface

`SetBodyLayer` 先用 `BodyLockRead` 判存在，**必须让它出作用域**再调
`body_interface.SetObjectLayer()`（后者拿写锁）。Jolt 会断言
"a lock of same or higher priority was already taken" —— 它是对的，真会死锁。
第一版就是这么写的，H46 直接把测试进程挂死。

## 7. 已知缺口

* `OverlapSphere` 等返回 `RaycastHit[]` 而不是 Unity 的 `Collider[]`；
  `bodyId` 目前只能和 `RigidBodyComponent.GetBodyId()` 比，没有
  `bodyId -> GameObject` 的反查（需要在场景里维护反表，未做）。
* `Sweep` / `IsOverlap`（`RigidBodyShape` 版）也支持了 `layer_mask`，但脚本侧
  走不到，只有 C++ 调用方在用。
* 形状仍然只有 box / sphere / capsule，没有 MeshCollider / ConvexHull。
* 没有碰撞事件（OnCollisionEnter/Stay/Exit）和物理材质。
