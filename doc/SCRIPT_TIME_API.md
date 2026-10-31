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

## 2. 拼写与 Unity 一致：`Time.deltaTime`

`Time.deltaTime` 是**静态属性**，和 Unity 的写法一字不差。

以前这里是 `Time.GetDeltaTime()`：`ZSCRIPTPROPERTY` 不接受静态访问器
（`zht/parser.py` 直接报 "getter must not be static"），只能落成静态方法。
`Color.red` 那一档把静态属性做出来了（静态属性走 pesapi 的 `Variables` 表，
后端把它装到**构造器**上；实例属性走 `Properties` 装到 prototype），于是这里
换回 Unity 的拼写。

**Get* 方法在 C++ 里照旧保留**（`ZSCRIPTPROPERTY` 的 getter 本来就得是个
`ZFUNCTION`），但**不再出现在 .d.ts 里**：静态访问器会被 `codegen.py` 从方法表
摘掉。不摘的话 MSVC 把同一个 `MethodCallback` 编进两个 COMDAT 段，链接器直接
拒收整个 obj（LNK1179，见 `doc/COLOR_SCRIPT_API.md`）。

所以：TS 写 `Time.deltaTime`，C++ 写 `Time::GetDeltaTime()`，两边各自是本语言
的习惯写法。

| Unity | 这里（.d.ts） | C++ |
| --- | --- | --- |
| `Time.deltaTime` | `Time.deltaTime` | `Time::GetDeltaTime()` |
| `Time.unscaledDeltaTime` | 同左 | `GetUnscaledDeltaTime()` |
| `Time.smoothDeltaTime` | 同左 | `GetSmoothDeltaTime()` |
| `Time.time` | 同左 | `GetTime()` |
| `Time.unscaledTime` | 同左 | `GetUnscaledTime()` |
| `Time.realtimeSinceStartup` | 同左（同一个量，另一个 getter） | `GetRealtimeSinceStartup()` |
| `Time.frameCount` | 同左 | `GetFrameCount()` |
| `Time.timeScale`（可写） | 同左 | `GetTimeScale()` / `SetTimeScale(v)` |
| `Time.fixedDeltaTime`（可写） | 同左 | `GetFixedDeltaTime()` / `SetFixedDeltaTime(v)` |
| `Time.fixedTime` | 同左 | `GetFixedTime()` |
| — | `Time.GetFps()` | `GetFps()`（Unity 没有 `Time.fps`） |
| — | `Time.GetFixedStepTotal()` | 同左（Unity 无直接对应） |

## 3. 导出的 API

```
// 静态属性（.d.ts 里的形状）
static readonly deltaTime: number
static readonly unscaledDeltaTime: number
static readonly smoothDeltaTime: number
static readonly time: number
static readonly unscaledTime: number
static readonly realtimeSinceStartup: number
static readonly frameCount: number
static          timeScale: number          // 可写
static          fixedDeltaTime: number     // 可写
static readonly fixedTime: number

// 方法
GetFps(): number                  平滑帧率
ResetTimeScale(): void           还回 1（不动累计时间与帧数）
IsLogicUpdating(): boolean        本帧是否推进逻辑（编辑器暂停时为 false）
GetFixedStepTotal(): number      累计固定步数，**与帧率无关**
GetFixedStepAlpha(): number      固定步之间的插值系数 [0,1)
```

量化（`ZHeaderTool generate --dump-ir`）：类 70 → 71，方法 265 → 276，静态方法
52 → 63，缺 Converter 0。

## 3.1 两套时间：Update 与 FixedUpdate

`GetDeltaTime()` 系列吃**帧间隔**，`GetFixedDeltaTime()` 系列吃**固定步长**。
脚本侧对应两个回调：

| | 频率 | dt | 用在哪 |
| --- | --- | --- | --- |
| `OnUpdate(dt)` | 每帧一次 | 帧间隔，一帧一个样 | 输入响应、UI、相机跟随 |
| `OnFixedUpdate(dt)` | 每个固定步一次，**一帧内可能 0 次或多次** | 恒为固定步长 | 物理、角色移动、状态机计时、任何要联网对齐的状态 |

判断标准只有一条：**结果需要跨机器复现吗？** 需要 ⇒ `OnFixedUpdate`。

```typescript
class Mover extends Behaviour {
    speed: number = 3;

    // 对：位移在固定步里推进，60Hz 和 144Hz 的机器上结果一致。
    OnFixedUpdate(dt: number) {
        const step = VectorMath.Scale(VectorMath.UnitX(), this.speed * dt);
        this.transform.position = VectorMath.Add(this.transform.position, step);
    }
}
```

⚠️ **两边都写同一个状态是典型 bug**：一帧跑几步随帧率变，结果就在几个值之间
横跳，而在开发机（帧率稳定）上完全看不出来。

⚠️ **"收到一次输入就做一次事"的逻辑不能放 `OnFixedUpdate`**：掉帧时一帧跑多
步会重复触发，高帧率时一帧 0 步会漏触发。那种逻辑属于 `OnUpdate`。

完整设计（累加器、死亡螺旋闸门、物理为什么挪进固定步）见
`doc/FIXED_TIME_STEP.md`。

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
63 项检查，覆盖 timeScale 缩放（T4/T5/T10）、暂停帧 delta 必须为零但帧数照涨
（T7）、负 delta 与负 timeScale 夹零（T6/T8）、`Reset` 与 `ResetTimeScale`
的区别（T11/T12）、脚本 API 与内部状态一致（T13/T14）、固定步进 API 与内部
累加器一致且写穿（T15）。

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
| `Time.realtimeSinceStartup` | 与 `GetUnscaledTime()` 是同一个量，重复 |
| `Time.captureFramerate` | 录屏用的，引擎没有对应机制 |
| 渲染侧固定步插值 | `GetFixedStepAlpha()` 已经导出，但引擎**还没**用它在本步与上一步的位姿之间插值。所以 50Hz 模拟 + 144Hz 渲染目前仍会抖。脚本可以先拿它自己做平滑；渲染插值是独立一件事，见 `doc/FIXED_TIME_STEP.md` §6 |

> 变更记录：`Time.fixedDeltaTime` / `FixedUpdate` 此前在这一节里，理由是"引擎
> 没有固定步长循环，导出一个没人消费的常量就是假 API"。固定步长循环已在
> `doc/FIXED_TIME_STEP.md` 落地（物理同步挪进固定步），这条理由不再成立，故
> 移出本节。
