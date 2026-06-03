#pragma once
#include "Runtime/Utility/NonCopyable.h"

#include <stdint.h>

enum FileFlags
{
    kFileFlagNone = 0,
    kFileFlagTemporary = 1 << 0,
    kFileFlagHidden = 1 << 2,
    kFileFlagNoBackup = 1 << 3,
};
enum FilePermission
{
    kReadPermission = 0,
    kWritePermission = 1,
    kReadWritePermission = 2,
    kAppendPermission = 3
};
enum FileAutoBehavior
{
    kNormalBehavior = 0,
    kSilentReturnOnOpenFail = 1 << 0,
    kRetryOnOpenFail = 1 << 1
};
enum FileSystemError
{
    kFileSystemErrorSuccess = 0,
    kFileSystemErrorUnknown,
};

class FileAccessorHandler;

enum FileReadFlags
{
    kFileReadNoFlags = 0,
};

template<typename T>
struct FileSizeTemplate
{
public:
    using ValueType = T;
    FileSizeTemplate()
        : value(0) {}
    FileSizeTemplate(const FileSizeTemplate<T>& s)
        : value(s.value) {}
    inline explicit FileSizeTemplate(const T& s)
        : value(s) {}

    template<typename T1>
    inline T1 Cast() const
    {
        return static_cast<T1>(value);
    }

    inline FileSizeTemplate<T>& operator+=(const FileSizeTemplate<T>& delta)
    {
        value += delta.value;
        return *this;
    }

    template<typename T1, typename = typename std::enable_if<std::is_same<T1, T>::value>::type>
    inline FileSizeTemplate<T>& operator+=(const T1& rhs)
    {
        value += rhs;
        return *this;
    }
    inline FileSizeTemplate<T> operator+(const FileSizeTemplate<T>& rhv) const { return FileSizeTemplate<T>(value + rhv.value); }
    inline FileSizeTemplate<T> operator-(const FileSizeTemplate<T>& rhv) const { return FileSizeTemplate<T>(value - rhv.value); }

    inline FileSizeTemplate<T> operator+(const T& rhv) const { return FileSizeTemplate<T>(value + rhv); }
    inline FileSizeTemplate<T> operator-(const T& rhv) const { return FileSizeTemplate<T>(value - rhv); }

    inline FileSizeTemplate<T> operator/(uint64_t div) const { return FileSizeTemplate<T>(value / div); }
    inline uint64_t operator/(FileSizeTemplate<T> div) const { return value / div.value; }

protected:
    ValueType value;
};

class FileSize : public FileSizeTemplate<uint64_t>
{
public:
    template<typename T, typename = typename std::enable_if<std::is_same<T, uint64_t>::value>::type>
    inline FileSize(T t)
        : FileSizeTemplate<uint64_t>(t)
    {
    }
    inline FileSize()
        : FileSizeTemplate<uint64_t>(0) {}
    inline FileSize(const FileSizeTemplate<uint64_t> v)
        : FileSizeTemplate<uint64_t>(v.Cast<uint64_t>()) {}
};

struct FileEntryData
{
    std::filesystem::path path;
    FileAccessorHandler* accessorHandler;
    union
    {
        void* accessorData;
    };
    FileFlags openFlags;
    mutable FileSystemError lastError;
};

class FileAccessorHandler : NonCopyable
{
public:
    virtual bool Open(FileEntryData& data, FilePermission permission, FileAutoBehavior behavior) = 0;
    virtual bool Close(FileEntryData& data) = 0;
    virtual bool Read(FileEntryData& data, FileSize from, uint64_t count, void* buffer, uint64_t* actual, FileReadFlags flags) = 0;
    virtual bool Read(FileEntryData& data, uint64_t size, void* buffer, uint64_t* actual, FileReadFlags flags) = 0;
    virtual bool Write(FileEntryData& data, FileSize at, uint64_t size, const void* buffer, uint64_t* actual) = 0;
    virtual bool Write(FileEntryData& data, uint64_t size, const void* buffer, uint64_t* actual) = 0;
    virtual FileSize Size(const FileEntryData& data) const = 0;
};

class FileSystemHandler : public FileAccessorHandler
{
public:
    virtual void InitializeFileEntry(FileEntryData& data) = 0;
};