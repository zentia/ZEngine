#include "LocalFileSystemWindowsShared.h"

static inline DWORD FileFlagsToWindowsAttribtue(uint32_t flags)
{
    DWORD v = 0;
    if (flags & kFileFlagTemporary)
    {
        v |= FILE_ATTRIBUTE_TEMPORARY;
    }
    return v;
}

bool LocalFileSystemWindowsShared::Open(FileEntryData& data, FilePermission permissions, [[maybe_unused]] FileAutoBehavior behavior)
{
    DWORD accessMode, shareMode, createMode;
    switch (permissions)
    {
        case kReadPermission:
            accessMode = FILE_GENERIC_READ;
            shareMode = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
            createMode = OPEN_EXISTING;
            break;
        case kWritePermission:
            accessMode = FILE_GENERIC_WRITE;
            shareMode = 0;
            createMode = CREATE_ALWAYS;
            break;
        case kReadWritePermission:
            accessMode = FILE_GENERIC_READ | FILE_GENERIC_WRITE;
            shareMode = FILE_SHARE_READ;
            createMode = OPEN_ALWAYS;
            break;
        case kAppendPermission:
            accessMode = FILE_GENERIC_WRITE;
            shareMode = 0;
            createMode = OPEN_ALWAYS;
            break;
        default:
            return INVALID_HANDLE_VALUE;
    }
    HANDLE hFile = (HANDLE)data.accessorData;
    DWORD winValue = FileFlagsToWindowsAttribtue(data.openFlags);

    hFile = CreateFileW(data.path.wstring().c_str(), accessMode, shareMode, nullptr, createMode, winValue, nullptr);

    data.accessorData = hFile;
    data.lastError = GetLastFileSystemError(hFile != INVALID_HANDLE_VALUE);

    if (data.accessorData == INVALID_HANDLE_VALUE)
        return false;

    data.accessorHandler = this;
    return true;
}

bool LocalFileSystemWindowsShared::Close(FileEntryData& data)
{
    HANDLE hFile = static_cast<HANDLE>(data.accessorData);
    if (hFile && hFile != INVALID_HANDLE_VALUE)
    {
        BOOL success = CloseHandle(hFile);

        data.lastError = GetLastFileSystemError(success);
        data.accessorData = nullptr;
        return success;
    }
    return true;
}

bool LocalFileSystemWindowsShared::Read(FileEntryData& data, FileSize from, uint64_t count, void* buffer, uint64_t* actual, [[maybe_unused]] FileReadFlags flags)
{
    HANDLE hFile = static_cast<HANDLE>(data.accessorData);
    if (hFile == nullptr || hFile == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    LARGE_INTEGER distanceToMove;
    distanceToMove.QuadPart = from.Cast<uint64_t>();
    if (SetFilePointerEx(hFile, distanceToMove, nullptr, FILE_BEGIN) == FALSE)
    {
        data.lastError = GetLastFileSystemError(false);
        return false;
    }

    BOOL success = TRUE;
    *actual = 0;

    while (count > 0 && success)
    {
        DWORD actualRead = 0;
        DWORD toRead = std::min<uint64_t>(count, std::numeric_limits<DWORD>::max());
        success = ::ReadFile(hFile, buffer, toRead, &actualRead, nullptr);
        data.lastError = GetLastFileSystemError(success);
        buffer = static_cast<char*>(buffer) + actualRead;
        *actual += actualRead;
        count -= toRead;
    }
    return success == TRUE && *actual > 0;
}

bool LocalFileSystemWindowsShared::Read(FileEntryData& data, uint64_t count, void* buffer, uint64_t* actual, FileReadFlags flags)
{
    HANDLE hFile = (HANDLE)data.accessorData;
    if (hFile == 0 || hFile == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    BOOL success = true;

    while (count > 0 && success)
    {
        DWORD actualRead = 0;
        DWORD toRead = std::min<uint64_t>(count, std::numeric_limits<DWORD>::max());
        success = ::ReadFile(hFile, (void*)buffer, toRead, &actualRead, NULL);
        data.lastError = GetLastFileSystemError(success != FALSE);
        buffer = static_cast<char*>(buffer) + actualRead;
        *actual += actualRead;
        count -= toRead;
    }

    return success == TRUE && *actual > 0 ? true : false;
}

bool LocalFileSystemWindowsShared::Write(FileEntryData& data, FileSize at, uint64_t size, const void* buffer, uint64_t* actual)
{
    HANDLE hFile = static_cast<HANDLE>(data.accessorData);
    if (hFile == nullptr || hFile == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    LARGE_INTEGER distanceToMove;
    distanceToMove.QuadPart = at.Cast<uint64_t>();
    BOOL success = TRUE;
    while (size > 0 && success)
    {
        DWORD actualWrite = 0;
        DWORD toWriteSize = std::min<uint64_t>(size, std::numeric_limits<DWORD>::max());
        success = ::WriteFile(hFile, buffer, static_cast<DWORD>(toWriteSize), &actualWrite, nullptr);

        data.lastError = GetLastFileSystemError(success != FALSE);
        buffer = static_cast<const char*>(buffer) + actualWrite;
        size -= toWriteSize;
        *actual += actualWrite;
    }
    return true;
}

bool LocalFileSystemWindowsShared::Write(FileEntryData& data, uint64_t size, const void* buffer, uint64_t* actual)
{
    HANDLE hFile = static_cast<HANDLE>(data.accessorData);
    if (hFile == nullptr || hFile == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    BOOL success = TRUE;
    *actual = 0;
    uint64_t totalExpected = size;
    while (size > 0 && success)
    {
        DWORD actualWrite = 0;
        DWORD toWriteSize = std::min<uint64_t>(size, std::numeric_limits<DWORD>::max());
        BOOL success = ::WriteFile(hFile, buffer, static_cast<DWORD>(toWriteSize), &actualWrite, nullptr);

        data.lastError = GetLastFileSystemError(success != FALSE);
        buffer = static_cast<const char*>(buffer) + actualWrite;
        size -= toWriteSize;
        *actual += actualWrite;
    }
    return success && (*actual == totalExpected);
}

FileSystemError LocalFileSystemWindowsShared::GetLastFileSystemError(bool operationSuccessful) const
{
    DWORD error = GetAndUpdateLastError(operationSuccessful);
    return ConvertToFileSystemError(error);
}

FileSystemError LocalFileSystemWindowsShared::ConvertToFileSystemError(DWORD error)
{
    switch (error)
    {
        case ERROR_SUCCESS:
            return kFileSystemErrorSuccess;
        default:
            return kFileSystemErrorUnknown;
    }
}

DWORD LocalFileSystemWindowsShared::GetAndUpdateLastError(bool operationSuccessful) const
{
    return m_LastError = operationSuccessful ? ERROR_SUCCESS : ::GetLastError();
}

FileSize LocalFileSystemWindowsShared::Size(const FileEntryData& data) const
{
    HANDLE hFile = static_cast<HANDLE>(data.accessorData);
    if (hFile != nullptr && hFile != INVALID_HANDLE_VALUE)
    {
        LARGE_INTEGER fileSizeBytes;
        if (GetFileSizeEx(hFile, &fileSizeBytes) != 0)
            return static_cast<uint64_t>(fileSizeBytes.QuadPart);
    }

    WIN32_FILE_ATTRIBUTE_DATA fileInfo;
    bool success = GetFileAttributesExW(data.path.c_str(), GetFileExInfoStandard, &fileInfo) != 0;
    data.lastError = GetLastFileSystemError(success);
    if (!success)
        return 0ull;

    if (!(fileInfo.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
    {
        LARGE_INTEGER li;
        li.HighPart = fileInfo.nFileSizeHigh;
        li.LowPart = fileInfo.nFileSizeLow;
        return static_cast<uint64_t>(li.QuadPart);
    }

    hFile = CreateFileW(data.path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE)
    {
        LARGE_INTEGER newPos, oldPos, endPos;
        endPos.QuadPart = newPos.QuadPart = 0;
        if (SetFilePointerEx(hFile, newPos, &oldPos, FILE_CURRENT) == TRUE)
        {
            SetFilePointerEx(hFile, newPos, &endPos, FILE_END);
            SetFilePointerEx(hFile, newPos, &endPos, FILE_BEGIN);
        }
        CloseHandle(hFile);
        return static_cast<uint64_t>(endPos.QuadPart);
    }

    data.lastError = GetLastFileSystemError(false);
    return 0ull;
}