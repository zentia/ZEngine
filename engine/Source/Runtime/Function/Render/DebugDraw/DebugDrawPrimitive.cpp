#include "DebugDrawPrimitive.h"
bool DebugDrawPrimitive::IsTimeOut(float delta_time)
{
    if (m_TimeType == _debugDrawTimeType_infinity)
    {
        return false;
    }
    else if (m_TimeType == _debugDrawTimeType_one_frame)
    {
        if (!m_Rendered)
        {
            m_Rendered = true;
            return false;
        }
        else
            return true;
    }
    else
    {
        m_LifeTime -= delta_time;
        return (m_LifeTime < 0.0f);
    }
    return false;
}

void DebugDrawPrimitive::SetTime(float in_life_time)
{
    if (fabs(in_life_time - k_debug_draw_infinity_life_time) < 1e-6)
    {
        m_TimeType = _debugDrawTimeType_infinity;
        m_LifeTime = 0.0f;
    }
    else if (fabs(in_life_time - k_debug_draw_one_frame) < 1e-6)
    {
        m_TimeType = _debugDrawTimeType_one_frame;
        m_LifeTime = 0.03f;
    }
    else
    {
        m_TimeType = _debugDrawTimeType_common;
        m_LifeTime = in_life_time;
    }
}