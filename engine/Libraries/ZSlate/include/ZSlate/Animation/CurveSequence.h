#pragma once

#include "ZSlate/Core/ZSlateTypes.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

namespace ZSlate
{

// =============================================================================
// Easing functions (UE ECurveEaseFunction analogue)
// =============================================================================

enum class EEasingType
{
    Linear,
    QuadIn, QuadOut, QuadInOut,
    CubicIn, CubicOut, CubicInOut,
    SineIn, SineOut, SineInOut,
    ExpoIn, ExpoOut, ExpoInOut,
    ElasticOut,
    BounceOut,
};

inline float ApplyEasing(float t, EEasingType type)
{
    switch (type)
    {
    case EEasingType::Linear:    return t;
    case EEasingType::QuadIn:    return t * t;
    case EEasingType::QuadOut:   return t * (2.0f - t);
    case EEasingType::QuadInOut: return t < 0.5f ? 2.0f * t * t : -1.0f + (4.0f - 2.0f * t) * t;
    case EEasingType::CubicIn:   return t * t * t;
    case EEasingType::CubicOut:  { float f = t - 1.0f; return f * f * f + 1.0f; }
    case EEasingType::CubicInOut: return t < 0.5f ? 4.0f * t * t * t
        : (t - 1.0f) * (2.0f * t - 2.0f) * (2.0f * t - 2.0f) + 1.0f;
    case EEasingType::SineIn:    return 1.0f - std::cos(t * 1.5707963f);
    case EEasingType::SineOut:   return std::sin(t * 1.5707963f);
    case EEasingType::SineInOut: return 0.5f * (1.0f - std::cos(3.14159f * t));
    case EEasingType::ElasticOut:
    {
        if (t == 0 || t == 1) return t;
        return std::pow(2.0f, -10.0f * t) * std::sin((t - 0.1f) * 5.0f * 3.14159f) + 1.0f;
    }
    case EEasingType::BounceOut:
    {
        float n1 = 7.5625f, d1 = 2.75f;
        if (t < 1.0f / d1)       return n1 * t * t;
        else if (t < 2.0f / d1)  { t -= 1.5f / d1; return n1 * t * t + 0.75f; }
        else if (t < 2.5f / d1)  { t -= 2.25f / d1; return n1 * t * t + 0.9375f; }
        else                     { t -= 2.625f / d1; return n1 * t * t + 0.984375f; }
    }
    default: return t;
    }
}

// =============================================================================
// FCurveHandle — a single animatable value (UE FCurveHandle analogue)
// =============================================================================
//
// Drives a property from start→end over a duration with easing.
//
class FCurveSequence;

struct FCurveHandle
{
    float Eval(const FCurveSequence& seq) const
    {
        float t = seq.GetLerp();
        return m_StartVal + (m_EndVal - m_StartVal) * ApplyEasing(t, m_Easing);
    }

    FCurveHandle& SetEasing(EEasingType e) { m_Easing = e; return *this; }

private:
    friend class FCurveSequence;
    float m_StartVal {0.0f};
    float m_EndVal {1.0f};
    float m_Delay {0.0f};
    EEasingType m_Easing {EEasingType::Linear};
};

// =============================================================================
// FCurveSequence — animation driver (UE FCurveSequence analogue)
// =============================================================================
//
// Drives one or more curve handles from 0→1 over a duration. Pause/resume/
// reverse supported. Tick calls advance the time.
//
class FCurveSequence
{
public:
    FCurveSequence() = default;

    // Play the sequence once (non-looping)
    void Play(float duration, bool loop = false)
    {
        m_Duration = duration;
        m_Time = 0.0f;
        m_Playing = true;
        m_Looping = loop;
        m_Reverse = false;
        m_Elapsed = 0.0f;
    }

    // Play in reverse (end → start)
    void PlayReverse(float duration)
    {
        Play(duration);
        m_Reverse = true;
        m_Time = duration;
    }

    void Pause()  { m_Playing = false; }
    void Resume() { m_Playing = true; }
    void Stop()   { m_Playing = false; m_Time = 0.0f; }

    bool IsPlaying() const { return m_Playing; }
    bool IsAtEnd() const
    {
        if (!m_Reverse) return m_Elapsed >= m_Duration && !m_Looping;
        return m_Time <= 0 && !m_Looping;
    }

    // Current lerp factor 0..1 (eased)
    float GetLerp() const
    {
        float raw;
        if (m_Reverse)
            raw = (m_Duration > 0) ? m_Time / m_Duration : 1.0f;
        else
            raw = (m_Duration > 0) ? m_Elapsed / m_Duration : 1.0f;
        return std::clamp(raw, 0.0f, 1.0f);
    }

    // Advance time. Returns true while still playing.
    bool Tick(float deltaSec)
    {
        if (!m_Playing) return false;

        m_Elapsed += deltaSec;

        if (m_Reverse)
            m_Time -= deltaSec;
        else
            m_Time += deltaSec;

        if (!m_Looping)
        {
            if (!m_Reverse && m_Elapsed >= m_Duration)
            {
                m_Time = m_Duration;
                m_Elapsed = m_Duration;
                return false;
            }
            if (m_Reverse && m_Time <= 0)
            {
                m_Time = 0;
                return false;
            }
        }
        else
        {
            if (m_Elapsed >= m_Duration)
                m_Elapsed = std::fmod(m_Elapsed, m_Duration);
            m_Time = std::fmod(m_Time, m_Duration);
        }

        return true;
    }

    // Fluent handle creation
    FCurveHandle& AddCurve(float start, float end)
    {
        m_Handles.emplace_back();
        auto& h = m_Handles.back();
        h.m_StartVal = start;
        h.m_EndVal = end;
        return h;
    }

    size_t GetCurveCount() const { return m_Handles.size(); }
    const FCurveHandle& GetCurve(size_t i) const { return m_Handles[i]; }
    FCurveHandle& GetCurve(size_t i) { return m_Handles[i]; }

private:
    std::vector<FCurveHandle> m_Handles;
    float m_Duration {1.0f};
    float m_Time {0.0f};
    float m_Elapsed {0.0f};
    bool  m_Playing {false};
    bool  m_Looping {false};
    bool  m_Reverse {false};
};

// =============================================================================
// FWidgetAnimation — per-widget animation sequence (UE FWidgetAnimation analogue)
// =============================================================================
//
// Binds a callback that receives an interpolation factor 0..1 each frame.
//
class FWidgetAnimation
{
public:
    using AnimCallback = std::function<void(float)>;

    void Play(float duration, AnimCallback callback, bool loop = false)
    {
        m_Seq.Play(duration, loop);
        m_Callback = std::move(callback);
    }

    bool Tick(float deltaSec)
    {
        if (!m_Seq.IsPlaying()) return false;
        m_Seq.Tick(deltaSec);
        if (m_Callback) m_Callback(m_Seq.GetLerp());
        if (m_Seq.IsAtEnd() && !m_Seq.IsPlaying()) return false;
        return true;
    }

    void Stop() { m_Seq.Stop(); }
    bool IsPlaying() const { return m_Seq.IsPlaying(); }

private:
    FCurveSequence m_Seq;
    AnimCallback m_Callback;
};

}  // namespace ZSlate
