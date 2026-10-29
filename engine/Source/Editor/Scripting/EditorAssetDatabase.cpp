#include "EditorAssetDatabase.h"

#include "Editor/AssetRegistry/AssetRegistry.h"
#include "Editor/EditorAsset/EditorAssetManager.h"
#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Resource/Asset/AssetManager.h"

#include <filesystem>
#include <string>
#include <vector>

namespace
{
    eastl::string ToEastl(const std::string& text)
    {
        return eastl::string(text.c_str(), text.size());
    }

    /// Resolved absolute path, or an empty path when there is no AssetManager
    /// (which is the case before the editor systems are up).
    std::filesystem::path Resolve(const eastl::string& asset_path)
    {
        if (asset_path.empty())
            return {};
        auto assets = GET_SYSTEM(AssetManager);
        if (!assets)
            return {};
        return assets->GetFullPath(asset_path);
    }
}  // namespace

eastl::string EditorAssetDatabase::GetFullPath(const eastl::string& asset_path)
{
    const std::filesystem::path resolved = Resolve(asset_path);
    if (resolved.empty())
        return {};
    return ToEastl(resolved.generic_string());
}

bool EditorAssetDatabase::Exists(const eastl::string& asset_path)
{
    const std::filesystem::path resolved = Resolve(asset_path);
    if (resolved.empty())
        return false;
    std::error_code ec;
    return std::filesystem::exists(resolved, ec);
}

eastl::string EditorAssetDatabase::GetGuid(const eastl::string& asset_path)
{
    const std::filesystem::path resolved = Resolve(asset_path);
    if (resolved.empty())
        return {};
    auto assets = GET_SYSTEM(AssetManager);
    if (!assets)
        return {};
    std::string guid;
    std::string type;
    assets->GetAssetGuidAndType(resolved, guid, type);
    return ToEastl(guid);
}

eastl::string EditorAssetDatabase::GetAssetType(const eastl::string& asset_path)
{
    const std::filesystem::path resolved = Resolve(asset_path);
    if (resolved.empty())
        return {};
    auto assets = GET_SYSTEM(AssetManager);
    if (!assets)
        return {};
    std::string guid;
    std::string type;
    assets->GetAssetGuidAndType(resolved, guid, type);
    return ToEastl(type);
}

eastl::string EditorAssetDatabase::GetPathForGuid(const eastl::string& guid)
{
    if (guid.empty())
        return {};
    auto assets = GET_SYSTEM(AssetManager);
    if (!assets)
        return {};
    std::filesystem::path path;
    if (!assets->TryGetPathForGuid(std::string(guid.c_str(), guid.size()), path))
        return {};
    return ToEastl(path.generic_string());
}

// ---- search (P4) -----------------------------------------------------------

namespace
{
    /// Runs the registry query, or returns empty when there is no registry.
    /// The registry only exists in an editor build (EditorAssetManager); the
    /// base AssetManager has no index to search, which is why this down-casts
    /// rather than adding a virtual that could never work at runtime.
    std::vector<AssetIndexEntry> QueryRegistry(const eastl::string& path_prefix,
                                               const eastl::string& asset_type,
                                               const eastl::string& name_substring)
    {
        auto* editor_assets = dynamic_cast<EditorAssetManager*>(GET_SYSTEM(AssetManager));
        if (editor_assets == nullptr)
            return {};

        AssetRegistryQueryFilter filter;
        filter.package_path_prefix = std::string(path_prefix.c_str(), path_prefix.size());
        filter.asset_type = std::string(asset_type.c_str(), asset_type.size());
        filter.name_substring = std::string(name_substring.c_str(), name_substring.size());
        return editor_assets->getAssetRegistry().FindAssets(filter);
    }
}  // namespace

std::vector<eastl::string> EditorAssetDatabase::FindAssets(const eastl::string& path_prefix,
                                                           const eastl::string& asset_type,
                                                           const eastl::string& name_substring)
{
    const std::vector<AssetIndexEntry> entries = QueryRegistry(path_prefix, asset_type, name_substring);
    std::vector<eastl::string> out;
    out.reserve(entries.size());
    for (const AssetIndexEntry& entry : entries)
    {
        out.push_back(ToEastl(entry.asset_path));
    }
    return out;
}

int EditorAssetDatabase::CountAssets(const eastl::string& path_prefix,
                                     const eastl::string& asset_type,
                                     const eastl::string& name_substring)
{
    return static_cast<int>(QueryRegistry(path_prefix, asset_type, name_substring).size());
}
