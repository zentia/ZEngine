// =====================================================================
// EnhancedInputFrameDeltaSmokeTest
// ---------------------------------------------------------------------
// Verifies EnhancedInputSystem::ComputeFrameDelta — the wall-clock delta
// that Tick() feeds into the trigger evaluator (trigger->Evaluate(value,
// delta_time)). Before this change Tick() hardcoded 1/60, so a held key
// drove TriggerHold at a fixed rate regardless of the real frame rate:
// at 30fps a 0.5s hold fired in ~1.0s of wall time instead of 0.5s.
//
// Cases:
//   D1 first frame (has_ticked=false) -> 0 (idle gap can't spike Hold)
//   D2 normal frame -> exact elapsed delta
//   D3 30fps cadence -> 1/30, NOT the old hardcoded 1/60
//   D4 stall (>250ms gap) -> clamped to k_MaxFrameDeltaS
//   D5 backwards-clock edge (negative raw) -> 0 (never negative delta)
//   D6 exactly the cap -> not clamped (boundary is inclusive)
//   D7 downstream: TriggerHold(0.5s) at 30fps fires at 0.5s of wall time
//      (proves the fix changes behaviour vs the old fixed 1/60 guess)
//
// Own main(); excluded from ZRuntime in Runtime/CMakeLists.txt. Pure CPU,
// no project / window / GPU — runs headless on every host (web via
// run_all_smoke_tests_web.py, native via wsl_verify_linux.sh).
// =====================================================================

#include "Runtime/Function/Input/EnhancedInputSystem.h"
#include "Runtime/Function/Input/InputTriggers.h"

#include <cstdio>
#include <cmath>

namespace
{
    int g_Checks = 0;
    int g_Failures = 0;

    void Check(bool condition, const char* name)
    {
        ++g_Checks;
        if (!condition)
        {
            std::fprintf(stderr, "  [FAIL] %s\n", name);
            ++g_Failures;
        }
        else
        {
            std::fprintf(stdout, "  [ ok ] %s\n", name);
        }
    }

    // Feed a TriggerHold a sequence of frames at a fixed cadence and report
    // the wall time (seconds) at which it first fires Triggered. The trigger
    // is driven by an actuated boolean value; each Evaluate advances the hold
    // timer by the per-frame delta.
    float HoldFireWallTime(float hold_time, float per_frame_delta)
    {
        TriggerHold hold(hold_time);
        InputActionValue actuated(true);
        float wall = 0.0f;
        for (int i = 0; i < 10000; ++i)
        {
            const uint8_t ev = hold.Evaluate(actuated, per_frame_delta);
            wall += per_frame_delta;
            if ((ev & static_cast<uint8_t>(ETriggerEvent::Triggered)) != 0)
            {
                return wall;
            }
        }
        return -1.0f; // never fired
    }
} // namespace

int main()
{
    // ---- D1: first frame reports 0 ------------------------------------
    {
        const float d = EnhancedInputSystem::ComputeFrameDelta(0.0, false, 123.456);
        Check(d == 0.0f, "D1 first frame (has_ticked=false) -> 0 delta");
    }

    // ---- D2: normal frame -> exact elapsed delta ----------------------
    {
        // 16.7ms real gap, has_ticked true.
        const float d = EnhancedInputSystem::ComputeFrameDelta(100.0, true, 100.0 + 1.0 / 60.0);
        Check(std::fabs(d - 1.0f / 60.0f) < 1e-6f, "D2 normal frame -> exact elapsed (~1/60)");
    }

    // ---- D3: 30fps cadence -> 1/30, not the old hardcoded 1/60 --------
    {
        const float d = EnhancedInputSystem::ComputeFrameDelta(100.0, true, 100.0 + 1.0 / 30.0);
        Check(std::fabs(d - 1.0f / 30.0f) < 1e-6f, "D3 30fps frame -> 1/30 (not the old fixed 1/60)");
        Check(d > 1.0f / 60.0f, "D3 30fps delta is larger than the old hardcoded 1/60");
    }

    // ---- D4: stall (>250ms) -> clamped to k_MaxFrameDeltaS ------------
    {
        const float d = EnhancedInputSystem::ComputeFrameDelta(100.0, true, 100.0 + 1.0); // 1s stall
        Check(d == EnhancedInputSystem::k_MaxFrameDeltaS, "D4 1s stall -> clamped to 250ms cap");
        Check(d < 1.0f, "D4 clamped delta is well below the raw 1s gap");
    }

    // ---- D5: backwards-clock edge -> 0 --------------------------------
    {
        const float d = EnhancedInputSystem::ComputeFrameDelta(100.0, true, 99.0); // -1s
        Check(d == 0.0f, "D5 backwards clock edge -> 0 (never negative delta)");
    }

    // ---- D6: exactly the cap -> not clamped (inclusive boundary) ------
    {
        const float d = EnhancedInputSystem::ComputeFrameDelta(
            100.0, true, 100.0 + EnhancedInputSystem::k_MaxFrameDeltaS);
        Check(std::fabs(d - EnhancedInputSystem::k_MaxFrameDeltaS) < 1e-9f,
              "D6 exactly-250ms gap -> returned as-is (inclusive boundary)");
    }

    // ---- D7: downstream — TriggerHold fires at real wall time ---------
    {
        // A 0.5s hold at 30fps (per-frame 1/30) should fire after ~0.5s of
        // wall time. With the OLD hardcoded 1/60 the same wall time would
        // only accumulate ~0.25s of hold, never firing — so this directly
        // proves the fix corrects the hold timing.
        const float wall30 = HoldFireWallTime(0.5f, 1.0f / 30.0f);
        Check(wall30 >= 0.49f && wall30 <= 0.51f,
              "D7 TriggerHold(0.5s) at 30fps fires at ~0.5s wall time");

        // At 60fps it should also fire at ~0.5s (within one frame).
        const float wall60 = HoldFireWallTime(0.5f, 1.0f / 60.0f);
        Check(wall60 >= 0.49f && wall60 <= 0.51f,
              "D7b TriggerHold(0.5s) at 60fps fires at ~0.5s wall time");

        // A slow 20fps cadence must still accumulate real time (not be
        // frozen), firing at ~0.5s even though each frame is 50ms.
        const float wall20 = HoldFireWallTime(0.5f, 1.0f / 20.0f);
        Check(wall20 >= 0.49f && wall20 <= 0.55f,
              "D7c TriggerHold(0.5s) at 20fps still fires at real ~0.5s wall time");
    }

    std::fprintf(stdout, "\nEnhancedInput frame-delta: %d checks, %d failed\n",
                 g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
