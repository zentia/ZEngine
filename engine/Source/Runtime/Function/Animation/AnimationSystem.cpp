#include "Runtime/Function/Animation/AnimationSystem.h"

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Function/Animation/AnimationLoader.h"
#include "Runtime/Function/Animation/Skeleton.h"
#include "resource/ResType/Data/BoneBlendMask.h"

#include <algorithm>
#include <cmath>
#include <vector>

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
    auto found = m_AnimationSkeletonMapCache.find(file_path);
    if (found != m_AnimationSkeletonMapCache.end())
    {
        return found->second;
    }

    // Used to emplace the freshly loaded map and then `return nullptr`, so a
    // first-time lookup always failed and every clip silently lost its bone
    // mapping. Load once, cache, then return what we just cached.
    AnimationLoader loader;
    PPtr<AnimSkelMap> res = loader.LoadAnimSkelMap(file_path);
    m_AnimationSkeletonMapCache.emplace(file_path, res);
    return res;
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

namespace
{
    /// Weight of clip `in_clip_index` as authored, or 1 when the list is short.
    float AuthoredWeight(const BlendState& in_blend_state, size_t in_clip_index)
    {
        if (in_clip_index < in_blend_state.blend_weight.size())
        {
            return in_blend_state.blend_weight[in_clip_index];
        }
        return 1.0f;
    }
}  // namespace

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
    blend_state_with_clip_data.blend_ratio = blend_state.blend_ratio;

    // Assets are dropped instead of dereferenced: an empty path or a deleted
    // asset used to crash here every single frame.
    for (const auto& iter : blend_state.blend_clip_file_path)
    {
        auto found = m_AnimationDataCache.find(iter);
        if (found == m_AnimationDataCache.end() || found->second == nullptr)
        {
            LOG_ERROR(ZAnimation, "AnimationSystem: clip '{}' could not be loaded", iter.c_str());
            continue;
        }
        blend_state_with_clip_data.blend_clip.push_back(*found->second);
    }
    for (const auto& iter : blend_state.blend_anim_skel_map_path)
    {
        auto found = m_AnimationSkeletonMapCache.find(iter);
        if (found == m_AnimationSkeletonMapCache.end())
        {
            LOG_ERROR(ZAnimation, "AnimationSystem: anim-skeleton map '{}' could not be loaded", iter.c_str());
            continue;
        }
        blend_state_with_clip_data.blend_anim_skel_map.push_back(found->second);
    }

    // A blend state is only usable when a clip AND its bone map are present.
    const size_t clip_count = std::min({static_cast<size_t>(std::max(blend_state.clip_count, 0)),
                                        blend_state_with_clip_data.blend_clip.size(),
                                        blend_state_with_clip_data.blend_anim_skel_map.size()});
    blend_state_with_clip_data.clip_count = static_cast<int>(clip_count);
    blend_state_with_clip_data.blend_clip.resize(clip_count);
    blend_state_with_clip_data.blend_anim_skel_map.resize(clip_count);
    blend_state_with_clip_data.blend_ratio.resize(clip_count);

    if (clip_count == 0)
    {
        LOG_ERROR(ZAnimation, "AnimationSystem: blend state has no usable clip");
        return blend_state_with_clip_data;
    }

    // Masks are optional: without them every bone takes every clip, which is
    // what "no mask authored" has always meant. The old code dereferenced
    // `blend_masks[0]` unconditionally, so any setup without masks crashed.
    std::vector<const BoneBlendMask*> blend_masks(clip_count, nullptr);
    size_t skeleton_bone_count = 0;
    for (size_t clip_index = 0; clip_index < blend_state.blend_mask_file_path.size() && clip_index < clip_count; ++clip_index)
    {
        auto found = m_SkeletonMaskCache.find(blend_state.blend_mask_file_path[clip_index]);
        if (found == m_SkeletonMaskCache.end() || found->second == nullptr)
        {
            continue;
        }
        blend_masks[clip_index] = found->second;
        TryLoadSkeleton(found->second->skeleton_file_path);
        auto skeleton = m_SkeletonDefinitionCache.find(found->second->skeleton_file_path);
        if (skeleton != m_SkeletonDefinitionCache.end() && skeleton->second != nullptr)
        {
            skeleton_bone_count = std::max(skeleton_bone_count, skeleton->second->bones_map.size());
        }
    }

    // No masks (or no skeleton behind them): derive the bone count from the
    // bone maps themselves so weights still line up with the skeleton.
    if (skeleton_bone_count == 0)
    {
        for (const auto& anim_skel_map : blend_state_with_clip_data.blend_anim_skel_map)
        {
            if (anim_skel_map == nullptr)
            {
                continue;
            }
            for (const int bone_index : anim_skel_map->convert)
            {
                if (bone_index >= 0)
                {
                    skeleton_bone_count = std::max(skeleton_bone_count, static_cast<size_t>(bone_index) + 1u);
                }
            }
        }
    }

    blend_state_with_clip_data.blend_weight.assign(clip_count, BoneBlendWeight {});
    for (size_t clip_index = 0; clip_index < clip_count; ++clip_index)
    {
        blend_state_with_clip_data.blend_weight[clip_index].blend_weight.assign(skeleton_bone_count, 0.0f);
    }

    for (size_t bone_index = 0; bone_index < skeleton_bone_count; ++bone_index)
    {
        float sum_weight = 0.0f;
        for (size_t clip_index = 0; clip_index < clip_count; ++clip_index)
        {
            const bool enabled = (blend_masks[clip_index] == nullptr) ||
                                 (bone_index < blend_masks[clip_index]->enabled.size() &&
                                  blend_masks[clip_index]->enabled[bone_index]);
            if (enabled)
            {
                sum_weight += AuthoredWeight(blend_state, clip_index);
            }
        }

        if (fabs(sum_weight) < 0.0001f)
        {
            // Every clip is masked out (or weighted 0) for this bone: fall back
            // to an even split instead of dividing by zero.
            sum_weight = static_cast<float>(clip_count);
            for (size_t clip_index = 0; clip_index < clip_count; ++clip_index)
            {
                blend_state_with_clip_data.blend_weight[clip_index].blend_weight[bone_index] = 1.0f / sum_weight;
            }
            continue;
        }

        for (size_t clip_index = 0; clip_index < clip_count; ++clip_index)
        {
            const bool enabled = (blend_masks[clip_index] == nullptr) ||
                                 (bone_index < blend_masks[clip_index]->enabled.size() &&
                                  blend_masks[clip_index]->enabled[bone_index]);
            blend_state_with_clip_data.blend_weight[clip_index].blend_weight[bone_index] =
                enabled ? (AuthoredWeight(blend_state, clip_index) / sum_weight) : 0.0f;
        }
    }
    return blend_state_with_clip_data;
}