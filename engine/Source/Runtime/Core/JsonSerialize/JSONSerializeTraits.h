#pragma once

#include "Runtime/Core/Serialize/TransferFunctions/TextSerializeTraits.h"

class JSONWrite;

template<typename T>
class JSONSerializeTraits : public TextSerializeTraits<T>
{
};

template<>
class JSONSerializeTraits<const char*>
{
public:
    static void Transfer(const char* const& data, JSONWrite& transfer);
};