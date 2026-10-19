#pragma once

// =============================================================================
// AssetBundle -- a mounted `.zbundle` (PR-PAK1).
//
// Previously a 16-line stub whose `GetImpl()` body was `return nullptr`, so
// every bundle lookup silently produced a null asset.
//
// Responsibilities:
//   * parse the header + entry table on mount (cheap: no blob is read);
//   * serve an asset's bytes on demand, decompressed + CRC-checked + CACHED
//     for the lifetime of the bundle (SerializedFile reads through a
//     MemoryCacheReader that holds the vector by reference, so the storage
//     must not move or die while any stream is alive);
//   * hand asset loads back to AssetManager, which resolves bundle-resident
//     paths transparently (see SerializedFile::InitializeRead).
//
// It deliberately does NOT re-implement object deserialisation: duplicating
// AssetManager::ReadObject's PPtr resolver wiring would be a second copy of
// the trickiest code in the engine, and the two would drift.
// =============================================================================

#include "Runtime/AssetBundle/AssetBundleFormat.h"
#include "Runtime/BaseClasses/Object.h"

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

class AssetBundle : public Object
{
public:
    /// Parses `path`. Does not read any asset blob. Returns false (with
    /// `out_error`) on a missing file, bad magic, unsupported version or a
    /// malformed entry table.
    bool LoadFromFile(const std::filesystem::path& path, std::string& out_error);

    bool IsLoaded() const { return m_Loaded; }
    const std::string& GetName() const { return m_Name; }
    const std::filesystem::path& GetFilePath() const { return m_FilePath; }
    uint16_t GetEntryCount() const { return m_Header.entry_count; }
    uint64_t GetBlobSize() const { return m_Header.blob_size; }

    const std::vector<AssetBundles::BundleEntry>& GetEntries() const { return m_Entries; }

    /// Entry lookup by registry-relative path ("Models/hero.zasset").
    const AssetBundles::BundleEntry* FindEntry(const std::string& asset_path) const;
    bool ContainsAsset(const std::string& asset_path) const { return FindEntry(asset_path) != nullptr; }

    /// Stored bytes of one asset, CRC-verified and cached. The returned
    /// pointer is valid until `Unload()`; it is null on failure.
    const std::vector<uint8_t>* GetAssetBytes(const std::string& asset_path, std::string& out_error);

    /// Names of bundles that must be mounted before this one (from the build
    /// manifest). Empty when the bundle was mounted without a manifest.
    void SetDependencies(std::vector<std::string> dependencies);
    const std::vector<std::string>& GetDependencies() const { return m_Dependencies; }

    /// Drops the parsed table and every cached blob. Any SerializedFile still
    /// holding a reference into a cached blob is left dangling -- callers must
    /// unload assets first (AssetManager::UnloadStream per asset).
    void Unload();

    Object* GetImpl(const Type* type, std::filesystem::path& path);

    template<typename AssetType>
    AssetType* Get(const std::string& asset_path)
    {
        std::filesystem::path path(asset_path);
        return static_cast<AssetType*>(GetImpl(TypeOf<AssetType>(), path));
    }

private:
    bool m_Loaded {false};
    std::string m_Name;
    std::filesystem::path m_FilePath;
    AssetBundles::BundleFileHeader m_Header {};
    std::vector<AssetBundles::BundleEntry> m_Entries;
    std::unordered_map<std::string, size_t> m_EntryIndex;  // path -> m_Entries slot
    std::vector<std::string> m_Dependencies;

    mutable std::mutex m_CacheMutex;
    /// Lazily populated; values are heap-stable (vector contents never move
    /// once emplaced because we never erase except in Unload()).
    std::unordered_map<std::string, std::vector<uint8_t>> m_ByteCache;
};
