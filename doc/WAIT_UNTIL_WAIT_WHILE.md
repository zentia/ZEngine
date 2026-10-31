# WaitUntil / WaitWhile（`yield` 一个闭包）

`engine/Source/Runtime/Function/Framework/Coroutine/ScriptYield.h`
`engine/Source/Runtime/Scripting/Wrapper/ScriptEnv.{h,cpp}`（shim + `GeneratorNext`
+ `CallPredicate`）
`engine/Source/Runtime/Function/Framework/Component/Behaviour.{h,cpp}`（pump 转发）
`engine/Source/Runtime/Function/Framework/Component/Script/TypeScriptComponent.{h,cpp}`
`engine/Source/Runtime/Scripting/Binding/ScriptBinding.cpp`（`.d.ts`）

对照 Unity：`UnityEngine.WaitUntil` / `UnityEngine.WaitWhile`。

在这之前，`ScriptYield.h` 里有一句注释把这两个明确推迟了：

> Coroutine.h's YieldInstruction also carries WaitUntil / WaitWhile ... A script
> can express `WaitUntil` (it is just a predicate), but the closure would
> outlive the frame that created it and there is no safe way to poll it after
> the env has been torn down.

现在这个前提不成立了：**`TypeScriptComponent` 已经握着每个协程的 JS 生成器**
（`m_CoroutineGenerators`），并在 `EndScriptCoroutine` / `TearDown` 里释放它们。
谓词可以进同一张表、走同一套释放时机。

---

## 1. 形状

```typescript
class Gate extends Behaviour {
    ready: boolean = false;

    *MyRoutine(): Generator<YieldInstruction, void, unknown> {
        yield new WaitUntil(() => this.ready);   // 到 ready 变 true 为止
        yield new WaitWhile(() => this.ready);   // 到 ready 变 false 为止
        // ...
    }
}
```

调度器**本来就**支持这两个等待（`Coroutine.cpp` 的 `TickUpdate` 轮询
`std::function<bool()>`）。这一层补的只是"脚本侧那个闭包怎么到轮询点"：

```
yield new WaitUntil(fn)
  -> shim: { __zWait: 'until', fn: fn }
  -> ScriptEnv::GeneratorNext: 为 fn 建 pesapi_value_ref，写进 ScriptYield::predicate
  -> TypeScriptComponent::StepScriptCoroutine: 存进 m_CoroutinePredicates[script_id-1]
  -> Behaviour::ScriptCoroutinePump: co_yield WaitUntil{[self, script_id]{ ... }}
  -> CoroutineScheduler::TickUpdate: 每帧调一次那个 lambda
  -> TypeScriptComponent::EvalScriptPredicate -> ScriptEnv::CallPredicate
```

## 2. 三个"看着对其实会挂"的地方

### 谓词必须每帧被问，不是 yield 那一帧快照一次

`WaitUntil(() => this.ready)` 的契约是"到它翻面为止一直问"。只在 yield 那一帧
调一次、把 bool 存下来，协程就永远挂着 —— 而且位掩码最终也能走到终点（如果
测试只是"多推几帧直到它动"），症状完全是隐形的。测试因此**数轮询次数**
（`untilPolls` / `predicate_polls`），不是只数"走到了第几步"。

### `WaitWhile` 是 `WaitUntil` 的**反**

两个都 `co_yield WaitUntil{...}` 的话，`WaitWhile` 的那个协程在谓词为真时立刻
恢复 —— 也就是第一次轮询就恢复，看起来"工作得很好"。所以两个必须分别测，
且 `WaitWhile` 那条要断言"谓词为真时**挡住**"。

### 强引用的释放点

`yield new WaitUntil(() => ...)` 在循环里每帧造一个**新闭包**。只放生成器、
不放谓词 ⇒ 每帧一个 GC 看不见的泄漏（value ref 是强引用）。

释放点三处，与生成器完全同构：

| 时机 | 做什么 |
|---|---|
| `StepScriptCoroutine`（每次 yield） | 先把上一个谓词放掉，再存新的（不管这次带不带） |
| `EndScriptCoroutine` | 放掉 |
| `TearDown` | 遍历放掉并清空 |

`SetCoroutinePredicate(id, nullptr)` 若槽位已不存在，会把传进来的新谓词**直接
放掉** —— 不能让它挂到下一次复用的槽位上（那会变成"释放了别人的谓词"）。

## 3. 为什么 `predicate_id` 就是 `script_id`

一个运行中的生成器最多有一个活谓词，所以复用 `StartScriptCoroutine` 交出的那个
整数即可。两张表**平行同下标**（`predicate_id == script_id`），分开是因为：

* 谓词可空（大部分 yield 不带），而生成器槽位一空就代表"协程结束了"；
* 混在一张表里，"这个协程还活着但没有谓词"和"协程已经结束"没法区分。

## 4. 降级而不是挂起

`CallPredicate` 抛异常 / `fn` 不是函数 / 拿不到 value ref —— 全部降级成
`ScriptYieldKind::kNull`（"下一帧继续"）并记一条日志。**一个永远不恢复的协程
比一个多跑了一帧的协程难查得多**，这是整个 `__zWait` 家族统一的取舍。

谓词调用时 `this` 传 `undefined`：谓词通常是箭头函数（词法捕获 `this`），传什么
都不影响；而传 `globalThis` 会让 `function () { return this.flag; }` 静默返回
`undefined` 而不是报 TypeError —— 那是 Unity 里"等待永不结束"最常见的一类成因。

## 5. 测试

| 层 | 在哪 | 钉什么 |
|---|---|---|
| 适配器（无 VM） | `UnityParitySmokeTest` BE44–BE53 | `CoroutineProbe` 自己回答 `EvalScriptPredicate`；每帧轮询 + WaitWhile 是反的 |
| 端到端（真 JS） | `ScriptComponentSmokeTest` K0–K13 | `.ts` 里的箭头函数真的被每帧问；`gateReady` 翻面后两个方向各恢复一次 |
| 接线 | `UnityParitySmokeTest` BF33–BF47 | shim 全局、`GeneratorNext` 的两个标记分支、`CallPredicate` 的声明+定义、pump 分别转发、`.d.ts` 两个构造器 |

端到端那段的位掩码沿用 `MyRoutine` 的 1 / 2 / 4，`untilPolls` 是顺序/频率探针。
