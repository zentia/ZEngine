#pragma once

#include "Runtime/BaseClasses/Object.h"

#include <string>
#include <vector>

class AnimSkelMap : public Object
{
    REGISTER_CLASS(AnimSkelMap);
    DECLARE_OBJECT_SERIALIZE();

public:
    std::vector<int> convert;
};