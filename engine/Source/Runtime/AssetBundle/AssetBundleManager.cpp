#include "AssetBundleManager.h"

#include "Runtime/Core/Memory/MemoryManager.h"
#include "Runtime/Resource/Asset/AssetManager.h"

#include <algorithm>

namespace
{
    /// `.../Build/Standalone/Bundles/foo.zbundle` -> "foo"
    std::string BundleNameFromPath(const std::filesystem::path& bundle_path)
    {
        return bundle_path.stem().string();
    }
}  // namespace

bool AssetBundleManager::LoadManifest(const std::filesystem::path& manifest_path)
{
    auto* asset_manager = GET_SYSTEM(AssetManager);
    if (asset_manager == nullptr)
    {
        LOG_ERROR(ZPak, "AssetBundleManager: AssetManager unavailable");
        return false;
    }

    auto* manifest = asset_manager->loadAsset<AssetBundleManifest>(manifest_path.generic_string().c_str());
    if (manifest == nullptr)
    {
        LOG_ERROR(ZPak, "AssetBundleManager: cannot read manifest {}", manifest_path.generic_string());
        return false;
    }

    m_Manifest = manifest;
    LOG_INFO(ZPak,
             "AssetBundleManager: manifest {} loaded ({} bundles, platform {})",
             manifest_path.generic_string(),
             manifest->bundles.size(),
             manifest->platform.c_str());
    return true;
}

bool AssetBundleManager::MountBundle(const std::filesystem::path& bundle_path)
{
    std::vector<std::string> visited;
    return MountBundleInternal(bundle_path, visited);
}

bool AssetBundleManager::MountBundleInternal(const std::filesystem::path& bundle_path,
                                             std::vector<std::string>& in_out_visited)
{
    const std::string name = BundleNameFromPath(bundle_path);

    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (m_Bundles.find(name) != m_Bundles.end())
        {
            return true;  // already mounted
        }
    }

    if (std::find(in_out_visited.begin(), in_out_visited.end(), name) != in_out_visited.end())
    {
        LOG_ERROR(ZPak, "AssetBundleManager: dependency cycle at bundle '{}'", name);
        return false;
    }
    in_out_visited.push_back(name);

    // Dependencies first: an asset inside this bundle may PPtr into another
    // one, and those refs resolve by path at read time.
    if (m_Manifest != nullptr)
    {
        if (const AssetBundleInfo* info = m_Manifest->FindBundle(name.c_str()))
        {
            for (const eastl::string& dependency : info->dependencies)
            {
                const std::filesystem::path dependency_path =
                    bundle_path.parent_path() / (std::string(dependency.c_str()) + ".zbundle");
                if (!MountBundleInternal(dependency_path, in_out_visited))
                {
                    LOG_ERROR(ZPak, "AssetBundleManager: bundle '{}' needs missing bundle '{}'", name, dependency.c_str());
                    return false;
                }
            }
        }
    }

    AssetBundle* bundle = MemoryManager::CreateObject<AssetBundle>();
    std::string error;
    if (!bundle->LoadFromFile(bundle_path, error))
    {
        MemoryManager::DestroyObject(bundle);
        LOG_ERROR(ZPak, "AssetBundleManager: {}", error);
        return false;
    }

    if (m_Manifest != nullptr)
    {
        if (const AssetBundleInfo* info = m_Manifest->FindBundle(name.c_str()))
        {
            std::vector<std::string> dependencies;
            dependencies.reserve(info->dependencies.size());
            for (const eastl::string& dependency : info->dependencies)
            {
                dependencies.push_back(dependency.c_str());
            }
            bundle->SetDependencies(std::move(dependencies));
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Bundles.emplace(name, bundle);
    }

    LOG_INFO(ZPak, "AssetBundleManager: mounted '{}' ({} assets) from {}", name, bundle->GetEntryCount(), bundle_path.generic_string());
    return true;
}

int AssetBundleManager::MountDirectory(const std::filesystem::path& dir, const std::filesystem::path& manifest_path)
{
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec))
    {
        LOG_ERROR(ZPak, "AssetBundleManager: bundle directory does not exist: {}", dir.generic_string());
        return 0;
    }

    if (!manifest_path.empty() && !LoadManifest(manifest_path))
    {
        return 0;
    }

    int mounted = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
    {
        if (!entry.is_regular_file() || entry.path().extension() != ".zbundle")
        {
            continue;
        }
        if (MountBundle(entry.path()))
        {
            ++mounted;
        }
    }
    return mounted;
}

bool AssetBundleManager::Unmount(const std::string& bundle_name)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    auto found = m_Bundles.find(bundle_name);
    if (found == m_Bundles.end())
    {
        return false;
    }

    found->second->Unload();
    MemoryManager::DestroyObject(found->second);
    m_Bundles.erase(found);
    LOG_INFO(ZPak, "AssetBundleManager: unmounted '{}'", bundle_name);
    return true;
}

void AssetBundleManager::UnmountAll()
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    for (auto& [name, bundle] : m_Bundles)
    {
        bundle->Unload();
        MemoryManager::DestroyObject(bundle);
    }
    m_Bundles.clear();
    m_Manifest = nullptr;
}

bool AssetBundleManager::IsMounted(const std::string& bundle_name) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_Bundles.find(bundle_name) != m_Bundles.end();
}

AssetBundle* AssetBundleManager::FindBundle(const std::string& bundle_name) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    auto found = m_Bundles.find(bundle_name);
    return found == m_Bundles.end() ? nullptr : found->second;
}

std::vector<std::string> AssetBundleManager::GetMountedBundleNames() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    std::vector<std::string> names;
    names.reserve(m_Bundles.size());
    for (const auto& [name, bundle] : m_Bundles)
    {
        names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    return names;
}

bool AssetBundleManager::ContainsAsset(const std::string& asset_path) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    for (const auto& [name, bundle] : m_Bundles)
    {
        if (bundle->ContainsAsset(asset_path))
        {
            return true;
        }
    }
    return false;
}

bool AssetBundleManager::TryReadAssetBytes(const std::filesystem::path& asset_path, std::vector<uint8_t>& out_bytes) const
{
    const std::string key = asset_path.generic_string();

    std::lock_guard<std::mutex> lock(m_Mutex);
    for (const auto& [name, bundle] : m_Bundles)
    {
        std::string error;
        const std::vector<uint8_t>* bytes = bundle->GetAssetBytes(key, error);
        if (bytes == nullptr)
        {
            continue;
        }
        out_bytes = *bytes;
        return true;
    }
    return false;
}
