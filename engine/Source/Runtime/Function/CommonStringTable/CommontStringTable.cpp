#include "CommonString.h"
#include "CommonStringTable.h"

const char* CommonStringTable::FindCommonString(const char* str, size_t length) const
{
    if (IsCommonString(str))
        return str;
    return nullptr;
}