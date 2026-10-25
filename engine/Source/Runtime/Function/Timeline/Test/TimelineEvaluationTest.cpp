// Headless test for Runtime/Function/Timeline/TimelineEvaluation.h -- the pure math
// TimelineDirector uses to decide where the playhead lands, which event clips fire in a
// tick, which clip windows are active and which phase an animation clip samples.
//
// Owns a main(), so it is REMOVE_ITEM'd from ZRuntime (Runtime/CMakeLists.txt).
// Run: scripts/verify_timeline_wasm.sh  (emcc + node, standard library only)

#include "Runtime/Function/Timeline/TimelineEvaluation.h"

#include <cmath>
#include <cstdio>

namespace
{
    int g_Checks = 0;
    int g_Failures = 0;

    void Check(bool condition, const char* expression, int line)
    {
        ++g_Checks;
        if (!condition)
        {
            ++g_Failures;
            std::printf("  FAIL line %d: %s\n", line, expression);
        }
    }

#define CHECK(expr) Check((expr), #expr, __LINE__)

    bool Near(float a, float b, float eps = 1e-5f) { return std::fabs(a - b) <= eps; }

    void BeginGroup(const char* name) { std::printf("[group] %s\n", name); }

    /// Plays a timeline tick by tick from `start` and counts how often `event_time` fires.
    int CountFires(float event_time, float start, float dt, float speed, float duration, bool loop, int ticks)
    {
        int fires = 0;
        float time = start;
        for (int i = 0; i < ticks; ++i)
        {
            const TimelineEval::TimeStep step = TimelineEval::Advance(time, dt, speed, duration, loop);
            fires += step.Crosses(event_time) ? 1 : 0;
            time = step.new_time;
            if (step.finished)
            {
                break;
            }
        }
        return fires;
    }
}  // namespace

int main()
{
    using namespace TimelineEval;

    BeginGroup("forward step covers [from, to)");
    {
        const TimeStep s = Advance(0.0f, 0.25f, 1.0f, 4.0f, false);
        CHECK(s.span_count == 1);
        CHECK(Near(s.new_time, 0.25f));
        CHECK(!s.finished && !s.wrapped);
        CHECK(s.Crosses(0.0f));    // an event at the very start fires on the first tick
        CHECK(!s.Crosses(0.25f));  // the tick boundary belongs to the NEXT tick
        const TimeStep s2 = Advance(s.new_time, 0.25f, 1.0f, 4.0f, false);
        CHECK(s2.Crosses(0.25f));
    }

    BeginGroup("consecutive ticks partition time: each event fires exactly once");
    {
        // dt does not divide the event times -- every point must still land in one tick.
        const float events[] = {0.0f, 0.05f, 0.1f, 1.0f, 2.345f, 3.3333f, 5.0f};
        for (float e : events)
        {
            CHECK(CountFires(e, 0.0f, 1.0f / 30.0f, 1.0f, 5.0f, false, 1000) == 1);
        }
        // The old director only fired an event while the playhead was INSIDE the event
        // clip; a short clip skipped by a long frame never fired. A point event cannot be
        // skipped.
        CHECK(CountFires(0.3f, 0.0f, 0.5f, 1.0f, 5.0f, false, 100) == 1);
    }

    BeginGroup("non-looping end: inclusive, finished, time held at duration");
    {
        const TimeStep s = Advance(3.9f, 0.25f, 1.0f, 4.0f, false);
        CHECK(s.finished);
        CHECK(!s.wrapped);
        CHECK(Near(s.new_time, 4.0f));
        CHECK(s.span_count == 1);
        CHECK(s.Crosses(4.0f));  // an event at the end still fires
        CHECK(s.Crosses(3.9f));
        CHECK(CountFires(4.0f, 0.0f, 0.3f, 1.0f, 4.0f, false, 100) == 1);
    }

    BeginGroup("looping keeps the remainder (old director snapped to 0)");
    {
        const TimeStep s = Advance(3.75f, 0.5f, 1.0f, 4.0f, true);
        CHECK(s.wrapped && !s.finished);
        CHECK(Near(s.new_time, 0.25f));
        CHECK(s.span_count == 2);
        CHECK(s.Crosses(3.9f));
        CHECK(s.Crosses(4.0f));  // end point passed on the way round
        CHECK(s.Crosses(0.0f));
        CHECK(s.Crosses(0.1f));
        CHECK(!s.Crosses(0.25f));
        CHECK(!s.Crosses(2.0f));
    }

    BeginGroup("looping lands exactly on the wrap point");
    {
        const TimeStep s = Advance(3.5f, 0.5f, 1.0f, 4.0f, true);
        CHECK(s.wrapped);
        CHECK(Near(s.new_time, 0.0f));
        CHECK(s.span_count == 1);
        CHECK(s.Crosses(4.0f));
        CHECK(!s.Crosses(0.0f));  // reached, not passed: next tick owns it
        CHECK(Advance(s.new_time, 0.5f, 1.0f, 4.0f, true).Crosses(0.0f));
    }

    BeginGroup("looping: events fire once per cycle, both ends distinct");
    {
        // 0.25 is exact in binary: 16 ticks per 4s cycle, 48 ticks = 3 cycles.
        CHECK(CountFires(0.0f, 0.0f, 0.25f, 1.0f, 4.0f, true, 48) == 3);
        CHECK(CountFires(4.0f, 0.0f, 0.25f, 1.0f, 4.0f, true, 48) == 3);
        CHECK(CountFires(1.0f, 0.0f, 0.25f, 1.0f, 4.0f, true, 48) == 3);
        CHECK(CountFires(2.6f, 0.0f, 0.25f, 1.0f, 4.0f, true, 48) == 3);
        // Non-dividing dt over many cycles: one fire per cycle, never two.
        const int fires = CountFires(1.234f, 0.0f, 1.0f / 60.0f, 1.0f, 2.0f, true, 60 * 20);
        CHECK(fires == 10);
    }

    BeginGroup("a tick longer than the timeline wraps once");
    {
        const TimeStep s = Advance(1.0f, 9.5f, 1.0f, 4.0f, true);
        CHECK(s.wrapped);
        CHECK(Near(s.new_time, 2.5f));  // (1 + 9.5) mod 4
        CHECK(s.span_count == 2);
        CHECK(s.new_time >= 0.0f && s.new_time < 4.0f);
    }

    BeginGroup("speed scales the step");
    {
        CHECK(Near(Advance(0.0f, 0.5f, 2.0f, 4.0f, false).new_time, 1.0f));
        CHECK(Near(Advance(0.0f, 0.5f, 0.5f, 4.0f, false).new_time, 0.25f));
    }

    BeginGroup("reverse playback mirrors: (to, from], end at 0 inclusive");
    {
        const TimeStep s = Advance(1.0f, 0.4f, -1.0f, 4.0f, false);
        CHECK(Near(s.new_time, 0.6f));
        CHECK(!s.finished);
        CHECK(s.Crosses(1.0f));
        CHECK(!s.Crosses(0.6f));
        const TimeStep end = Advance(0.3f, 0.4f, -1.0f, 4.0f, false);
        CHECK(end.finished);
        CHECK(Near(end.new_time, 0.0f));
        CHECK(end.Crosses(0.0f));
        CHECK(CountFires(2.0f, 4.0f, 1.0f / 30.0f, -1.0f, 4.0f, false, 1000) == 1);

        const TimeStep wrap = Advance(0.2f, 0.5f, -1.0f, 4.0f, true);
        CHECK(wrap.wrapped);
        CHECK(Near(wrap.new_time, 3.7f));
        CHECK(wrap.Crosses(0.0f));
        CHECK(wrap.Crosses(0.1f));
        CHECK(wrap.Crosses(4.0f));
        CHECK(wrap.Crosses(3.9f));
        CHECK(!wrap.Crosses(3.7f));
        CHECK(!wrap.Crosses(2.0f));
    }

    BeginGroup("degenerate inputs never produce spans or NaN time");
    {
        const TimeStep zero_dur = Advance(1.0f, 0.1f, 1.0f, 0.0f, false);
        CHECK(zero_dur.span_count == 0 && zero_dur.finished && zero_dur.new_time == 0.0f);
        const TimeStep zero_dur_loop = Advance(1.0f, 0.1f, 1.0f, 0.0f, true);
        CHECK(zero_dur_loop.span_count == 0 && !zero_dur_loop.finished);
        const TimeStep paused = Advance(1.0f, 0.1f, 0.0f, 4.0f, true);
        CHECK(paused.span_count == 0 && Near(paused.new_time, 1.0f));
        const TimeStep nan_dt = Advance(1.0f, std::nanf(""), 1.0f, 4.0f, true);
        CHECK(nan_dt.span_count == 0 && Near(nan_dt.new_time, 1.0f));
        const TimeStep nan_time = Advance(std::nanf(""), 0.1f, 1.0f, 4.0f, false);
        CHECK(std::isfinite(nan_time.new_time) && Near(nan_time.new_time, 0.1f));
        const TimeStep clamped = Advance(9.0f, 0.1f, 1.0f, 4.0f, false);
        CHECK(clamped.finished && Near(clamped.new_time, 4.0f));
    }

    BeginGroup("clip window is half-open; zero-length never active");
    {
        CHECK(!ClipActiveAt(1.0f, 2.0f, 0.999f));
        CHECK(ClipActiveAt(1.0f, 2.0f, 1.0f));
        CHECK(ClipActiveAt(1.0f, 2.0f, 2.999f));
        CHECK(!ClipActiveAt(1.0f, 2.0f, 3.0f));
        CHECK(!ClipActiveAt(1.0f, 0.0f, 1.0f));
        CHECK(!ClipActiveAt(1.0f, -1.0f, 0.5f));
        // A clip that ends exactly at a non-looping timeline's end is inactive once the
        // playhead is held at the end, so the director exits it (restores activation,
        // stops audio) instead of leaving it applied.
        CHECK(!ClipActiveAt(2.0f, 2.0f, 4.0f));
    }

    BeginGroup("animation phase");
    {
        CHECK(Near(AnimationPhase(0.5f, 1.0f, 2.0f, false), 0.25f));
        CHECK(Near(AnimationPhase(2.5f, 1.0f, 1.0f, true), 0.5f));
        CHECK(Near(AnimationPhase(1.0f, 2.0f, 4.0f, true), 0.5f));
        const float held = AnimationPhase(10.0f, 1.0f, 1.0f, false);
        CHECK(held < 1.0f && held > 0.9999f);  // held on the last frame, not wrapped to 0
        CHECK(held == MaxHoldPhase());
        CHECK(AnimationPhase(-1.0f, 1.0f, 1.0f, false) == 0.0f);
        CHECK(Near(AnimationPhase(0.25f, -1.0f, 1.0f, true), 0.75f));
        CHECK(AnimationPhase(1.0f, 1.0f, 0.0f, true) == 0.0f);
        CHECK(AnimationPhase(std::nanf(""), 1.0f, 1.0f, true) == 0.0f);
        // Every looping phase is in [0, 1), including values a hair below an integer.
        bool all_in_range = true;
        for (int i = 0; i < 10000; ++i)
        {
            const float p = AnimationPhase(i * 0.0137f, 1.3f, 0.7f, true);
            all_in_range = all_in_range && p >= 0.0f && p < 1.0f;
        }
        CHECK(all_in_range);
        CHECK(AnimationPhase(std::nextafter(3.0f, 0.0f), 1.0f, 1.0f, true) < 1.0f);
    }

    BeginGroup("animation length resolution");
    {
        CHECK(ResolveAnimationLength(true, 1.5f, 3.0f, 1.0f) == 1.5f);
        CHECK(ResolveAnimationLength(false, 1.5f, 3.0f, 1.0f) == 3.0f);
        CHECK(ResolveAnimationLength(true, 0.0f, 3.0f, 2.0f) == 6.0f);
        CHECK(ResolveAnimationLength(false, 0.0f, 3.0f, -2.0f) == 6.0f);
        CHECK(ResolveAnimationLength(false, 0.0f, 0.0f, 1.0f) == 0.0f);
        // Fallback length => the clip plays exactly once across its window.
        const float len = ResolveAnimationLength(false, 0.0f, 3.0f, 1.0f);
        CHECK(Near(AnimationPhase(1.5f, 1.0f, len, false), 0.5f));
    }

    BeginGroup("transition table");
    {
        CHECK(Transition(false, false) == ClipTransition::None);
        CHECK(Transition(false, true) == ClipTransition::Enter);
        CHECK(Transition(true, true) == ClipTransition::Stay);
        CHECK(Transition(true, false) == ClipTransition::Exit);
    }

    std::printf("\n%d checks, %d failures\n", g_Checks, g_Failures);
    if (g_Failures == 0)
    {
        std::printf("TimelineEvaluationTest: PASS\n");
    }
    return g_Failures == 0 ? 0 : 1;
}
