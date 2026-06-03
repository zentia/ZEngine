#pragma once

// =============================================================================
// asset_file.h -- on-disk header layout for `.zasset` binary assets.
//
// Historically this header also declared an `AssetFile` class with
// `saveAsset` / `loadAsset` / `loadMetadata` / `isValidAssetFile` /
// `loadHeader` members. Those members were stubs from day one (template
// bodies were entirely commented out, `readMetadata` / `writeMetadata`
// were `return true;` no-ops, and `isValidAssetFile` / `loadHeader` had
// zero callers anywhere in the engine).
//
// Route B (see doc/BINDLESS_TEXTURE_PATH.md PR10) replaced every real
// asset-write site with `AssetManager::WriteObjectToDiskThreadSafe`, which
// goes through `SerializedFile` and produces a real binary body. The
// stubs were removed in P2 #9 to stop them showing up in code search and
// suggesting a non-existent persistence layer.
//
// What survives in this header:
//   * `AssetFileHeader`  -- the on-disk 176-byte header layout. Stamped
//     at file offset 0 by `SerializedFile::WriteHeaderAndMetadata` (the
//     canonical writer for every `.zasset`; see the P2 #6 comment block
//     in `Runtime/Core/Serialize/SerializedFile.{h,cpp}`). Read back by
//     `Editor/asset_registry/asset_registry.cpp::scanSingleAsset` via a
//     direct `ifstream::read` -- the registry only needs the prefix, so
//     it doesn't pay the cost of instantiating a SerializedFile.
//   * `AssetMetadata`    -- the in-memory contract type filled in by
//     `AssetImporter::Import(...)` overrides (texture / shader / data
//     table / xlsx importers all output through this struct). Pure
//     transient state; not written to disk by anyone today.
//
// If a real metadata persistence layer ever lands, reintroduce the
// reader/writer there -- not here -- so the on-disk-format header keeps
// describing layout only.
// =============================================================================

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

constexpr uint32_t k_zasset_magic = 0x5A415353;  // "ZASS"
constexpr uint32_t k_zasset_version = 1;

struct AssetFileHeader
{
    uint32_t magic;
    uint32_t version;
    char guid[37];
    char asset_type[64];
    uint64_t metadata_offset;
    uint64_t metadata_size;
    uint64_t data_offset;
    uint64_t data_size;
    uint64_t reserved[4];
};
static_assert(sizeof(AssetFileHeader) == 176, "AssetFileHeader must be 176 bytes");

struct AssetMetadata
{
    std::string guid;
    std::string source_file_path;
    std::filesystem::file_time_type source_file_time;
    std::vector<std::string> dependencies;
    std::unordered_map<std::string, std::string> custom_metadata;
};
