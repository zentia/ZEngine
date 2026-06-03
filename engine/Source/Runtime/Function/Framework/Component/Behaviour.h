#pragma once

#include "Runtime/Function/Framework/Component/Component.h"

class Behaviour : public Component
{
    REGISTER_CLASS_TRAITS(kTypeIsAbstract);
    REGISTER_CLASS(Behaviour);
    DECLARE_OBJECT_SERIALIZE();
};