#pragma once

#include "Runtime/BaseClasses/Object.h"
#include "Runtime/Core/Time/TimeState.h"

#include "ScriptTime.generated.h"

// 脚本侧的时间查询。
//
// 为什么是 `Time.GetDeltaTime()` 而不是 `Time.deltaTime`
// -------------------------------------------------------
// Unity 的 `Time.deltaTime` 是**静态属性**。这里的 ZSCRIPTPROPERTY 不支持
// 静态访问器 —— `zht/parser.py` 会直接报错：
//
//     ZSCRIPTPROPERTY '%s': getter '%s' must not be static
//
// 所以只能导出成静态方法，脚本写 `Time.GetDeltaTime()`。这是工具链约束，
// 不是笔误；和 VectorMath 用静态方法的原因（ZCLASS 必须派生 Object，
// Vector3 进不了绑定表）是同一类取舍。
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
//   * 非 Tick 上下文（OnAwake / OnStart / 事件回调）里读当前 delta
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

    /// 真实帧间隔（秒）。不受 timeScale、不受暂停影响。
    /// Unity 的 `Time.unscaledDeltaTime`。UI 动画与"暂停时也要走"的计时
    /// 用它。
    ZFUNCTION()
    static float GetUnscaledDeltaTime() { return TimeState::Get().GetUnscaledDeltaTime(); }

    /// 平滑过的帧间隔（秒），EMA（alpha = 1/100）。Unity 的
    /// `Time.smoothDeltaTime`。想让镜头跟随不受单帧卡顿影响时用这个。
    ZFUNCTION()
    static float GetSmoothDeltaTime() { return TimeState::Get().GetSmoothDeltaTime(); }

    // ---- 累计 ----

    /// 自引擎启动（或进入 Play）以来的逻辑时间累计（秒），受 timeScale。
    /// Unity 的 `Time.time`。
    ZFUNCTION()
    static float GetTime() { return TimeState::Get().GetTime(); }

    /// 真实时间累计（秒）。Unity 的 `Time.unscaledTime`。
    ZFUNCTION()
    static float GetUnscaledTime() { return TimeState::Get().GetUnscaledTime(); }

    /// 已经跑过的帧数（含暂停帧）。Unity 的 `Time.frameCount`。
    ZFUNCTION()
    static int GetFrameCount() { return TimeState::Get().GetFrameCount(); }

    /// 平滑帧率（帧/秒）。
    ZFUNCTION()
    static int GetFps() { return TimeState::Get().GetFps(); }

    // ---- 时间缩放 ----

    /// 当前时间缩放：1 = 正常，0 = 逻辑冻结。Unity 的 `Time.timeScale`。
    ZFUNCTION()
    static float GetTimeScale() { return TimeState::Get().GetTimeScale(); }

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

    // ---- 状态 ----

    /// 本帧是否推进逻辑。编辑器暂停时为 false（帧照跑、时间不涨）。
    ZFUNCTION()
    static bool IsLogicUpdating() { return TimeState::Get().IsLogicUpdating(); }
};
