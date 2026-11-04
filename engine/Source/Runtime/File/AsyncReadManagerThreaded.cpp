#include "AsyncReadManagerThreaded.h"

#include "File.h"
void AsyncReadManagerThreaded::SyncRequest(AsyncReadCommand* request)
{
    std::lock_guard<std::mutex> lock(m_FileCacheMutex);
    File* file = m_SyncOpenFilesCache.OpenCache(request->path);

    if (file != nullptr)
    {
        request->totalBtyesRead = file->Read(request->offset, request->buffer, request->size, request->flags);
    }
}

void AsyncReadManagerThreaded::ForceCloseFile(const std::filesystem::path& path)
{
    std::lock_guard<std::mutex> lock(m_FileCacheMutex);
    m_SyncOpenFilesCache.ForceClose(path);
}