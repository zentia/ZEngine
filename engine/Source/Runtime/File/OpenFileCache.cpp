#include "OpenFileCache.h"

#include "Runtime/Core/Serialize/WriteData.h"
#include "Runtime/Resource/Asset/AssetManager.h"

#include <utility>

OpenFileCache::~OpenFileCache()
{
    CloseAll();
}

int OpenFileCache::FindSlot(const std::filesystem::path& path) const
{
    for (int i = 0; i < kOpenedFileCacheCount; ++i)
    {
        if (!m_Filenames[i].empty() && m_Filenames[i] == path)
        {
            return i;
        }
    }
    return -1;
}

int OpenFileCache::PickSlot()
{
    for (int i = 0; i < kOpenedFileCacheCount; ++i)
    {
        if (m_Filenames[i].empty())
        {
            return i;
        }
    }

    // 全满：按轮转淘汰，避免每次只用一个槽位、把其余 9 个空着。
    const int slot = static_cast<int>(m_NextSlot % kOpenedFileCacheCount);
    m_NextSlot = (m_NextSlot + 1) % kOpenedFileCacheCount;
    m_Cache[slot].Close();
    m_Filenames[slot].clear();
    return slot;
}

File* OpenFileCache::OpenCache(const std::filesystem::path& path)
{
    if (path.empty())
    {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(m_Mutex);

    const int cached = FindSlot(path);
    if (cached >= 0)
    {
        return &m_Cache[cached];
    }

    const int slot = PickSlot();
    if (!m_Cache[slot].Open(path, kReadPermission))
    {
        return nullptr;
    }
    m_Filenames[slot] = path;
    return &m_Cache[slot];
}

void OpenFileCache::ForceClose(const std::filesystem::path& path)
{
    if (path.empty())
    {
        return;
    }

    std::lock_guard<std::mutex> lock(m_Mutex);
    const int slot = FindSlot(path);
    if (slot >= 0)
    {
        m_Cache[slot].Close();
        m_Filenames[slot].clear();
    }
}

void OpenFileCache::CloseAll()
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    for (int i = 0; i < kOpenedFileCacheCount; ++i)
    {
        if (!m_Filenames[i].empty())
        {
            m_Cache[i].Close();
            m_Filenames[i].clear();
        }
    }
}
