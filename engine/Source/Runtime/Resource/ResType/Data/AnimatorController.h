#pragma once

// =============================================================================
// AnimatorController -- the animation state machine (PR-SK3).
//
// Before this, `AnimationComponent` could only play a fixed set of clips mixed
// by a static weight, always looping, with no speed control and no events: a
// character could not react to gameplay at all. This asset is Unity's
// AnimatorController (states + parameters + transitions), adapted to what the
// engine already has:
//
//   * a state owns ONE clip, referenced by asset PATH (the same addressing the
//     existing BlendState uses) -- no new reference mechanism is introduced;
//   * a transition is evaluated against PARAMETERS the game code sets
//     (`SetFloat("speed", 3)`), mirroring Unity, because hard-coding
//     "if speed > x then run" in every character would be unreviewable;
//   * the evaluator (AnimatorInstance) feeds the EXISTING mixer: it emits a
//     BlendState with at most two clips and a crossfade weight, so
//     `AnimationSystem::BuildBlendStateWithClipData()` + `Skeleton::ApplyAnimation`
//     stay the single sampling path. Nothing new was written that can disagree
//     with the existing pose math.
//
// Enums are persisted as `int` because the serializer has no enum trait; the
// enum types exist for call sites and are cast at the boundary.
// =============================================================================

#include "Runtime/BaseClasses/Object.h"
#include "Runtime/Core/Serialize/SerializeUtility.h"

#include <EASTL/string.h>

#include <vector>
#include "AnimatorController.generated.h"

enum class AnimatorParameterType : int
{
    Float = 0,
    Int = 1,
    Bool = 2,
    /// Consumed by the first transition that reads it, then cleared. A trigger
    /// is the only way to express "this happened" without the game code having
    /// to pulse a bool itself.
    Trigger = 3,
};

enum class AnimatorConditionMode : int
{
    If = 0,       // bool: true
    IfNot = 1,    // bool: false
    Greater = 2,  // float / int: value > threshold
    Less = 3,
    Equals = 4,
    NotEqual = 5,
};

enum class AnimatorWrapMode : int
{
    Loop = 0,
    /// Plays once and holds the last frame.
    Once = 1,
    /// Plays once and holds, but never signals "finished", so exit-time
    /// transitions keep waiting. Distinguished from Once for authoring clarity.
    ClampForever = 2,
    PingPong = 3,
};

/// One gameplay-facing knob. Values live on the AnimatorInstance; the asset
/// only carries the default.
struct AnimatorParameter
{
    DECLARE_SERIALIZE(AnimatorParameter)

    eastl::string name;
    int type {static_cast<int>(AnimatorParameterType::Float)};
    float default_float {0.0f};
    int default_int {0};
    bool default_bool {false};
};

/// One predicate of a transition. ALL conditions of a transition must hold.
struct AnimatorCondition
{
    DECLARE_SERIALIZE(AnimatorCondition)

    eastl::string parameter;
    int mode {static_cast<int>(AnimatorConditionMode::Greater)};
    float threshold {0.0f};
};

/// A named callback at a point in the clip. Fired ONCE per loop of the state.
struct AnimatorEvent
{
    DECLARE_SERIALIZE(AnimatorEvent)

    /// Normalized time in [0, 1].
    float normalized_time {0.0f};
    eastl::string function_name;
};

struct AnimatorState
{
    DECLARE_SERIALIZE(AnimatorState)

    eastl::string name;
    /// `.zasset` path of the AnimationAsset (same addressing as BlendState).
    eastl::string clip_file_path;
    /// `.zasset` path of the AnimSkelMap mapping clip channels -> bones.
    eastl::string anim_skel_map_path;
    int wrap_mode {static_cast<int>(AnimatorWrapMode::Loop)};
    /// Time scale. 2 = twice as fast; negative plays backwards (Loop only).
    float speed {1.0f};
    /// Clip duration in SECONDS. Required: `AnimationClip` only stores a frame
    /// count and the bake FPS is not persisted, so seconds cannot be derived.
    /// <= 0 means "hold at time 0" (an authored-but-unfilled state).
    float clip_length {0.0f};
    std::vector<AnimatorEvent> events;

    /// Editor-only: node position on the Animator graph canvas, in canvas
    /// pixels. Authored by dragging in ZSlateAnimatorWindow and persisted so a
    /// reopened controller keeps its layout. Both default to 0, which the
    /// panel treats as "not laid out yet" and auto-arranges (see
    /// ZSlateAnimatorWindow::AutoLayout). SafeBinaryRead leaves them at 0 for
    /// every controller written before this existed.
    float pos_x {0.0f};
    float pos_y {0.0f};
};

struct AnimatorTransition
{
    DECLARE_SERIALIZE(AnimatorTransition)

    int from_state {0};
    int to_state {0};
    std::vector<AnimatorCondition> conditions;
    /// Crossfade length in seconds. 0 = snap.
    float duration {0.0f};
    /// When true the transition additionally waits until the source state has
    /// played past `exit_time` (normalized).
    bool has_exit_time {false};
    float exit_time {0.0f};
    /// Higher wins when several transitions are eligible at once.
    int priority {0};
};

ZCLASS()
class AnimatorController : public Object
{
    ZGENERATED_BODY(AnimatorController)

public:
    ZPROPERTY()
    std::vector<AnimatorParameter> parameters;
    ZPROPERTY()
    std::vector<AnimatorState> states;
    ZPROPERTY()
    std::vector<AnimatorTransition> transitions;
    /// Index into `states`; clamped on first evaluate. -1 = nothing plays.
    ZPROPERTY()
    int default_state {0};

    int FindState(const char* state_name) const;
    int FindParameter(const char* parameter_name) const;
};
