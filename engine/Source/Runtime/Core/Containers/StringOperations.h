#pragma once
#include "Runtime/Utility/TypeUtility.h"

#include <stdint.h>

enum class ComparisonType
{
    CaseSensitive,
    IgnoreCase,
};

template<typename TChar>
class BasicStringOperations
{
public:
    using ValueType = TChar;
    using SizeType = size_t;
    using UnsignedValueType = typename Conditional<
        sizeof(ValueType) == 1,
        uint8_t,
        typename Conditional<sizeof(ValueType) == 2,
                             uint16_t,
                             typename Conditional<sizeof(ValueType) == 4, uint32_t, NullType>::Type>::Type>::Type;

    static int CompareNoCheck(const ValueType* str,
                              SizeType len,
                              const ValueType* compareStr,
                              ComparisonType comparisonType = ComparisonType::CaseSensitive)
    {
        const ValueType* last = str + len;

        if (comparisonType == ComparisonType::IgnoreCase)
        {
            for (const ValueType* ptr = str; ptr < last; ++ptr, ++compareStr)
            {
                if (std::tolower(*ptr) != std::tolower(*compareStr))
                {
                    return (static_cast<UnsignedValueType>(std::tolower(*ptr)) -
                            static_cast<UnsignedValueType>(std::tolower(*compareStr)));
                }
                else if (*compareStr == ValueType(0))
                {
                    return 1;
                }
            }
            return -static_cast<UnsignedValueType>(std::tolower(*compareStr));
        }

        for (const ValueType* ptr = str; ptr < last; ++ptr, ++compareStr)
        {
            if ((*ptr) != (*compareStr))
            {
                return (static_cast<UnsignedValueType>(*ptr) - static_cast<UnsignedValueType>(*compareStr));
            }
            else if (*compareStr == ValueType(0))
            {
                return 1;
            }
        }

        return -static_cast<UnsignedValueType>(*compareStr);
    }
};