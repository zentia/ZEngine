#pragma once

#include <cstdlib>
#include <stdint.h>

#if defined(_MSC_VER)
    #include <intrin.h>
#endif

inline void SwapEndianBytes(uint32_t& i)
{
#if defined(_MSC_VER)
    i = _byteswap_ulong(i);
#else
    i = __builtin_bswap32(i);
#endif
}

inline void SwapEndianBytes(uint64_t& i)
{
#if defined(_MSC_VER)
    i = _byteswap_uint64(i);
#else
    i = __builtin_bswap64(i);
#endif
}
