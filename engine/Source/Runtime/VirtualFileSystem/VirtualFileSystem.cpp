#include "VirtualFileSystem.h"

#include "Runtime/File/FileSystem.h"

FileSystemEntry::FileSystemEntry(std::filesystem::path path)
{
    Set(path);
}

void FileSystemEntry::Set(std::filesystem::path& path)
{
    m_Data = FileEntryData();
    m_Data.path = path;
    FileSystemHandler* handler = GET_SYSTEM(FileSystem)->GetHandlerForPath(path);
    handler->InitializeFileEntry(m_Data);
}

FileSize FileSystemEntry::Size() const
{
    return m_Data.accessorHandler != nullptr ? m_Data.accessorHandler->Size(m_Data) : FileSize();
}

FileAccessor::~FileAccessor()
{
    Close();
}

bool FileAccessor::Open(FileSystemEntry& entry, FilePermission permissions, FileAutoBehavior behavior)
{
    m_Data = entry.m_Data;
    m_Data.accessorData = nullptr;
    m_OwnsData = true;
    bool result = m_Data.accessorHandler->Open(m_Data, permissions, behavior);
    m_IsOpen = result;
    return result;
}

bool FileAccessor::Close()
{
    return m_Data.accessorHandler != nullptr ? m_Data.accessorHandler->Close(m_Data) : false;
}

bool FileAccessor::Read(FileSize from, uint64_t count, void* buffer, uint64_t* actual, FileReadFlags flags)
{
    return m_Data.accessorHandler->Read(m_Data, from, count, buffer, actual, flags);
}

bool FileAccessor::Read(uint64_t size, void* buffer, uint64_t* actual, FileReadFlags flags)
{
    bool result = m_Data.accessorHandler != nullptr ? m_Data.accessorHandler->Read(m_Data, size, buffer, actual, flags) : false;
    return result;
}

bool FileAccessor::Write(uint64_t size, const void* buffer, uint64_t* actual)
{
    return m_Data.accessorHandler != nullptr ? m_Data.accessorHandler->Write(m_Data, size, buffer, actual) : false;
}

bool FileAccessor::Write(FileSize at, uint64_t size, const void* buffer, uint64_t* actual)
{
    return m_Data.accessorHandler != nullptr ? m_Data.accessorHandler->Write(m_Data, at, size, const_cast<void*>(buffer), actual) : false;
}

FileSize FileAccessor::Size() const
{
    return m_Data.accessorHandler != nullptr ? m_Data.accessorHandler->Size(m_Data) : FileSize();
}
