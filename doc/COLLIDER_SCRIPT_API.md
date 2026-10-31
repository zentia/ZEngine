# 碰撞体脚本 API（`Collider`）+ 命中带回对象

切片 11。补的是物理故事里最后一个洞：此前脚本**拿不到任何碰撞体**，而且射线打中了
也不知道打中的是谁。

## 为什么 `Collider` 不是组件

Unity 的 `Collider` 是独立组件（静态碰撞体 = 只有 Collider 没有 Rigidbody）。
**本引擎不是**：碰撞形状（`RigidBodyShape`）是 `RigidBodyComponent` 资源的一部分，
一个对象要么有刚体（连带它的形状），要么一个碰撞体都没有。

所以 `Collider` 沿用 `Camera` 门面的做法 —— 它是一个**绑在 GameObject 上的视图
对象**，不是新组件：

```ts
const col = Collider.FromGameObject(go);
if (col.isValid) { ... }
```

**不要**写 `go.GetComponent("Collider")`：不存在这个组件类型，它返回 null，而 null
和"这个对象没有碰撞体"看起来一模一样。

不造一个真正的 `ColliderComponent`，是因为那要动序列化、prefab、编辑器、导入器四条
链，还会产生"同一碰撞体两份真值源"（res 里的形状 vs 组件上的字段）。视图的成本差
两个数量级，且没有双真值源。

## API

### 获取

```ts
Collider.FromGameObject(go)   // 同一对象永远返回同一个实例，可以放心缓存
Collider.FromBodyId(hit.bodyId)
```

`FromGameObject` **不会**因为对象没有 RigidBody 而返回 null —— 那种情况返回一个
`isValid == false` 的 Collider。只有传入 null 才返回 null。区分这两者是有意的。

### 形状

```ts
col.shapeCount
col.GetShapeType(i)      // ColliderShapeType.box / sphere / capsule / invalid
col.GetShapeCenter(i)    // 局部偏移
col.GetShapeSize(i)      // 盒：全尺寸
col.GetShapeRadius(i)    // 球 / 胶囊
col.GetShapeHeight(i)    // 胶囊：含帽总高
```

两个换算，都是 Unity 的口径，写错不报错：

| Unity | 本引擎 |
| --- | --- |
| `BoxCollider.size` | **全尺寸** = 2 × 资源里的 `m_HalfExtents` |
| `CapsuleCollider.height` | **含两端半球帽** = 2 × (`m_HalfHeight` + radius) |

后者和 `CharacterController.height` 同一个口径（见
`doc/CHARACTER_CONTROLLER_SCRIPT_API.md`）：直接把 `m_HalfHeight × 2` 当 height，
胶囊会凭空矮掉两个半径。

越界下标一律返回 `invalid` / 零向量，不读越界内存。

### 包围盒

```ts
col.boundsCenter      // Vector3
col.boundsSize        // Vector3（全尺寸）
col.Contains(point)
col.ClosestPoint(point)
```

Unity 的 `collider.bounds` 是一个 `Bounds` 值类型；本引擎没有这个脚本类型，所以拆成
`boundsCenter` / `boundsSize` 两个 Vector3 getter。

**包围盒从资源算，不查物理场景**：有场景没场景答案一致，而且能在 headless 下测。
代价是它反映"按 Transform 摆好之后"的 AABB，不含 Jolt 那一步的膨胀（Unity 的 bounds
也不含）。

三个容易漏掉的细节，都各有一条断言守着：

* **形状的局部偏移**要算（`m_LocalTransform.m_Position`）—— 忘了它就是"碰撞体永远
  在对象原点"。
* **对象的旋转**要算进 AABB —— 一个绕 Z 转 90° 的 4×1×1 盒子，世界 AABB 是 1×4×1
  而不是 4×1×1。漏了它的结果是"歪着放的碰撞体看起来是正的"。
* **所有形状取并集**，不是只看第一个。

`ClosestPoint` 是 **AABB 上**的最近点，不是 Unity 那种精确几何最近点。差别在盒体的
角上最明显。需要精确解时用 `Physics.Raycast` 从那个点往各个方向打。

没有碰撞体时，`ClosestPoint` 返回**点自己**而不是零向量 —— 零向量会被读成"包围盒
在原点"，那是一个完全错误且更难查的答案。

### 触发器

```ts
col.isTrigger = true;
```

走 `PhysicsScene::SetRigidBodySensor` / `IsRigidBodySensor`（Jolt 的 sensor）。
没有活动物理场景时：读返回 `false`，写是空操作。

## 命中带回对象：`RaycastHit.gameObject`

```ts
const hit = Physics.Raycast(origin, dir, 100, Physics.AllLayers);
if (hit.valid && hit.gameObject != null) {
    if (hit.gameObject.CompareTag("Enemy")) { ... }
}
```

Unity 的 `hit.collider.gameObject` / `hit.transform` 是每次射线回来最先被读的两个
字段。只给 `bodyId` 的话，每个从 Unity 工程贴过来的 `hit.transform.CompareTag(...)`
都得改写。

`gameObject` 为 null 表示"这个 body 没有主人" —— 工具直接塞进 Jolt 的纯几何体就是
这种，**不是错误**。

### 实现：只有一个汇流点

`Physics.cpp` 里所有查询（Raycast / *Cast / Overlap*）都汇流到 `ToRaycastHit()`。
所以"命中带回对象"只在那一个函数里补一次，20 个脚本方法一起生效 —— 不用逐个改，
也不会漏。

链路是三节，断任何一节脚本都拿到 `undefined` 而 C++ 侧毫无症状：

```
PhysicsQuery.h  RaycastHit.gameObject 字段
      ↓
Physics.cpp     ToRaycastHit 里 ResolveBodyOwner(body_id)
      ↓
ScriptBinding   Converter<RaycastHit> 写进 JS + .d.ts 声明
```

### ⚠️ 一个真实的坑：Converter 的声明顺序

`Converter<RaycastHit>` 里用到的 `Converter<GameObject*>` 是**非依赖名字**，在模板
**定义点**绑定。所以 `Converter<RaycastHit>` 必须排在「Object 派生指针」那个特化
**之后**，否则它绑到主模板上：

```
static_assert: no ZScript::Converter for this type
```

报错落在绑定表深处，看上去像"GameObject 没有绑定"，实际是声明顺序。AD7 用源码位置
比较（`find("Object-derived pointers") < find("struct Converter<RaycastHit>")`）守着
这个结构 —— 字符串计数守不住它。

## 降级契约

没有 `WorldManager` / 没有活动场景 / 对象没有 RigidBody 时：

* `FromGameObject` —— 返回一个 `isValid == false` 的 Collider（不返回 null）
* 形状 getter —— 0 / 零向量 / `invalid`
* `boundsCenter` / `boundsSize` —— 零向量；`Contains` false；`ClosestPoint` 返回点自己
* `isTrigger` —— 读 false，写空操作
* `FromBodyId` —— 查不到主人就返回 null

## 测试

`UnityParitySmokeTest` 的 **AC 组**（42 项，行为）与 **AD 组**（21 项，接线门）。
AC 组全部走资源读，不碰物理场景，所以能真正断言形状尺寸、包围盒与旋转。

16 个变异，15 个捕获。两个值得记的盲区：

* **M13「转换出去时漏字段」** —— 接线门只数 `"gameObject"` 的话，FromJs 那一行也
  命中，于是漏掉 ToJs 的实现是全绿的。必须数**具体那一行**（`Converter<GameObject*>::ToJs`）。
* **M9「没有碰撞体时 ClosestPoint 返回零向量」** —— 测试里从未对无效碰撞体调过
  ClosestPoint。补了 AC37。

**M10 是等价变异体**：`isTrigger` 的"没有场景"分支在没有活场景的测试里被
`HasBody()` 挡住，永远不可达。代码保留那道防御（`HasBody()` 为真但场景消失是可能
的），但承认它测不到。

总计 813 checks / 0 failures。
