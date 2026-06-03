#pragma once
#include <stdint.h>
struct LocalSerializedObjectIdentifier
{
    int32_t localSerializedFileIndex;
    int64_t localIdentifierInFile;
};