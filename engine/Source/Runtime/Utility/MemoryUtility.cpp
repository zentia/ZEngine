#include "MemoryUtility.h"

void MemoryConstrainedSrc(void* dst, const void* src, int size, const void* srcFrom, void* srcTo)
{
    uint8_t* fromClamped = std::clamp((uint8_t*)src, (uint8_t*)srcFrom, (uint8_t*)srcTo);
    uint8_t* toClamped = std::clamp((uint8_t*)src + size, (uint8_t*)srcFrom, (uint8_t*)srcTo);

    int offset = fromClamped - (uint8_t*)src;
    size = toClamped - fromClamped;
    memcpy((uint8_t*)dst + offset, (uint8_t*)src + offset, size);
}