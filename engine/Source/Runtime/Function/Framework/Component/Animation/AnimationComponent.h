#pragma once

#include "Runtime/Function/Animation/AnimatorInstance.h"
#include "Runtime/Function/Animation/Skeleton.h"
#include "Runtime/Function/Framework/Component/Component.h"
#include "Runtime/Resource/ResType/Components/Animation.h"

class AnimationComponent : public Component
{
    REGISTER_CLASS(AnimationComponent);
    DECLARE_OBJECT_SERIALIZE();

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
    eastl::string GetClipPath() const;
    eastl::string GetAnimSkelMapPath() const;
    float         GetClipLength() const;

    void SetControllerPath(const char* path);
    void SetSkeletonPath(const char* path);
    /// Assigns slot 0 and grows the parallel arrays into a valid single-clip
    /// state (length / ratio / weight default to 1s / 0 / 1).
    void SetClipPath(const char* path);
    void SetAnimSkelMapPath(const char* path);
    void SetClipLength(float seconds);

protected:
    AnimationComponentRes m_AnimationRes;

    Skeleton m_Skeleton;

    /// Non-null only when a controller loaded successfully. Owned by the asset
    /// system; the component must not free it.
    const AnimatorController* m_Controller {nullptr};
    AnimatorInstance m_Animator;
};