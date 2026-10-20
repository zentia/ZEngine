#include "AssetBundleFormat.h"

#include <cstdio>
#include <cstring>
#include <fstream>

namespace AssetBundles
{
    namespace
    {
        constexpr uint32_t kCrc32Polynomial = 0xEDB88320u;

        uint32_t* Crc32Table()
        {
            static uint32_t table[256] = {};
            static bool built = false;
            if (!built)
            {
                for (uint32_t index = 0; index < 256; ++index)
                {
                    uint32_t value = index;
                    for (int bit = 0; bit < 8; ++bit)
                    {
                        value = (value & 1u) ? (kCrc32Polynomial ^ (value >> 1)) : (value >> 1);
                    }
                    table[index] = value;
                }
                built = true;
            }
            return table;
        }

        void AppendU32(std::vector<uint8_t>& out, uint32_t value)
        {
            out.insert(out.end(),
                       reinterpret_cast<const uint8_t*>(&value),
                       reinterpret_cast<const uint8_t*>(&value) + sizeof(value));
        }

        void AppendU64(std::vector<uint8_t>& out, uint64_t value)
        {
            out.insert(out.end(),
                       reinterpret_cast<const uint8_t*>(&value),
                       reinterpret_cast<const uint8_t*>(&value) + sizeof(value));
        }

        void AppendString(std::vector<uint8_t>& out, const std::string& value)
        {
            AppendU32(out, static_cast<uint32_t>(value.size()));
            out.insert(out.end(), value.begin(), value.end());
        }

        bool ReadU32(const uint8_t*& cursor, const uint8_t* end, uint32_t& out)
        {
            if (end - cursor < static_cast<ptrdiff_t>(sizeof(uint32_t)))
            {
                return false;
            }
            std::memcpy(&out, cursor, sizeof(uint32_t));
            cursor += sizeof(uint32_t);
            return true;
        }

        bool ReadU64(const uint8_t*& cursor, const uint8_t* end, uint64_t& out)
        {
            if (end - cursor < static_cast<ptrdiff_t>(sizeof(uint64_t)))
            {
                return false;
            }
            std::memcpy(&out, cursor, sizeof(uint64_t));
            cursor += sizeof(uint64_t);
            return true;
        }

        bool ReadString(const uint8_t*& cursor, const uint8_t* end, std::string& out)
        {
            uint32_t length = 0;
            if (!ReadU32(cursor, end, length))
            {
                return false;
            }
            if (end - cursor < static_cast<ptrdiff_t>(length))
            {
                return false;
            }
            out.assign(reinterpret_cast<const char*>(cursor), length);
            cursor += length;
            return true;
        }
    }  // namespace

    uint32_t Crc32(const uint8_t* data, size_t size)
    {
        const uint32_t* table = Crc32Table();
        uint32_t crc = 0xFFFFFFFFu;
        for (size_t index = 0; index < size; ++index)
        {
            crc = table[(crc ^ data[index]) & 0xFFu] ^ (crc >> 8);
        }
        return crc ^ 0xFFFFFFFFu;
    }

    bool WriteBundleFile(const std::filesystem::path& out_path,
                         const std::vector<BundleSourceEntry>& entries,
                         BundleCompression compression,
                         std::string& out_error)
    {
        if (compression != BundleCompression::Stored)
        {
            out_error = "only stored (uncompressed) bundles are supported in v1";
            return false;
        }

        for (size_t i = 0; i < entries.size(); ++i)
        {
            for (size_t j = i + 1; j < entries.size(); ++j)
            {
                if (entries[i].path == entries[j].path)
                {
                    out_error = "duplicate asset path in bundle: " + entries[i].path;
                    return false;
                }
            }
        }

        // Entry table first, so the header (which points at it) can be written
        // once the blob offsets are known.
        std::vector<BundleEntry> table;
        table.reserve(entries.size());

        std::vector<uint8_t> entry_table;
        uint64_t cursor = 0;
        for (const BundleSourceEntry& source : entries)
        {
            BundleEntry entry;
            entry.path = source.path;
            entry.guid = source.guid;
            entry.type = source.type;
            entry.offset = cursor;
            entry.stored_size = source.bytes.size();
            entry.uncompressed_size = source.bytes.size();
            entry.crc32 = Crc32(source.bytes.data(), source.bytes.size());
            entry.flags = 0;

            AppendString(entry_table, entry.path);
            AppendString(entry_table, entry.guid);
            AppendString(entry_table, entry.type);
            AppendU64(entry_table, entry.offset);
            AppendU64(entry_table, entry.stored_size);
            AppendU64(entry_table, entry.uncompressed_size);
            AppendU32(entry_table, entry.crc32);
            AppendU32(entry_table, entry.flags);

            table.push_back(entry);
            cursor = AlignUp(cursor + entry.stored_size, kBundleAlignment);
        }

        BundleFileHeader header;
        header.magic = kBundleMagic;
        header.version = kBundleVersion;
        header.compression = compression;
        header.entry_count = static_cast<uint16_t>(table.size());
        header.entry_table_offset = sizeof(BundleFileHeader);
        header.entry_table_size = entry_table.size();
        header.blob_offset = AlignUp(header.entry_table_offset + header.entry_table_size, kBundleAlignment);
        header.blob_size = cursor;

        if (table.size() > 0xFFFFu)
        {
            out_error = "too many entries for one bundle";
            return false;
        }

        std::error_code ec;
        if (!out_path.parent_path().empty())
        {
            std::filesystem::create_directories(out_path.parent_path(), ec);
        }

        std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
        if (!out.is_open())
        {
            out_error = "cannot open bundle for writing: " + out_path.string();
            return false;
        }

        out.write(reinterpret_cast<const char*>(&header), sizeof(header));
        if (!entry_table.empty())
        {
            out.write(reinterpret_cast<const char*>(entry_table.data()), static_cast<std::streamsize>(entry_table.size()));
        }

        // Padding between the table and the blob.
        const uint64_t pad_end = header.blob_offset;
        const uint64_t written_so_far = sizeof(BundleFileHeader) + entry_table.size();
        for (uint64_t i = written_so_far; i < pad_end; ++i)
        {
            out.put(0);
        }

        for (size_t i = 0; i < entries.size(); ++i)
        {
            const std::vector<uint8_t>& bytes = entries[i].bytes;
            if (!bytes.empty())
            {
                out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            }
            const uint64_t next = AlignUp(table[i].offset + bytes.size(), kBundleAlignment);
            for (uint64_t pad = bytes.size(); pad < next - table[i].offset; ++pad)
            {
                out.put(0);
            }
        }

        out.flush();
        if (!out.good())
        {
            out_error = "write failed: " + out_path.string();
            return false;
        }
        return true;
    }

    bool ReadBundleHeader(const std::filesystem::path& in_path,
                          BundleFileHeader& out_header,
                          std::vector<BundleEntry>& out_entries,
                          std::string& out_error)
    {
        std::ifstream in(in_path, std::ios::binary);
        if (!in.is_open())
        {
            out_error = "cannot open bundle: " + in_path.string();
            return false;
        }

        BundleFileHeader header {};
        in.read(reinterpret_cast<char*>(&header), sizeof(header));
        if (!in.good() || header.magic != kBundleMagic)
        {
            out_error = "not a .zbundle (bad magic): " + in_path.string();
            return false;
        }
        if (header.version != kBundleVersion)
        {
            out_error = "unsupported bundle version " + std::to_string(header.version);
            return false;
        }

        std::vector<uint8_t> table(static_cast<size_t>(header.entry_table_size));
        in.seekg(static_cast<std::streamoff>(header.entry_table_offset), std::ios::beg);
        if (header.entry_table_size > 0)
        {
            in.read(reinterpret_cast<char*>(table.data()), static_cast<std::streamsize>(table.size()));
            if (!in.good())
            {
                out_error = "truncated entry table: " + in_path.string();
                return false;
            }
        }

        const uint8_t* cursor = table.data();
        const uint8_t* end = table.data() + table.size();
        std::vector<BundleEntry> entries;
        entries.reserve(header.entry_count);
        for (uint16_t i = 0; i < header.entry_count; ++i)
        {
            BundleEntry entry;
            if (!ReadString(cursor, end, entry.path) || !ReadString(cursor, end, entry.guid) ||
                !ReadString(cursor, end, entry.type) || !ReadU64(cursor, end, entry.offset) ||
                !ReadU64(cursor, end, entry.stored_size) || !ReadU64(cursor, end, entry.uncompressed_size) ||
                !ReadU32(cursor, end, entry.crc32) || !ReadU32(cursor, end, entry.flags))
            {
                out_error = "malformed entry table: " + in_path.string();
                return false;
            }
            entries.push_back(std::move(entry));
        }

        out_header = header;
        out_entries = std::move(entries);
        return true;
    }

    bool ReadBundleEntryBytes(const std::filesystem::path& in_path,
                              const BundleFileHeader& header,
                              const BundleEntry& entry,
                              std::vector<uint8_t>& out_bytes,
                              std::string& out_error)
    {
        if (header.compression != BundleCompression::Stored)
        {
            out_error = "bundle uses an unsupported compression scheme";
            return false;
        }

        std::ifstream in(in_path, std::ios::binary);
        if (!in.is_open())
        {
            out_error = "cannot open bundle: " + in_path.string();
            return false;
        }

        const std::streamoff position = static_cast<std::streamoff>(header.blob_offset + entry.offset);
        in.seekg(position, std::ios::beg);
        if (!in.good())
        {
            out_error = "cannot seek to asset blob: " + entry.path;
            return false;
        }

        out_bytes.resize(static_cast<size_t>(entry.stored_size));
        if (entry.stored_size > 0)
        {
            in.read(reinterpret_cast<char*>(out_bytes.data()), static_cast<std::streamsize>(out_bytes.size()));
            if (!in.good())
            {
                out_error = "truncated asset blob: " + entry.path;
                return false;
            }
        }

        const uint32_t checksum = Crc32(out_bytes.data(), out_bytes.size());
        if (checksum != entry.crc32)
        {
            out_error = "crc mismatch on " + entry.path + " (bundle is corrupt)";
            return false;
        }
        return true;
    }

    std::string MakeScriptEntryPath(const std::string& module_id)
    {
        if (module_id.empty())
        {
            return {};
        }

        std::string normalised;
        normalised.reserve(module_id.size());
        for (const char c : module_id)
        {
            normalised.push_back(c == '\\' ? '/' : c);
        }
        // Drop a leading '/' or "./": the entry key is relative.
        while (!normalised.empty() && (normalised.front() == '/' || normalised.front() == '.'))
        {
            normalised.erase(normalised.begin());
            if (!normalised.empty() && normalised.front() == '/')
            {
                normalised.erase(normalised.begin());
            }
        }
        if (normalised.empty())
        {
            return {};
        }

        const std::string prefix(kScriptEntryPrefix);
        if (normalised.size() >= prefix.size() && normalised.compare(0, prefix.size(), prefix) == 0)
        {
            return normalised;
        }

        std::string out = prefix;
        out += normalised;
        // The loader's module id has no extension; the entry does.
        const std::string kSuffix = ".js";
        if (out.size() < kSuffix.size() || out.compare(out.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0)
        {
            out += kSuffix;
        }
        return out;
    }

    bool ParseScriptEntryPath(const std::string& entry_path, std::string& out_module_id)
    {
        const std::string prefix(kScriptEntryPrefix);
        const std::string kSuffix = ".js";
        if (entry_path.size() <= prefix.size() + kSuffix.size())
        {
            return false;
        }
        if (entry_path.compare(0, prefix.size(), prefix) != 0)
        {
            return false;
        }
        if (entry_path.compare(entry_path.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0)
        {
            return false;
        }
        out_module_id = entry_path.substr(prefix.size(), entry_path.size() - prefix.size() - kSuffix.size());
        return !out_module_id.empty();
    }
}  // namespace AssetBundles
