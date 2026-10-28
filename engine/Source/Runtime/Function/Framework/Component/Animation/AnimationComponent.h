#pragma once

#include "Runtime/Function/Animation/AnimatorInstance.h"
#include "Runtime/Function/Animation/Skeleton.h"
#include "Runtime/Function/Framework/Component/Component.h"
#include "Runtime/Resource/ResType/Components/Animation.h"
#include "AnimationComponent.generated.h"

ZCLASS()
class AnimationComponent : public Component
{
    ZGENERATED_BODY(AnimationComponent)

public:
    AnimationComponent() = default;

    void PostLoadResource(GameObject* parent_object) override;

    // Skeleton and controller paths are only resolved in PostLoadResource, so an
    // undo that restores them must re-run it (the Inspector does the same on edit).
    void PostEditUndo() override { PostLoadResource(GetParentObject()); }

    void Tick(float delta_time) override;

    const AnimationResult& GetResult() const;

    const Skeleton& GetSkeleton() const;

    /// PR-SK3: the state machine, when `controller_file_path` is set. Game code
    /// drives it with SetFloat / SetTrigger / CrossFade.
    ZFUNCTION()
    bool HasController() const { return m_Controller != nullptr; }
    AnimatorInstance& GetAnimator() { return m_Animator; }
    const AnimatorInstance& GetAnimator() const { return m_Animator; }

    // ---- State machine, forwarded to m_Animator ----------------------------
    //
    // Why forward instead of exporting GetAnimator(): AnimatorInstance is not a
    // reflected type (no ZCLASS, so scripts can never hold one), and every one of
    // its entry points takes `const char*` -- a Converter for that would have to
    // own a buffer that outlives the call, the same trap the path setters below
    // call out. These wrappers take eastl::string, which already crosses the
    // boundary.
    //
    // Every one of them is a no-op (with a LOG_WARNING) when no controller is
    // loaded -- AnimatorInstance guards on `m_Controller == nullptr` internally --
    // so a script cannot take the process down by driving an animator that isn't
    // there. Getters return the same defaults (0 / false / "").
    ZFUNCTION()
    void SetFloat(const eastl::string& name, float value) { m_Animator.SetFloat(name.c_str(), value); }
    ZFUNCTION()
    void SetInt(const eastl::string& name, int value) { m_Animator.SetInt(name.c_str(), value); }
    ZFUNCTION()
    void SetBool(const eastl::string& name, bool value) { m_Animator.SetBool(name.c_str(), value); }
    /// Trigger 由状态机消费：条件满足后自动复位。
    ZFUNCTION()
    void SetTrigger(const eastl::string& name) { m_Animator.SetTrigger(name.c_str()); }
    ZFUNCTION()
    void ResetTrigger(const eastl::string& name) { m_Animator.ResetTrigger(name.c_str()); }
    ZFUNCTION()
    float GetFloat(const eastl::string& name) const { return m_Animator.GetFloat(name.c_str()); }
    ZFUNCTION()
    int GetInt(const eastl::string& name) const { return m_Animator.GetInt(name.c_str()); }
    ZFUNCTION()
    bool GetBool(const eastl::string& name) const { return m_Animator.GetBool(name.c_str()); }

    /// 立刻切到某状态（不混出）。为什么叫 PlayState 不叫 Play：本类同时管"片段"和
    /// "状态机"，光一个 Play 分不清是播片段还是切状态。
    ZFUNCTION()
    void PlayState(const eastl::string& state_name, float normalized_time = 0.0f)
    {
        m_Animator.Play(state_name.c_str(), normalized_time);
    }
    /// 在 `duration` 秒内混出到某状态（0 = 直接切）。
    ZFUNCTION()
    void CrossFade(const eastl::string& state_name, float duration, float normalized_time = 0.0f)
    {
        m_Animator.CrossFade(state_name.c_str(), duration, normalized_time);
    }

    /// 当前状态名；没控制器 = ""。
    ZFUNCTION()
    eastl::string GetCurrentStateName() const { return eastl::string(m_Animator.GetCurrentStateName()); }
    ZFUNCTION()
    bool IsInTransition() const { return m_Animator.IsInTransition(); }
    /// 当前状态的归一化进度 [0, 1]；没控制器 = 0。用于把特效 / 判定对齐到动作进度。
    ZFUNCTION()
    float GetNormalizedTime() const { return m_Animator.GetNormalizedTime(m_Animator.GetCurrentStateIndex()); }

    // ---- Editor-facing slot access -----------------------------------------
    // `m_AnimationRes` stays protected on purpose. A BlendState is a set of
    // PARALLEL vectors and a clip only plays when its bone map, length, ratio
    // and weight entries all exist -- Tick() returns early and the character
    // silently freezes otherwise. Callers (the Inspector) must not hand-roll
    // that layout, so the component owns the invariant instead.
    ZFUNCTION()
    const eastl::string& GetControllerPath() const { return m_AnimationRes.controller_file_path; }
    ZFUNCTION()
    const eastl::string& GetSkeletonPath() const { return m_AnimationRes.skeleton_file_path; }
    /// Slot 0 of the legacy blend state; empty when nothing is assigned.
    ZFUNCTION()
    eastl::string GetClipPath() const;
    ZFUNCTION()
    eastl::string GetAnimSkelMapPath() const;
    ZFUNCTION()
    float GetClipLength() const;

    /// The `const char*` setters below stay unbound on purpose: a
    /// ZScript::Converter for `const char*` would have to own a buffer that
    /// outlives the call, which is a lifetime trap. Add an eastl::string
    /// overload if scripts need to set these.
    // 不导出：只改路径不重载，脚本调了看不到任何变化（重载在 PostLoadResource 里，
    // 编辑器改完会手动重跑）。导出等于送一个静默失效的 API。
    void SetControllerPath(const char* path);
    void SetSkeletonPath(const char* path);
    /// Assigns slot 0 and grows the parallel arrays into a valid single-clip
    /// state (length / ratio / weight default to 1s / 0 / 1).
    ///
    /// The eastl::string overload is the script-facing one: Tick() re-reads the
    /// blend state every frame, so this one DOES take effect immediately. It
    /// delegates to the const char* version, which overload resolution still
    /// prefers for a string literal (standard beats user-defined conversion).
    ZFUNCTION()
    void SetClipPath(const eastl::string& path);
    void SetClipPath(const char* path);
    void SetAnimSkelMapPath(const char* path);
    ZFUNCTION()
    void SetClipLength(float seconds);

    // ---- Timeline override --------------------------------------------------
    // A Timeline animation clip drives the pose directly: while an override is set,
    // Tick() samples `clip_path` at `phase` (in [0, 1)) through the component's own
    // slot-0 bone map and does not advance anything itself -- the controller and the
    // authored blend state are left untouched, and resume where they were once the
    // override is cleared. Nothing here is serialized, so a Timeline playing in the
    // editor never dirties the scene.
    /// Returns false (and sets nothing) when the component has no slot-0 bone map to
    /// sample through, or the path is empty.
    ///
    /// 不导出：这是 Timeline 动画轨道占用姿势的通道，不是玩法接口。给脚本会和
    /// Timeline 抢同一个 m_HasTimelineOverride 标志，谁最后写谁赢，属于纯坑。
    bool SetTimelineOverride(const eastl::string& clip_path, float phase);
    void ClearTimelineOverride();
    bool HasTimelineOverride() const { return m_HasTimelineOverride; }

protected:
    ZPROPERTY()
    AnimationComponentRes m_AnimationRes;

    Skeleton m_Skeleton;

    /// Non-null only when a controller loaded successfully. Owned by the asset
    /// system; the component must not free it.
    const AnimatorController* m_Controller {nullptr};
    AnimatorInstance m_Animator;

    bool m_HasTimelineOverride {false};
    /// Transient single-clip blend state the override samples (see SetTimelineOverride).
    BlendState m_TimelineBlendState {};

    /// Reused resolve buffer for Tick (P0.2). Deliberately NOT a ZPROPERTY: it
    /// is frame-local scratch, not state -- it holds borrowed clip pointers
    /// (see BlendStateWithClipData) and must never be serialized or shown in
    /// the Inspector. Lives here, rather than inside AnimationSystem, so each
    /// component keeps its own capacity and nothing is shared across objects.
    BlendStateWithClipData m_ClipDataScratch {};
};