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
}
