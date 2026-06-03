#pragma once

#include "Runtime/BaseClasses/Object.h"

#include <string>
#include <vector>
class BoneBlendMask : public Object
{
    REGISTER_CLASS(BoneBlendMask);
    DECLARE_OBJECT_SERIALIZE(BoneBlendMask);

public:
    eastl::string skeleton_file_path;
    std::vector<int> enabled;
};