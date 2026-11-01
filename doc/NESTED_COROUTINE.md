# 嵌套协程（`yield return StartCoroutine(other)`）

`engine/Source/Runtime/Function/Framework/Coroutine/ScriptYield.h`（`kCoroutine` / `coroutine_id`）
`engine/Source/Runtime/Scripting/Wrapper/ScriptEnv.cpp`（shim 句柄 + `GeneratorNext` 分支）
`engine/Source/Runtime/Function/Framework/Component/Behaviour.cpp`（pump 转发）
`engine/Source/Runtime/Scripting/Binding/ScriptBinding.cpp`（`.d.ts` 的 `Coroutine extends YieldInstruction`）

对照 Unity：`yield return StartCoroutine("Other")`。

调度器早就支持 `WaitForCoroutine{id}`（`Coroutine.cpp` 的 `TickUpdate` 里
`entry.blocked = IsRunning(nested->id)`），缺的只是"脚本 yield 出来的那个句柄
怎么变成 `id`"这一层。

---

## 1. 形状

```typescript
class Chain extends Behaviour {
    *Outer(): Generator<YieldInstruction | null, void, unknown> {
        yield this.StartCoroutine("Inner");   // 等 Inner 整个跑完
        // ...
    }

    *Inner(): Generator<YieldInstruction | null, void, unknown> {
        yield null;
        yield null;
    }
}
```

链路：

```
this.StartCoroutine("Inner")
  -> JS shim: component.StartCoroutine(name) -> id -> new g.Coroutine(id, name)
  -> yield 那个对象
  -> ScriptEnv::GeneratorNext: 读 __zId（数字） -> ScriptYield{kCoroutine, coroutine_id}
  -> Behaviour::ScriptCoroutinePump: co_yield WaitForCoroutine {yielded.coroutine_id}
  -> CoroutineScheduler::TickUpdate: entry.blocked = IsRunning(id)
```

## 2. 句柄**不带** `__zWait`，所以必须在读标记之前判

同一个家族里另外六个（`WaitForSeconds` / `WaitForFixedUpdate` / …）都是
"shim 造一个 `{__zWait: 'xxx'}` 对象，`GeneratorNext` 读标记"。嵌套协程不是：
Unity 里就是直接 yield `StartCoroutine` 返回的那个句柄，没有第二个构造器，
所以 shim 也不给它打 `__zWait`。

`GeneratorNext` 因此按**形状**认（`__zId` 是数字），且这段必须在
`pesapi_get_property(..., "__zWait")` **之前**。顺序反了的话：

* 句柄上 `__zWait` 是 `undefined` ⇒ 落进"未知对象"分支 ⇒ `kNull`；
* 结果是"等内层"静默变成"等一帧"，外层提前一帧继续，**没有任何日志**。

这条顺序在 `UnityParitySmokeTest` 的 BF51 里是靠比较两处在源码里的**位置**
钉住的 —— 只 grep 字符串抓不到顺序。

## 3. `.d.ts` 里 `extends` 不是装饰

```typescript
declare class YieldInstruction { private __zWait: string; }
declare class Coroutine extends YieldInstruction { private __zId: number; ... }
```

`YieldInstruction` 带**私有**字段 ⇒ TypeScript 用名义类型判定：`Coroutine`
只有真的是子类才能塞进 `Generator<YieldInstruction | null, ...>`。不 `extends`
的话用户必须写 `yield c as any`，等于把这一层的类型检查整个关掉。

## 4. 帧序（为什么内层跑完的那一帧外层还醒不过来）

`CoroutineScheduler::ResumePhase` 先把跑完的收进 `finished`，**循环结束之后**
才统一 `Stop(id)`。所以：

| 帧 | `nestedInner` | `nestedSteps` |
|---|---|---|
| `StartNestedRoutine()` 返回时 | 1（同步段） | 1（同步段） |
| Update #1 | 3 | 1（内层还在跑 ⇒ 挡住） |
| Update #2 | 7（跑完，本帧末才被 Stop） | 1 |
| Update #3 | — | 3 |
| Update #4 | — | 7 |

`N6b`（Update #2 结束时外层仍是 1）看起来是在钉一个实现细节，但它的作用是
**帧序哨兵**：谁改了 `Stop` 的时机，`N7` 的帧数就得跟着改，而不是静默偏一帧。

## 5. 语义取舍

* **`id == 0`** 只能是手写 `{ __zId: 0 }`（shim 在 `StartCoroutine` 失败时返回
  的是 `null`，走 `kNull` 那条）。0 就是 `kInvalidId`，调度器永远不会分配它，
  所以 `WaitForCoroutine{0}` 与 `kNull` 行为等价 —— 分支里仍然显式降级并记一条
  日志，因为那说明用户手搓了句柄。
* **内层被 `StopCoroutine` 掉**：`IsRunning` 为假 ⇒ 外层恢复。与 Unity 一致
  （Unity 里 stop 掉被等待的协程，等待方也会继续）。
* **不需要记账**：`coroutine_id` 是调度器的整数 id，不是 JS 值，`predicate`
  那套 `pesapi_value_ref` 释放路径完全不涉及。

## 6. 测试

| 层 | 在哪 | 钉什么 |
|---|---|---|
| 端到端（真 JS） | `ScriptComponentSmokeTest` N0–N9 | 内层跨 3 帧；**内层还在跑的那一帧外层必须停住**（N5 是唯一真判据） |
| 接线 | `UnityParitySmokeTest` BF49–BF55 | `__zId` 的写与读、`__zId` 分支在 `__zWait` 之前、`kCoroutine`/`coroutine_id` 入 POD、pump 转发、`Coroutine extends YieldInstruction` |

"把句柄当成 `yield null`"的实现，N8 的 `nestedSteps` 照样是 7 —— 只有 N5 能
把它抓出来。所以内层必须多于一段 `yield`。
