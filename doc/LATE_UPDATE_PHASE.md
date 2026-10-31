# LateUpdate 阶段 / OnEnable / OnDisable

`engine/Source/Runtime/Function/Framework/Component/Component.h`（`LateTick`）
`engine/Source/Runtime/BaseClasses/GameObject.{h,cpp}`（`GameObject::LateTick`）
`engine/Source/Runtime/Function/Framework/Level/Level.{h,cpp}`（`Level::LateTick`）
`engine/Source/Runtime/Function/Framework/World/WorldManager.{h,cpp}`
`engine/Source/Runtime/Application/Application.cpp`（调用点）
`engine/Source/Runtime/Function/Framework/Component/Script/TypeScriptComponent.{h,cpp}`
`engine/Source/Runtime/Scripting/Binding/ScriptBinding.cpp`（`kBehaviourMethodLines`）
`engine/Source/Runtime/Scripting/Wrapper/ScriptEnv.cpp`（`Behaviour.prototype`）

对照 Unity：`MonoBehaviour.LateUpdate()` / `MonoBehaviour.OnEnable()` /
`MonoBehaviour.OnDisable()`。

在这之前，`LateTick` 在整个 `engine/` 里是零命中的；`OnEnable` / `OnDisable`
同样一个都没有（`Component::OnOwnerActiveChanged` 在，但那是渲染层去挂/摘
render entity 用的，脚本侧没有对应物）。

---

## 1. 一帧的形状

```
InputSystem::Tick
  └─ 固定步循环（0..n 次，步长恒定）
       ├─ WorldManager::FixedTick  -> Level::FixedTick -> GameObject::FixedTick
       │                                                └─ 物理
       └─ CoroutineScheduler::TickFixedUpdate
  ├─ PhysicsManager::DispatchContactEvents
  ├─ WorldManager::Tick            -> Level::Tick      -> GameObject::Tick
  ├─ CoroutineScheduler::TickUpdate
  ├─ WorldManager::LateTick        -> Level::LateTick  -> GameObject::LateTick   ← 本次新增
  └─ CoroutineScheduler::TickLateUpdate
```

`LateTick` 插在 `TickUpdate` **之后**、`TickLateUpdate` **之前**。反过来的话，
"等 LateUpdate 之后再继续"的协程会早于组件的 LateUpdate 恢复 —— 那种协程通常
是去读相机位姿（而相机位姿正是在 LateUpdate 里更新的），于是拿到上一帧的值。

### `Level::LateTick` 相对 `Level::Tick` 有意省略的三件事

| | Tick | LateTick | 为什么 |
|---|---|---|---|
| 物理 | 不推进 | 不推进 | 物理只在 `FixedTick` 里（见 `doc/FIXED_TIME_STEP.md`） |
| `Character::Tick` | 调 | **不调** | 那是每帧一次的相机跟随 / 输入快照，一帧动两遍 |
| `TickDestroyTimers` + `FlushPendingDestroys` | 调 | **不调** | 销毁在帧末统一收口 |
| `GetTransformChangeDispatch().DispatchChanges()` | 调 | **调** | 相机在 LateUpdate 里改了自己的位置，这一帧渲染必须看到新位姿 —— 否则"慢一帧的跟随"又回来了，而那正是 LateUpdate 被发明出来要消灭的抖动 |

---

## 2. 三个 Tick 的启用判定必须逐字一致

`GameObject::Tick` / `FixedTick` / `LateTick` 三个函数体是同一个形状：

```cpp
if (!IsActiveInHierarchy())            return;
for (auto& component : m_Components) {
    if (component == nullptr || !component->isEnabled()) continue;
    if (!shouldComponentTick(component->GetTypeName())) continue;
    component->XTick(delta_time);
}
```

这不是复制粘贴的懒惰，是**不变式**。少判任何一条的后果不是"少调一次"，而是
"Update 跑了、LateUpdate 没跑" —— 组件状态于是静默地在两套节奏之间错位，而且
只在特定帧率 / 特定激活状态下复现。

机器可读的守门见 `UnityParitySmokeTest` 的 `LUW11` / `LUW12`（数
`IsActiveInHierarchy()` 与 `shouldComponentTick(` 的出现次数）。

---

## 3. 脚本面

```typescript
class FollowCamera extends Behaviour {
    OnUpdate(dt: number): void      { /* 自己动 */ }
    OnLateUpdate(dt: number): void  { /* 追别人这一帧的最终位姿 */ }
    OnEnable(): void                { /* 订阅 */ }
    OnDisable(): void               { /* 退订 */ }
}
```

四个方法都在 `Behaviour.prototype` 上有空实现，所以脚本可以只实现需要的那几个
（与 Unity 一致）。

`OnLateUpdate(delta_time)` 拿的是**帧间隔**，与 `OnUpdate` 同一个值，不是固定
步长。

### OnEnable / OnDisable 的触发条件

是**合取** `enabled && activeInHierarchy`（Unity 的 `Behaviour.isActiveAndEnabled`），
不是"某一个条件变了就发"：

| 操作 | 结果 |
|---|---|
| `component.enabled = true` 且对象在层级里开着 | `OnEnable` |
| 对象关着时把 `enabled` 来回拨 | **什么都不发** |
| `SetActive(true)` 且组件 enabled | `OnEnable` |
| 组件 disabled 时 `SetActive(true)` | **什么都不发** |

实现上就是 `TypeScriptComponent::RefreshEnabledState()` 一个函数，
`SetEnabled()` 与 `OnOwnerActiveChanged()` 都只调它 —— 两条路各自去判"是不是
自己那条变了"就会漏掉"两个条件同时满足不同方向变化"的组合。

顺序（Unity 同）：

* 绑定时：**`OnAwake` → `OnEnable` → `OnStart`**
* 销毁时：**`OnDisable` → `OnDestroy`**

`OnDisable` 在 `OnDestroy` 之前是有意的：销毁中的对象在脚本看来"已经不 enabled
了"，任何在 `OnDisable` 里做的清理（注销事件、还回对象池）因此发生在还能安全
访问 `this` 的最后时刻。

---

## 4. 测试里两个"看着测了其实没测"的坑

1. **`GameObject`）的探针组件必须报得出一个已注册的类型名。**
   `shouldComponentTick(component->GetTypeName())` 每趟都会求值，而
   `Object::GetTypeName()` 走 `GetRuntimeTypes().types[m_CachedTypeIndex]` ——
   数组只有 1024 项，未注册类型的索引是 `Type::DefaultTypeIndex`
   (0x80000000)。不是返回空串，是**越界读**。
   `UnityParitySmokeTest` 的 `LateTickProbe` 因此故意不带 `REGISTER_CLASS`：
   继承 `Component` 的那份 `GetTypeVirtualInternal()`，报 "Component"，而
   `Component` 是 `RegisterRuntimeTypesForTest()` 显式注册过的。

2. **顺序断言要跨组件。** 只测"同一个组件上 Update 在 LateUpdate 之前"（日志
   `UL`）分不出"GameObject 先跑完所有 Tick 再跑 LateTick"和"在 Tick 的循环体
   里顺手调了 LateTick" —— 后者在一个组件上也给出 `UL`。测试挂了两个探针，
   断言共享日志是 `aubualbl` 而不是 `aualbubl`。
