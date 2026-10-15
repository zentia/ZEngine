#include "Collection.h"
#include "EvidenceFiles.h"
#include "miniz.h"
#include <algorithm>
#include <set>
#include <stdexcept>

namespace zengine::envcheck::collector
{
namespace
{
struct Stream
{
    EvidenceFile* file;
    const std::atomic_bool* cancel;
};
size_t Read(void* opaque, mz_uint64 offset, void* data, size_t size)
{
    auto& stream = *static_cast<Stream*>(opaque);
    if (stream.cancel && stream.cancel->load()) return 0;
    return stream.file->Read(offset, data, size);
}
size_t Write(void* opaque, mz_uint64 offset, const void* data, size_t size)
{
    auto& stream = *static_cast<Stream*>(opaque);
    if (stream.cancel && stream.cancel->load()) return 0;
    return stream.file->Write(offset, data, size);
}
void Archive(const std::filesystem::path& directory, const std::filesystem::path& output,
    const std::vector<std::string>& files, std::uint64_t maxFile, std::uint64_t maxTotal, const std::atomic_bool* cancel)
{
    if (files.empty() || files.size() > 32) throw std::runtime_error("归档文件数量不合法");
    std::set<std::string> names;
    std::uint64_t total = 0;
    for (const auto& name : files)
    {
        if (name.empty() || name.size() > 128 || name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") != std::string::npos ||
            name.find("..") != std::string::npos || !names.insert(name).second)
            throw std::runtime_error("归档文件名不合法");
        EvidenceFile source(directory / name);
        const auto size = source.Size();
        if (size > maxFile || size > maxTotal - total) throw std::runtime_error("报告超过归档上限");
        total += size;
    }
    const auto temporary = std::filesystem::path(output.wstring() + L".partial");
    bool created = false;
    try
    {
        {
            EvidenceFile destination(temporary, true); created = true;
            Stream target{&destination, cancel};
            mz_zip_archive zip{};
            zip.m_pIO_opaque = &target; zip.m_pWrite = Write;
            if (!mz_zip_writer_init_v2(&zip, 0, MZ_ZIP_FLAG_WRITE_ZIP64)) throw std::runtime_error("ZIP64初始化失败");
            try
            {
                std::uint64_t archivedBytes = 0;
                for (const auto& name : files)
                {
                    EvidenceFile source(directory / name);
                    const auto size = source.Size();
                    if (size > maxFile || size > maxTotal - archivedBytes) throw std::runtime_error("报告超过归档上限");
                    archivedBytes += size;
                    Stream input{&source, cancel};
                    if (!mz_zip_writer_add_read_buf_callback(&zip, name.c_str(), Read, &input, size,
                        nullptr, nullptr, 0, MZ_BEST_SPEED | MZ_ZIP_FLAG_WRITE_HEADER_SET_SIZE, nullptr, 0, nullptr, 0))
                        throw std::runtime_error(cancel && cancel->load() ? "打包已取消，原始证据保留" : "ZIP64文件写入失败");
                }
                if (!mz_zip_writer_finalize_archive(&zip) || !FlushFileBuffers(destination.Get()))
                    throw std::runtime_error("ZIP64目录或磁盘写入失败");
            }
            catch (...) { mz_zip_writer_end(&zip); throw; }
            mz_zip_writer_end(&zip);
            mz_zip_archive verify{};
            verify.m_pIO_opaque = &target; verify.m_pRead = Read;
            if (!mz_zip_reader_init(&verify, destination.Size(), 0)) throw std::runtime_error("ZIP64回读失败");
            const bool valid = mz_zip_validate_archive(&verify, 0) != 0;
            const std::string validationError = mz_zip_get_error_string(mz_zip_get_last_error(&verify));
            mz_zip_reader_end(&verify);
            if (!valid) throw std::runtime_error(cancel && cancel->load() ? "校验已取消，原始证据保留" : "ZIP64回读校验失败: " + validationError);
        }
        if (!MoveFileW(temporary.c_str(), output.c_str())) throw std::runtime_error("无法发布ZIP，拒绝覆盖现有文件");
        created = false;
    }
    catch (...) { if (created) { std::error_code error; std::filesystem::remove(temporary, error); } throw; }
}
}
void WriteZip(const std::filesystem::path& directory, const std::filesystem::path& output, const std::vector<std::string>& files)
{
    Archive(directory, output, files, 16ull << 20, 64ull << 20, nullptr);
}
void WriteZip64(const std::filesystem::path& directory, const std::filesystem::path& output,
                const std::vector<std::string>& files, const std::atomic_bool& cancel)
{
    Archive(directory, output, files, 64ull << 30, 128ull << 30, &cancel);
}
}
