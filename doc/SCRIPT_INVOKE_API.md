# 脚本侧延时调用（Invoke / InvokeRepeating / CancelInvoke）

> 接口清单见 `engine/Source/Runtime/Function/Framework/Component/Behaviour.h`。
> 本文只讲**为什么是协程**和**踩过的坑**。

## 1. 为什么定时器是协程，而不是 `Behaviour::Tick` 里的一个列表

最直觉的做法：在 `Behaviour` 里放 `eastl::vector<PendingInvoke>`，然后 override
`Tick(dt)` 递减、触发。

**不行**，因为三个 `Behaviour` 子类里有两个 override 了 `Tick`
（`TypeScriptComponent`、`AIControllerComponent`）。泵放在 `Behaviour::Tick`
里，就只有"记得调 `Super::Tick` 的子类"才会走到；忘了的那一个：

* 能编译
* 能运行
* `Invoke` 静默永不触发，零日志

这正是接线门要抓的那类失败。所以改成把定时器交给 `CoroutineScheduler`——
它由 `Application::LogicalTick` 每帧驱动，与组件是否 override `Tick` 无关，
**从结构上消除了"子类必须配合"这个前提**。

附带好处：延时用 `WaitForSeconds`，与 Unity 一样走**缩放时间**（timeScale 生效）。

## 2. ⚠️ ZFUNCTION 不能打在 C++ 的 `Behaviour` 上

第一版就是这么写的，然后 `ReflectionCodegenSmokeTest` 红了：

```
BEHAVIOUR: synthetic class 'Behaviour' collides with a bound class
```

原因：`Behaviour` **也是**合成脚本类的名字（`ScriptBinding.cpp` 的
`g_BehaviourClass`，给 TS 行为脚本提供 `gameObject` / `transform` / `component`
和生命周期钩子）。C++ 类一旦有 ZFUNCTION 就会被发布成绑定类，`.d.ts` 里就出现
第二个 `declare class Behaviour` —— tsc 报重复标识符，而报错发生在用户工程里，
完全指不回引擎。

三级结构（改动后）：

| 层 | 名字 | 职责 |
| --- | --- | --- |
| C++ `Behaviour` | 不发布 | 定时器状态、取消记账、投递钩子；**没有 ZFUNCTION** |
| C++ `TypeScriptComponent` | `TypeScriptComponent` | 5 个 ZFUNCTION 转发到基类 |
| JS `g.Behaviour.prototype`（`ScriptEnv.cpp`） | `Behaviour` | `this.Invoke(...)` → `this.component.Invoke(...)` |
| 合成类 `.d.ts` 行（`ScriptBinding.cpp`） | `Behaviour` | 四个方法签名，仅供 tsc |

于是 TS 侧就是 Unity 的写法：

```typescript
class Blinker extends Behaviour {
    OnStart() { this.InvokeRepeating("Blink", 1.0, 0.5); }
    Blink() { /* ... */ }
}
```

⚠️ `.d.ts` 里那几行**只是声明**，没有实现就不转发，会在运行时炸。所以接线门
K4e/K4f（声明存在）+ K4h/K4i（`ScriptEnv` 里原型真的转发了）是成对的两条。

## 3. 投递路径

```
this.Invoke(name, t)                       // TS
  └─ Behaviour.prototype.Invoke            // ScriptEnv 的 JS 转发
       └─ TypeScriptComponent::Invoke      // ZFUNCTION
            └─ Behaviour::Invoke
                 └─ CoroutineScheduler::Start(InvokeOnceRoutine{...})
                      └─ co_yield WaitForSeconds{t}
                           └─ Behaviour::FireInvoke(name)
                                ├─ InvokeBehaviourMethod(name)  // TypeScriptComponent 覆写 → 自己的 JS 实例
                                └─ 否则 GameObject::SendMessageByName(name)
```

`InvokeBehaviourMethod` 默认返回 false，所以非脚本的 `Behaviour`
（`AIControllerComponent`、`RenderCamera`）走广播路径 —— 这也是 Unity 的语义：
`Invoke` 打在自己身上，`SendMessage` 才是广播。

## 4. 参数约定（与 Unity 对齐）

| 情形 | 行为 |
| --- | --- |
| `time <= 0` | 拒绝并打日志，**不排队** |
| `repeatRate <= 0` | 同上（0 会让协程在一帧内空转） |
| `methodName` 空 | 拒绝 |
| `CancelInvoke("")` | 取消该 Behaviour 上**全部**（Unity 的 `CancelInvoke()` 无参写法；pesapi 表达不了"无参"，用空串顶） |
| `CancelInvoke("不存在的名字")` | 逐个比对，不匹配则什么也不取消（不会退化成"全取消"） |

⚠️ `time <= 0` 一定要**拒绝而不是立即调用**：立刻调用会让调用点看起来生效了，
然后在作者早已忘记的某个地方"莫名触发一次"。

## 5. 生命周期

`~Behaviour()` 调 `CancelAllInvokes()`。协程持有的是 `Behaviour*` 裸指针，
组件析构后那一帧若协程还活着就是 use-after-free —— 所以这一步是必须的，
不是清理卫生。

同理，协程里**不持 PPtr**：持 PPtr 会让"已取消但还在队列里"的协程把这个组件
多续一帧命，Unity 不是这个行为。

## 6. 测试与门禁

`UnityParitySmokeTest` 的 J（行为）/ K（接线门）两组。变异验证互补：

| 变异 | 结果 |
| --- | --- |
| `InvokeOnceRoutine` 不 yield（忽略延时） | J4 / J6 / J7 红 |
| `CancelInvoke` 只从表里移除、不 `scheduler->Stop()` | J13 / J17 红 |

## 7. 已知缺口

* `IsInvoking()` 无参版（Unity 的"是否有任何 invoke"）没做，只能按名字问。
* 取消后 `m_Invokes` 里已结束的条目靠 `PruneFinishedInvokes()` 惰性清理
  （`IsInvoking` / 取消时调用），不会主动回收 —— 数量极小，可接受。
