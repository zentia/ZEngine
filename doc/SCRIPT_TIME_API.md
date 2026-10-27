# 脚本侧时间 API（Time）

> 配套：输入查询（键盘 / 鼠标 / 触摸 / action）在 `doc/SCRIPT_INPUT_API.md`。
> 那边有个坑值得先读：帧内量（按键边沿、鼠标位移）由 `EndFrame()` 滚动，脚本
> 读到的是**上一帧**的值。
>
> 端到端（真起 QuickJS VM、从 JS 里读 `Time.*`、并从 JS 调 `SetTimeScale` 反过来
> 影响 C++ 的下一帧）见 `doc/SCRIPT_VM_E2E.md`。

## 1. 先说清楚：脚本并不缺 delta

`TypeScriptComponent::Tick(dt)` 会把本帧步长直接转发给脚本的 `OnUpdate(dt)`：

```typescript
class Mover extends Behaviour {
    speed: number = 3;

    OnUpdate(dt: number) {
        const step = VectorMath.Scale(VectorMath.UnitX(), this.speed * dt);
        this.transform.position = VectorMath.Add(this.transform.position, step);
    }
}
```

所以"每帧移动"这类代码一直写得出来，`Time` 不是为它加的。`Time` 补的是
`OnUpdate` 的参数给不了的四类东西：

| 缺的量 | 为什么 `dt` 顶不了 |
| --- | --- |
| 累计时间 | `dt` 只在当前这一次调用里有，累加要自己存字段，而且每个脚本各存一份、各错一次 |
| 帧数 / 帧率 | 与帧绑定的全局计数，脚本侧无从得知 |
| **timeScale** | 脚本唯一能改变"世界推进速度"的手段，之前根本不存在 |
| 非 Tick 上下文的 delta | `OnAwake` / `OnStart` / 事件回调里没有 `dt` 参数 |

## 2. 为什么是 `Time.GetDeltaTime()` 而不是 `Time.deltaTime`

Unity 的 `Time.deltaTime` 是**静态属性**。这里的 `ZSCRIPTPROPERTY` 不支持静态
访问器，`zht/parser.py` 会直接报错：

```
ZSCRIPTPROPERTY '%s': getter '%s' must not be static
```

所以只能导出成静态方法。这是工具链约束，不是笔误 —— 和 `VectorMath` 用静态
方法的原因（`ZCLASS` 必须派生 `Object`，`Vector3` 进不了绑定表）是同一类取舍。

| Unity | 这里 |
| --- | --- |
| `Time.deltaTime` | `Time.GetDeltaTime()` |
| `Time.unscaledDeltaTime` | `Time.GetUnscaledDeltaTime()` |
| `Time.smoothDeltaTime` | `Time.GetSmoothDeltaTime()` |
| `Time.time` | `Time.GetTime()` |
| `Time.unscaledTime` | `Time.GetUnscaledTime()` |
| `Time.frameCount` | `Time.GetFrameCount()` |
| `Time.timeScale` | `Time.GetTimeScale()` / `Time.SetTimeScale(v)` |

## 3. 导出的 API（11 个）

```
GetDeltaTime(): number            本帧逻辑步长（秒），受 timeScale；暂停时为 0
GetUnscaledDeltaTime(): number    真实帧间隔（秒），不受 timeScale、不受暂停
GetSmoothDeltaTime(): number      平滑帧间隔（秒），EMA alpha = 1/100
GetTime(): number                 逻辑时间累计（秒），受 timeScale
GetUnscaledTime(): number         真实时间累计（秒）
GetFrameCount(): number           已跑帧数（含暂停帧）
GetFps(): number                  平滑帧率
GetTimeScale(): number            1 = 正常，0 = 冻结
SetTimeScale(scale: number): void 负数被夹到 0
ResetTimeScale(): void           还回 1（不动累计时间与帧数）
IsLogicUpdating(): boolean        本帧是否推进逻辑（编辑器暂停时为 false）
```

量化（`ZHeaderTool generate --dump-ir`）：类 70 → 71，方法 265 → 276，静态方法
52 → 63，缺 Converter 0。

## 4. timeScale 的语义 —— 引擎真的按它走

`SetTimeScale(0.5)` 不是"脚本读到的数变小了"。`Application::TickOneFrame` 每帧
从 `TimeState` 取出**缩放后的步长**，用它驱动 `LogicalTick`（世界 tick、物理、
动画）和 `RendererTick`。所以脚本读到的 `GetDeltaTime()` 和世界实际用的
**是同一个数**，不存在"物理照常、脚本慢放"的不一致。

代价要说清楚：渲染侧吃的是同一个缩放后的步长，所以 `timeScale = 0` 时纯渲染
驱动的效果也停。要"暂停时 UI 继续动"，用 `GetUnscaledDeltaTime()` 自己算。

**默认值零回归**：`timeScale` 默认 1.0、`should_update_logic` 为 true 时，
缩放后的步长与改动前传入的 `simulation_delta_time` 完全相等，帧循环行为不变。

### 两个坑

1. **timeScale 是全局持久的**，不随关卡卸载重置。编辑器里：
   * 进入 Play（`Editor::SetPlaybackState`）→ `TimeState::Reset()`，时间与帧数
     归零、timeScale 回 1；
   * 退出 Play → `ResetTimeScale()`，只还 timeScale。
   编辑模式下脚本组件也会 Tick（`g_editorTickComponentTypes` 里放了
   `TypeScriptComponent`），所以在编辑模式设过 timeScale 要自己还回去，否则
   编辑模式的逻辑按倍速走，表现像"编辑器变卡"。
2. **负数被夹到 0**。负的时间步长会让物理积分和动画采样往回走，是静默的错误
   数据，不如直接拒绝。同理，负的帧 delta 也夹到 0（时钟回拨防御）。

## 5. 状态放在哪：为什么不直接读 Application

时间状态在 `Runtime/Core/Time/TimeState.h`，是一个静态单例，不是
`IEngineSystem`。两个理由：

* **可测**：`Application` 只有系统注册表起来之后才有实例。`TimeState` 是纯数据
  + 纯算术，冒烟测试可以直接 `BeginFrame(...)` 推进若干帧并断言，headless。
* **谁都能读**：`Time.*` 是静态方法，拿不到也不该拿 `Application` 实例。

唯一写者是 `Application::TickOneFrame`（每帧开头一次），唯一例外是
`SetTimeScale`。

`Application::CalculateFPS` 自己也有 `m_FrameCount` / `m_Fps`（窗口标题用），
算法和 `TimeState` 一样。两处并存是有意的 —— 合并就必须让 `TimeState` 依赖
系统注册表，可测性就没了。

## 6. 冒烟测试

`engine/Source/Runtime/Scripting/ScriptTime/Test/ScriptTimeSmokeTest.cpp`，
57 项检查，覆盖 timeScale 缩放（T4/T5/T10）、暂停帧 delta 必须为零但帧数照涨
（T7）、负 delta 与负 timeScale 夹零（T6/T8）、`Reset` 与 `ResetTimeScale`
的区别（T11/T12）、脚本 API 与内部状态一致（T13/T14）。

有 cmake 的机器：

```bash
cmake -B build -DZENGINE_BUILD_SCRIPT_TIME_SMOKE_TEST=ON
cmake --build build --target ScriptTimeSmokeTest
ctest --test-dir build -R ScriptTimeSmokeTest
```

没有 cmake 的机器（`TimeState.cpp` 是新文件，旧的 `libZRuntime.a` 里没有它的
符号，要用 `--extra-source` 一起编）：

```bash
python3 scripts/run_smoke_test.py \
    engine/Source/Runtime/Scripting/ScriptTime/Test/ScriptTimeSmokeTest.cpp \
    --extra-source engine/Source/Runtime/Core/Time/TimeState.cpp
```

负向验证：把 `TimeState::BeginFrame` 里的 `simulation_delta_time * m_TimeScale`
改成 `simulation_delta_time`（去掉缩放），9 项检查失败（T4 / T5 / T6b / T6c /
T10 / T13i），之后已还原。

## 7. 刻意没做的

| Unity API | 为什么不做 |
| --- | --- |
| `Time.fixedDeltaTime` / `FixedUpdate` | 引擎**没有**固定步长循环（全仓无 `FixedUpdate` / `fixed_delta`）。导出一个没人消费的常量就是假 API |
| `Time.realtimeSinceStartup` | 与 `GetUnscaledTime()` 是同一个量，重复 |
| `Time.captureFramerate` / `Time.maximumDeltaTime` | 前者是录屏用的（引擎没有），后者是防螺旋死亡的钳制 —— 现在调用方只有 `Application`，加钳制是多一个没人验证的分支 |
