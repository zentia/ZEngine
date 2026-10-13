#pragma once
#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace zengine::envcheck::collector
{
class EvidenceFile
{
public:
    explicit EvidenceFile(const std::filesystem::path& path, bool create = false);
    ~EvidenceFile();
    EvidenceFile(const EvidenceFile&) = delete;
    EvidenceFile& operator=(const EvidenceFile&) = delete;
    HANDLE Get() const { return handle; }
    std::uint64_t Size() const;
    size_t Read(std::uint64_t offset, void* data, size_t size) const;
    size_t Write(std::uint64_t offset, const void* data, size_t size) const;
private:
    HANDLE handle = INVALID_HANDLE_VALUE;
};
std::string Sha256File(const std::filesystem::path& file, const std::atomic_bool* cancel = nullptr);
void WriteZip64(const std::filesystem::path& directory, const std::filesystem::path& output,
                const std::vector<std::string>& files, const std::atomic_bool& cancel);
struct DumpMetadata
{
    std::uint32_t processId = 0;
    std::uint32_t exceptionCode = 0;
    std::uint32_t timestamp = 0;
    std::uint64_t memoryBytes = 0;
    std::uint64_t fileBytes = 0;
};
DumpMetadata ValidateFullDump(const std::filesystem::path& file, std::uint32_t expectedPid);
}
