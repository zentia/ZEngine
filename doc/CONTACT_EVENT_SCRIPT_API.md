# 碰撞 / 触发器事件

TS 侧：

```ts
class MyThing extends Behaviour {
    OnCollisionEnter(other?: GameObject) { Debug.Log("hit " + other?.name); }
    OnCollisionStay(other?: GameObject) {}
    OnCollisionExit(other?: GameObject) {}
    OnTriggerEnter(other?: GameObject) {}
    OnTriggerStay(other?: GameObject) {}
    OnTriggerExit(other?: GameObject) {}
}
```

对位 Unity 的 `MonoBehaviour.OnCollisionEnter/Stay/Exit` 与
`OnTriggerEnter/Stay/Exit`。

---

## 1. 之前的状态：物理层把碰撞整个丢掉了

`PhysicsContactListener::OnContactAdded` 的开头是：

```cpp
if (!body1_is_sensor && !body2_is_sensor)
{
    // Ordinary collision: not a trigger, nothing to report.
    return;
}
```

也就是说**只有 sensor（触发器）会产生事件，普通碰撞一条都不产生**。这是最
难查的一类缺口：触发器那条路是通且有测试的，于是"物理事件"看起来已经做完
了，而 `OnCollisionEnter` 无论框架层写什么都不可能触发 —— 编译得过、跑得
起来、零报错。

`PollTriggerEvents` 有实现、有测试，但**全引擎没有任何调用点**。同样是"实现
了但没接线"。

本片同时补上两边：物理层产生碰撞事件，框架层把两者都派发出去。

---

## 2. 三态 phase：为什么 `Stay` 不能直接来自 Jolt

Jolt 的 `ContactListener` 给三个回调：

| Jolt | 频率 |
| --- | --- |
| `OnContactAdded` | 新接触，一次 |
| `OnContactPersisted` | **每个子步**一次（60Hz 模拟，一个接触跨 3 步就来 3 次） |
| `OnContactRemoved` | 接触消失，一次 |

Unity 的 `OnCollisionStay` 是**每帧一次**。所以直接把 `OnContactPersisted`
排进队列会得到"一帧 3 条 Stay"，而 `OnContactAdded` 只有 Enter —— 中间那一
档必须自己推导。

做法：

* `m_Collisions`：当前还在接触的 pair，几何由 `OnContactPersisted` 持续刷新。
* `m_CollisionEvents`：本帧待发的 enter / exit。
* `m_ActiveLastPoll`：**上一次 poll 时**还在接触的 pair 集合。

`PollCollisionEvents()` 先把 enter/exit 按到达顺序给出去，然后对每个仍在接触
的 pair 检查"上一次 poll 时它也在吗" —— 在就补一条 `stay`。

判据是"上一次**已经接触**"而不是"上一次已经报过 stay"：新撞上的 pair 这一轮
走 enter，下一轮才轮到 stay。否则"刚撞上"那一帧会同时收到 Enter 和 Stay，
而 Unity 只发 Enter。

触发器那条路完全同构（`m_Active` + `m_TriggerActiveLastPoll`）。

### `m_IsEnter` 与 `m_Phase`

`PhysicsTriggerEvent::m_IsEnter` 是 2026-10 之前的唯一判据，二值，表达不了
stay。保留它（现有测试在用），但语义收敛为 `true = Enter`；三态一律读
`m_Phase`。新代码不要用 `m_IsEnter` 判"是不是 exit"。

---

## 3. body id → GameObject

Jolt 的事件只给 `uint32` 的 body id，脚本要的是"撞到哪个 GameObject"。归属表
放在 `PhysicsManager`（不是 `PhysicsScene` —— scene 是纯物理层，Web / slim 构
建连 Jolt 都没有，不该知道 GameObject）：

```cpp
void RegisterBodyOwner(uint32_t body_id, GameObject* owner);
void UnregisterBodyOwner(uint32_t body_id);
GameObject* FindBodyOwner(uint32_t body_id) const;
```

由 `RigidBodyComponent` 在 `CreateRigidBody` / `RemoveRigidBody` / 析构时维护。

⚠️ 析构里的注销**先于**"场景还在吗"的检查：关卡拆除时场景可能已经没了，但
归属表那条记录会留着。body id 是被 Jolt 复用的，留着它的后果是下一次有 body
复用这个 id 时，接触事件派发给一个已经销毁的对象。

---

## 4. 派发

`PhysicsManager::DispatchContactEvents()` 排空每个场景的两个队列，把事件翻成
方法名，然后 `GameObject::SendMessageWithOther(method, other)`。

**两边都发**：Unity 在触发器自己和被触发的那个对象上都调回调。只发一边是常
见误解。

调用点只有一个 —— `Application::LogicalTick` 里固定步循环之后。物理在
`WorldManager::FixedTick` 里推进，事件要等它跑完才存在；而队列是**被排空**
的，所以整个帧里派发两次的第二次什么都拿不到，表现为"回调偶尔不触发"（门禁
R12 断言 `DispatchContactEvents` 在 Application.cpp 里恰好出现 1 次）。

### 带参的 SendMessage

`GameObject::SendMessageByName` 不带参数，而 `OnCollisionEnter(other)` 必须有
`other`。新增：

```
GameObject::SendMessageWithOther(const char*, GameObject*)
  -> Component: TypeScriptComponent::InvokeMethodWithOther
  -> ScriptingManager::InvokeInstanceMethodGameObject
  -> ScriptEnv::InvokeInstanceMethodGameObject   (用 ZScript::Converter<GameObject*>::ToJs)
```

参数走的是绑定表同一条路径，所以脚本拿到的 `other` 与 `gameObject` 属性是同
一种东西。

⚠️ 与 `SendMessageByName` 不同，`SendMessageWithOther` **不**在"没人收到"时
打警告：场景里绝大多数碰撞体身上没有脚本，"没人接"是常态，打出来就是刷屏。

### 没有 `Collision` 对象

Unity 的签名是 `OnCollisionEnter(Collision collision)`，可以读
`collision.contacts[]` / `relativeVelocity` / `impulse`。这里只给 `other`
GameObject。

接触点 / 法线 / 相对速度在 `PhysicsCollisionEvent` 里已经有（`m_Point` /
`m_Normal` / `m_RelativeSpeed`），但没往脚本传 —— 要传就得先在脚本侧有一个
`declare interface Collision` 和对应的 Converter，再让 pesapi 支持"以结构化
值为参数调用方法"。需要的时候按 `RaycastHit` 的先例加即可（见
`doc/PHYSICS_SCRIPT_API.md`）。

---

## 5. 测试

`UnityParitySmokeTest` 的 Q / R 两组（405 项全绿）：

**Q 组（行为）** —— 零重力场景（否则"还接触着吗"取决于落体时间，测试会随帧
率变）：

* 一个 dynamic 撞 static 墙：`Q5` enter、`Q6` 两个 body id 都对、
  `Q7` 接近速度非零。
* 持续接触 10 步：`Q8` 有 stay，`Q9` **没有**重复的 enter。
* 拉开：`Q11` 有 exit。
* sensor + 穿过的 walker：`Q14/Q15/Q16` 三态齐全。
* 归属表往返 + 空派发：`Q17–Q20`。

**R 组（源码级）**：`OnContactPersisted` 存在、两条路径都喂 `Collide`、phase
是三态枚举、主循环恰好一个派发点、两处拆除都注销、`.d.ts` 声明了回调。

### 变异验证（三组互补）

| 变异 | 红灯 |
| --- | --- |
| `OnContactAdded` 恢复成丢掉碰撞 | Q5 Q6 Q7 Q8 Q11 R2 |
| 入队条件改成 `if (true)`（persisted 也排队） | Q9 R2 |
| 去掉两处 stay 的补发 | Q8 Q15（Q5/Q11 仍绿 ⇒ 覆盖互补） |
| 主循环删掉派发调用 | R12 |

---

## 6. 已知缺口

* 脚本侧没有 `Collision` 对象（见上）。
* `OnCollisionStay` 的接触点取 `contacts[0]`，与 Unity 一致（不保证是哪个）。
* `m_Point` 由 Jolt 的 `RVec3` 降到单精度，远离原点的大世界会丢精度 —— 只用于
  表现层。
