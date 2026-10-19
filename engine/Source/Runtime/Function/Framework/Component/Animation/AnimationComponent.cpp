#include "Runtime/Function/Framework/Component/Animation/AnimationComponent.h"

#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Function/Animation/AnimationSystem.h"
#include "Runtime/Resource/Asset/AssetManager.h"

#include <algorithm>
#include <cmath>

namespace
{
    // BlendState stores parallel vectors; slot `slot` is only usable when the
    // matching entry exists, so grow-with-default rather than push_back()
    // (which would leave the other arrays a slot short).
    void EnsureSlot(std::vector<float>& values, size_t slot, float fallback)
    {
        if (values.size() <= slot)
        {
            values.resize(slot + 1, fallback);
        }
    }
}  // namespace

IMPLEMENT_REGISTER_CLASS(AnimationComponent)
IMPLEMENT_OBJECT_SERIALIZE(AnimationComponent)

template<typename TransferFunction>
void AnimationComponent::Transfer(TransferFunction& transfer)
{
    Super::Transfer(transfer);
    transfer.Transfer(m_AnimationRes, "animation_res");
}

INSTANTIATE_TEMPLATE_TRANSFER_EXPORTED(AnimationComponent)

void AnimationComponent::PostLoadResource(GameObject* parent_object)
{
    m_ParentObject = parent_object;

    SkeletonData* skeleton_res = AnimationSystem::TryLoadSkeleton(m_AnimationRes.skeleton_file_path);
    if (skeleton_res == nullptr)
    {
        // Used to dereference null here and take the whole process down on a
        // missing .skeleton asset. A component that cannot animate is far
        // cheaper than a crash on level load.
        LOG_ERROR(ZAnimation, "AnimationComponent: skeleton '{}' could not be loaded",
                  m_AnimationRes.skeleton_file_path.c_str());
        return;
    }

    m_Skeleton.BuildSkeleton(*skeleton_res);

    // PR-SK3: an optional state machine. A missing / unreadable controller is
    // reported and the component falls back to the old blend state, so a broken
    // reference degrades to "plays the old way" instead of a dead character.
    if (m_AnimationRes.controller_file_path.empty())
    {
        return;
    }

    auto* asset_manager = GET_SYSTEM(AssetManager);
    if (asset_manager == nullptr)
    {
        return;
    }

    AnimatorController* controller =
        asset_manager->loadAsset<AnimatorController>(m_AnimationRes.controller_file_path);
    if (controller == nullptr)
    {
        LOG_ERROR(ZAnimation, "AnimationComponent: controller '{}' could not be loaded",
                  m_AnimationRes.controller_file_path.c_str());
        return;
    }

    std::string error;
    if (!m_Animator.Initialize(*controller, error))
    {
        LOG_ERROR(ZAnimation, "AnimationComponent: {}", error);
        return;
    }
    m_Controller = controller;
}

void AnimationComponent::Tick(float delta_time)
{
    // PR-SK3: a controller supersedes the hand-authored blend state entirely.
    // The state machine emits a BlendState of its own (one clip, or two during
    // a crossfade) so both paths share the one sampler.
    if (m_Controller != nullptr)
    {
        m_Animator.Tick(delta_time);
        const BlendStateWithClipData& clip_data = AnimationSystem::GetBlendStateWithClipData(m_Animator.GetBlendState());
        m_Skeleton.ApplyAnimation(clip_data);
        m_AnimationRes.animation_result = m_Skeleton.OutputAnimationResult();
        return;
    }

    BlendState& blend_state = m_AnimationRes.blend_state;

    // A component authored without a clip (or whose clip list is still empty
    // while streaming) used to index [0] unconditionally and crash.
    if (blend_state.blend_clip_file_length.empty() || blend_state.blend_ratio.empty())
    {
        return;
    }

    // Advance every clip's phase and wrap it into [0, 1). The mixer samples with
    // WrapMode::Loop, so a phase outside that range would be meaningless.
    const size_t clip_count = std::min(blend_state.blend_clip_file_length.size(), blend_state.blend_ratio.size());
    for (size_t clip_index = 0; clip_index < clip_count; ++clip_index)
    {
        const float length = blend_state.blend_clip_file_length[clip_index];
        if (length <= 0.0f)
        {
            continue;  // Zero/negative length: hold the current phase.
        }

        float ratio = blend_state.blend_ratio[clip_index] + (delta_time / length);
        ratio -= std::floor(ratio);
        blend_state.blend_ratio[clip_index] = ratio;
    }

    const BlendStateWithClipData& clip_data = AnimationSystem::GetBlendStateWithClipData(blend_state);
    m_Skeleton.ApplyAnimation(clip_data);
    m_AnimationRes.animation_result = m_Skeleton.OutputAnimationResult();
}

// ---- Editor-facing slot access ---------------------------------------------

eastl::string AnimationComponent::GetClipPath() const
{
    const std::vector<eastl::string>& clips = m_AnimationRes.blend_state.blend_clip_file_path;
    return clips.empty() ? eastl::string() : clips.front();
}

eastl::string AnimationComponent::GetAnimSkelMapPath() const
{
    const std::vector<eastl::string>& maps = m_AnimationRes.blend_state.blend_anim_skel_map_path;
    return maps.empty() ? eastl::string() : maps.front();
}

float AnimationComponent::GetClipLength() const
{
    const std::vector<float>& lengths = m_AnimationRes.blend_state.blend_clip_file_length;
    return lengths.empty() ? 0.0f : lengths.front();
}

void AnimationComponent::SetControllerPath(const char* path)
{
    m_AnimationRes.controller_file_path = (path != nullptr) ? path : "";
}

void AnimationComponent::SetSkeletonPath(const char* path)
{
    m_AnimationRes.skeleton_file_path = (path != nullptr) ? path : "";
}

void AnimationComponent::SetClipPath(const char* path)
{
    BlendState& blend_state = m_AnimationRes.blend_state;
    if (blend_state.blend_clip_file_path.empty())
    {
        blend_state.blend_clip_file_path.emplace_back();
    }
    blend_state.blend_clip_file_path[0] = (path != nullptr) ? path : "";

    // Without length / ratio / weight, Tick() bails out before it ever samples a
    // clip, so a "clip assigned" component would stand perfectly still.
    EnsureSlot(blend_state.blend_clip_file_length, 0, 1.0f);
    EnsureSlot(blend_state.blend_ratio, 0, 0.0f);
    EnsureSlot(blend_state.blend_weight, 0, 1.0f);
    blend_state.clip_count = static_cast<int>(blend_state.blend_clip_file_path.size());
}

void AnimationComponent::SetAnimSkelMapPath(const char* path)
{
    BlendState& blend_state = m_AnimationRes.blend_state;
    if (blend_state.blend_anim_skel_map_path.empty())
    {
        blend_state.blend_anim_skel_map_path.emplace_back();
    }
    blend_state.blend_anim_skel_map_path[0] = (path != nullptr) ? path : "";
    // AnimationSystem only counts a clip as usable when its bone map is present,
    // so keep the two lists the same length.
    if (blend_state.blend_anim_skel_map_path.size() > blend_state.blend_clip_file_path.size())
    {
        blend_state.blend_clip_file_path.resize(blend_state.blend_anim_skel_map_path.size());
    }
    blend_state.clip_count = static_cast<int>(blend_state.blend_clip_file_path.size());
}

void AnimationComponent::SetClipLength(float seconds)
{
    EnsureSlot(m_AnimationRes.blend_state.blend_clip_file_length, 0, 1.0f);
    m_AnimationRes.blend_state.blend_clip_file_length[0] = seconds;
}

const AnimationResult& AnimationComponent::GetResult() const
{
    return m_AnimationRes.animation_result;
}

const Skeleton& AnimationComponent::GetSkeleton() const
{
    return m_Skeleton;
}