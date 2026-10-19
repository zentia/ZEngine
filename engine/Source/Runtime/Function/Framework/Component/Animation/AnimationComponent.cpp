#include "Runtime/Function/Framework/Component/Animation/AnimationComponent.h"

#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/Function/Animation/AnimationSystem.h"

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

    auto skeleton_res = AnimationSystem::TryLoadSkeleton(m_AnimationRes.skeleton_file_path);

    m_Skeleton.BuildSkeleton(*skeleton_res);
}

void AnimationComponent::Tick(float delta_time)
{
    m_AnimationRes.blend_state.blend_ratio[0] += (delta_time / m_AnimationRes.blend_state.blend_clip_file_length[0]);
    m_AnimationRes.blend_state.blend_ratio[0] -= floor(m_AnimationRes.blend_state.blend_ratio[0]);

    const BlendStateWithClipData& clip_data = AnimationSystem::GetBlendStateWithClipData(m_AnimationRes.blend_state);
    m_Skeleton.ApplyAnimation(clip_data);
    m_AnimationRes.animation_result = m_Skeleton.OutputAnimationResult();
}

const AnimationResult& AnimationComponent::GetResult() const
{
    return m_AnimationRes.animation_result;
}

const Skeleton& AnimationComponent::GetSkeleton() const
{
    return m_Skeleton;
}