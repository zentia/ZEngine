#pragma once
#include "Runtime/BaseClasses/PPtr.h"
#include "Runtime/Core/Serialize/SerializeUtility.h"
#include "Runtime/Resource/ResType/Data/AnimSkelMap.h"
#include "Runtime/Resource/ResType/Data/AnimationClip.h"

#include <vector>
class BoneBlendWeight
{
public:
    std::vector<float> blend_weight;
};

class BlendStateWithClipData
{
public:
    DECLARE_SERIALIZE(BlendStateWithClipData)

    int clip_count;
    std::vector<AnimationClip> blend_clip;
    std::vector<PPtr<AnimSkelMap>> blend_anim_skel_map;
    std::vector<BoneBlendWeight> blend_weight;
    std::vector<float> blend_ratio;
};

class BlendState
{
public:
    int clip_count;
    std::vector<eastl::string> blend_clip_file_path;
    std::vector<float> blend_clip_file_length;
    std::vector<eastl::string> blend_anim_skel_map_path;
    std::vector<float> blend_weight;
    std::vector<eastl::string> blend_mask_file_path;
    std::vector<float> blend_ratio;
};