#pragma once
#include "Runtime/VirtualFileSystem/VirtualFileSystem.h"
#include "Runtime/VirtualFileSystem/VirtualFileSystemTypes.h"

class File
{
public:
    File();

    bool Open(const std::filesystem::path& path, FilePermission perm, FileAutoBehavior behavior = kNormalBehavior);
    bool OpenFileSystemEntry(const FileSystemEntry& fileSystemEntry, FilePermission permission, FileAutoBehavior behavior = kNormalBehavior);
    bool Close();

    size_t Read(FileSize position, void* buffer, size_t size, FileReadFlags flag = kFileReadNoFlags);
    bool Write(const void* buffer, size_t size);
    bool Write(FileSize pos, const void* buffer, size_t size);

private:
    FileSystemEntry* m_Entry;
    FileAccessor* m_Accessor;
    FileSize m_Position;
    FileFlags m_OpenFlags;
};