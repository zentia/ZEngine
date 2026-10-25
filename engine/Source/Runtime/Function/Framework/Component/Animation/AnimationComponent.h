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

    // ---- Editor-facing slot access -----------------------------------------
    // `m_AnimationRes` stays protected on purpose. A BlendState is a set of
    // PARALLEL vectors and a clip only plays when its bone map, length, ratio
    // and weight entries all exist -- Tick() returns early and the character
    // silently freezes otherwise. Callers (the Inspector) must not hand-roll
    // that layout, so the component owns the invariant instead.
    const eastl::string& GetControllerPath() const { return m_AnimationRes.controller_file_path; }
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
    void SetControllerPath(const char* path);
    void SetSkeletonPath(const char* path);
    /// Assigns slot 0 and grows the parallel arrays into a valid single-clip
    /// state (length / ratio / weight default to 1s / 0 / 1).
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
};