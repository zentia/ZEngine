#include "Runtime/Function/Animation/AnimationSystem.h"

#include "Runtime/Function/Animation/AnimationLoader.h"
#include "Runtime/Function/Animation/Skeleton.h"
#include "resource/ResType/Data/BoneBlendMask.h"

std::map<eastl::string, SkeletonData*> AnimationSystem::m_SkeletonDefinitionCache;
std::map<eastl::string, std::shared_ptr<AnimationClip>> AnimationSystem::m_AnimationDataCache;
std::map<eastl::string, PPtr<AnimSkelMap>> AnimationSystem::m_AnimationSkeletonMapCache;
std::map<eastl::string, BoneBlendMask*> AnimationSystem::m_SkeletonMaskCache;

SkeletonData* AnimationSystem::TryLoadSkeleton(eastl::string file_path)
{
    SkeletonData* res;
    AnimationLoader loader;
    auto found = m_SkeletonDefinitionCache.find(file_path);
    if (found == m_SkeletonDefinitionCache.end())
    {
        res = loader.LoadSkeletonData(file_path);
        m_SkeletonDefinitionCache.emplace(file_path, res);
    }
    else
    {
        res = found->second;
    }
    return res;
}

std::shared_ptr<AnimationClip> AnimationSystem::TryLoadAnimation(eastl::string file_path)
{
    std::shared_ptr<AnimationClip> res;
    AnimationLoader loader;
    auto found = m_AnimationDataCache.find(file_path);
    if (found == m_AnimationDataCache.end())
    {
        res = loader.LoadAnimationClipData(file_path);
        m_AnimationDataCache.emplace(file_path, res);
    }
    else
    {
        res = found->second;
    }
    return res;
}

AnimSkelMap* AnimationSystem::TryLoadAnimationSkeletonMap(eastl::string file_path)
{
    AnimationLoader loader;
    auto found = m_AnimationSkeletonMapCache.find(file_path);
    if (found == m_AnimationSkeletonMapCache.end())
    {
        PPtr<AnimSkelMap> res = loader.LoadAnimSkelMap(file_path);
        m_AnimationSkeletonMapCache.emplace(file_path, res);
    }
    else
    {
        return found->second;
    }
    return nullptr;
}

BoneBlendMask* AnimationSystem::TryLoadSkeletonMask(eastl::string file_path)
{
    BoneBlendMask* res;
    AnimationLoader loader;
    auto found = m_SkeletonMaskCache.find(file_path);
    if (found == m_SkeletonMaskCache.end())
    {
        res = loader.LoadSkeletonMask(file_path);
        m_SkeletonMaskCache.emplace(file_path, res);
    }
    else
    {
        res = found->second;
    }
    return res;
}

BlendStateWithClipData AnimationSystem::GetBlendStateWithClipData(const BlendState& blend_state)
{
    for (auto animation_file_path : blend_state.blend_clip_file_path)
    {
        TryLoadAnimation(animation_file_path);
    }
    for (auto anim_skel_map_path : blend_state.blend_anim_skel_map_path)
    {
        TryLoadAnimationSkeletonMap(anim_skel_map_path);
    }
    for (auto skeleton_mask_path : blend_state.blend_mask_file_path)
    {
        TryLoadSkeletonMask(skeleton_mask_path);
    }

    BlendStateWithClipData blend_state_with_clip_data;
    blend_state_with_clip_data.clip_count = blend_state.clip_count;
    blend_state_with_clip_data.blend_ratio = blend_state.blend_ratio;
    for (const auto& iter : blend_state.blend_clip_file_path)
    {
        blend_state_with_clip_data.blend_clip.push_back(*m_AnimationDataCache[iter]);
    }
    for (const auto& iter : blend_state.blend_anim_skel_map_path)
    {
        blend_state_with_clip_data.blend_anim_skel_map.push_back(m_AnimationSkeletonMapCache[iter]);
    }
    std::vector<BoneBlendMask*> blend_masks;
    for (auto& iter : blend_state.blend_mask_file_path)
    {
        blend_masks.push_back(m_SkeletonMaskCache[iter]);
        TryLoadAnimationSkeletonMap(m_SkeletonMaskCache[iter]->skeleton_file_path);
    }
    size_t skeleton_bone_count = m_SkeletonDefinitionCache[blend_masks[0]->skeleton_file_path]->bones_map.size();
    blend_state_with_clip_data.blend_weight.resize(blend_state.clip_count);
    for (size_t clip_index = 0; clip_index < blend_state.clip_count; clip_index++)
    {
        blend_state_with_clip_data.blend_weight[clip_index].blend_weight.resize(skeleton_bone_count);
    }
    for (size_t bone_index = 0; bone_index < skeleton_bone_count; bone_index++)
    {
        float sum_weight = 0;
        for (size_t clip_index = 0; clip_index < blend_state.clip_count; clip_index++)
        {
            if (blend_masks[clip_index]->enabled[bone_index])
            {
                sum_weight += blend_state.blend_weight[clip_index];
            }
        }
        if (fabs(sum_weight) < 0.0001f)
        {
            // LOG_ERROR
        }
        for (size_t clip_index = 0; clip_index < blend_state.clip_count; clip_index++)
        {
            if (blend_masks[clip_index]->enabled[bone_index])
            {
                blend_state_with_clip_data.blend_weight[clip_index].blend_weight[bone_index] =
                    blend_state.blend_weight[clip_index] / sum_weight;
            }
            else
            {
                blend_state_with_clip_data.blend_weight[clip_index].blend_weight[bone_index] = 0;
            }
        }
    }
    return blend_state_with_clip_data;
}