#pragma once

#include <stdint.h>

// Align value up to a positive base. Uses integer division so non-power-of-two
// bases (e.g. 80 for D3D12 structured-buffer strides) work correctly.
inline uint32_t RoundUp(uint32_t value, uint32_t base)
{
    if (base == 0u)
    {
        return value;
    }
    return (value + base - 1u) / base * base;
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