#pragma once

#include "Runtime/VirtualFileSystem/LocalFileSystem.h"

class LocalFileSystemLinux : public LocalFileSystemHandler
{
public:
    bool Open(FileEntryData& data, FilePermission permissions, FileAutoBehavior behavior) override;
    bool Close(FileEntryData& data) override;
    bool Read(FileEntryData& data, FileSize from, uint64_t size, void* buffer, uint64_t* actual, FileReadFlags flags) override;
    bool Read(FileEntryData& data, uint64_t size, void* buffer, uint64_t* actual, FileReadFlags flags) override;
    bool Write(FileEntryData& data, FileSize at, uint64_t size, const void* buffer, uint64_t* actual) override;
    bool Write(FileEntryData& data, uint64_t size, const void* buffer, uint64_t* actual) override;
    FileSize Size(const FileEntryData& data) const override;

private:
    FileSystemError GetLastFileSystemError(bool operationSuccessful) const;
    static FileSystemError ConvertToFileSystemError(int error);
    int GetAndUpdateLastError(bool operationSuccessful) const;

    mutable int m_LastError {0};
};
