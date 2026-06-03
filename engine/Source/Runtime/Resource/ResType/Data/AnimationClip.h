#pragma once
#include "Runtime/Core/Math/Quaternion.h"
#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Core/Serialize/SerializeUtility.h"

#include <vector>

class AnimNodeMap
{
public:
    DECLARE_SERIALIZE(AnimNodeMap)

    std::vector<std::string> convert;
};

class AnimationChannel
{
public:
    DECLARE_SERIALIZE(AnimationChannel)

    std::string name;
    std::vector<Vector3> position_keys;
    std::vector<Quaternion> rotation_keys;
    std::vector<Vector3> scaling_keys;
};

class AnimationClip
{
    DECLARE_SERIALIZE(AnimationClip)

public:
    int total_frame {0};
    int node_count {0};
    std::vector<AnimationChannel> node_channels;
};

class AnimationAsset : public Object
{
    REGISTER_CLASS(AnimationAsset);
    DECLARE_OBJECT_SERIALIZE(AnimationAsset)

public:
    AnimNodeMap node_map;
    AnimationClip clip_data;
    eastl::string skeleton_file_path;
};
