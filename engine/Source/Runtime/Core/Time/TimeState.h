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
//
// 固定步进（FixedUpdate）
// ----------------------
// 见类内 "固定时间步进" 一节的注释。`GetFixedStepCount()` 由 BeginFrame
// 算好，Application::LogicalTick 按它分发。

#include <algorithm>

class TimeState
{
public:
    /// 唯一的实例。首次调用时构造，进程生命期内不析构（故意的：静态析构
    /// 顺序可能晚于其它静态对象的最后一次读取）。
    static TimeState& Get();

    // ---- 固定时间步进（FixedUpdate）----
    //
    // 为什么要这一层
    // --------------
    // 帧间隔（GetDeltaTime）是**墙钟**决定的：同一段模拟，在 60Hz 和 144Hz
    // 的机器上跑出来的步数与步长都不一样。任何"每步积分"的东西（物理、
    // 角色移动、状态机计时）只要吃帧间隔，结果就跟帧率绑死了 —— 换台机器
    // 复现不了，联网对不上，回放也对不上。
    //
    // 固定步进把这两件事拆开：模拟永远以 `m_FixedDeltaTime` 为步长推进，
    // 一帧内跑 0..N 次（N = GetFixedStepCount），多出来的零头留在累加器里
    // 等下一帧。于是"模拟推进了多少"只取决于累计时间，与帧率无关。
    //
    // 这是 Unity 的 FixedUpdate / Time.fixedDeltaTime 的同一套语义。

    /// 默认固定步长 1/50 秒（与 Unity 默认一致）。
    static constexpr float kDefaultFixedDeltaTime = 1.0f / 50.0f;

    /// 单帧最多跑几次固定步。这是"死亡螺旋"的闸门：一帧卡了 2 秒时不能
    /// 真的去补 100 步（越补越慢、越慢越要补）。
    static constexpr int kDefaultMaxSubsteps = 5;

    /// 固定步长的下限。0 或负数会让 while 循环永不退出，静默取代价巨大的
    /// 正确做法是夹住而不是"忽略这次设置"—— 后者让调用方以为设成功了。
    static constexpr float kMinFixedDeltaTime = 1.0f / 1000.0f;

    static constexpr int kMinSubstepsCap = 1;
    static constexpr int kMaxSubstepsCap = 64;

    float GetFixedDeltaTime() const { return m_FixedDeltaTime; }

    /// 设置固定步长。<= kMinFixedDeltaTime 的值被夹到 kMinFixedDeltaTime。
    ///
    /// 改这个值**不会**让已经过去的模拟重算，累加器里的零头也照旧参与 ——
    /// 也就是说中途改步长会让"累计到现在的模拟量"出现一次跳变。要干净的
    /// 时间线就在关卡加载时设一次，别在运行时来回改。
    void SetFixedDeltaTime(float fixed_delta_time);

    int GetMaxSubsteps() const { return m_MaxSubsteps; }

    /// 设置单帧固定步上限，夹到 [kMinSubstepsCap, kMaxSubstepsCap]。
    void SetMaxSubsteps(int max_substeps);

    /// **本帧要跑几次固定步**。Application::LogicalTick 按这个数循环分发
    /// FixedTick；为 0 表示本帧零头还不够一步（帧率高于固定步率时常见）。
    int GetFixedStepCount() const { return m_FixedStepCount; }

    /// 累计跑过的固定步数（不受帧率影响）。
    ///
    /// 这是"模拟现在是第几 tick"，与 GetFrameCount() 是两码事：后者帧率
    /// 一变就变。回放、网络同步、确定性测试一律以它为准。
    int GetFixedStepTotal() const { return m_FixedStepTotal; }

    /// 固定步推进的逻辑时间累计（秒），= 固定步数 × 步长累加而来。
    /// 与 GetTime() 的区别同上：这个是帧率无关的。
    float GetFixedTime() const { return m_FixedTime; }

    /// 固定步之间的插值系数，[0, 1)：本帧走完最后一个整步之后，累加器里
    /// 剩下的零头占一个固定步的比例。
    ///
    /// 渲染侧拿它在本步与上一步的位姿之间插值，才能让"50Hz 模拟 + 144Hz
    /// 渲染"不抖。⚠️ 目前**引擎还没有消费它**（渲染插值没接），脚本可以
    /// 先拿去做相机平滑之类的自定义插值。
    float GetFixedStepAlpha() const { return m_FixedAlpha; }

    /// 累加器余量（秒），主要用于测试与诊断。
    float GetFixedAccumulator() const { return m_FixedAccumulator; }

    // ---- 每帧一次（Application::TickOneFrame）----

    /// @param simulation_delta_time 编辑器算好的"逻辑"步长（暂停时为 0）
    /// @param frame_delta_time      真实帧间隔（墙钟），不受暂停影响
    /// @param should_update_logic   本帧是否推进逻辑（编辑器暂停时为 false）
    ///
    /// 顺序：先算本帧 delta（= simulation_delta_time * timeScale，不推进
    /// 逻辑时为 0），再累加时间、帧数、平滑帧率。
    void BeginFrame(float simulation_delta_time, float frame_delta_time, bool should_update_logic);

    /// 回到初始状态。给测试用，也给"关卡重载 / 退出 Play"这类需要把时间
    /// 归零的场合用。帧数、固定步数、累加器一并归零。
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

    /// 把本帧的（已缩放）逻辑 delta 灌进累加器，算出本帧该跑几次固定步。
    ///
    /// 由 BeginFrame 在算完 m_DeltaTime 之后调用 —— 固定步进吃的是**缩放后**
    /// 的逻辑时间，所以 timeScale = 0.5 时固定步也跟着减半，timeScale = 0
    /// 时一步都不跑（这是对的：慢放/暂停必须同时冻结 FixedUpdate，否则
    /// 物理会照常跑）。
    ///
    /// 不推进逻辑的帧（编辑器暂停）同样不推进累加器：暂停应当是"时间停了"
    /// 而不是"攒着等恢复那一帧一起补"—— 后者会让恢复瞬间爆出一大批步，
    /// 且步进总数与"暂停了多久"耦合，破坏确定性。
    void AdvanceFixedSteps(bool should_update_logic);

    float m_DeltaTime {0.0f};
    float m_UnscaledDeltaTime {0.0f};
    float m_Time {0.0f};
    float m_UnscaledTime {0.0f};
    int   m_FrameCount {0};
    float m_TimeScale {1.0f};

    float m_AverageFrameDuration {0.0f};
    int   m_Fps {0};
    bool  m_LogicUpdating {false};

    // 固定步进
    float m_FixedDeltaTime {kDefaultFixedDeltaTime};
    int   m_MaxSubsteps {kDefaultMaxSubsteps};
    float m_FixedAccumulator {0.0f};
    int   m_FixedStepCount {0};
    int   m_FixedStepTotal {0};
    float m_FixedTime {0.0f};
    float m_FixedAlpha {0.0f};
};
