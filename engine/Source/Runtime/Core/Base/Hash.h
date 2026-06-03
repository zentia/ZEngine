#pragma once

#include <EASTL/functional.h>
#include <cstddef>

template<typename T>
inline void hash_combine(std::size_t& seed, const T& v)
{
    seed ^= eastl::hash<T> {}(v) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}

template<typename T, typename... Ts>
inline void hash_combine(std::size_t& seed, const T& v, Ts... rest)
{
    hash_combine(seed, v);
    (hash_combine(seed, rest), ...);
}
