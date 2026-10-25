// =============================================================================
// LocalFileSystemWeb.cpp
// -----------------------------------------------------------------------------
// stdio-backed LocalFileSystem for Emscripten. Routes through Emscripten's
// virtual FS so files preloaded with `--preload-file` or mounted via IDBFS /
// FETCHFS are reachable. Operations on missing files return false instead of
// crashing.
// =============================================================================
#include "LocalFileSystemWeb.h"

#include <cstdio>
#include <string>

#include <sys/stat.h>

namespace
{
    const char* PermissionToMode(FilePermission p)
    {
        switch (p)
        {
            case kReadPermission:
                return "rb";
            case kWritePermission:
                return "wb";
            case kReadWritePermission:
                return "rb+";
            case kAppendPermission:
                return "ab";
        }
        return "rb";
    }
}  // namespace

bool LocalFileSystemWeb::Open(FileEntryData& data, FilePermission permissions, FileAutoBehavior /*behavior*/)
{
    const std::string p = data.path.string();
    std::FILE* f = std::fopen(p.c_str(), PermissionToMode(permissions));
    if (!f)
    {
        data.accessorData = nullptr;
        data.lastError = kFileSystemErrorUnknown;
        return false;
    }
    data.accessorData = f;
    data.lastError = kFileSystemErrorSuccess;
    return true;
}

bool LocalFileSystemWeb::Close(FileEntryData& data)
{
    if (auto* f = static_cast<std::FILE*>(data.accessorData))
    {
        std::fclose(f);
        data.accessorData = nullptr;
        return true;
    }
    return false;
}

bool LocalFileSystemWeb::Read(FileEntryData& data, FileSize from, uint64_t size, void* buffer, uint64_t* actual, FileReadFlags /*flags*/)
{
    auto* f = static_cast<std::FILE*>(data.accessorData);
    if (!f)
        return false;
    if (std::fseek(f, static_cast<long>(from.Cast<uint64_t>()), SEEK_SET) != 0)
        return false;
    const size_t n = std::fread(buffer, 1, static_cast<size_t>(size), f);
    if (actual)
        *actual = n;
    return n == size;
}

bool LocalFileSystemWeb::Read(FileEntryData& data, uint64_t size, void* buffer, uint64_t* actual, FileReadFlags /*flags*/)
{
    auto* f = static_cast<std::FILE*>(data.accessorData);
    if (!f)
        return false;
    const size_t n = std::fread(buffer, 1, static_cast<size_t>(size), f);
    if (actual)
        *actual = n;
    return n == size;
}

bool LocalFileSystemWeb::Write(FileEntryData& data, FileSize at, uint64_t size, const void* buffer, uint64_t* actual)
{
    auto* f = static_cast<std::FILE*>(data.accessorData);
    if (!f)
        return false;
    if (std::fseek(f, static_cast<long>(at.Cast<uint64_t>()), SEEK_SET) != 0)
        return false;
    const size_t n = std::fwrite(buffer, 1, static_cast<size_t>(size), f);
    if (actual)
        *actual = n;
    return n == size;
}

bool LocalFileSystemWeb::Write(FileEntryData& data, uint64_t size, const void* buffer, uint64_t* actual)
{
    auto* f = static_cast<std::FILE*>(data.accessorData);
    if (!f)
        return false;
    const size_t n = std::fwrite(buffer, 1, static_cast<size_t>(size), f);
    if (actual)
        *actual = n;
    return n == size;
}

FileSize LocalFileSystemWeb::Size(const FileEntryData& data) const
{
    auto* f = static_cast<std::FILE*>(data.accessorData);
    if (!f)
    {
        // Callers ask for the size of entries they have NOT opened --
        // FileCacherRead sizes its file with a bare FileSystemEntry(path).Size()
        // before any read. The Linux / Windows handlers answer that with
        // stat() / GetFileAttributesEx(); returning 0 here made every on-disk
        // .zasset fail SerializedFile's header check (fileSize > actual size)
        // on the web build.
        struct stat file_stat {};
        const std::string path_string = data.path.generic_string();
        if (::stat(path_string.c_str(), &file_stat) != 0 || !S_ISREG(file_stat.st_mode))
            return FileSize();
        return FileSize(static_cast<uint64_t>(file_stat.st_size));
    }
    const long cur = std::ftell(f);
    if (std::fseek(f, 0, SEEK_END) != 0)
        return FileSize();
    const long end = std::ftell(f);
    std::fseek(f, cur, SEEK_SET);
    if (end < 0)
        return FileSize();
    return FileSize(static_cast<uint64_t>(end));
}
