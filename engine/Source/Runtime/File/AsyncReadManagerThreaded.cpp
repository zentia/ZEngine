#include "AsyncReadManagerThreaded.h"

#include "File.h"

OpenFileCache& AsyncReadManagerThreaded::GetThreadCache()
{
    const std::thread::id threadId = std::this_thread::get_id();

    std::lock_guard<std::mutex> lock(m_CacheTableMutex);

    auto it = m_ThreadCaches.find(threadId);
    if (it != m_ThreadCaches.end())
    {
        // unordered_map 是节点式的，rehash 不会让指向 value 的引用失效。
        return *it->second;
    }

    auto cache = std::make_unique<OpenFileCache>();
    OpenFileCache* raw = cache.get();
    m_ThreadCaches.emplace(threadId, std::move(cache));
    return *raw;
}

void AsyncReadManagerThreaded::SyncRequest(AsyncReadCommand* request)
{
    File* file = GetThreadCache().OpenCache(request->path);

    if (file != nullptr)
    {
        request->totalBtyesRead = file->Read(request->offset, request->buffer, request->size, request->flags);
    }
}

void AsyncReadManagerThreaded::ForceCloseFile(const std::filesystem::path& path)
{
    std::lock_guard<std::mutex> lock(m_CacheTableMutex);
    for (auto& entry : m_ThreadCaches)
    {
        entry.second->ForceClose(path);
    }
}

void AsyncReadManagerThreaded::Shutdown()
{
    std::lock_guard<std::mutex> lock(m_CacheTableMutex);
    m_ThreadCaches.clear();
}
