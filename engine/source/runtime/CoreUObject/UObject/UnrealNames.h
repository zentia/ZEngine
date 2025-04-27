#pragma once

#define REGISTER_NAME(num, name) name = num,
#include "core/HAL/Platform.h"

enum class EName : uint32
{
    #include "UnrealNames.inl"
    MaxHardCodedNameIndex,
};