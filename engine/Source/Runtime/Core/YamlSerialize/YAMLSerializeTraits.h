#pragma once

#include "Runtime/Core/Serialize/TransferFunctions/TextSerializeTraits.h"

class YAMLWrite;

template<typename T>
class YAMLSerializeTraits : public TextSerializeTraits<T>
{
};

template<>
class YAMLSerializeTraits<const char*>
{
public:
    static void Transfer(const char* const& data, YAMLWrite& transfer);
};
