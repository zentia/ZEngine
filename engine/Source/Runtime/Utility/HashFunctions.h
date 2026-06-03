#pragma once

#include "xxhash.h"

inline uint32_t ComputeHash32(const void* data, size_t dataSize)
{
    return XXH32(data, dataSize, 0x8f37154b);
}