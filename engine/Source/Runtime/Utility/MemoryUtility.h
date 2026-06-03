#pragma once

template<typename T>
T LoadUnaligned(const void* src, size_t arrayIndex = 0)
{
    return static_cast<const T*>(src)[arrayIndex];
}

void MemoryConstrainedSrc(void* dst, const void* src, int size, const void* srcFrom, void* srcTo);

template<typename T>
void StoreUnaligned(void* dst, const T& src, size_t arrayIndex = 0)
{
    static_cast<T*>(dst)[arrayIndex] = src;
}