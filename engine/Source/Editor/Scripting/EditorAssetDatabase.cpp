#include "EditorAssetDatabase.h"

#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Resource/Asset/AssetManager.h"

#include <filesystem>
#include <string>

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
