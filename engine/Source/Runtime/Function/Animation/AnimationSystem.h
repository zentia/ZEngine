#pragma once

#include "Runtime/Resource/ResType/Data/AnimSkelMap.h"
#include "Runtime/Resource/ResType/Data/AnimationClip.h"
#include "Runtime/Resource/ResType/Data/BlendState.h"
#include "Runtime/Resource/ResType/Data/BoneBlendMask.h"
#include "Runtime/Resource/ResType/Data/SkeletonData.h"

#include <map>
#include <memory>

class AnimationSystem
{
private:
    static std::map<eastl::string, SkeletonData*> m_SkeletonDefinitionCache;
    static std::map<eastl::string, std::shared_ptr<AnimationClip>> m_AnimationDataCache;
    static std::map<eastl::string, PPtr<AnimSkelMap>> m_AnimationSkeletonMapCache;
    static std::map<eastl::string, BoneBlendMask*> m_SkeletonMaskCache;

public:
    static SkeletonData* TryLoadSkeleton(eastl::string file_path);
    static std::shared_ptr<AnimationClip> TryLoadAnimation(eastl::string file_path);
    static AnimSkelMap* TryLoadAnimationSkeletonMap(eastl::string file_path);
    static BoneBlendMask* TryLoadSkeletonMask(eastl::string file_path);
    static BlendStateWithClipData GetBlendStateWithClipData(const BlendState& blend_state);

    AnimationSystem() = default;
};