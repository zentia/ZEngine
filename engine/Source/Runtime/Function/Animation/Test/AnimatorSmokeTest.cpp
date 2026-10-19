// =============================================================================
// Animator (state machine) smoke test (PR-SK3).
//
// Headless: no window, no GPU, no assets on disk. It exercises the evaluator
// that decides WHICH clip plays and with what weight -- everything a character
// needs in order to react to gameplay, which the engine could not do before
// (AnimationComponent mixed a fixed clip list at a static weight, always
// looping, with no speed and no events).
//
//   cmake -B build -DZENGINE_BUILD_ANIMATOR_SMOKE_TEST=ON
//   cmake --build build --target AnimatorSmokeTest
//   ./build/.../AnimatorSmokeTest
// =============================================================================

#include "RegisterRuntime.h"

#include "Runtime/BaseClasses/ObjectManager.h"
#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Core/Memory/MemoryManager.h"
#include "Runtime/Function/Animation/AnimatorInstance.h"
#include "Runtime/Resource/Asset/AssetManager.h"
#include "Runtime/Resource/Asset/RuntimeAssetManager.h"
#include "Runtime/Resource/ResType/Data/AnimatorController.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace
{
    int g_Checks = 0;
    int g_Failures = 0;

    void Check(bool condition, const char* name)
    {
        ++g_Checks;
        if (!condition)
        {
            ++g_Failures;
            std::printf("[FAIL] %s\n", name);
        }
        else
        {
            std::printf("[ ok ] %s\n", name);
        }
    }

    void CheckNear(float in_actual, float in_expected, float in_tolerance, const char* name)
    {
        const bool ok = (in_actual >= in_expected - in_tolerance) && (in_actual <= in_expected + in_tolerance);
        if (!ok)
        {
            std::printf("       (got %f, expected %f)\n", static_cast<double>(in_actual), static_cast<double>(in_expected));
        }
        Check(ok, name);
    }

    AnimatorParameter MakeFloat(const char* name, float default_value)
    {
        AnimatorParameter parameter;
        parameter.name = name;
        parameter.type = static_cast<int>(AnimatorParameterType::Float);
        parameter.default_float = default_value;
        return parameter;
    }

    AnimatorParameter MakeTrigger(const char* name)
    {
        AnimatorParameter parameter;
        parameter.name = name;
        parameter.type = static_cast<int>(AnimatorParameterType::Trigger);
        return parameter;
    }

    AnimatorState MakeState(const char* name, float clip_length, float speed = 1.0f,
                            AnimatorWrapMode wrap = AnimatorWrapMode::Loop)
    {
        AnimatorState state;
        state.name = name;
        state.clip_file_path = (std::string(name) + ".anim.zasset").c_str();
        state.anim_skel_map_path = (std::string(name) + ".animskelmap.zasset").c_str();
        state.clip_length = clip_length;
        state.speed = speed;
        state.wrap_mode = static_cast<int>(wrap);
        return state;
    }

    AnimatorCondition MakeCondition(const char* parameter, AnimatorConditionMode mode, float threshold)
    {
        AnimatorCondition condition;
        condition.parameter = parameter;
        condition.mode = static_cast<int>(mode);
        condition.threshold = threshold;
        return condition;
    }
}  // namespace

int main()
{
    RegisterCore();
    RegisterPlatform();
    if (!SystemRegistry::GetInstance().InitializeAll(false))
    {
        std::printf("FATAL: system bring-up failed\n");
        return 77;
    }

    auto* object_manager = GET_SYSTEM(ObjectManager);
    Object* produced = object_manager->Produce(TypeOf<AnimatorController>(), 0);
    if (produced == nullptr)
    {
        std::printf("FATAL: cannot produce an AnimatorController\n");
        return 77;
    }
    auto* controller = static_cast<AnimatorController*>(produced);

    // idle (1s) <-> run (0.5s), driven by the "speed" parameter.
    controller->parameters.push_back(MakeFloat("speed", 0.0f));
    controller->parameters.push_back(MakeTrigger("jump"));
    controller->states.push_back(MakeState("idle", 1.0f));
    controller->states.push_back(MakeState("run", 0.5f, 2.0f));
    controller->states.push_back(MakeState("jump", 0.4f, 1.0f, AnimatorWrapMode::Once));

    AnimatorTransition to_run;
    to_run.from_state = 0;
    to_run.to_state = 1;
    to_run.conditions.push_back(MakeCondition("speed", AnimatorConditionMode::Greater, 0.5f));
    to_run.duration = 0.25f;
    to_run.priority = 0;
    controller->transitions.push_back(to_run);

    AnimatorTransition to_jump;
    to_jump.from_state = 0;
    to_jump.to_state = 2;
    to_jump.conditions.push_back(MakeCondition("jump", AnimatorConditionMode::If, 0.0f));
    to_jump.duration = 0.1f;
    to_jump.priority = 10;  // wins over to_run when both are eligible.
    controller->transitions.push_back(to_jump);

    AnimatorTransition back_to_idle;
    back_to_idle.from_state = 1;
    back_to_idle.to_state = 0;
    back_to_idle.conditions.push_back(MakeCondition("speed", AnimatorConditionMode::Less, 0.5f));
    back_to_idle.duration = 0.2f;
    controller->transitions.push_back(back_to_idle);

    controller->default_state = 0;

    AnimatorInstance animator;
    std::string error;
    Check(animator.Initialize(*controller, error), "A1: controller initialises");
    Check(animator.GetCurrentStateIndex() == 0, "A1: starts in the default state");
    Check(std::string(animator.GetCurrentStateName()) == "idle", "A1: default state is named");
    CheckNear(animator.GetFloat("speed"), 0.0f, 1.0e-5f, "A1: parameters take their authored default");

    // ---- A2: parameter-driven transition + crossfade ------------------------
    animator.Tick(0.1f);
    Check(animator.GetCurrentStateIndex() == 0, "A2: no transition while the condition is false");
    CheckNear(animator.GetNormalizedTime(0), 0.1f, 1.0e-4f, "A2: the idle playhead advances in seconds/length");

    animator.SetFloat("speed", 1.0f);
    animator.Tick(0.1f);
    Check(animator.IsInTransition() && animator.GetNextStateIndex() == 1, "A2: raising 'speed' starts the fade to run");
    Check(animator.GetBlendState().clip_count == 2, "A2: a fade feeds TWO clips to the mixer");
    // The fade is only STARTED by this tick, so it still contributes nothing.
    CheckNear(animator.GetBlendState().blend_weight[1], 0.0f, 1.0e-4f, "A2: a fresh fade starts at weight 0");

    animator.Tick(0.1f);
    CheckNear(animator.GetBlendState().blend_weight[1], 0.4f, 1.0e-4f, "A2: fade weight is elapsed / duration");
    CheckNear(animator.GetBlendState().blend_weight[0] + animator.GetBlendState().blend_weight[1], 1.0f, 1.0e-5f,
              "A2: fade weights sum to 1 (the mixer normalises by them)");

    animator.Tick(0.15f);
    Check(!animator.IsInTransition() && animator.GetCurrentStateIndex() == 1, "A2: the fade promotes the target to current");
    Check(animator.GetBlendState().clip_count == 1, "A2: a settled state feeds ONE clip");
    CheckNear(animator.GetBlendState().blend_weight[0], 1.0f, 1.0e-5f, "A2: a settled state has full weight");

    // run has speed 2 and length 0.5 -> 0.1s of wall clock is 0.4 of the clip.
    animator.Play("run");
    animator.Tick(0.1f);
    CheckNear(animator.GetNormalizedTime(1), 0.4f, 1.0e-4f, "A2: per-state speed scales the playhead");

    // ---- A3: back-transition when the parameter drops -----------------------
    animator.SetFloat("speed", 0.0f);
    animator.Tick(0.05f);
    Check(animator.IsInTransition() && animator.GetNextStateIndex() == 0, "A3: dropping 'speed' fades back to idle");
    animator.Tick(0.2f);
    Check(animator.GetCurrentStateIndex() == 0, "A3: back-transition completes");

    // ---- A4: trigger fires exactly one transition ---------------------------
    animator.SetTrigger("jump");
    animator.Tick(0.01f);
    Check(animator.GetNextStateIndex() == 2, "A4: a trigger starts its transition");
    Check(!animator.GetBool("jump"), "A4: the trigger is consumed as it fires");
    animator.Tick(0.2f);
    Check(animator.GetCurrentStateIndex() == 2, "A4: the jump state became current");

    // ---- A5: priority resolves simultaneous candidates ----------------------
    {
        AnimatorInstance priority_animator;
        priority_animator.Initialize(*controller, error);
        priority_animator.SetFloat("speed", 1.0f);
        priority_animator.SetTrigger("jump");
        priority_animator.Tick(0.01f);
        Check(priority_animator.GetNextStateIndex() == 2, "A5: the higher-priority transition wins");
    }

    // ---- A6: wrap modes -----------------------------------------------------
    {
        AnimatorController wrap_controller;
        wrap_controller.states.push_back(MakeState("once", 1.0f, 1.0f, AnimatorWrapMode::Once));
        wrap_controller.states.push_back(MakeState("pingpong", 1.0f, 1.0f, AnimatorWrapMode::PingPong));
        wrap_controller.states.push_back(MakeState("loop", 1.0f, 1.0f, AnimatorWrapMode::Loop));
        wrap_controller.default_state = 0;

        AnimatorInstance wrap_animator;
        wrap_animator.Initialize(wrap_controller, error);

        wrap_animator.Tick(0.5f);
        CheckNear(wrap_animator.GetNormalizedTime(0), 0.5f, 1.0e-4f, "A6: Once plays normally before the end");
        wrap_animator.Tick(1.0f);
        CheckNear(wrap_animator.GetNormalizedTime(0), 1.0f, 1.0e-4f, "A6: Once clamps at the last frame");
        wrap_animator.Tick(1.0f);
        CheckNear(wrap_animator.GetNormalizedTime(0), 1.0f, 1.0e-4f, "A6: Once stays clamped");

        wrap_animator.Play("pingpong");
        wrap_animator.Tick(0.5f);
        CheckNear(wrap_animator.GetNormalizedTime(1), 0.5f, 1.0e-4f, "A6: PingPong plays forward");
        wrap_animator.Tick(0.75f);
        CheckNear(wrap_animator.GetNormalizedTime(1), 0.75f, 1.0e-4f, "A6: PingPong reflects at the end");
        wrap_animator.Tick(0.6f);
        CheckNear(wrap_animator.GetNormalizedTime(1), 0.15f, 1.0e-4f, "A6: PingPong plays back after the bounce");

        wrap_animator.Play("loop");
        wrap_animator.Tick(1.25f);
        CheckNear(wrap_animator.GetNormalizedTime(2), 0.25f, 1.0e-4f, "A6: Loop wraps into the next pass");
    }

    // ---- A7: events fire once per pass --------------------------------------
    {
        AnimatorController event_controller;
        AnimatorState state = MakeState("walk", 2.0f);
        AnimatorEvent event;
        event.normalized_time = 0.5f;
        event.function_name = "Footstep";
        state.events.push_back(event);
        event_controller.states.push_back(state);
        event_controller.default_state = 0;

        AnimatorInstance event_animator;
        event_animator.Initialize(event_controller, error);

        event_animator.Tick(0.5f);
        Check(event_animator.GetFiredEvents().empty(), "A7: the event has not fired before its time");
        event_animator.Tick(0.5f);
        Check(event_animator.GetFiredEvents().size() == 1 && event_animator.GetFiredEvents()[0] == "Footstep",
              "A7: the event fires when the playhead crosses it");
        event_animator.Tick(0.5f);
        Check(event_animator.GetFiredEvents().empty(), "A7: the event does not refire within the same pass");
        // 1.5 -> 2.5s wraps back to 0.25 of the clip, so the 0.5 mark is still
        // ahead in the NEW pass: no refire yet.
        event_animator.Tick(1.0f);
        Check(event_animator.GetFiredEvents().empty(), "A7: wrapping resets the event for the next pass");
        event_animator.Tick(0.5f);
        Check(event_animator.GetFiredEvents().size() == 1, "A7: the event fires again on the next loop");
    }

    // ---- A8: exit time gates a transition ------------------------------------
    {
        AnimatorController exit_controller;
        exit_controller.parameters.push_back(MakeFloat("go", 0.0f));
        exit_controller.states.push_back(MakeState("windup", 1.0f));
        exit_controller.states.push_back(MakeState("swing", 1.0f));

        AnimatorTransition exit_transition;
        exit_transition.from_state = 0;
        exit_transition.to_state = 1;
        exit_transition.conditions.push_back(MakeCondition("go", AnimatorConditionMode::Greater, 0.0f));
        exit_transition.has_exit_time = true;
        exit_transition.exit_time = 0.9f;
        exit_transition.duration = 0.1f;
        exit_controller.transitions.push_back(exit_transition);
        exit_controller.default_state = 0;

        AnimatorInstance exit_animator;
        exit_animator.Initialize(exit_controller, error);
        exit_animator.SetFloat("go", 1.0f);

        exit_animator.Tick(0.5f);
        Check(!exit_animator.IsInTransition(), "A8: an exit-time transition waits for its exit time");
        exit_animator.Tick(0.45f);
        Check(exit_animator.IsInTransition(), "A8: it starts once the playhead passes the exit time");
    }

    // ---- A9: explicit control ------------------------------------------------
    {
        AnimatorInstance direct;
        direct.Initialize(*controller, error);
        direct.Play("run", 0.25f);
        Check(direct.GetCurrentStateIndex() == 1, "A9: Play jumps to a state");
        CheckNear(direct.GetNormalizedTime(1), 0.25f, 1.0e-4f, "A9: Play can seek into the clip");

        direct.CrossFade("idle", 0.5f);
        Check(direct.IsInTransition() && direct.GetNextStateIndex() == 0, "A9: CrossFade starts a fade");
        direct.Tick(0.25f);
        CheckNear(direct.GetBlendState().blend_weight[1], 0.5f, 1.0e-4f, "A9: CrossFade weight tracks its duration");

        direct.CrossFade("run", 0.0f);
        Check(direct.GetCurrentStateIndex() == 1 && !direct.IsInTransition(), "A9: a zero-length CrossFade snaps");
    }

    // ---- A10: misuse is survivable ------------------------------------------
    {
        AnimatorInstance misuse;
        AnimatorController empty_controller;
        Check(!misuse.Initialize(empty_controller, error), "A10: a controller with no states is rejected");

        AnimatorInstance ok;
        ok.Initialize(*controller, error);
        ok.SetFloat("nope", 1.0f);   // unknown parameter
        ok.SetBool("speed", true);   // wrong type
        ok.Play("nope");             // unknown state
        ok.Tick(0.1f);
        Check(ok.GetCurrentStateIndex() == 0, "A10: bad parameters / state names do not derail playback");
    }

    // ---- A11: the controller round-trips through the real serializer --------
    // A state machine that survives in memory but not on disk is worthless, and
    // this is the only place the nested vectors (conditions / events) are proven
    // to serialize.
    {
        auto* asset_manager = GET_SYSTEM(AssetManager);
        if (asset_manager != nullptr)
        {
            const std::filesystem::path path =
                std::filesystem::temp_directory_path() / "animator_smoke_controller.zasset";
            std::error_code ec;
            std::filesystem::remove(path, ec);

            const bool written = asset_manager->WriteObjectToDiskThreadSafe(path, *controller);
            Check(written, "A11: controller writes to disk");

            AnimatorController* loaded = asset_manager->loadAsset<AnimatorController>(path.generic_string().c_str());
            Check(loaded != nullptr, "A11: controller reads back");
            if (loaded != nullptr)
            {
                Check(loaded->parameters.size() == controller->parameters.size(), "A11: parameters survive");
                Check(loaded->states.size() == controller->states.size(), "A11: states survive");
                Check(loaded->transitions.size() == controller->transitions.size(), "A11: transitions survive");
                Check(loaded->transitions[0].conditions.size() == 1 &&
                          loaded->transitions[0].conditions[0].parameter == "speed",
                      "A11: nested conditions survive");
                Check(loaded->states[2].wrap_mode == static_cast<int>(AnimatorWrapMode::Once),
                      "A11: per-state wrap mode survives");
                Check(loaded->FindState("run") == 1 && loaded->FindParameter("jump") == 1,
                      "A11: name lookups work after a round-trip");
            }
            std::filesystem::remove(path, ec);
        }
        else
        {
            Check(false, "A11: AssetManager unavailable for the round-trip");
        }
    }

    MemoryManager::DestroyObject(produced);

    std::printf("\nAnimator smoke test: %d check(s), %d failure(s)\n", g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
