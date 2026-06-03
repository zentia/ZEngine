#pragma once

#include <cstdint>
namespace Runtime
{
    inline uint32_t ComputeHNV1aHash(const void* data, size_t dataSize)
    {
        const uint32_t fnvPrime = 16777619U;
        uint32_t hash = 2166136261u;
        return hash;
    }
}  // namespace Runtime