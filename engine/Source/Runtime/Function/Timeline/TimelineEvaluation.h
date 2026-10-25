#pragma once

// Pure, engine-free timeline math used by TimelineDirector.
//
// Everything here depends on the standard library only, so the rules that decide
// "which events fire this tick", "where does a looping timeline land" and "which
// animation phase does a clip sample" are unit-tested headless
// (Test/TimelineEvaluationTest.cpp, scripts/verify_timeline_wasm.sh) instead of only
// being observable through a running editor.

#include <algorithm>
#include <cmath>

namespace TimelineEval
{
    /// A stretch of timeline time covered by one Tick, as an interval with explicit
    /// end-point inclusion. Forward playback covers [from, to) so a point that lies
    /// exactly on a tick boundary belongs to exactly one tick; the very last stretch of
    /// a non-looping timeline also includes its end point so an event placed at the
    /// end still fires. Reverse playback mirrors that: (to, from].
    struct TimeSpan
    {
        float lo {0.0f};
        float hi {0.0f};
        bool lo_inclusive {true};
        bool hi_inclusive {false};

        bool Contains(float t) const
        {
            const bool above = lo_inclusive ? (t >= lo) : (t > lo);
            const bool below = hi_inclusive ? (t <= hi) : (t < hi);
            return above && below;
        }
    };

    struct TimeStep
    {
        float new_time {0.0f};
        TimeSpan spans[2];
        int span_count {0};
        /// A looping timeline went past an end and wrapped around.
        bool wrapped {false};
        /// A non-looping timeline reached its end (or its start, playing backwards).
        bool finished {false};

        bool Crosses(float t) const
        {
            for (int i = 0; i < span_count; ++i)
            {
                if (spans[i].Contains(t))
                {
                    return true;
                }
            }
            return false;
        }
    };

    /// Advances `time` by `delta_time * speed` on a timeline of length `duration`.
    ///
    /// Looping keeps the remainder (the old director reset to exactly 0 and dropped it,
    /// so a loop drifted by up to a frame every cycle). A single tick longer than the
    /// whole timeline only reports the stretch up to the wrap and the stretch after it;
    /// the full cycles in between are skipped -- a frame should never be longer than
    /// the timeline, and replaying N cycles of events in one frame is worse than not.
    inline TimeStep Advance(float time, float delta_time, float speed, float duration, bool loop)
    {
        TimeStep step;
        if (!(duration > 0.0f))
        {
            step.new_time = 0.0f;
            step.finished = !loop;
            return step;
        }

        time = std::clamp(std::isfinite(time) ? time : 0.0f, 0.0f, duration);
        step.new_time = time;

        const float delta = delta_time * speed;
        if (!std::isfinite(delta) || delta == 0.0f)
        {
            return step;
        }

        const float target = time + delta;
        if (delta > 0.0f)
        {
            if (target < duration)
            {
                step.spans[step.span_count++] = TimeSpan {time, target, true, false};
                step.new_time = target;
            }
            else if (!loop)
            {
                step.spans[step.span_count++] = TimeSpan {time, duration, true, true};
                step.new_time = duration;
                step.finished = true;
            }
            else
            {
                float rem = std::fmod(target, duration);
                if (!(rem >= 0.0f && rem < duration))
                {
                    rem = 0.0f;
                }
                // The end point is passed on the way round, so an event authored at
                // exactly `duration` fires here; time 0 is the next, distinct point.
                step.spans[step.span_count++] = TimeSpan {time, duration, true, true};
                // When the wrap lands exactly on 0, time 0 is reached but not passed;
                // it belongs to the next tick's [0, ...) stretch, so no second span.
                if (rem > 0.0f)
                {
                    step.spans[step.span_count++] = TimeSpan {0.0f, rem, true, false};
                }
                step.new_time = rem;
                step.wrapped = true;
            }
        }
        else
        {
            if (target > 0.0f)
            {
                step.spans[step.span_count++] = TimeSpan {target, time, false, true};
                step.new_time = target;
            }
            else if (!loop)
            {
                step.spans[step.span_count++] = TimeSpan {0.0f, time, true, true};
                step.new_time = 0.0f;
                step.finished = true;
            }
            else
            {
                float rem = duration - std::fmod(-target, duration);
                if (!(rem > 0.0f && rem <= duration))
                {
                    rem = duration;
                }
                if (time > 0.0f)
                {
                    step.spans[step.span_count++] = TimeSpan {0.0f, time, true, true};
                }
                if (rem < duration)
                {
                    step.spans[step.span_count++] = TimeSpan {rem, duration, false, true};
                }
                step.new_time = rem;
                step.wrapped = true;
            }
        }
        return step;
    }

    /// Half-open clip window [start, start + duration), matching TimelineClip::containsTime.
    /// A zero- or negative-length clip is never active (event clips are points and are
    /// handled with TimeStep::Crosses instead).
    inline bool ClipActiveAt(float start, float duration, float time)
    {
        return duration > 0.0f && time >= start && time < start + duration;
    }

    /// The largest phase strictly below 1. The skeleton sampler wraps its phase
    /// (WrapMode::Loop), so a non-looping clip held at exactly 1.0 would pop back to its
    /// first frame.
    inline float MaxHoldPhase()
    {
        return std::nextafter(1.0f, 0.0f);
    }

    /// Phase in [0, 1) an animation clip samples at `local_time` seconds into its window.
    inline float AnimationPhase(float local_time, float speed, float length, bool loop)
    {
        if (!(length > 0.0f) || !std::isfinite(local_time) || !std::isfinite(speed))
        {
            return 0.0f;
        }
        float phase = (local_time * speed) / length;
        if (loop)
        {
            phase -= std::floor(phase);
            // floor() of a value a hair below an integer can round the difference up to 1.
            return phase >= 1.0f ? 0.0f : phase;
        }
        return std::clamp(phase, 0.0f, MaxHoldPhase());
    }

    /// Length (seconds) an animation clip's phase is measured against.
    ///
    /// The timeline clip stores only a path, and AnimationClip carries a frame count but
    /// no frame rate, so the real length is only known when the target component already
    /// authors that same clip (its slot-0 length). Otherwise one playthrough is stretched
    /// over the clip window at speed 1 -- the clip plays exactly once if the author
    /// sized the window to the animation, which is the common case.
    inline float ResolveAnimationLength(bool component_has_same_clip,
                                        float component_length,
                                        float clip_duration,
                                        float clip_speed)
    {
        if (component_has_same_clip && component_length > 0.0f)
        {
            return component_length;
        }
        const float stretched = clip_duration * std::fabs(clip_speed);
        return stretched > 0.0f ? stretched : 0.0f;
    }

    enum class ClipTransition
    {
        None,   // inactive before and after
        Enter,  // became active this evaluation
        Stay,   // active before and after
        Exit    // became inactive this evaluation
    };

    inline ClipTransition Transition(bool was_active, bool is_active)
    {
        if (is_active)
        {
            return was_active ? ClipTransition::Stay : ClipTransition::Enter;
        }
        return was_active ? ClipTransition::Exit : ClipTransition::None;
    }
}  // namespace TimelineEval
