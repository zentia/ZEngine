#include "OpenFileCache.h"

#include "Runtime/Core/Serialize/WriteData.h"
#include "Runtime/Resource/Asset/AssetManager.h"

File* OpenFileCache::OpenCache(const std::filesystem::path& path)
{
    if (path.empty())
    {
        return nullptr;
    }
    int32_t lruIndex = 0;
    if (!m_Cache[lruIndex].Open(path, kReadPermission))
    {
        return nullptr;
    }
    m_Filenames[lruIndex] = path;
    return &m_Cache[lruIndex];
}

void OpenFileCache::ForceClose(const std::filesystem::path& path)
{
    if (path.empty())
    {
        return;
    }

    for (int i = 0; i < kOpenedFileCacheCount; i++)
    {
        if (m_Filenames[i] == path)
        {
            m_Cache[i].Close();
            m_Filenames[i].clear();
            return;
        }
    }
}