#pragma once

#include "Runtime/BaseClasses/Object.h"

#include <string>
#include <vector>
#include "BoneBlendMask.generated.h"
ZCLASS()
class BoneBlendMask : public Object
{
    ZGENERATED_BODY(BoneBlendMask)

public:
    ZPROPERTY()
    eastl::string skeleton_file_path;
    ZPROPERTY()
    std::vector<int> enabled;
};