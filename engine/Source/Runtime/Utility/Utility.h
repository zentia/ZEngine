#pragma once

#include <stdint.h>

inline uint32_t RoundUp(uint32_t value, int32_t base)
{
    return (value + base - 1) & (-base);
}

struct StringWithHash
{
    eastl::string str;
    size_t hash_val;

    StringWithHash(const eastl::string& s)
        : str(s), hash_val(eastl::hash<eastl::string> {}(s)) {}

    bool operator==(const StringWithHash& other) const
    {
        return hash_val == other.hash_val && str == other.str;
    }
};

namespace eastl
{
    template<>
    struct hash<StringWithHash>
    {
        size_t operator()(const StringWithHash& s) const noexcept
        {
            return s.hash_val;
        }
    };
}  // namespace eastl