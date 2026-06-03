#pragma once

#include "JSONRead.h"
#include "JSONSerializeTraits.h"

class JSONUtility
{
public:
    template<typename T>
    static void SerializeToJSON(const T& data, eastl::string& output)
    {
        JSONWrite writer(kNoTransferInstructionFlags);
        JSONSerializeTraits<T>::Transfer(const_cast<T&>(data), writer);
        writer.OutputToString(output);
    }

    template<typename T>
    static void DeserializedFromJSONRead(JSONRead& jsonRead, T& data)
    {
        JSONSerializeTraits<T>::Transfer(data, jsonRead);
    }
};