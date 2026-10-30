#include "Runtime/Core/Time/TimeState.h"

TimeState& TimeState::Get()
{
    // 函数内静态：构造顺序安全（首次调用才构造），且不像全局对象那样参与
    // 静态析构顺序的竞态。进程退出时它确实会被析构，但那时已经没有任何
    // 其它静态对象会再读它。
    static TimeState instance;
    return instance;
}

void TimeState::BeginFrame(float simulation_delta_time, float frame_delta_time, bool should_update_logic)
{
    // 负的 delta 只可能来自时钟回拨或调用方算错。让它进到累计时间里的后果
    // 是"时间倒流"—— 物理往回积分、动画往回采样，而且是静默的。夹到 0。
    m_UnscaledDeltaTime = frame_delta_time > 0.0f ? frame_delta_time : 0.0f;

    m_LogicUpdating = should_update_logic;

    if (should_update_logic)
    {
        const float scaled = simulation_delta_time * m_TimeScale;
        m_DeltaTime = scaled > 0.0f ? scaled : 0.0f;
    }
    else
    {
        // 不推进逻辑的帧（编辑器暂停）：逻辑 delta 必须是 0，但帧本身
        // 照样发生 —— unscaled 那边照常累加，帧数照常涨。
        m_DeltaTime = 0.0f;
    }

    m_Time += m_DeltaTime;
    m_UnscaledTime += m_UnscaledDeltaTime;
    m_FrameCount++;

    // EMA 平滑帧时长。第一帧直接取当前值（否则要从 0 慢慢爬上来，前 100 帧
    // 的 fps 是错的）。
    if (m_FrameCount == 1)
    {
        m_AverageFrameDuration = m_UnscaledDeltaTime;
    }
    else
    {
        m_AverageFrameDuration =
            m_AverageFrameDuration * (1.0f - kFpsAlpha) + m_UnscaledDeltaTime * kFpsAlpha;
    }

    m_Fps = m_AverageFrameDuration > 0.0f ? static_cast<int>(1.0f / m_AverageFrameDuration) : 0;

    AdvanceFixedSteps(should_update_logic);
}

void TimeState::AdvanceFixedSteps(bool should_update_logic)
{
    m_FixedStepCount = 0;

    if (!should_update_logic)
    {
        // 暂停：累加器冻结，零头留着。恢复之后接着走，不会"补帧"。
        m_FixedAlpha = 0.0f;
        return;
    }

    m_FixedAccumulator += m_DeltaTime;

    // 吃掉所有能吃的整步，但不超过 m_MaxSubsteps。
    //
    // 注意这里用的是累加器而不是帧 delta：帧率比固定步率高时（144Hz vs
    // 50Hz）多数帧的 result 是 0 步，零头跨帧攒着 —— 这正是"模拟量与帧率
    // 解耦"的实现方式。
    int steps = 0;
    while (m_FixedAccumulator >= m_FixedDeltaTime && steps < m_MaxSubsteps)
    {
        m_FixedAccumulator -= m_FixedDeltaTime;
        ++steps;
    }

    if (steps == m_MaxSubsteps && m_FixedAccumulator >= m_FixedDeltaTime)
    {
        // 追不上：本帧已经跑满上限，后面还压着一堆步。
        //
        // 只能丢弃。继续攒着的后果是下一帧又要补，越补越慢 —— 经典的死亡
        // 螺旋，表现是卡一次之后帧率永久下降。丢弃确实丢掉了这段模拟时间
        // （内容表现为"卡的时候慢放了一下"），但那是唯一能收敛的选择。
        //
        // ⚠️ 这一步让"总步数"在极端卡顿时依赖帧率。所有依赖步数做确定性
        // 的东西（回放/同步）都要保证不触发这个分支 —— 正常帧率下不会。
        m_FixedAccumulator = 0.0f;
    }

    m_FixedStepCount = steps;
    m_FixedStepTotal += steps;
    m_FixedTime += static_cast<float>(steps) * m_FixedDeltaTime;
    m_FixedAlpha = m_FixedAccumulator / m_FixedDeltaTime;
}

void TimeState::SetFixedDeltaTime(float fixed_delta_time)
{
    // 夹而不是拒：<= 0 会让 AdvanceFixedSteps 的 while 变成死循环，那是
    // 挂起不是错误，比夹住糟糕得多。
    m_FixedDeltaTime = std::max(fixed_delta_time, kMinFixedDeltaTime);
}

void TimeState::SetMaxSubsteps(int max_substeps)
{
    m_MaxSubsteps = std::clamp(max_substeps, kMinSubstepsCap, kMaxSubstepsCap);
}

void TimeState::SetTimeScale(float scale)
{
    // 负数夹到 0，理由见头文件注释：负的时间步长是静默的错误数据。
    m_TimeScale = scale > 0.0f ? scale : 0.0f;
}

void TimeState::Reset()
{
    m_DeltaTime            = 0.0f;
    m_UnscaledDeltaTime    = 0.0f;
    m_Time                 = 0.0f;
    m_UnscaledTime         = 0.0f;
    m_FrameCount           = 0;
    m_TimeScale            = 1.0f;
    m_AverageFrameDuration = 0.0f;
    m_Fps                  = 0;
    m_LogicUpdating        = false;

    // 固定步进：步长与上限是**配置**，不随 Reset 回默认（Reset 的语义是
    // "时间归零"，不是"设置归零" —— 否则测试里设好的参数会被冲掉）。
    // 累加器与计数属于时间状态，归零。
    m_FixedAccumulator = 0.0f;
    m_FixedStepCount   = 0;
    m_FixedStepTotal   = 0;
    m_FixedTime        = 0.0f;
    m_FixedAlpha       = 0.0f;
}
