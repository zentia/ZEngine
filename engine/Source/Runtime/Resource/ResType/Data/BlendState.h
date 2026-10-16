#pragma once
#include "Runtime/BaseClasses/PPtr.h"
#include "Runtime/Core/Serialize/SerializeUtility.h"
#include "Runtime/Resource/ResType/Data/AnimSkelMap.h"
#include "Runtime/Resource/ResType/Data/AnimationClip.h"

#include <vector>
class BoneBlendWeight
{
public:
    DECLARE_SERIALIZE(BoneBlendWeight)

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
    DECLARE_SERIALIZE(BlendState)

    int clip_count;
    std::vector<eastl::string> blend_clip_file_path;
    std::vector<float> blend_clip_file_length;
    std::vector<eastl::string> blend_anim_skel_map_path;
    std::vector<float> blend_weight;
    std::vector<eastl::string> blend_mask_file_path;
    std::vector<float> blend_ratio;
};

template<typename TransferFunction>
void BoneBlendWeight::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(blend_weight, "blend_weight");
}

template<typename TransferFunction>
void BlendState::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(clip_count, "clip_count");
    transfer.Transfer(blend_clip_file_path, "blend_clip_file_path");
    transfer.Transfer(blend_clip_file_length, "blend_clip_file_length");
    transfer.Transfer(blend_anim_skel_map_path, "blend_anim_skel_map_path");
    transfer.Transfer(blend_weight, "blend_weight");
    transfer.Transfer(blend_mask_file_path, "blend_mask_file_path");
    transfer.Transfer(blend_ratio, "blend_ratio");
}

INSTANTIATE_TEMPLATE_TRANSFER_WITH_DECL(BoneBlendWeight, , Transfer, void)
INSTANTIATE_TEMPLATE_TRANSFER_WITH_DECL(BlendState, , Transfer, void)
