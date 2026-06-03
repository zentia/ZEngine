#pragma once

#include "Runtime/VirtualFileSystem/VirtualFileSystemTypes.h"

struct AsyncReadCommand
{
    enum Status
    {
        kCompleted = 0,
        kInProgress = 1,
    };

    std::filesystem::path path;

    Status status;

    FileReadFlags flags;

    size_t size;

    uint8_t* buffer;
    FileSize offset;

    size_t totalBtyesRead;
};