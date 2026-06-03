#pragma once

#include "Runtime/Core/YamlSerialize/YAMLRead.h"
#include "Runtime/Core/YamlSerialize/YAMLSerializeTraits.h"
#include "Runtime/Core/YamlSerialize/YAMLWrite.h"

class YAMLUtility
{
public:
    template<typename T>
    static void SerializeToYAML(const T& data, eastl::string& output)
    {
        YAMLWrite writer(kNoTransferInstructionFlags);
        YAMLSerializeTraits<T>::Transfer(const_cast<T&>(data), writer);
        writer.OutputToString(output);
    }

    template<typename T>
    static void DeserializeFromYAML(const char* yamlText, T& data)
    {
        YAMLRead reader(yamlText, kNoTransferInstructionFlags);
        YAMLSerializeTraits<T>::Transfer(data, reader);
    }

    template<typename T>
    static void DeserializeFromYAMLRead(YAMLRead& yamlRead, T& data)
    {
        YAMLSerializeTraits<T>::Transfer(data, yamlRead);
    }
};
