#pragma once
#include "CoreUObject/UObject/ObjectPtr.h"
#include "function/framework/object/object.h"

namespace Zentia
{
    class ULightComponent;
}

class ALight : public Zentia::AActor
{
public:
private:
    TObjectPtr<Zentia::ULightComponent> LightComponent;
};