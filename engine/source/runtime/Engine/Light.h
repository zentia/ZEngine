#pragma once
#include "CoreUObject/UObject/ObjectPtr.h"
#include "function/framework/object/object.h"

namespace Z
{
    class ULightComponent;
}

REFLECTION_TYPE(ALight)
CLASS(ALight : public Z::AActor)
{
    REFLECTION_BODY(ALight)
public:
private:
    TObjectPtr<Z::ULightComponent> LightComponent;
};