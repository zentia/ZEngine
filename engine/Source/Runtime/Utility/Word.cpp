#include "Word.h"

const char kHexToLiteral[16] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
void BytesToHexString(const void* data, size_t bytes, char* str)
{
    for (size_t i = 0; i < bytes; i++)
    {
        uint8_t b = ((uint8_t*)data)[i];
        str[2 * i + 0] = kHexToLiteral[b >> 4];
        str[2 * i + 1] = kHexToLiteral[b & 0xf];
    }
}