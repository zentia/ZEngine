#include "Collection.h"
#include "DiagnosisUtil.h"
#include "EvidenceFiles.h"

// minizip-ng's headers are not self-contained: mz_zip_rw.h uses mz_stream_*_cb
// from mz_strm.h and the MZ_* result codes from mz.h without including either,
// so they have to come first (same order upstream's own minizip.c uses).
#include "mz.h"
#include "mz_strm.h"
#include "mz_zip.h"
#include "mz_zip_rw.h"

#include <algorithm>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace zengine::envcheck::collector
{
namespace
{
// A source file being streamed into an entry.
//
// minizip-ng reads an entry sequentially, so the offset lives in the struct
// instead of being passed per call (miniz passed it in). `cancelled` is what
// makes a cancel visible to the caller: minizip-ng treats a 0-byte read as a
// normal end-of-stream, so a cancelled read would otherwise produce a silently
// truncated entry -- which for crash evidence is worse than failing.
struct Source
{
    EvidenceFile* file = nullptr;
    std::uint64_t offset = 0;
    const std::atomic_bool* cancel = nullptr;
    bool cancelled = false;
};

int32_t Read(void* opaque, void* data, int32_t size)
{
    auto& source = *static_cast<Source*>(opaque);
    if (source.cancelled) return 0;
    if (source.cancel && source.cancel->load()) { source.cancelled = true; return 0; }
    if (size <= 0) return 0;
    const auto read = source.file->Read(source.offset, data, static_cast<size_t>(size));
    source.offset += read;
    return static_cast<int32_t>(read);
}

// minizip-ng owns the archive file itself -- it needs seek/tell to write and
// then re-read the central directory -- so the .partial is opened by path
// rather than through EvidenceFile. That costs us EvidenceFile's CREATE_NEW,
// hence the explicit refusal below: a stale .partial means an earlier run died
// and its evidence is still on disk, so we must not silently overwrite it.
// (The real "do not overwrite the published report" guarantee is still
// MoveFileW() at the end, which fails if the destination already exists.)
void FlushToDisk(const std::filesystem::path& file)
{
    const HANDLE handle = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("ZIP64目录或磁盘写入失败");
    const bool flushed = FlushFileBuffers(handle) != FALSE;
    CloseHandle(handle);
    if (!flushed) throw std::runtime_error("ZIP64目录或磁盘写入失败");
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
    std::error_code existsError;
    if (std::filesystem::exists(temporary, existsError)) throw std::runtime_error("无法独占访问证据文件，可能仍在写入或权限不足");
    const std::string temporaryPath = diagnosis::WideToUtf8(temporary.wstring());
    bool created = false;
    try
    {
        {
            void* writer = mz_zip_writer_create();
            if (!writer) throw std::runtime_error("ZIP64初始化失败");
            // Every exit path must release the writer, otherwise the .partial
            // stays locked and the cleanup below cannot remove it.
            struct Writer
            {
                void* handle;
                ~Writer() { mz_zip_writer_delete(&handle); }
            } owner{writer};

            mz_zip_writer_set_compress_method(writer, MZ_COMPRESS_METHOD_DEFLATE);
            mz_zip_writer_set_compress_level(writer, 1); // fastest; was MZ_BEST_SPEED under miniz
            if (mz_zip_writer_open_file(writer, temporaryPath.c_str(), 0, 0) != MZ_OK)
                throw std::runtime_error("ZIP64初始化失败");
            created = true;
            std::uint64_t archivedBytes = 0;
            for (const auto& name : files)
            {
                EvidenceFile source(directory / name);
                const auto size = source.Size();
                if (size > maxFile || size > maxTotal - archivedBytes) throw std::runtime_error("报告超过归档上限");
                archivedBytes += size;
                Source input{&source, 0, cancel, false};
                mz_zip_file info{};
                info.filename = name.c_str();
                info.compression_method = MZ_COMPRESS_METHOD_DEFLATE;
                info.zip64 = MZ_ZIP64_FORCE; // always ZIP64, as MZ_ZIP_FLAG_WRITE_ZIP64 did
                if (mz_zip_writer_add_info(writer, &input, Read, &info) != MZ_OK || input.cancelled)
                    throw std::runtime_error(cancel && cancel->load() ? "打包已取消，原始证据保留" : "ZIP64文件写入失败");
            }
            if (mz_zip_writer_close(writer) != MZ_OK) throw std::runtime_error("ZIP64目录或磁盘写入失败");
        }
        FlushToDisk(temporary);
        {
            void* reader = mz_zip_reader_create();
            if (!reader) throw std::runtime_error("ZIP64回读失败");
            struct Reader
            {
                void* handle;
                ~Reader() { mz_zip_reader_delete(&handle); }
            } owner{reader};

            if (mz_zip_reader_open_file(reader, temporaryPath.c_str()) != MZ_OK)
                throw std::runtime_error("ZIP64回读失败");
            std::set<std::string> seen;
            std::vector<char> buffer(64 * 1024);
            for (int32_t entry = mz_zip_reader_goto_first_entry(reader); entry == MZ_OK;
                 entry = mz_zip_reader_goto_next_entry(reader))
            {
                if (cancel && cancel->load()) throw std::runtime_error("校验已取消，原始证据保留");
                mz_zip_file* info = nullptr;
                if (mz_zip_reader_entry_get_info(reader, &info) != MZ_OK || !info || !info->filename ||
                    !seen.insert(std::string(info->filename)).second)
                    throw std::runtime_error("ZIP64回读校验失败");
                if (mz_zip_reader_entry_open(reader) != MZ_OK) throw std::runtime_error("ZIP64回读校验失败");
                // Read the entry to the end: mz_zip_reader_entry_close() is
                // what compares the CRC32 and returns MZ_CRC_ERROR.
                int32_t read = 0;
                do
                {
                    if (cancel && cancel->load()) throw std::runtime_error("校验已取消，原始证据保留");
                    read = mz_zip_reader_entry_read(reader, buffer.data(), static_cast<int32_t>(buffer.size()));
                } while (read > 0);
                if (read < 0 && read != MZ_END_OF_STREAM) throw std::runtime_error("ZIP64回读校验失败");
                if (mz_zip_reader_entry_close(reader) != MZ_OK) throw std::runtime_error("ZIP64回读校验失败: 内容校验不匹配");
            }
            if (seen != names) throw std::runtime_error("ZIP64回读校验失败: 条目不一致");
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
