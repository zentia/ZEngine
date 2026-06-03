#pragma once

#include "File.h"
#include "Runtime/BaseClasses/Object.h"

#include <filesystem>

class OpenFileCache
{
public:
    File* OpenCache(const std::filesystem::path& path);
    void ForceClose(const std::filesystem::path& path);

private:
    enum
    {
        kOpenedFileCacheCount = 10
    };
    File m_Cache[kOpenedFileCacheCount];
    std::filesystem::path m_Filenames[kOpenedFileCacheCount];
};