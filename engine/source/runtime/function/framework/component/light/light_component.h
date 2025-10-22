#pragma once
#include "core/meta/reflection/reflection.h"
#include "function/framework/component/component.h"

namespace Z
{
    REFLECTION_TYPE(ULightComponent)
    CLASS(ULightComponent : public Component, WhiteListFields)
    {
        REFLECTION_BODY(ULightComponent)

    public:

    };
}