#pragma once
#include "Runtime/VirtualFileSystem/LocalFileSystem.h"

class LocalFileSystemWindowsShared : public LocalFileSystemHandler
{
public:
    virtual bool Open(FileEntryData& data, FilePermission permissions, FileAutoBehavior behavior);
    virtual bool Close(FileEntryData& data);
    virtual bool Read(FileEntryData& data, FileSize from, uint64_t size, void* buffer, uint64_t* actual, FileReadFlags flags);
    virtual bool Read(FileEntryData& data, uint64_t size, void* buffer, uint64_t* actual, FileReadFlags flags);
    virtual bool Write(FileEntryData& data, FileSize at, uint64_t size, const void* buffer, uint64_t* actual);
    virtual bool Write(FileEntryData& data, uint64_t size, const void* buffer, uint64_t* actual);
    virtual FileSize Size(const FileEntryData& data) const;

protected:
    FileSystemError GetLastFileSystemError(bool operationSuccessful) const;
    static FileSystemError ConvertToFileSystemError(DWORD error);
    virtual DWORD GetAndUpdateLastError(bool operationSuccessful) const;

    mutable DWORD m_LastError;
};