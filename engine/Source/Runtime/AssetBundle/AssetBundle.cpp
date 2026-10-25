#include "AssetBundle.h"

#include "Runtime/BaseClasses/ObjectManager.h"
#include "Runtime/BaseClasses/Type.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Resource/Asset/AssetManager.h"

#include <algorithm>
#include <filesystem>

namespace
{
    /// True when `in_candidate` ends with `in_suffix` on a path boundary, i.e.
    /// "Assets/Models/hero.zasset" matches "Models/hero.zasset" but
    /// "Assets/Models/hero.zasset.bak" does not.
    bool EndsWithPath(const std::string& in_candidate, const std::string& in_suffix)
    {
        if (in_suffix.empty() || in_candidate.size() < in_suffix.size())
        {
            return false;
        }
        if (in_candidate.compare(in_candidate.size() - in_suffix.size(), in_suffix.size(), in_suffix) != 0)
        {
            return false;
        }
        // Either an exact match or a '/' immediately before the suffix.
        return in_candidate.size() == in_suffix.size() ||
               in_candidate[in_candidate.size() - in_suffix.size() - 1] == '/' ||
               in_candidate[in_candidate.size() - in_suffix.size() - 1] == '\\';
    }
}  // namespace

bool AssetBundle::LoadFromFile(const std::filesystem::path& path, std::string& out_error)
{
    Unload();

    AssetBundles::BundleFileHeader header;
    std::vector<AssetBundles::BundleEntry> entries;
    if (!AssetBundles::ReadBundleHeader(path, header, entries, out_error))
    {
        return false;
    }

    m_Header = header;
    m_Entries = std::move(entries);
    m_EntryIndex.clear();
    for (size_t index = 0; index < m_Entries.size(); ++index)
    {
        m_EntryIndex.emplace(m_Entries[index].path, index);
    }

    m_FilePath = path;
    m_Name = path.stem().string();
    m_Loaded = true;
    return true;
}

const AssetBundles::BundleEntry* AssetBundle::FindEntry(const std::string& in_asset_path) const
{
    if (!m_Loaded || in_asset_path.empty())
    {
        return nullptr;
    }

    // Entries are stored as normalised registry keys ("Models/hero.zasset"); a
    // caller's "./Models/hero.zasset", "Models/x/../hero.zasset" or backslashed
    // spelling names the same entry. Lexical only -- the file is not on disk.
    // Backslashes are folded first: off Windows, path() treats them as filename
    // characters, but a bundle key never contains one.
    std::string asset_path = in_asset_path;
    std::replace(asset_path.begin(), asset_path.end(), '\\', '/');
    asset_path = std::filesystem::path(asset_path).lexically_normal().generic_string();

    auto found = m_EntryIndex.find(asset_path);
    if (found != m_EntryIndex.end())
    {
        return &m_Entries[found->second];
    }

    // Absolute-vs-relative mismatch: the build stores registry-relative keys
    // ("Models/hero.zasset") while a caller typically passes an absolute path.
    for (const AssetBundles::BundleEntry& entry : m_Entries)
    {
        if (EndsWithPath(asset_path, entry.path))
        {
            return &entry;
        }
    }
    return nullptr;
}

const std::vector<uint8_t>* AssetBundle::GetAssetBytes(const std::string& asset_path, std::string& out_error)
{
    const AssetBundles::BundleEntry* entry = FindEntry(asset_path);
    if (entry == nullptr)
    {
        out_error = "bundle '" + m_Name + "' does not contain " + asset_path;
        return nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(m_CacheMutex);
        auto cached = m_ByteCache.find(entry->path);
        if (cached != m_ByteCache.end())
        {
            return &cached->second;
        }
    }

    std::vector<uint8_t> bytes;
    if (!AssetBundles::ReadBundleEntryBytes(m_FilePath, m_Header, *entry, bytes, out_error))
    {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(m_CacheMutex);
    // Insert-or-return-existing: a concurrent reader may have won the race.
    auto inserted = m_ByteCache.emplace(entry->path, std::move(bytes));
    return &inserted.first->second;
}

void AssetBundle::SetDependencies(std::vector<std::string> dependencies)
{
    m_Dependencies = std::move(dependencies);
}

void AssetBundle::Unload()
{
    std::lock_guard<std::mutex> lock(m_CacheMutex);
    m_ByteCache.clear();
    m_Entries.clear();
    m_EntryIndex.clear();
    m_Dependencies.clear();
    m_Header = AssetBundles::BundleFileHeader {};
    m_Name.clear();
    m_FilePath.clear();
    m_Loaded = false;
}

Object* AssetBundle::GetImpl(const Type* type, std::filesystem::path& path)
{
    if (!m_Loaded)
    {
        LOG_ERROR(ZPak, "AssetBundle: not loaded; cannot serve {}", path.generic_string());
        return nullptr;
    }

    auto* asset_manager = GET_SYSTEM(AssetManager);
    if (asset_manager == nullptr)
    {
        return nullptr;
    }

    const std::string key = path.generic_string();
    const AssetBundles::BundleEntry* entry = FindEntry(key);
    if (entry == nullptr)
    {
        LOG_ERROR(ZPak, "AssetBundle '{}' does not contain {}", m_Name, key);
        return nullptr;
    }

    const Type* resolved_type = type;
    if (resolved_type == nullptr && !entry->type.empty())
    {
        resolved_type = TypeManager::GetInstance().ClassNameToType(entry->type.c_str());
    }
    if (resolved_type == nullptr)
    {
        LOG_ERROR(ZPak, "AssetBundle: unknown asset type for {}", entry->path);
        return nullptr;
    }

    // AssetManager re-enters SerializedFile::InitializeRead(path), which serves
    // these bytes from this bundle instead of touching the filesystem.
    return asset_manager->ReadObject(path, resolved_type);
}
