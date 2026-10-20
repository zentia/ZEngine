#pragma once

// =============================================================================
// `.zbundle` container format (PR-PAK1).
//
// ZEngine has no pak file yet, which means a player build can only be shipped
// by copying the whole `Assets/` tree next to the executable -- no dependency
// pruning, no content-only updates, no way to know what a scene actually needs.
//
// Design (UE `.pak` + Unity `AssetBundle` hybrid, leaning UE):
//
//   * A bundle stores COOKED `.zasset` FILES VERBATIM, one blob per asset.
//     Re-serialising every object into a new container would mean a second
//     binary format to keep in sync with `SerializedFile`; archiving the bytes
//     the asset pipeline already produced keeps one writer and one reader.
//     (Unity does re-serialise; UE's pak also stores cooked .uasset bytes.)
//   * Assets are addressed by their REGISTRY-RELATIVE path (forward slashes,
//     e.g. "Models/hero.zasset") -- the same key AssetRegistry uses, so the
//     runtime `AssetManager` hook can serve `loadAsset<...>()` transparently.
//   * Dependencies live in a separate manifest asset
//     (`AssetBundleManifest`, a normal `.zasset`) exactly like Unity's
//     `.manifest`: loading a bundle therefore tells you which other bundles
//     must be mounted first.
//
// Layout:
//
//   [BundleFileHeader]      fixed 64 bytes, little-endian
//   [entry table]           entry_count * length-prefixed records
//   [padding to 16 bytes]
//   [blob]                  entry_count * stored bytes, 16-byte aligned starts
//
// v1 ships STORED (uncompressed) blobs. `compression` is per-bundle and
// already plumbed through the header + entry (`uncompressed_size`) so a
// compressor can be added without a format change.
// =============================================================================

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace AssetBundles
{
    // "ZBND" read as little-endian u32.
    constexpr uint32_t kBundleMagic = 0x444E425A;
    constexpr uint16_t kBundleVersion = 1;
    constexpr uint64_t kBundleAlignment = 16;

    enum class BundleCompression : uint16_t
    {
        Stored = 0,
        // Reserved: zlib (deflate). Not produced by v1 -- no compressor is
        // linked into ZRuntime today.
        Zlib = 1,
    };

    /// Fixed prefix of every `.zbundle`. 64 bytes, 8-byte aligned fields.
    struct BundleFileHeader
    {
        uint32_t magic {kBundleMagic};
        uint16_t version {kBundleVersion};
        BundleCompression compression {BundleCompression::Stored};
        uint16_t entry_count {0};
        uint64_t entry_table_offset {0};
        uint64_t entry_table_size {0};
        uint64_t blob_offset {0};
        uint64_t blob_size {0};
        uint64_t reserved[3] {0, 0, 0};
    };

    /// One asset inside a bundle. `offset` is RELATIVE TO `blob_offset`.
    struct BundleEntry
    {
        /// Registry-relative asset path, forward slashes ("Models/hero.zasset").
        std::string path;
        /// Asset GUID from the .zasset header (may be empty for legacy files).
        std::string guid;
        /// Runtime asset type name ("MeshData", "Texture2D", ...).
        std::string type;
        uint64_t offset {0};
        uint64_t stored_size {0};
        uint64_t uncompressed_size {0};
        uint32_t crc32 {0};
        uint32_t flags {0};
    };

    /// Source material handed to the writer: entry metadata + raw asset bytes.
    struct BundleSourceEntry
    {
        std::string path;
        std::string guid;
        std::string type;
        std::vector<uint8_t> bytes;
    };

    /// CRC-32 (IEEE 802.3, reflected). No zlib dependency -- ZRuntime links
    /// none, and a bundle writer must not pull one in just for a checksum.
    uint32_t Crc32(const uint8_t* data, size_t size);

    inline uint64_t AlignUp(uint64_t value, uint64_t alignment)
    {
        return (value + alignment - 1) / alignment * alignment;
    }

    /// Writes a `.zbundle`. `entries` may be empty (an empty bundle is legal
    /// and is what "this root pulled in nothing" produces). Returns false and
    /// fills `out_error` on any I/O failure. Blobs are written in the order
    /// given; duplicate paths are rejected (an asset belongs to one bundle).
    bool WriteBundleFile(const std::filesystem::path& out_path,
                         const std::vector<BundleSourceEntry>& entries,
                         BundleCompression compression,
                         std::string& out_error);

    /// Reads header + entry table. The blob itself is NOT read -- the runtime
    /// seeks into the file per asset (see AssetBundle::GetAssetBytes), so a
    /// mounted bundle costs entry-table memory, not whole-file memory.
    bool ReadBundleHeader(const std::filesystem::path& in_path,
                          BundleFileHeader& out_header,
                          std::vector<BundleEntry>& out_entries,
                          std::string& out_error);

    /// Reads one entry's stored bytes out of an already-open bundle file.
    bool ReadBundleEntryBytes(const std::filesystem::path& in_path,
                              const BundleFileHeader& header,
                              const BundleEntry& entry,
                              std::vector<uint8_t>& out_bytes,
                              std::string& out_error);

    // -------------------------------------------------------------------------
    // Compiled script modules
    // -------------------------------------------------------------------------
    // A bundle is a path -> bytes map, and one entry table carries both kinds
    // of payload a build ships. Script modules are the second kind: they are
    // the tsc output under <project>/Intermediate/Scripts/, they are NOT
    // registry assets (so they have no registry-relative path and no GUID),
    // and the loader addresses them by MODULE ID -- "foo/Bar", no extension.
    //
    // They are therefore stored under a reserved prefix. "Intermediate/" cannot
    // collide with a registry asset because AssetRegistry indexes the content
    // directory only, and Intermediate/ is a peer of it at the project root.
    constexpr const char* kScriptEntryPrefix = "Intermediate/Scripts/";
    /// The bundle every compiled module goes into. Kept separate from `shared`
    /// so "does this build ship scripts" is one manifest lookup.
    constexpr const char* kScriptBundleName = "scripts";

    /// "foo/Bar" -> "Intermediate/Scripts/foo/Bar.js".
    /// Empty input yields an empty string (callers must check); an id that
    /// already carries the prefix is returned unchanged.
    std::string MakeScriptEntryPath(const std::string& module_id);

    /// Inverse of `MakeScriptEntryPath`: strips the prefix and the ".js"
    /// suffix. False when `entry_path` is not a script entry.
    bool ParseScriptEntryPath(const std::string& entry_path, std::string& out_module_id);
}  // namespace AssetBundles
