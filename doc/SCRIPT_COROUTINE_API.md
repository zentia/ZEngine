# 脚本协程（StartCoroutine / yield）

Unity 的协程表面在 TypeScript 里的完整写法：

```ts
class Mover extends Behaviour {
    OnStart(): void {
        // 句柄可以丢掉，也可以留着喂给 StopCoroutine
        this.StartCoroutine("FadeOut");
    }

    *FadeOut(): Generator<YieldInstruction, void, unknown> {
        yield new WaitForSeconds(0.5);   // 缩放时间，timeScale=0 时不走
        yield new WaitForEndOfFrame();   // 所有 Update 之后、present 之前
        this.Done();
    }
}
```

## 一、适配器：一个 C++20 协程对一个 JS 生成器

```
Behaviour::StartCoroutine("FadeOut")
  └─ TypeScriptComponent::StartScriptCoroutine  -> ScriptEnv::InvokeInstanceMethodReturning
        （调 instance.FadeOut()，把返回的生成器攥在 m_CoroutineGenerators 里）
  └─ CoroutineScheduler::Start(ScriptCoroutinePump(this, script_id))

ScriptCoroutinePump 每次被 resume：
  └─ ScriptEnv::GeneratorNext  -> generator.next() 一次
  └─ 把脚本 yield 出来的值翻译成引擎的 YieldInstruction，co_yield 出去
```

`CoroutineScheduler` 完全不知道 JS 的存在，所以四相位、timeScale、
`StopAllForOwner` 全部照旧工作。

### 为什么只提供字符串重载

Unity 有两个重载：`StartCoroutine(IEnumerator)` 和 `StartCoroutine(string)`。
生成器对象穿不过绑定表（表里只有引擎自己的类型），所以只有字符串版。这不是凑
数的替代品 —— 字符串重载本身就是 Unity 的公开 API。

## 二、ScriptYield.h：为什么单独一个头

`ScriptYield` 要穿过两个方向相反的边界：

```
ScriptEnv.cpp（持有 pesapi） -> Behaviour / TypeScriptComponent -> Coroutine.h
```

`Behaviour.h` 被几乎每个组件间接包含，把 pesapi.h 拖进去等于把 QuickJS 拖进
每一个编译单元。所以它是个零依赖的 POD，两侧各自在边界上转换。

它**故意不是**完整的 `YieldInstruction`：不带 `WaitUntil` / `WaitWhile` /
`WaitForCoroutine`，因为那些要持有 `std::function` 或引擎侧 id，而脚本闭包在
创建它的那一帧之后就没人能安全地轮询了。

未知 / 畸形的值一律降级成 `kNull`（下一帧继续），**绝不**降级成"永久挂起"。
一个静默再也不动的协程比一个早跑一帧的协程难查得多。

## 三、生成器的所有权：两条出口，一次释放

一个协程可能以两种方式结束：

| 出口 | 谁在跑 | 谁必须释放 |
|---|---|---|
| 协程体自己跑完 | 泵走到自己的 `co_return` | 泵（`ReleaseScriptCoroutine`） |
| 有人调了 `Stop*` | 调度器直接丢掉协程帧，泵**永远到不了**出口 | `StopCoroutine` / `StopAllCoroutines` / 析构 |

"谁先到谁释放"会导致双释放；`PendingCoroutine::generator_owned` 这个标志位让
释放恰好一次。

⚠️ 少了泵那一侧释放会怎样：自然结束的协程会一直攥着 JS 值，直到将来某次
`Start` / `IsCoroutineRunning` 顺手剪枝 —— 而那两个调用可能永远不来。
`UnityParitySmokeTest` 的 **BE12b** 就是钉这个的（它在"没有任何人触发剪枝"
的前提下断言 `released == 1`）。

反过来，`ScriptComponentSmokeTest` 的 **H9b** 钉 `done` 真的把条目摘掉了：
`GeneratorNext` 如果永远报告 `done=false`，泵会一直 `next()`，步数停在 7 看
起来"跑完了"，但调度器里多了一个永不结束的条目。

## 四、相位

| 脚本写 | 引擎 resume 自 |
|---|---|
| `yield null` / `yield undefined` / `yield 0.5` | `TickUpdate`（数字按 WaitForSeconds 处理） |
| `new WaitForSeconds(s)` | `TickUpdate`，**缩放**时间 |
| `new WaitForSecondsRealtime(s)` | `TickUpdate`，墙钟时间（timeScale=0 也走） |
| `new WaitForFixedUpdate()` | `TickFixedUpdate` |
| `new WaitForLateUpdate()` | `TickLateUpdate` |
| `new WaitForEndOfFrame()` | `TickEndOfFrame` |

相位错了是**静默**的：等 EndOfFrame 却在 Update 里 resume，照样产出一个值，
只是晚了一帧。`ScriptYield.kind` 存的是相位本身而不是推断出来的，四个 `Tick`
各看各的。

## 五、API

| TypeScript | C++ |
|---|---|
| `this.StartCoroutine(name: string): Coroutine \| null` | `Behaviour::StartCoroutine` |
| `this.StopCoroutine(handle: Coroutine): void` | `Behaviour::StopCoroutine(name)` |
| `this.StopCoroutine(name: string): void` | 同上（JS shim 从句柄里取方法名） |
| `this.StopAllCoroutines(): void` | `Behaviour::StopAllCoroutines` |
| — | `Behaviour::IsCoroutineRunning(name)`（`ZFUNCTION` 名 `IsCoroutineRunning`） |

`StartCoroutine` 的第一段是**同步**跑完的（返回时已经跑到第一个 yield），和
Unity 一样。失败返回 `null` 并记一条日志：空名字、没有调度器、那个方法没返回生成器。

`Behaviour` 析构时自动 `StopAllCoroutines()`：一个比宿主活得久的协程会在下一帧
resume 进已经释放的脚本状态。

## 六、测试分布

| 位置 | 覆盖 |
|---|---|
| `UnityParitySmokeTest` BE | 适配器。`CoroutineProbe` 用一串预设的 yield 回答三个钩子 —— 从 `Behaviour` 那边看，它**就是**一个生成器，所以这一段完全不需要 VM |
| `UnityParitySmokeTest` BF | 接线门（声明 / ZFUNCTION / shim / .d.ts） |
| `ScriptComponentSmokeTest` H | **端到端**。上面每一条 C++ 断言在 JS shim 全错的情况下都能保持绿色，只有这一组能证明 `__zWait` → ScriptEnv → 调度器真的接上了 |

变异用例见 `.mutate_sc.py`（20 个，全部被抓）。
