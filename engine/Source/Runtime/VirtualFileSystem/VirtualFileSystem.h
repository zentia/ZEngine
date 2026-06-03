#pragma once
#include "VirtualFileSystemTypes.h"

class FileSystemEntry
{
public:
    FileSystemEntry(std::filesystem::path path);

    void Set(std::filesystem::path& path);
    FileSize Size() const;
    void SetOpenFlags(FileFlags fileFlags)
    {
        m_Data.openFlags = fileFlags;
    }

private:
    friend class FileAccessor;
    FileEntryData m_Data;
};

class FileAccessor
{
public:
    virtual ~FileAccessor();

    bool Open(FileSystemEntry& entry, FilePermission permissions, FileAutoBehavior behavior = kNormalBehavior);
    bool Close();
    bool Read(FileSize from, uint64_t count, void* buffer, uint64_t* actual, FileReadFlags flags = kFileReadNoFlags);
    bool Read(uint64_t size, void* buffer, uint64_t* actual, FileReadFlags flags = kFileReadNoFlags);
    bool Write(uint64_t size, const void* buffer, uint64_t* actual);
    bool Write(FileSize at, uint64_t size, const void* buffer, uint64_t* actual);
    FileSize Size() const;

protected:
    FileEntryData m_Data;
    bool m_OwnsData;
    bool m_IsOpen;
};