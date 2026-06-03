#pragma once

#include "Runtime/BaseClasses/Object.h"
#include "Runtime/BaseClasses/Type.h"
class EditorResources
{
public:
    template<typename T>
    T* Load(std::filesystem::path& path)
    {
        return nullptr;
    }
};