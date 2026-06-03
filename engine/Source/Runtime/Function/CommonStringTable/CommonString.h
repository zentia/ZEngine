#pragma once

namespace CommonString
{
    extern const char* const BufferBegin;
    extern const char* const BufferEnd;
    enum
    {
        IdBase = -1,
#define COMMON_STRING_ENTRY(a, b) Id_##a,
#include "CommonStrings.h"
#undef COMMON_STRING_ENTRY
        IdEmpty,
        Count = IdEmpty
    };
#define COMMON_STRING_ENTRY(a, b) extern const char* const gLiteral_##a;
#include "CommonStrings.h"
#undef COMMON_STRING_ENTRY
    extern const char* const gLiteralEmpty;
}  // namespace CommonString

#define COMMON_STRING(a)    CommonString::gLiteral_##a
#define COMMON_STRING_EMPRY CommonString::gLiteralEmpty

inline bool IsCommonString(const char* str)
{
    return str == nullptr || (str >= CommonString::BufferBegin && str <= CommonString::BufferEnd);
}