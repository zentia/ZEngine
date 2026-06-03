#include "File.h"

#include "Runtime/Utility/RetriableOperation.h"

File::File()
    : m_Entry(nullptr), m_Accessor(nullptr), m_OpenFlags(kFileFlagNone) {}

bool File::Open(const std::filesystem::path& path, FilePermission perm, FileAutoBehavior behavior)
{
    FileSystemEntry fileSystemEntry(path);
    return OpenFileSystemEntry(fileSystemEntry, perm, behavior);
}

class OpenFileOperation
{
public:
    OpenFileOperation(FileAccessor* fileAccessor, FileSystemEntry* entry, FilePermission permission, FileAutoBehavior behavior)
        : m_FileAccessor(fileAccessor), m_FileSystemEntry(entry), m_Permission(permission), m_Behavior(behavior) {}

    bool Execute()
    {
        return m_FileAccessor->Open(*m_FileSystemEntry, m_Permission, m_Behavior);
    }

private:
    FileAccessor* m_FileAccessor;
    FileSystemEntry* m_FileSystemEntry;
    FilePermission m_Permission;
    FileAutoBehavior m_Behavior;
};

bool File::OpenFileSystemEntry(const FileSystemEntry& fileSystemEntry, FilePermission permission, FileAutoBehavior behavior)
{
    if (m_Entry != nullptr)
    {
        MemoryManager::DestroyObject(m_Entry);
    }
    if (m_Accessor)
    {
        m_Accessor->Close();
        MemoryManager::DestroyObject(m_Accessor);
    }
    m_Position = static_cast<uint64_t>(0);
    m_Entry = MemoryManager::CreateObject<FileSystemEntry>(fileSystemEntry);
    m_Accessor = MemoryManager::CreateObject<FileAccessor>();

    if (m_OpenFlags != kFileFlagNone)
    {
        m_Entry->SetOpenFlags(m_OpenFlags);
    }
    const int retryCount = behavior & kRetryOnOpenFail ? 5 : 0;
    OpenFileOperation operation(m_Accessor, m_Entry, permission, behavior);
    if (!RetriableOperation::Perform(operation))
    {
        Close();
        return false;
    }
    return true;
}

bool File::Close()
{
    bool success = true;
    if (m_Accessor != nullptr)
    {
        success = m_Accessor->Close();
    }
    MEMORY_DELETE(m_Entry);
    MEMORY_DELETE(m_Accessor);
    m_Accessor = nullptr;

    return success;
}

size_t File::Read(FileSize position, void* buffer, size_t size, FileReadFlags flags)
{
    size_t totallyRead = 0;
    void* tmpBuffer = buffer;
    uint64_t actuallyReadThisAttempt = 0;
    m_Position = position;
    while (size > 0 && m_Accessor->Read(m_Position, size, tmpBuffer, &actuallyReadThisAttempt, flags) && actuallyReadThisAttempt > 0)
    {
        totallyRead += actuallyReadThisAttempt;
        tmpBuffer = static_cast<char*>(tmpBuffer) + actuallyReadThisAttempt;
        size -= actuallyReadThisAttempt;
        m_Position += actuallyReadThisAttempt;
    }
    return totallyRead;
}

bool File::Write(const void* buffer, size_t size)
{
    uint64_t actual = 0;
    bool success = m_Accessor->Write(size, buffer, &actual);
    return success && actual == size;
}

bool File::Write(FileSize position, const void* buffer, size_t size)
{
    uint64_t actual = 0;
    bool success = m_Accessor->Write(position, size, buffer, &actual);
    return success && actual == size;
}
