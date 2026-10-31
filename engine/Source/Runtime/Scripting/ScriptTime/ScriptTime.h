#pragma once

#include "Runtime/BaseClasses/Object.h"
#include "Runtime/Core/Time/TimeState.h"

#include "ScriptTime.generated.h"

// 脚本侧的时间查询。
//
// 拼写与 Unity 一致：`Time.deltaTime`，不是 `Time.GetDeltaTime()`
// ---------------------------------------------------------------
// Unity 的这些是**静态属性**。以前 ZSCRIPTPROPERTY 不接受静态访问器
// （`zht/parser.py` 直接报 "getter must not be static"），只能落成
// `Time.GetDeltaTime()`；`Color.red` 那一档把静态属性做出来了（静态属性走
// pesapi 的 `Variables` 表，由后端装到**构造器**上），于是这里换成 Unity 的
// 拼写。Get* 方法在 C++ 里照旧保留（ZSCRIPTPROPERTY 的 getter 本来就得是个
// ZFUNCTION），但**不再出现在 .d.ts 里** —— 静态访问器会被 codegen 从方法表
// 摘掉，否则 MSVC 把同一个 MethodCallback 编进两个 COMDAT 段（LNK1179）。
//
// 也就是说：TS 写 `Time.deltaTime`，C++ 写 `Time::GetDeltaTime()`，两边各自
// 是本语言的习惯写法。
//
// 注意：脚本的 `OnUpdate(dt)` **本来就收到本帧的 delta**。所以"每帧移动"
// 不需要 Time：
//
//     OnUpdate(dt: number) { this.transform.position = ... dt ... }
//
// Time 补的是 OnUpdate 参数给不了的那些量：
//   * 累计时间 `GetTime()` —— dt 只能累加，函数一返回就丢了上下文
//   * `GetFrameCount()` / `GetFps()` —— 帧数与帧率
//   * `GetTimeScale()` / `SetTimeScale()` —— 全局慢放 / 冻结，脚本唯一能
//     影响世界推进速度的手段
//   * `GetFixedDeltaTime()` / `GetFixedStepTotal()` —— 固定步进那一半的时间
//     （见下面"两套时间"）
//   * 非 Tick 上下文（OnAwake / OnStart / 事件回调）里读当前 delta
//
// 两套时间：Update 与 FixedUpdate
// -------------------------------
// `GetDeltaTime()` 系列吃帧间隔，`GetFixedDeltaTime()` 系列吃固定步长。脚本
// 侧同样是两套回调：`OnUpdate(dt)` 每帧一次、`OnFixedUpdate(dt)` 每个固定步
// 一次（一帧内可能 0 次或多次）。
//
// 判断该用哪套的标准很简单：**结果需要跨机器复现吗？** 物理、角色移动、状态
// 机计时、任何要联网对齐的状态推进 => OnFixedUpdate。其余表现层（输入响应、
// UI、相机）=> OnUpdate。两边都写同一个状态是典型 bug：帧率一变，结果就在
// "一帧跑几步"之间横跳。
//
// 关于 timeScale 的作用范围
// --------------------------
// `SetTimeScale(0.5)` 之后，**引擎驱动世界用的就是缩放后的步长** —— 物理、
// 动画、Level::Tick 全都慢一半，脚本读到的 `GetDeltaTime()` 也是那个缩放后
// 的值。不存在"脚本慢放但物理照常"的不一致。
//
// 代价：渲染侧也吃同一个 delta（Application::TickOneFrame 把缩放后的步长
// 传给 RendererTick），所以 timeScale=0 时纯渲染驱动的效果也停。需要"暂停
// 时 UI 继续动"的场合用 `GetUnscaledDeltaTime()` 自己算。
//
// `Time` 不该被实例化 —— 它只是脚本侧的命名空间。header-only（同
// BasicShape.h 的 Box / Sphere / Capsule，无对应 .cpp）。

ZCLASS()
class Time : public Object
{
    ZGENERATED_BODY(Time)

public:
    // ---- 每帧步长 ----

    /// 本帧的逻辑步长（秒）。受 timeScale 影响；编辑器暂停时为 0。
    /// Unity 的 `Time.deltaTime`。
    ZFUNCTION()
    static float GetDeltaTime() { return TimeState::Get().GetDeltaTime(); }
    ZSCRIPTPROPERTY(Name = "deltaTime", Getter = "GetDeltaTime")

    /// 真实帧间隔（秒）。不受 timeScale、不受暂停影响。
    /// Unity 的 `Time.unscaledDeltaTime`。UI 动画与"暂停时也要走"的计时
    /// 用它。
    ZFUNCTION()
    static float GetUnscaledDeltaTime() { return TimeState::Get().GetUnscaledDeltaTime(); }
    ZSCRIPTPROPERTY(Name = "unscaledDeltaTime", Getter = "GetUnscaledDeltaTime")

    /// 平滑过的帧间隔（秒），EMA（alpha = 1/100）。Unity 的
    /// `Time.smoothDeltaTime`。想让镜头跟随不受单帧卡顿影响时用这个。
    ZFUNCTION()
    static float GetSmoothDeltaTime() { return TimeState::Get().GetSmoothDeltaTime(); }
    ZSCRIPTPROPERTY(Name = "smoothDeltaTime", Getter = "GetSmoothDeltaTime")

    // ---- 累计 ----

    /// 自引擎启动（或进入 Play）以来的逻辑时间累计（秒），受 timeScale。
    /// Unity 的 `Time.time`。
    ZFUNCTION()
    static float GetTime() { return TimeState::Get().GetTime(); }
    ZSCRIPTPROPERTY(Name = "time", Getter = "GetTime")

    /// 真实时间累计（秒）。Unity 的 `Time.unscaledTime`。
    ZFUNCTION()
    static float GetUnscaledTime() { return TimeState::Get().GetUnscaledTime(); }
    ZSCRIPTPROPERTY(Name = "unscaledTime", Getter = "GetUnscaledTime")

    /// Unity 里 `realtimeSinceStartup` 与 `unscaledTime` 是同一个量，只是叫法
    /// 不同（前者强调"含启动前的初始化"）。
    ///
    /// ⚠️ 它必须是**另一个函数**，不能让两个 ZSCRIPTPROPERTY 共用
    /// `GetUnscaledTime`：同一个静态访问器挂两次，MSVC 就把
    /// `PropertyCallback<同一个指针>` 编进两个 COMDAT 段，链接器直接拒收整个
    /// obj（LNK1179，与 `Color` 那次同源）。转发一层是这里最省事的解法。
    ZFUNCTION()
    static float GetRealtimeSinceStartup() { return GetUnscaledTime(); }
    ZSCRIPTPROPERTY(Name = "realtimeSinceStartup", Getter = "GetRealtimeSinceStartup")

    /// 已经跑过的帧数（含暂停帧）。Unity 的 `Time.frameCount`。
    ZFUNCTION()
    static int GetFrameCount() { return TimeState::Get().GetFrameCount(); }
    ZSCRIPTPROPERTY(Name = "frameCount", Getter = "GetFrameCount")

    /// 平滑帧率（帧/秒）。
    ///
    /// ⚠️ Unity **没有** `Time.fps`（要看帧率得自己 1/deltaTime）。这里额外
    /// 留一个方法，因为引擎内部已经在算（HUD 要用），脚本自己算会重复一遍 EMA。
    ZFUNCTION()
    static int GetFps() { return TimeState::Get().GetFps(); }

    // ---- 时间缩放 ----

    /// 当前时间缩放：1 = 正常，0 = 逻辑冻结。Unity 的 `Time.timeScale`。
    ZFUNCTION()
    static float GetTimeScale() { return TimeState::Get().GetTimeScale(); }
    /// Unity 的 `Time.timeScale` 是**可写**的（`Time.timeScale = 0` 是暂停
    /// 的标准写法），所以这里带 setter。
    ZSCRIPTPROPERTY(Name = "timeScale", Getter = "GetTimeScale", Setter = "SetTimeScale")

    /// 设置时间缩放。**负数会被夹到 0**（时间不能倒流）。
    ///
    /// 这是全局持久状态：退出 Play 时引擎会重置（见
    /// Editor::SetPlaybackState），但在编辑模式下脚本组件也会 Tick，那时设
    /// 的值要自己还回去。
    ZFUNCTION()
    static void SetTimeScale(float scale) { TimeState::Get().SetTimeScale(scale); }

    /// 把 timeScale 恢复成 1（不动累计时间与帧数）。
    ZFUNCTION()
    static void ResetTimeScale() { TimeState::Get().ResetTimeScale(); }

    // ---- 固定步进（FixedUpdate）----

    /// 固定步长（秒）。Unity 的 `Time.fixedDeltaTime`。
    ///
    /// 这是 `OnFixedUpdate(dt)` 收到的那个 dt —— 恒定值，与帧率无关。
    /// 与之相对，`GetDeltaTime()` 是帧间隔，一帧一个样。
    ZFUNCTION()
    static float GetFixedDeltaTime() { return TimeState::Get().GetFixedDeltaTime(); }
    /// Unity 的 `Time.fixedDeltaTime` 同样可写（改物理步长）。
    ZSCRIPTPROPERTY(Name = "fixedDeltaTime", Getter = "GetFixedDeltaTime",
                    Setter = "SetFixedDeltaTime")

    /// 设置固定步长。<= 1ms 的值被夹到 1ms（0 会让引擎的固定步循环变成
    /// 死循环）。见 TimeState::SetFixedDeltaTime。
    ZFUNCTION()
    static void SetFixedDeltaTime(float fixed_delta_time)
    {
        TimeState::Get().SetFixedDeltaTime(fixed_delta_time);
    }

    /// 累计跑过的固定步数。**与帧率无关** —— 这是"模拟现在是第几 tick"。
    ///
    /// 做回放 / 联网同步 / 确定性自检时以它为准，不要用 `GetFrameCount()`：
    /// 后者帧率一变就变。
    ///
    /// 名字与 TimeState 对齐（那边 `GetFixedStepCount()` 是**本帧**步数，是
    /// 另一回事，不导出给脚本 —— 脚本不该关心引擎内部一帧跑了几步）。
    ZFUNCTION()
    static int GetFixedStepTotal() { return TimeState::Get().GetFixedStepTotal(); }

    /// 固定步推进的逻辑时间累计（秒）。Unity 的 `Time.fixedTime`。
    ZFUNCTION()
    static float GetFixedTime() { return TimeState::Get().GetFixedTime(); }
    ZSCRIPTPROPERTY(Name = "fixedTime", Getter = "GetFixedTime")

    /// 固定步之间的插值系数 [0,1)。
    ///
    /// ⚠️ 引擎目前**没有**用它做渲染插值（50Hz 模拟 + 144Hz 渲染仍会抖），
    /// 它只给脚本自己做平滑用。渲染插值是独立的一件事，见
    /// TimeState::GetFixedStepAlpha 的注释。
    ZFUNCTION()
    static float GetFixedStepAlpha() { return TimeState::Get().GetFixedStepAlpha(); }

    // ---- 状态 ----

    /// 本帧是否推进逻辑。编辑器暂停时为 false（帧照跑、时间不涨）。
    ZFUNCTION()
    static bool IsLogicUpdating() { return TimeState::Get().IsLogicUpdating(); }
};
