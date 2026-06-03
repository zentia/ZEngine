#pragma once

#include "Runtime/Resource/ResType/Data/AnimSkelMap.h"
#include "Runtime/Resource/ResType/Data/AnimationClip.h"
#include "Runtime/Resource/ResType/Data/BoneBlendMask.h"
#include "Runtime/Resource/ResType/Data/SkeletonData.h"

#include <memory>

template<typename T>
class PPtr;

class AnimationLoader
{
public:
    std::shared_ptr<AnimationClip> LoadAnimationClipData(eastl::string animation_clip_url);
    SkeletonData* LoadSkeletonData(eastl::string skeleton_data_url);
    PPtr<AnimSkelMap> LoadAnimSkelMap(eastl::string anim_skel_map_url);
    BoneBlendMask* LoadSkeletonMask(eastl::string skeleton_mask_file_url);
};