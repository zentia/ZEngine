#pragma once

#include "Runtime/BaseClasses/Object.h"

#include <string>
#include <vector>
#include "AnimSkelMap.generated.h"

ZCLASS()
class AnimSkelMap : public Object
{
    ZGENERATED_BODY(AnimSkelMap)

public:
    ZPROPERTY()
    std::vector<int> convert;
};