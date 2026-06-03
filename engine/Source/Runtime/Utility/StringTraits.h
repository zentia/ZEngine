#pragma once

namespace StringTraits
{
    template<typename TChar>
    inline const TChar* AsConstTChars(const TChar* value)
    {
        return value;
    }

    template<typename TChar, typename TAlloc>
    inline const TChar* AsConstTChars(const eastl::basic_string<TChar, TAlloc>& value)
    {
        return value.c_str();
    }
}  // namespace StringTraits