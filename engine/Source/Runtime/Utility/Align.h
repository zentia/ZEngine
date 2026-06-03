#pragma once
#include <stdint.h>
template<typename T, typename V>
inline T AlignToPowerOfTwo(T value, T alignment)
{
    return (value + (alignment - 1)) & ~(alignment - 1);
}

inline uint32_t Align4(uint32_t size)
{
    return AlignToPowerOfTwo<uint32_t, uint32_t>(size, 4);
}

inline uint32_t Align4LeftOver(uint32_t size)
{
    return Align4(size) - size;
}