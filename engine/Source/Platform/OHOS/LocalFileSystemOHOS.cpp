#include "LocalFileSystemOHOS.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>

namespace
{
    int* GetFileDescriptorStorage(const FileEntryData& data)
    {
        return static_cast<int*>(data.accessorData);
    }

    int GetFileDescriptor(const FileEntryData& data)
    {
        const int* fd_storage = static_cast<const int*>(data.accessorData);
        return fd_storage != nullptr ? *fd_storage : -1;
    }

    int FilePermissionToOpenFlags(FilePermission permissions)
    {
        switch (permissions)
        {
            case kReadPermission:
                return O_RDONLY;
            case kWritePermission:
                return O_WRONLY | O_CREAT | O_TRUNC;
            case kReadWritePermission:
                return O_RDWR | O_CREAT;
            case kAppendPermission:
                return O_WRONLY | O_CREAT | O_APPEND;
            default:
                return -1;
        }
    }
}  // namespace

bool LocalFileSystemOHOS::Open(FileEntryData& data, FilePermission permissions, [[maybe_unused]] FileAutoBehavior behavior)
{
    const int open_flags = FilePermissionToOpenFlags(permissions);
    if (open_flags < 0)
    {
        data.lastError = GetLastFileSystemError(false);
        return false;
    }

    const std::string path_string = data.path.generic_string();
    const int fd = ::open(path_string.c_str(), open_flags, 0666);
    if (fd < 0)
    {
        data.lastError = GetLastFileSystemError(false);
        return false;
    }

    data.accessorData = new int(fd);
    data.accessorHandler = this;
    data.lastError = GetLastFileSystemError(true);
    return true;
}

bool LocalFileSystemOHOS::Close(FileEntryData& data)
{
    int* fd_storage = GetFileDescriptorStorage(data);
    if (fd_storage == nullptr)
    {
        return true;
    }

    const int fd = *fd_storage;
    const bool success = (fd >= 0) ? (::close(fd) == 0) : true;
    data.lastError = GetLastFileSystemError(success);
    delete fd_storage;
    data.accessorData = nullptr;
    return success;
}

bool LocalFileSystemOHOS::Read(
    FileEntryData& data,
    FileSize from,
    uint64_t count,
    void* buffer,
    uint64_t* actual,
    [[maybe_unused]] FileReadFlags flags)
{
    const int fd = GetFileDescriptor(data);
    if (fd < 0 || buffer == nullptr || actual == nullptr)
    {
        data.lastError = GetLastFileSystemError(false);
        return false;
    }

    *actual = 0;
    uint64_t offset = from.Cast<uint64_t>();
    while (count > 0)
    {
        const size_t to_read = static_cast<size_t>(std::min<uint64_t>(count, static_cast<uint64_t>(std::numeric_limits<size_t>::max())));
        const auto bytes_read = ::pread(fd, buffer, to_read, static_cast<off_t>(offset));
        if (bytes_read < 0)
        {
            data.lastError = GetLastFileSystemError(false);
            return false;
        }
        if (bytes_read == 0)
        {
            break;
        }

        buffer = static_cast<char*>(buffer) + bytes_read;
        *actual += static_cast<uint64_t>(bytes_read);
        count -= static_cast<uint64_t>(bytes_read);
        offset += static_cast<uint64_t>(bytes_read);
    }

    data.lastError = GetLastFileSystemError(true);
    return *actual > 0;
}

bool LocalFileSystemOHOS::Read(
    FileEntryData& data,
    uint64_t count,
    void* buffer,
    uint64_t* actual,
    [[maybe_unused]] FileReadFlags flags)
{
    const int fd = GetFileDescriptor(data);
    if (fd < 0 || buffer == nullptr || actual == nullptr)
    {
        data.lastError = GetLastFileSystemError(false);
        return false;
    }

    *actual = 0;
    while (count > 0)
    {
        const size_t to_read = static_cast<size_t>(std::min<uint64_t>(count, static_cast<uint64_t>(std::numeric_limits<size_t>::max())));
        const auto bytes_read = ::read(fd, buffer, to_read);
        if (bytes_read < 0)
        {
            data.lastError = GetLastFileSystemError(false);
            return false;
        }
        if (bytes_read == 0)
        {
            break;
        }

        buffer = static_cast<char*>(buffer) + bytes_read;
        *actual += static_cast<uint64_t>(bytes_read);
        count -= static_cast<uint64_t>(bytes_read);
    }

    data.lastError = GetLastFileSystemError(true);
    return *actual > 0;
}

bool LocalFileSystemOHOS::Write(FileEntryData& data, FileSize at, uint64_t size, const void* buffer, uint64_t* actual)
{
    const int fd = GetFileDescriptor(data);
    if (fd < 0 || buffer == nullptr || actual == nullptr)
    {
        data.lastError = GetLastFileSystemError(false);
        return false;
    }

    *actual = 0;
    uint64_t offset = at.Cast<uint64_t>();
    while (size > 0)
    {
        const size_t to_write = static_cast<size_t>(std::min<uint64_t>(size, static_cast<uint64_t>(std::numeric_limits<size_t>::max())));
        const auto bytes_written = ::pwrite(fd, buffer, to_write, static_cast<off_t>(offset));
        if (bytes_written < 0)
        {
            data.lastError = GetLastFileSystemError(false);
            return false;
        }
        if (bytes_written == 0)
        {
            break;
        }

        buffer = static_cast<const char*>(buffer) + bytes_written;
        *actual += static_cast<uint64_t>(bytes_written);
        size -= static_cast<uint64_t>(bytes_written);
        offset += static_cast<uint64_t>(bytes_written);
    }

    data.lastError = GetLastFileSystemError(true);
    return size == 0;
}

bool LocalFileSystemOHOS::Write(FileEntryData& data, uint64_t size, const void* buffer, uint64_t* actual)
{
    const int fd = GetFileDescriptor(data);
    if (fd < 0 || buffer == nullptr || actual == nullptr)
    {
        data.lastError = GetLastFileSystemError(false);
        return false;
    }

    *actual = 0;
    const uint64_t expected = size;
    while (size > 0)
    {
        const size_t to_write = static_cast<size_t>(std::min<uint64_t>(size, static_cast<uint64_t>(std::numeric_limits<size_t>::max())));
        const auto bytes_written = ::write(fd, buffer, to_write);
        if (bytes_written < 0)
        {
            data.lastError = GetLastFileSystemError(false);
            return false;
        }
        if (bytes_written == 0)
        {
            break;
        }

        buffer = static_cast<const char*>(buffer) + bytes_written;
        *actual += static_cast<uint64_t>(bytes_written);
        size -= static_cast<uint64_t>(bytes_written);
    }

    data.lastError = GetLastFileSystemError(true);
    return *actual == expected;
}

FileSize LocalFileSystemOHOS::Size(const FileEntryData& data) const
{
    const int fd = GetFileDescriptor(data);
    struct stat file_stat {};

    if (fd >= 0)
    {
        const bool success = (::fstat(fd, &file_stat) == 0);
        const_cast<FileEntryData&>(data).lastError = GetLastFileSystemError(success);
        if (success)
        {
            return FileSize(static_cast<uint64_t>(file_stat.st_size));
        }
    }

    const std::string path_string = data.path.generic_string();
    const bool success = (::stat(path_string.c_str(), &file_stat) == 0);
    const_cast<FileEntryData&>(data).lastError = GetLastFileSystemError(success);
    if (!success)
    {
        return FileSize();
    }

    return FileSize(static_cast<uint64_t>(file_stat.st_size));
}

FileSystemError LocalFileSystemOHOS::GetLastFileSystemError(bool operationSuccessful) const
{
    return ConvertToFileSystemError(GetAndUpdateLastError(operationSuccessful));
}

FileSystemError LocalFileSystemOHOS::ConvertToFileSystemError(int error)
{
    switch (error)
    {
        case 0:
            return kFileSystemErrorSuccess;
        default:
            return kFileSystemErrorUnknown;
    }
}

int LocalFileSystemOHOS::GetAndUpdateLastError(bool operationSuccessful) const
{
    m_LastError = operationSuccessful ? 0 : errno;
    return m_LastError;
}
