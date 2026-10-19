#pragma once

// =============================================================================
// AnimatorInstance -- evaluates an AnimatorController (PR-SK3).
//
// One instance per animating entity: it owns the RUNTIME STATE (parameter
// values, each state's playhead, the active transition) while the controller
// asset stays shared and immutable -- splitting them the same way Unity does,
// so ten characters can share one controller.
//
// It does not sample any pose itself. `GetBlendState()` emits at most two
// clips with a crossfade weight and feeds the existing
// `AnimationSystem::GetBlendStateWithClipData()` -> `Skeleton::ApplyAnimation`
// path, so there is exactly one sampler in the engine.
// =============================================================================

#include "Runtime/Resource/ResType/Data/AnimatorController.h"
#include "Runtime/Resource/ResType/Data/BlendState.h"

#include <EASTL/string.h>

#include <string>
#include <vector>

class AnimatorInstance
{
public:
    /// Binds a controller and resets runtime state to the defaults.
    bool Initialize(const AnimatorController& controller, std::string& out_error);
    bool IsValid() const { return m_Controller != nullptr; }
    void Reset();

    // ---- Parameters (game code -> state machine) ---------------------------
    void SetFloat(const char* name, float value);
    void SetInt(const char* name, int value);
    void SetBool(const char* name, bool value);
    void SetTrigger(const char* name);
    void ResetTrigger(const char* name);

    float GetFloat(const char* name) const;
    int GetInt(const char* name) const;
    bool GetBool(const char* name) const;

    // ---- Driving ------------------------------------------------------------
    void Tick(float delta_time);

    /// Input for `AnimationSystem::GetBlendStateWithClipData()`. One clip when
    /// settled, two with weights (1-w, w) during a crossfade.
    const BlendState& GetBlendState() const { return m_BlendState; }

    int GetCurrentStateIndex() const { return m_CurrentState; }
    int GetNextStateIndex() const { return m_TransitionTarget; }
    bool IsInTransition() const { return m_TransitionTarget >= 0; }
    const char* GetCurrentStateName() const;
    /// Normalized time [0, 1] of `state_index`, regardless of which is active.
    float GetNormalizedTime(int state_index) const;

    /// Events fired during the last `Tick()`, cleared at the start of the next.
    const std::vector<eastl::string>& GetFiredEvents() const { return m_FiredEvents; }

    // ---- Explicit control ---------------------------------------------------
    /// Jumps to a state immediately (no fade).
    void Play(const char* state_name, float normalized_time = 0.0f);
    /// Fades to a state over `duration` seconds (0 = snap).
    void CrossFade(const char* state_name, float duration, float normalized_time = 0.0f);

    const AnimatorController* GetController() const { return m_Controller; }

private:
    struct ParameterValue
    {
        float float_value {0.0f};
        int int_value {0};
        bool bool_value {false};
    };

    struct StateRuntime
    {
        /// Playhead in seconds, already wrapped/clamped per the state's mode.
        float time {0.0f};
        /// Per-event "already fired" flags; reset when the playhead wraps or is
        /// seeked backwards, so a looping clip fires each pass.
        std::vector<bool> fired;
        int direction {1};  // PingPong only.
    };

    void AdvanceState(int state_index, float delta_time);
    void CollectEvents(int state_index);
    /// Not const: a satisfied Trigger condition CONSUMES the trigger.
    bool ConditionsMet(const AnimatorTransition& transition);
    bool ExitTimeReached(const AnimatorTransition& transition) const;
    void StartTransition(const AnimatorTransition& transition, int to_state);
    void EnterState(int state_index, float normalized_time);
    void RebuildBlendState();

    const AnimatorController* m_Controller {nullptr};

    std::vector<ParameterValue> m_Values;

    int m_CurrentState {-1};
    int m_TransitionTarget {-1};
    float m_TransitionTime {0.0f};
    float m_TransitionDuration {0.0f};

    std::vector<StateRuntime> m_StateRuntime;

    BlendState m_BlendState;
    std::vector<eastl::string> m_FiredEvents;
};
