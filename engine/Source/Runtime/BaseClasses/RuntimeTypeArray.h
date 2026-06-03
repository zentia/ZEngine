#pragma once
#include <stdint.h>

class Type;

class RuntimeTypeArray
{
public:
    static const uint32_t MAX_RUNTIME_TYPES = 1024;
    uint32_t count;
    Type* types[MAX_RUNTIME_TYPES];
};