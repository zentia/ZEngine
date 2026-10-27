#pragma once

// 引擎的"当前时间"状态：本帧 delta、累计时间、帧数、时间缩放、平滑帧率。
//
// 为什么是一个独立的小单例，而不是塞进 Application
// --------------------------------------------------
// 1. **可测**：Application 是 IEngineSystem，只有系统注册表起来之后才有实例。
//    时间状态是纯数据 + 纯算术，做成静态单例之后冒烟测试可以直接
//    `TimeState::Get().BeginFrame(0.016f, 0.016f, true)` 推进若干帧并断言，
//    不需要拉起引擎（headless、无窗口、无 GPU）。
// 2. **谁都能读**：脚本侧的 `Time.*` 是静态方法，拿不到（也不该拿）Application
//    实例 —— 那是系统层。
//
// 谁写、谁读
// ----------
// 唯一写者是 `Application::TickOneFrame`（每帧开头一次），其它地方只读。
// 唯一例外是 `SetTimeScale`，脚本通过 `Time.SetTimeScale()` 调用。
//
// 与 Application::CalculateFPS 的关系
// -----------------------------------
// Application 自己也维护 m_FrameCount / m_Fps（用于窗口标题），算法和这里
// 一样（同样的 EMA 平滑、同样的 alpha）。两处并存是有意的：Application 那份
// 服务于标题栏，这份服务于脚本与逻辑。不要试图合并 —— 合并就必须让
// TimeState 依赖系统注册表，可测性就没了。
//
// 精度
// ----
// 累计时间是 float 秒。跑过 ~10 万秒（约 28 小时）后 float 的分辨率掉到
// ~8ms，`GetTime()` 会开始跳。Unity 的 Time.time 同样是 float，有同样的
// 问题；需要长时间绝对计时的场合用 `GetFrameCount()` 或自己存 double。

class TimeState
{
public:
    /// 唯一的实例。首次调用时构造，进程生命期内不析构（故意的：静态析构
    /// 顺序可能晚于其它静态对象的最后一次读取）。
    static TimeState& Get();

    // ---- 每帧一次（Application::TickOneFrame）----

    /// @param simulation_delta_time 编辑器算好的"逻辑"步长（暂停时为 0）
    /// @param frame_delta_time      真实帧间隔（墙钟），不受暂停影响
    /// @param should_update_logic   本帧是否推进逻辑（编辑器暂停时为 false）
    ///
    /// 顺序：先算本帧 delta（= simulation_delta_time * timeScale，不推进
    /// 逻辑时为 0），再累加时间、帧数、平滑帧率。
    void BeginFrame(float simulation_delta_time, float frame_delta_time, bool should_update_logic);

    /// 回到初始状态。给测试用，也给"关卡重载 / 退出 Play"这类需要把时间
    /// 归零的场合用。帧数也一并归零。
    void Reset();

    // ---- 读 ----

    /// 本帧逻辑步长（秒）。受 timeScale 影响；本帧不推进逻辑时为 0。
    /// 这是引擎驱动 Level::Tick / 物理 / 动画用的那个数 —— 脚本读到的和
    /// 世界实际用的**是同一个数**，不存在两套时间。
    float GetDeltaTime() const { return m_DeltaTime; }

    /// 真实帧间隔（秒）。不受 timeScale、不受暂停影响。做 UI 动画、做与
    /// 游戏逻辑无关的计时时用它，否则慢放/暂停时 UI 也会跟着停。
    float GetUnscaledDeltaTime() const { return m_UnscaledDeltaTime; }

    /// 自引擎启动（或上次 Reset）以来的逻辑时间累计（秒），受 timeScale。
    float GetTime() const { return m_Time; }

    /// 真实时间累计（秒）。
    float GetUnscaledTime() const { return m_UnscaledTime; }

    /// 已经跑过的帧数（含不推进逻辑的帧 —— 暂停时帧数照涨，时间不涨）。
    int GetFrameCount() const { return m_FrameCount; }

    /// 平滑过的帧间隔（秒）。EMA，alpha = 1/100，与 Application 的 FPS 计算
    /// 同参。用于"平均帧时长"类需求。
    float GetSmoothDeltaTime() const { return m_AverageFrameDuration; }

    /// 平滑帧率（帧/秒），由平滑帧间隔换算。
    int GetFps() const { return m_Fps; }

    /// 本帧是否推进逻辑。编辑器暂停时为 false。
    bool IsLogicUpdating() const { return m_LogicUpdating; }

    // ---- 时间缩放 ----

    /// 时间缩放。1 = 正常速度，0 = 逻辑完全冻结（渲染照常、帧照常）。
    float GetTimeScale() const { return m_TimeScale; }

    /// 设置时间缩放。**负数会被夹到 0**：负的时间步长会让物理积分和动画
    /// 采样往回走，那是静默的错误数据，不如直接拒绝。上限不夹 —— 100 倍
    /// 快进是合法需求（回放、倍速）。

    /// 注意这是**全局持久**状态：脚本设成 0.5 之后即使退出 Play 也不会
    /// 自己恢复。退出播放要重置的场合调 `ResetTimeScale()`。
    void SetTimeScale(float scale);

    /// 把 timeScale 恢复成 1（不动累计时间与帧数）。
    void ResetTimeScale() { m_TimeScale = 1.0f; }

private:
    TimeState() = default;

    /// EMA 平滑系数，与 Application::s_Fpsalpha 同一个值。
    static constexpr float kFpsAlpha = 1.0f / 100.0f;

    float m_DeltaTime {0.0f};
    float m_UnscaledDeltaTime {0.0f};
    float m_Time {0.0f};
    float m_UnscaledTime {0.0f};
    int   m_FrameCount {0};
    float m_TimeScale {1.0f};

    float m_AverageFrameDuration {0.0f};
    int   m_Fps {0};
    bool  m_LogicUpdating {false};
};
