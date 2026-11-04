#pragma once

#include <filesystem>
#include "Runtime/VirtualFileSystem/VirtualFileSystem.h"
#include "Runtime/VirtualFileSystem/VirtualFileSystemTypes.h"

class File
{
public:
    File();

    // 析构即关闭：m_Entry / m_Accessor 都是 MemoryManager 分配的裸指针，不关就漏。
    // 有了它，栈上的 File（File::Open 里的临时对象、OpenFileCache 的槽位）在离开
    // 作用域时也能把句柄还回去，不必依赖调用方记得 Close()。
    ~File();

    // 裸指针 + 独占句柄，拷贝等于双重关闭，直接禁掉；移动转移所有权。
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    File(File&& other) noexcept;
    File& operator=(File&& other) noexcept;

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