#pragma once

#include "Runtime/BaseClasses/Object.h"
#include "Runtime/VirtualFileSystem/VirtualFileSystemTypes.h"

struct WriteInfo
{
    FileSize headerOffset;
};

struct WriteData
{
    int64_t localIdentifierInFile;
    Object* objectPtr;
};