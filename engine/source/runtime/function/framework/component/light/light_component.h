#pragma once
#include "core/meta/reflection/reflection.h"
#include "function/framework/component/component.h"

namespace Zentia
{
    REFLECTION_TYPE(ULightComponent)
    CLASS(ULightComponent : public Component, WhiteListFields)
    {
        REFLECTION_BODY(ULightComponent)

    public:

    };
}