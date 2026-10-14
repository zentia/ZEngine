#include "EvidenceFiles.h"
#include <DbgHelp.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <stdexcept>

namespace zengine::envcheck::collector
{
EvidenceFile::EvidenceFile(const std::filesystem::path& path, bool create)
{
    handle = CreateFileW(path.c_str(), create ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ,
        create ? 0 : FILE_SHARE_READ, nullptr, create ? CREATE_NEW : OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("无法独占访问证据文件，可能仍在写入或权限不足");
    FILE_ATTRIBUTE_TAG_INFO info{};
    if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &info, sizeof(info)) ||
        (info.FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)))
    {
        CloseHandle(handle); handle = INVALID_HANDLE_VALUE;
        throw std::runtime_error("证据文件不能是链接或目录");
    }
}
EvidenceFile::~EvidenceFile() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
std::uint64_t EvidenceFile::Size() const
{
    LARGE_INTEGER value{};
    if (!GetFileSizeEx(handle, &value) || value.QuadPart < 0) throw std::runtime_error("无法读取证据文件大小");
    return static_cast<std::uint64_t>(value.QuadPart);
}
size_t EvidenceFile::Read(std::uint64_t offset, void* data, size_t size) const
{
    LARGE_INTEGER position{}; position.QuadPart = static_cast<LONGLONG>(offset);
    DWORD count = 0;
    if (size > MAXDWORD || !SetFilePointerEx(handle, position, nullptr, FILE_BEGIN) ||
        !ReadFile(handle, data, static_cast<DWORD>(size), &count, nullptr)) return 0;
    return count;
}
size_t EvidenceFile::Write(std::uint64_t offset, const void* data, size_t size) const
{
    LARGE_INTEGER position{}; position.QuadPart = static_cast<LONGLONG>(offset);
    DWORD count = 0;
    if (size > MAXDWORD || !SetFilePointerEx(handle, position, nullptr, FILE_BEGIN) ||
        !WriteFile(handle, data, static_cast<DWORD>(size), &count, nullptr)) return 0;
    return count;
}
std::string Sha256File(const std::filesystem::path& path, const std::atomic_bool* cancel)
{
    EvidenceFile file(path);
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::vector<UCHAR> object;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("SHA256初始化失败");
    try
    {
        DWORD objectSize = 0, returned = 0;
        if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &returned, 0) < 0)
            throw std::runtime_error("SHA256属性读取失败");
        object.resize(objectSize);
        if (BCryptCreateHash(algorithm, &hash, object.data(), objectSize, nullptr, 0, 0) < 0)
            throw std::runtime_error("SHA256创建失败");
        std::array<unsigned char, 65536> buffer{};
        const auto length = file.Size();
        for (std::uint64_t offset = 0; offset < length;)
        {
            if (cancel && cancel->load()) throw std::runtime_error("已取消哈希计算，原始文件保留");
            auto count = static_cast<size_t>(std::min<std::uint64_t>(buffer.size(), length - offset));
            if (file.Read(offset, buffer.data(), count) != count || BCryptHashData(hash, buffer.data(), static_cast<ULONG>(count), 0) < 0)
                throw std::runtime_error("SHA256读取失败");
            offset += count;
        }
        std::array<unsigned char, 32> digest{};
        if (BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) throw std::runtime_error("SHA256结束失败");
        BCryptDestroyHash(hash); hash = nullptr;
        BCryptCloseAlgorithmProvider(algorithm, 0); algorithm = nullptr;
        std::string result;
        for (auto byte : digest) { result += "0123456789abcdef"[byte >> 4]; result += "0123456789abcdef"[byte & 15]; }
        return result;
    }
    catch (...) { if (hash) BCryptDestroyHash(hash); if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0); throw; }
}
DumpMetadata ValidateFullDump(const std::filesystem::path& path, std::uint32_t expectedPid)
{
    EvidenceFile file(path);
    const auto length = file.Size();
    auto inFile = [&](std::uint64_t offset, std::uint64_t bytes) { return offset <= length && bytes <= length - offset; };
    auto read = [&](std::uint64_t offset, void* data, size_t bytes) {
        if (!inFile(offset, bytes) || file.Read(offset, data, bytes) != bytes) throw std::runtime_error("dump不完整：流或内存范围超出文件");
    };
    MINIDUMP_HEADER header{}; read(0, &header, sizeof(header));
    if (header.Signature != MINIDUMP_SIGNATURE || (header.Version & 0xffff) != MINIDUMP_VERSION ||
        !(header.Flags & MiniDumpWithFullMemory) || header.NumberOfStreams == 0 || header.NumberOfStreams > 4096)
        throw std::runtime_error("不是有效的完整内存dump");
    if (!inFile(header.StreamDirectoryRva, static_cast<std::uint64_t>(header.NumberOfStreams) * sizeof(MINIDUMP_DIRECTORY)))
        throw std::runtime_error("dump目录截断");
    DumpMetadata result; result.fileBytes = length; result.timestamp = header.TimeDateStamp;
    bool hasMemory = false, hasException = false, hasPid = false, hasThreads = false;
    DWORD exceptionThread = 0;
    std::vector<DWORD> threadIds;
    std::uint64_t memoryBase = 0;
    std::uint64_t metadataEnd = std::max<std::uint64_t>(sizeof(header), header.StreamDirectoryRva + static_cast<std::uint64_t>(header.NumberOfStreams) * sizeof(MINIDUMP_DIRECTORY));
    for (DWORD i = 0; i < header.NumberOfStreams; ++i)
    {
        MINIDUMP_DIRECTORY entry{};
        read(header.StreamDirectoryRva + static_cast<std::uint64_t>(i) * sizeof(entry), &entry, sizeof(entry));
        if (!inFile(entry.Location.Rva, entry.Location.DataSize)) throw std::runtime_error("dump数据流截断");
        metadataEnd = std::max(metadataEnd, static_cast<std::uint64_t>(entry.Location.Rva) + entry.Location.DataSize);
        if (entry.StreamType == Memory64ListStream)
        {
            std::array<ULONG64, 2> info{};
            if (entry.Location.DataSize < sizeof(info)) throw std::runtime_error("dump内存目录缺失");
            read(entry.Location.Rva, info.data(), sizeof(info));
            if (hasMemory || info[0] == 0 || info[0] > 1000000 ||
                info[0] > (entry.Location.DataSize - sizeof(info)) / sizeof(MINIDUMP_MEMORY_DESCRIPTOR64))
                throw std::runtime_error("dump内存目录无效");
            memoryBase = info[1];
            std::uint64_t cursor = info[1];
            for (std::uint64_t j = 0; j < info[0]; ++j)
            {
                MINIDUMP_MEMORY_DESCRIPTOR64 descriptor{};
                read(entry.Location.Rva + sizeof(info) + j * sizeof(descriptor), &descriptor, sizeof(descriptor));
                if (!inFile(cursor, descriptor.DataSize)) throw std::runtime_error("完整内存dump尚未写完或已截断");
                cursor += descriptor.DataSize;
            }
            result.memoryBytes = cursor - info[1]; hasMemory = result.memoryBytes != 0;
        }
        else if (entry.StreamType == ExceptionStream)
        {
            MINIDUMP_EXCEPTION_STREAM exception{};
            if (entry.Location.DataSize < sizeof(exception)) throw std::runtime_error("dump异常信息截断");
            read(entry.Location.Rva, &exception, sizeof(exception));
            result.exceptionCode = exception.ExceptionRecord.ExceptionCode;
            exceptionThread = exception.ThreadId;
            if (!inFile(exception.ThreadContext.Rva, exception.ThreadContext.DataSize) || exception.ThreadContext.DataSize < sizeof(CONTEXT))
                throw std::runtime_error("dump异常线程上下文缺失");
            hasException = true;
        }
        else if (entry.StreamType == MiscInfoStream)
        {
            MINIDUMP_MISC_INFO info{};
            if (entry.Location.DataSize >= sizeof(info))
            {
                read(entry.Location.Rva, &info, sizeof(info));
                if (hasPid || info.SizeOfInfo < sizeof(info)) throw std::runtime_error("dump进程标识流无效");
                hasPid = (info.Flags1 & MINIDUMP_MISC1_PROCESS_ID) != 0;
                result.processId = info.ProcessId;
                if (info.Flags1 & MINIDUMP_MISC1_PROCESS_TIMES) result.processCreatedUnix = info.ProcessCreateTime;
            }
        }
        else if (entry.StreamType == ModuleListStream)
        {
            DWORD count = 0;
            if (!result.modulePaths.empty() || entry.Location.DataSize < sizeof(count)) throw std::runtime_error("dump模块列表无效");
            read(entry.Location.Rva, &count, sizeof(count));
            if (count > 4096 || count > (entry.Location.DataSize - sizeof(count)) / sizeof(MINIDUMP_MODULE)) throw std::runtime_error("dump模块数无效");
            for (DWORD j = 0; j < count; ++j)
            {
                MINIDUMP_MODULE module{};
                read(entry.Location.Rva + sizeof(count) + static_cast<std::uint64_t>(j) * sizeof(module), &module, sizeof(module));
                DWORD bytes = 0; read(module.ModuleNameRva, &bytes, sizeof(bytes));
                if (bytes == 0 || bytes > 32768 || bytes % sizeof(wchar_t)) throw std::runtime_error("dump模块名称无效");
                std::wstring name(bytes / sizeof(wchar_t), L'\0');
                read(static_cast<std::uint64_t>(module.ModuleNameRva) + sizeof(bytes), name.data(), bytes);
                if (name.find(L'\0') != std::wstring::npos) throw std::runtime_error("dump模块名称含空字符");
                result.modulePaths.push_back(std::move(name));
            }
        }
        else if (entry.StreamType == ThreadListStream)
        {
            DWORD count = 0;
            if (entry.Location.DataSize >= sizeof(count))
            {
                read(entry.Location.Rva, &count, sizeof(count));
                hasThreads = count > 0 && count <= 100000 && count <= (entry.Location.DataSize - sizeof(count)) / sizeof(MINIDUMP_THREAD);
                if (!hasThreads) throw std::runtime_error("dump线程列表无效");
                for (DWORD j = 0; j < count; ++j)
                {
                    MINIDUMP_THREAD thread{};
                    read(entry.Location.Rva + sizeof(count) + static_cast<std::uint64_t>(j) * sizeof(thread), &thread, sizeof(thread));
                    if (!inFile(thread.ThreadContext.Rva, thread.ThreadContext.DataSize) || thread.ThreadContext.DataSize < sizeof(CONTEXT) ||
                        !inFile(thread.Stack.Memory.Rva, thread.Stack.Memory.DataSize))
                        throw std::runtime_error("dump线程上下文或栈范围截断");
                    threadIds.push_back(thread.ThreadId);
                }
            }
        }
    }
    if (!hasMemory || memoryBase < metadataEnd || !hasException || !hasPid || !hasThreads || !result.processId || (expectedPid && result.processId != expectedPid) ||
        std::find(threadIds.begin(), threadIds.end(), exceptionThread) == threadIds.end())
        throw std::runtime_error("dump缺少完整内存/异常/线程信息或PID不符");
    return result;
}
}
