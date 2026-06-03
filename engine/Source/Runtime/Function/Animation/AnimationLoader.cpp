
#include "AnimationLoader.h"

#include "Runtime/BaseClasses/PPtr.h"
#include "Runtime/Function/Animation/Utilities.h"
#include "Runtime/Resource/Asset/AssetManager.h"
#include "Runtime/Resource/ResType/Data/AnimationClip.h"
#include "Runtime/Resource/ResType/Data/BoneBlendMask.h"
#include "Runtime/Resource/ResType/Data/MeshData.h"
#include "Runtime/Resource/ResType/Data/SkeletonData.h"

namespace
{
    void addBoneBind(MeshData& mesh_bind, size_t bone_index, size_t vertex_index, float weight)
    {
        bone_index++;
        // 			if (!mesh_bind.bind[vertex_index])
        // 				mesh_bind.bind[vertex_index] =
        // std::make_shared<SkeletonBinding>();
        SkeletonBinding& vertex_binding = mesh_bind.bind[vertex_index];
        if (vertex_binding.index0 == 0)
        {
            vertex_binding.index0 = bone_index;
            vertex_binding.weight0 = weight;
            return;
        }
        if (vertex_binding.index1 == 0)
        {
            vertex_binding.index1 = bone_index;
            vertex_binding.weight1 = weight;
            return;
        }
        if (vertex_binding.index2 == 0)
        {
            vertex_binding.index2 = bone_index;
            vertex_binding.weight2 = weight;
            return;
        }
        if (vertex_binding.index3 == 0)
        {
            vertex_binding.index3 = bone_index;
            vertex_binding.weight3 = weight;
            return;
        }
        // LOG_ERROR
    }
}  // namespace

std::shared_ptr<AnimationClip> AnimationLoader::LoadAnimationClipData(eastl::string animation_clip_url)
{
    AnimationAsset* animation_clip = GET_SYSTEM(AssetManager)->loadAsset<AnimationAsset>(animation_clip_url);
    return std::make_shared<AnimationClip>(animation_clip->clip_data);
}

SkeletonData* AnimationLoader::LoadSkeletonData(eastl::string skeleton_data_url)
{
    return GET_SYSTEM(AssetManager)->loadAsset<SkeletonData>(skeleton_data_url);
}

PPtr<AnimSkelMap> AnimationLoader::LoadAnimSkelMap(eastl::string anim_skel_map_url)
{
    return GET_SYSTEM(AssetManager)->loadAsset<AnimSkelMap>(anim_skel_map_url);
}

BoneBlendMask* AnimationLoader::LoadSkeletonMask(eastl::string skeleton_mask_file_url)
{
    return GET_SYSTEM(AssetManager)->loadAsset<BoneBlendMask>(skeleton_mask_file_url);
}