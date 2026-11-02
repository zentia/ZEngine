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
// `AnimationSystem::BuildBlendStateWithClipData()` -> `Skeleton::ApplyAnimation`
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

    // ---- Clip lengths (P0.1) ------------------------------------------------
    //
    // Initialize() seeds these from each state's authored `clip_length`. A state
    // whose length is unfilled (<= 0) can be completed here from the CLIP's own
    // baked `AnimationClip::duration` -- see AnimationComponent, which is the
    // layer that may touch assets. This class deliberately stays asset-free:
    // one controller is shared by every instance, and an evaluator that loaded
    // assets could not be driven headless.
    //
    /// Overrides state `state_index`'s length, in seconds. Ignored for an
    /// out-of-range index or a non-positive length.
    void SetStateClipLength(int state_index, float seconds);
    /// Effective length of `state_index` in seconds (authored, or whatever
    /// SetStateClipLength resolved). 0 means "still unknown".
    float GetStateClipLength(int state_index) const;

    // ---- Driving ------------------------------------------------------------

    /// Unity 的 `animator.speed`：全局播放速率（1 = 原速，0 = 冻结）。
    ///
    /// 放在**实例**上而不是控制器上，与参数值同侧：控制器是共享资产，十个角色
    /// 共用一份，其中一个角色慢放不能改到别人。Unity 也是这个分法。
    ///
    /// ⚠️ 负数夹到 0 而不是原样接受：倒放需要 `AdvanceState` 支持反向推进
    /// （PingPong 的 `direction` 只覆盖"播到头再往回"，不是负 dt），现在没有那条
    /// 路 —— 接受 -1 会得到一个"看起来在跑、其实卡在时间 0"的实例。
    float GetSpeed() const { return m_Speed; }
    void SetSpeed(float speed) { m_Speed = speed > 0.0f ? speed : 0.0f; }

    void Tick(float delta_time);

    /// Input for `AnimationSystem::BuildBlendStateWithClipData()`. One clip when
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

    // ---- 控制器形状（脚本要靠它枚举，不然只能靠作者口述）--------------------
    //
    // 这些**只**读共享资产，不改运行时状态，所以是 const 且恒一致。
    // 没有控制器时一律返回 0 / ""，与上面那组参数 getter 的约定一致。

    /// 参数个数（Unity 的 `animator.parameterCount`）。
    int GetParameterCount() const;
    /// 参数名；越界返回空串。
    const char* GetParameterName(int index) const;
    /// 状态个数。
    int GetStateCount() const;
    /// 状态名；越界返回空串。
    const char* GetStateName(int index) const;

    /// 当前状态是不是叫这个名字（Unity 的
    /// `animator.GetCurrentAnimatorStateInfo(0).IsName(name)`）。
    ///
    /// 单列一个方法而不是让脚本自己比 `GetCurrentStateName()`：那个字符串每次
    /// 都要跨边界搬一次，而"我在不在 Idle"是每帧都可能问的事。
    bool IsCurrentState(const char* state_name) const;

    // ---- Explicit control ---------------------------------------------------
    /// Jumps to a state immediately (no fade).
    void Play(const char* state_name, float normalized_time = 0.0f);
    /// Fades to a state over `duration` seconds (0 = snap).
    void CrossFade(const char* state_name, float duration, float normalized_time = 0.0f);

    /// Unity 的 `animator.Rebind()`：播放头归零、参数回到控制器默认值、跳回
    /// 默认状态。
    ///
    /// 与 `Reset()` 的关系：就是它。留两个名字没有意义，所以 `Rebind()` 不在本
    /// 类里 —— 组件层那个 `Rebind()` 直接调 `Reset()`。
    ///
    /// ⚠️ 会把 `SetStateClipLength()` 解析出来的长度一起丢掉（重新按资产值播种）。
    /// 那是"从资产重新绑定"该有的行为，调用方要的是"回到初始状态"。
    void Rebind() { Reset(); }

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

    /// Effective clip length per state, in seconds (P0.1). Seeded from the
    /// authored `AnimatorState::clip_length`; an unfilled entry may be resolved
    /// from the clip asset via SetStateClipLength. Every timing read goes
    /// through this rather than the controller, so the asset-derived length and
    /// the authored one cannot disagree at runtime.
    std::vector<float> m_StateLength;

    int m_CurrentState {-1};
    int m_TransitionTarget {-1};
    float m_TransitionTime {0.0f};
    float m_TransitionDuration {0.0f};

    std::vector<StateRuntime> m_StateRuntime;

    BlendState m_BlendState;
    std::vector<eastl::string> m_FiredEvents;

    /// Unity 的 `animator.speed`。`Initialize` 不动它 —— 重新绑定控制器不该把
    /// 脚本设过的速率一起清掉。
    float m_Speed {1.0f};
};
