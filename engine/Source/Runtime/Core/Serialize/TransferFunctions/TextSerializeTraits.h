#pragma once

#include "Runtime/Core/Serialize/SerializeTraits.h"

template<typename T>
class TextSerializeTraitsBase
{
public:
    template<typename TransferFunction>
    inline static void Transfer(T& data, TransferFunction& transfer)
    {
        SerializeTraits<T>::Transfer(data, transfer);
    }
};

template<typename T>
class TextSerializeTraits : public TextSerializeTraitsBase<T>
{
};