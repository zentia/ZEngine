#include "AnimatorInstance.h"

#include "Runtime/Core/Base/Macro.h"

#include <algorithm>
#include <cmath>

namespace
{
    float Fract(float in_value)
    {
        return in_value - std::floor(in_value);
    }
}  // namespace

bool AnimatorInstance::Initialize(const AnimatorController& controller, std::string& out_error)
{
    m_Controller = nullptr;
    m_Values.clear();
    m_StateRuntime.clear();
    m_CurrentState = -1;
    m_TransitionTarget = -1;
    m_TransitionTime = 0.0f;
    m_TransitionDuration = 0.0f;
    m_FiredEvents.clear();

    if (controller.states.empty())
    {
        out_error = "animator controller has no states";
        return false;
    }

    m_Controller = &controller;

    m_Values.reserve(controller.parameters.size());
    for (const AnimatorParameter& parameter : controller.parameters)
    {
        ParameterValue value;
        value.float_value = parameter.default_float;
        value.int_value = parameter.default_int;
        value.bool_value = parameter.default_bool;
        m_Values.push_back(value);
    }

    m_StateRuntime.assign(controller.states.size(), StateRuntime {});
    for (size_t state_index = 0; state_index < controller.states.size(); ++state_index)
    {
        m_StateRuntime[state_index].fired.assign(controller.states[state_index].events.size(), false);
    }

    m_CurrentState = std::clamp(controller.default_state, 0, static_cast<int>(controller.states.size()) - 1);
    RebuildBlendState();
    return true;
}

void AnimatorInstance::Reset()
{
    if (m_Controller != nullptr)
    {
        std::string error;
        Initialize(*m_Controller, error);
    }
}

const char* AnimatorInstance::GetCurrentStateName() const
{
    if (m_Controller == nullptr || m_CurrentState < 0 || m_CurrentState >= static_cast<int>(m_Controller->states.size()))
    {
        return "";
    }
    return m_Controller->states[static_cast<size_t>(m_CurrentState)].name.c_str();
}

float AnimatorInstance::GetNormalizedTime(int state_index) const
{
    if (m_Controller == nullptr || state_index < 0 || state_index >= static_cast<int>(m_Controller->states.size()))
    {
        return 0.0f;
    }
    const AnimatorState& state = m_Controller->states[static_cast<size_t>(state_index)];
    if (state.clip_length <= 0.0f)
    {
        return 0.0f;
    }
    const float normalized = m_StateRuntime[static_cast<size_t>(state_index)].time / state.clip_length;
    return std::clamp(normalized, 0.0f, 1.0f);
}

// ---- Parameters -------------------------------------------------------------

void AnimatorInstance::SetFloat(const char* name, float value)
{
    const int index = (m_Controller != nullptr) ? m_Controller->FindParameter(name) : -1;
    if (index < 0)
    {
        LOG_WARNING(ZAnimation, "AnimatorInstance: SetFloat on unknown parameter '{}'", name != nullptr ? name : "");
        return;
    }
    if (m_Controller->parameters[static_cast<size_t>(index)].type != static_cast<int>(AnimatorParameterType::Float))
    {
        LOG_WARNING(ZAnimation, "AnimatorInstance: parameter '{}' is not a Float", name != nullptr ? name : "");
        return;
    }
    m_Values[static_cast<size_t>(index)].float_value = value;
}

void AnimatorInstance::SetInt(const char* name, int value)
{
    const int index = (m_Controller != nullptr) ? m_Controller->FindParameter(name) : -1;
    if (index < 0)
    {
        LOG_WARNING(ZAnimation, "AnimatorInstance: SetInt on unknown parameter '{}'", name != nullptr ? name : "");
        return;
    }
    if (m_Controller->parameters[static_cast<size_t>(index)].type != static_cast<int>(AnimatorParameterType::Int))
    {
        LOG_WARNING(ZAnimation, "AnimatorInstance: parameter '{}' is not an Int", name != nullptr ? name : "");
        return;
    }
    m_Values[static_cast<size_t>(index)].int_value = value;
}

void AnimatorInstance::SetBool(const char* name, bool value)
{
    const int index = (m_Controller != nullptr) ? m_Controller->FindParameter(name) : -1;
    if (index < 0)
    {
        LOG_WARNING(ZAnimation, "AnimatorInstance: SetBool on unknown parameter '{}'", name != nullptr ? name : "");
        return;
    }
    if (m_Controller->parameters[static_cast<size_t>(index)].type != static_cast<int>(AnimatorParameterType::Bool))
    {
        LOG_WARNING(ZAnimation, "AnimatorInstance: parameter '{}' is not a Bool", name != nullptr ? name : "");
        return;
    }
    m_Values[static_cast<size_t>(index)].bool_value = value;
}

void AnimatorInstance::SetTrigger(const char* name)
{
    const int index = (m_Controller != nullptr) ? m_Controller->FindParameter(name) : -1;
    if (index < 0)
    {
        LOG_WARNING(ZAnimation, "AnimatorInstance: SetTrigger on unknown parameter '{}'", name != nullptr ? name : "");
        return;
    }
    if (m_Controller->parameters[static_cast<size_t>(index)].type != static_cast<int>(AnimatorParameterType::Trigger))
    {
        LOG_WARNING(ZAnimation, "AnimatorInstance: parameter '{}' is not a Trigger", name != nullptr ? name : "");
        return;
    }
    m_Values[static_cast<size_t>(index)].bool_value = true;
}

void AnimatorInstance::ResetTrigger(const char* name)
{
    const int index = (m_Controller != nullptr) ? m_Controller->FindParameter(name) : -1;
    if (index < 0)
    {
        return;
    }
    m_Values[static_cast<size_t>(index)].bool_value = false;
}

float AnimatorInstance::GetFloat(const char* name) const
{
    const int index = (m_Controller != nullptr) ? m_Controller->FindParameter(name) : -1;
    return index < 0 ? 0.0f : m_Values[static_cast<size_t>(index)].float_value;
}

int AnimatorInstance::GetInt(const char* name) const
{
    const int index = (m_Controller != nullptr) ? m_Controller->FindParameter(name) : -1;
    return index < 0 ? 0 : m_Values[static_cast<size_t>(index)].int_value;
}

bool AnimatorInstance::GetBool(const char* name) const
{
    const int index = (m_Controller != nullptr) ? m_Controller->FindParameter(name) : -1;
    return index >= 0 && m_Values[static_cast<size_t>(index)].bool_value;
}

// ---- Evaluation --------------------------------------------------------------

void AnimatorInstance::AdvanceState(int state_index, float delta_time)
{
    const AnimatorState& state = m_Controller->states[static_cast<size_t>(state_index)];
    StateRuntime& runtime = m_StateRuntime[static_cast<size_t>(state_index)];
    const float length = state.clip_length;
    if (length <= 0.0f)
    {
        return;  // Unfilled state: hold at time 0 rather than dividing by zero.
    }

    switch (static_cast<AnimatorWrapMode>(state.wrap_mode))
    {
        case AnimatorWrapMode::Loop:
        {
            runtime.time += delta_time * state.speed;
            if (runtime.time < 0.0f || runtime.time >= length)
            {
                runtime.time = Fract(runtime.time / length) * length;
                // New pass: events may fire again.
                std::fill(runtime.fired.begin(), runtime.fired.end(), false);
            }
            break;
        }
        case AnimatorWrapMode::Once:
        case AnimatorWrapMode::ClampForever:
        {
            runtime.time = std::clamp(runtime.time + delta_time * state.speed, 0.0f, length);
            break;
        }
        case AnimatorWrapMode::PingPong:
        {
            runtime.time += delta_time * state.speed * static_cast<float>(runtime.direction);
            if (runtime.time > length)
            {
                runtime.time = length - (runtime.time - length);
                runtime.direction = -1;
                std::fill(runtime.fired.begin(), runtime.fired.end(), false);
            }
            else if (runtime.time < 0.0f)
            {
                runtime.time = -runtime.time;
                runtime.direction = 1;
                std::fill(runtime.fired.begin(), runtime.fired.end(), false);
            }
            // A single delta_time larger than 2 * length can overshoot both
            // ends; clamping keeps the playhead legal (Unity clamps too).
            runtime.time = std::clamp(runtime.time, 0.0f, length);
            break;
        }
    }
}

void AnimatorInstance::CollectEvents(int state_index)
{
    const AnimatorState& state = m_Controller->states[static_cast<size_t>(state_index)];
    StateRuntime& runtime = m_StateRuntime[static_cast<size_t>(state_index)];
    runtime.fired.resize(state.events.size(), false);

    const float normalized = GetNormalizedTime(state_index);
    for (size_t event_index = 0; event_index < state.events.size(); ++event_index)
    {
        if (runtime.fired[event_index])
        {
            continue;
        }
        if (normalized >= state.events[event_index].normalized_time)
        {
            runtime.fired[event_index] = true;
            m_FiredEvents.push_back(state.events[event_index].function_name);
        }
    }
}

bool AnimatorInstance::ExitTimeReached(const AnimatorTransition& transition) const
{
    if (!transition.has_exit_time)
    {
        return true;
    }
    return GetNormalizedTime(m_CurrentState) >= transition.exit_time;
}

bool AnimatorInstance::ConditionsMet(const AnimatorTransition& transition)
{
    if (transition.conditions.empty())
    {
        return true;
    }

    for (const AnimatorCondition& condition : transition.conditions)
    {
        const int index = m_Controller->FindParameter(condition.parameter.c_str());
        if (index < 0)
        {
            // An unknown parameter can never be satisfied -- authoring error,
            // and silently treating it as "false" would hide it.
            LOG_WARNING(ZAnimation, "AnimatorInstance: condition on unknown parameter '{}'",
                        condition.parameter.c_str());
            return false;
        }

        const AnimatorParameter& definition = m_Controller->parameters[static_cast<size_t>(index)];
        const ParameterValue& value = m_Values[static_cast<size_t>(index)];

        switch (static_cast<AnimatorParameterType>(definition.type))
        {
            case AnimatorParameterType::Float:
            {
                const float lhs = value.float_value;
                const float rhs = condition.threshold;
                const bool pass = (condition.mode == static_cast<int>(AnimatorConditionMode::Greater))   ? (lhs > rhs)
                                  : (condition.mode == static_cast<int>(AnimatorConditionMode::Less))    ? (lhs < rhs)
                                  : (condition.mode == static_cast<int>(AnimatorConditionMode::Equals))  ? (std::fabs(lhs - rhs) < 0.0001f)
                                  : (condition.mode == static_cast<int>(AnimatorConditionMode::NotEqual))? (std::fabs(lhs - rhs) >= 0.0001f)
                                                                                                          : false;
                if (!pass)
                {
                    return false;
                }
                break;
            }
            case AnimatorParameterType::Int:
            {
                const int lhs = value.int_value;
                const int rhs = static_cast<int>(condition.threshold);
                const bool pass = (condition.mode == static_cast<int>(AnimatorConditionMode::Greater))   ? (lhs > rhs)
                                  : (condition.mode == static_cast<int>(AnimatorConditionMode::Less))    ? (lhs < rhs)
                                  : (condition.mode == static_cast<int>(AnimatorConditionMode::Equals))  ? (lhs == rhs)
                                  : (condition.mode == static_cast<int>(AnimatorConditionMode::NotEqual))? (lhs != rhs)
                                                                                                          : false;
                if (!pass)
                {
                    return false;
                }
                break;
            }
            case AnimatorParameterType::Bool:
            {
                const bool pass = (condition.mode == static_cast<int>(AnimatorConditionMode::If))        ? value.bool_value
                                  : (condition.mode == static_cast<int>(AnimatorConditionMode::IfNot))   ? !value.bool_value
                                  : (condition.mode == static_cast<int>(AnimatorConditionMode::Equals))  ? (value.bool_value == (condition.threshold != 0.0f))
                                  : (condition.mode == static_cast<int>(AnimatorConditionMode::NotEqual))? (value.bool_value != (condition.threshold != 0.0f))
                                                                                                          : false;
                if (!pass)
                {
                    return false;
                }
                break;
            }
            case AnimatorParameterType::Trigger:
            {
                if (!value.bool_value)
                {
                    return false;
                }
                break;
            }
        }
    }

    // All conditions hold: consume every trigger the transition read, so a
    // trigger fires exactly one transition (Unity's behaviour).
    for (const AnimatorCondition& condition : transition.conditions)
    {
        const int index = m_Controller->FindParameter(condition.parameter.c_str());
        if (index < 0)
        {
            continue;
        }
        if (m_Controller->parameters[static_cast<size_t>(index)].type == static_cast<int>(AnimatorParameterType::Trigger))
        {
            m_Values[static_cast<size_t>(index)].bool_value = false;
        }
    }
    return true;
}

void AnimatorInstance::StartTransition(const AnimatorTransition& transition, int to_state)
{
    if (transition.duration <= 0.0f)
    {
        // Snap: no crossfade to mix, so just take the state.
        m_CurrentState = to_state;
        m_TransitionTarget = -1;
        m_TransitionTime = 0.0f;
        m_TransitionDuration = 0.0f;
        return;
    }

    m_TransitionTarget = to_state;
    m_TransitionTime = 0.0f;
    m_TransitionDuration = transition.duration;
    // The target starts from its beginning; the source keeps playing so the
    // crossfade has motion on both sides (Unity does the same).
    EnterState(to_state, 0.0f);
}

void AnimatorInstance::EnterState(int state_index, float normalized_time)
{
    if (state_index < 0 || state_index >= static_cast<int>(m_StateRuntime.size()))
    {
        return;
    }
    StateRuntime& runtime = m_StateRuntime[static_cast<size_t>(state_index)];
    const AnimatorState& state = m_Controller->states[static_cast<size_t>(state_index)];
    runtime.time = std::clamp(normalized_time, 0.0f, 1.0f) * std::max(0.0f, state.clip_length);
    runtime.direction = 1;
    std::fill(runtime.fired.begin(), runtime.fired.end(), false);
}

void AnimatorInstance::Tick(float delta_time)
{
    m_FiredEvents.clear();
    m_BlendState = BlendState {};

    if (m_Controller == nullptr || m_CurrentState < 0)
    {
        return;
    }
    if (!(delta_time > 0.0f))
    {
        RebuildBlendState();
        return;
    }

    const size_t state_count = m_Controller->states.size();

    AdvanceState(m_CurrentState, delta_time);
    CollectEvents(m_CurrentState);

    if (m_TransitionTarget >= 0)
    {
        // Both sides advance during a fade, and fade completion is what
        // promotes the target to current.
        AdvanceState(m_TransitionTarget, delta_time);
        CollectEvents(m_TransitionTarget);

        m_TransitionTime += delta_time;
        if (m_TransitionTime >= m_TransitionDuration)
        {
            m_CurrentState = m_TransitionTarget;
            m_TransitionTarget = -1;
            m_TransitionTime = 0.0f;
            m_TransitionDuration = 0.0f;
        }
    }
    else
    {
        // Candidates first, conditions second: conditions CONSUME triggers, so
        // evaluating every transition before choosing would let a losing one
        // eat the trigger.
        std::vector<const AnimatorTransition*> candidates;
        for (const AnimatorTransition& transition : m_Controller->transitions)
        {
            if (transition.from_state != m_CurrentState)
            {
                continue;
            }
            if (transition.to_state < 0 || transition.to_state >= static_cast<int>(state_count))
            {
                continue;
            }
            if (!ExitTimeReached(transition))
            {
                continue;
            }
            candidates.push_back(&transition);
        }
        std::stable_sort(candidates.begin(), candidates.end(), [](const AnimatorTransition* in_lhs, const AnimatorTransition* in_rhs) {
            return in_lhs->priority > in_rhs->priority;
        });

        for (const AnimatorTransition* candidate : candidates)
        {
            if (ConditionsMet(*candidate))
            {
                StartTransition(*candidate, candidate->to_state);
                break;
            }
        }
    }

    RebuildBlendState();
}

void AnimatorInstance::RebuildBlendState()
{
    BlendState output;
    if (m_Controller == nullptr || m_CurrentState < 0 || m_CurrentState >= static_cast<int>(m_Controller->states.size()))
    {
        m_BlendState = output;
        return;
    }

    const AnimatorState& from = m_Controller->states[static_cast<size_t>(m_CurrentState)];

    float fade = 0.0f;
    if (m_TransitionTarget >= 0 && m_TransitionDuration > 0.0f)
    {
        fade = std::clamp(m_TransitionTime / m_TransitionDuration, 0.0f, 1.0f);
    }

    output.blend_clip_file_path.push_back(from.clip_file_path);
    output.blend_anim_skel_map_path.push_back(from.anim_skel_map_path);
    output.blend_clip_file_length.push_back(from.clip_length);
    output.blend_weight.push_back(1.0f - fade);
    output.blend_ratio.push_back(GetNormalizedTime(m_CurrentState));

    if (m_TransitionTarget >= 0)
    {
        const AnimatorState& to = m_Controller->states[static_cast<size_t>(m_TransitionTarget)];
        output.blend_clip_file_path.push_back(to.clip_file_path);
        output.blend_anim_skel_map_path.push_back(to.anim_skel_map_path);
        output.blend_clip_file_length.push_back(to.clip_length);
        output.blend_weight.push_back(fade);
        output.blend_ratio.push_back(GetNormalizedTime(m_TransitionTarget));
    }

    // No masks: a crossfade blends every bone. GetBlendStateWithClipData()
    // treats an empty mask list as "all bones enabled".
    output.clip_count = static_cast<int>(output.blend_clip_file_path.size());
    m_BlendState = output;
}

// ---- Explicit control ---------------------------------------------------------

void AnimatorInstance::Play(const char* state_name, float normalized_time)
{
    if (m_Controller == nullptr)
    {
        return;
    }
    const int index = m_Controller->FindState(state_name);
    if (index < 0)
    {
        LOG_WARNING(ZAnimation, "AnimatorInstance: Play on unknown state '{}'", state_name != nullptr ? state_name : "");
        return;
    }
    EnterState(index, normalized_time);
    m_CurrentState = index;
    m_TransitionTarget = -1;
    m_TransitionTime = 0.0f;
    m_TransitionDuration = 0.0f;
    RebuildBlendState();
}

void AnimatorInstance::CrossFade(const char* state_name, float duration, float normalized_time)
{
    if (m_Controller == nullptr)
    {
        return;
    }
    const int index = m_Controller->FindState(state_name);
    if (index < 0)
    {
        LOG_WARNING(ZAnimation, "AnimatorInstance: CrossFade on unknown state '{}'", state_name != nullptr ? state_name : "");
        return;
    }
    if (duration <= 0.0f)
    {
        Play(state_name, normalized_time);
        return;
    }
    EnterState(index, normalized_time);
    m_TransitionTarget = index;
    m_TransitionTime = 0.0f;
    m_TransitionDuration = duration;
    RebuildBlendState();
}
