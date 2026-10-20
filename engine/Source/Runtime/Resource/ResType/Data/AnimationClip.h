#pragma once
#include "Runtime/Core/Math/Quaternion.h"
#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Core/Serialize/SerializeUtility.h"

#include <vector>
#include "AnimationClip.generated.h"

class AnimNodeMap
{
public:
    DECLARE_SERIALIZE(AnimNodeMap)

    std::vector<eastl::string> convert;
};

class AnimationChannel
{
public:
    DECLARE_SERIALIZE(AnimationChannel)

    eastl::string name;
    std::vector<Vector3> position_keys;
    std::vector<Quaternion> rotation_keys;
    std::vector<Vector3> scaling_keys;
};

class AnimationClip
{
public:
    DECLARE_SERIALIZE(AnimationClip)

    int total_frame {0};
    int node_count {0};
    std::vector<AnimationChannel> node_channels;
};

ZCLASS()
class AnimationAsset : public Object
{
    ZGENERATED_BODY(AnimationAsset)

public:
    ZPROPERTY()
    AnimNodeMap node_map;
    ZPROPERTY()
    AnimationClip clip_data;
    ZPROPERTY()
    eastl::string skeleton_file_path;
};
