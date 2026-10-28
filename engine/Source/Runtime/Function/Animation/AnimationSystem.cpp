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

// ---- single-lookup cache accessors (P0.2) ----------------------------------
//
// Each one does exactly ONE map lookup on the hot path. A failed load is cached
// (as nullptr / a null PPtr) so a missing asset is not retried every frame --
// the same contract the TryLoad* wrappers have always had.

SkeletonData* AnimationSystem::FindOrLoadSkeleton(const eastl::string& file_path)
{
    auto found = m_SkeletonDefinitionCache.find(file_path);
    if (found == m_SkeletonDefinitionCache.end())
    {
        AnimationLoader loader;
        found = m_SkeletonDefinitionCache.emplace(file_path, loader.LoadSkeletonData(file_path)).first;
    }
    return found->second;
}

const AnimationClip* AnimationSystem::FindOrLoadClip(const eastl::string& file_path)
{
    auto found = m_AnimationDataCache.find(file_path);
    if (found == m_AnimationDataCache.end())
    {
        AnimationLoader loader;
        found = m_AnimationDataCache.emplace(file_path, loader.LoadAnimationClipData(file_path)).first;
    }
    // The cache owns the clip; callers get a borrowed pointer and must not
    // outlive the Tick (see BlendStateWithClipData's lifetime note).
    return found->second.get();
}

const PPtr<AnimSkelMap>& AnimationSystem::FindOrLoadSkelMap(const eastl::string& file_path)
{
    auto found = m_AnimationSkeletonMapCache.find(file_path);
    if (found == m_AnimationSkeletonMapCache.end())
    {
        AnimationLoader loader;
        found = m_AnimationSkeletonMapCache.emplace(file_path, loader.LoadAnimSkelMap(file_path)).first;
    }
    return found->second;
}

BoneBlendMask* AnimationSystem::FindOrLoadMask(const eastl::string& file_path)
{
    auto found = m_SkeletonMaskCache.find(file_path);
    if (found == m_SkeletonMaskCache.end())
    {
        AnimationLoader loader;
        found = m_SkeletonMaskCache.emplace(file_path, loader.LoadSkeletonMask(file_path)).first;
    }
    return found->second;
}

// ---- public wrappers --------------------------------------------------------

SkeletonData* AnimationSystem::TryLoadSkeleton(const eastl::string& file_path)
{
    return FindOrLoadSkeleton(file_path);
}

std::shared_ptr<AnimationClip> AnimationSystem::TryLoadAnimation(const eastl::string& file_path)
{
    auto found = m_AnimationDataCache.find(file_path);
    if (found == m_AnimationDataCache.end())
    {
        AnimationLoader loader;
        found = m_AnimationDataCache.emplace(file_path, loader.LoadAnimationClipData(file_path)).first;
    }
    return found->second;
}

AnimSkelMap* AnimationSystem::TryLoadAnimationSkeletonMap(const eastl::string& file_path)
{
    // Used to emplace the freshly loaded map and then `return nullptr`, so a
    // first-time lookup always failed and every clip silently lost its bone
    // mapping. Load once, cache, then return what we just cached.
    return FindOrLoadSkelMap(file_path);
}

BoneBlendMask* AnimationSystem::TryLoadSkeletonMask(const eastl::string& file_path)
{
    return FindOrLoadMask(file_path);
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

void AnimationSystem::BuildBlendStateWithClipData(const BlendState& blend_state, BlendStateWithClipData& out_clip_data)
{
    BlendStateWithClipData& blend_state_with_clip_data = out_clip_data;

    // Reused scratch: clear FIRST, always. `blend_weight` is deliberately NOT
    // cleared here -- it is resized and its inner vectors assigned below, which
    // is what keeps their capacity across frames.
    blend_state_with_clip_data.clip_count = 0;
    blend_state_with_clip_data.blend_clip.clear();
    blend_state_with_clip_data.blend_anim_skel_map.clear();
    blend_state_with_clip_data.blend_ratio = blend_state.blend_ratio;

    // The separate "prime the cache" loops that used to sit here are gone: each
    // path is now resolved once, by FindOrLoad*, at the point of use.
    //
    // Assets are dropped instead of dereferenced: an empty path or a deleted
    // asset used to crash here every single frame.
    for (const eastl::string& clip_path : blend_state.blend_clip_file_path)
    {
        // Borrowed, not copied: this used to `push_back(*found->second)`, i.e.
        // deep-copy the whole clip (every channel name, every position /
        // rotation / scale key) once per clip per frame.
        const AnimationClip* clip = FindOrLoadClip(clip_path);
        if (clip == nullptr)
        {
            LOG_ERROR(ZAnimation, "AnimationSystem: clip '{}' could not be loaded", clip_path.c_str());
            continue;
        }
        blend_state_with_clip_data.blend_clip.push_back(clip);
    }
    for (const eastl::string& map_path : blend_state.blend_anim_skel_map_path)
    {
        // Pushed unconditionally, including a null PPtr. FindOrLoadSkelMap
        // always produces an entry, so the old "could not be loaded" branch
        // here was unreachable; a null map is handled where it is dereferenced
        // (Skeleton::ApplyAnimation skips that clip). Keeping this list the
        // same length as the path list is what the clip_count floor below
        // relies on.
        blend_state_with_clip_data.blend_anim_skel_map.push_back(FindOrLoadSkelMap(map_path));
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
        blend_state_with_clip_data.blend_weight.clear();
        return;
    }

    // Masks are optional: without them every bone takes every clip, which is
    // what "no mask authored" has always meant. The old code dereferenced
    // `blend_masks[0]` unconditionally, so any setup without masks crashed.
    // thread_local rather than a plain local: this is one allocation per frame
    // per animating object otherwise, and the function never recurses. Cleared
    // by the assign() below, so no state leaks between frames.
    thread_local std::vector<const BoneBlendMask*> blend_masks;
    blend_masks.assign(clip_count, nullptr);
    size_t skeleton_bone_count = 0;
    for (size_t clip_index = 0; clip_index < blend_state.blend_mask_file_path.size() && clip_index < clip_count; ++clip_index)
    {
        BoneBlendMask* mask = FindOrLoadMask(blend_state.blend_mask_file_path[clip_index]);
        if (mask == nullptr)
        {
            continue;
        }
        blend_masks[clip_index] = mask;
        // One lookup: this used to call TryLoadSkeleton (which finds, then
        // emplaces) and then find() the same key again.
        const SkeletonData* skeleton = FindOrLoadSkeleton(mask->skeleton_file_path);
        if (skeleton != nullptr)
        {
            skeleton_bone_count = std::max(skeleton_bone_count, skeleton->bones_map.size());
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

    // resize + per-entry assign, NOT `assign(clip_count, BoneBlendWeight{})`:
    // the latter overwrites every entry with a fresh BoneBlendWeight, throwing
    // away the inner vector's buffer, so a reused instance reallocated
    // `clip_count` bone-length float vectors every frame. resize() leaves the
    // surviving entries alone and assign() refills them in place.
    blend_state_with_clip_data.blend_weight.resize(clip_count);
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
}