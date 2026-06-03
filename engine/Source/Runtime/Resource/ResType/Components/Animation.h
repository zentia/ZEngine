#pragma once

#include "Runtime/Core/Serialize/SerializeUtility.h"
#include "Runtime/Resource/ResType/Data/BlendState.h"

#include <vector>

class AnimationResultElement
{
public:
    int index;
    Matrix4x4_ transform;
};

class AnimationResult
{
public:
    std::vector<AnimationResultElement> node;
};

class AnimationComponentRes
{
public:
    DECLARE_SERIALIZE(AnimationComponentRes)

    eastl::string skeleton_file_path;
    BlendState blend_state;
    // animation to skeleton map
    float frame_position;  // 0-1

    AnimationResult animation_result;
};