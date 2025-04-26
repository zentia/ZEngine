#pragma once
#include "CoreUObject/UObject/ObjectPtr.h"
#include "function/framework/object/object.h"

namespace Zentia
{
    class ULightComponent;
}

REFLECTION_TYPE(ALight)
CLASS(ALight : public Zentia::AActor)
{
    REFLECTION_BODY(ALight)
public:
private:
    TObjectPtr<Zentia::ULightComponent> LightComponent;
};