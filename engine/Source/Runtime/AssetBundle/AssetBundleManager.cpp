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

    /// True when `candidate` ends with `suffix` on a path boundary, so
    /// "Assets/Models/hero.zasset" matches "Models/hero.zasset" (a caller
    /// typically holds an absolute path while the bundle stores the
    /// registry-relative key). Kept here rather than shared with
    /// AssetBundle.cpp's copy so neither header has to expose it.
    bool EndsWithPath(const std::string& candidate, const std::string& suffix)
    {
        if (suffix.empty() || candidate.size() < suffix.size())
        {
            return false;
        }
        if (candidate.compare(candidate.size() - suffix.size(), suffix.size(), suffix) != 0)
        {
            return false;
        }
        if (candidate.size() == suffix.size())
        {
            return true;
        }
        const char before = candidate[candidate.size() - suffix.size() - 1];
        return before == '/' || before == '\\';
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
        IndexBundleGuidsLocked(*bundle);
        if (m_BundleDirectory.empty())
        {
            m_BundleDirectory = bundle_path.parent_path();
        }
    }

    LOG_INFO(ZPak, "AssetBundleManager: mounted '{}' ({} assets) from {}", name, bundle->GetEntryCount(), bundle_path.generic_string());
    return true;
}

void AssetBundleManager::IndexBundleGuidsLocked(const AssetBundle& bundle)
{
    for (const AssetBundles::BundleEntry& entry : bundle.GetEntries())
    {
        if (entry.guid.empty())
        {
            continue;
        }
        // First mount wins: an asset belongs to exactly one bundle (the build
        // rejects duplicates), so a collision means a stale bundle is mounted
        // alongside a fresh one, and the already-mounted copy is the one every
        // reader has been served from.
        m_GuidToPath.emplace(entry.guid, entry.path);
    }
}

void AssetBundleManager::RebuildGuidIndexLocked()
{
    m_GuidToPath.clear();
    for (const auto& [name, bundle] : m_Bundles)
    {
        IndexBundleGuidsLocked(*bundle);
    }
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

bool AssetBundleManager::FindPackagedBuild(const std::filesystem::path& build_root,
                                          std::filesystem::path& out_bundles_dir,
                                          std::filesystem::path& out_manifest_path) const
{
    std::error_code ec;
    if (build_root.empty() || !std::filesystem::is_directory(build_root, ec))
    {
        return false;
    }

    std::vector<std::filesystem::path> platform_dirs;
    for (const auto& entry : std::filesystem::directory_iterator(build_root, ec))
    {
        if (entry.is_directory(ec))
        {
            platform_dirs.push_back(entry.path());
        }
    }
    // Sorted so the same tree always yields the same answer; a shipped build
    // carries exactly one platform directory anyway.
    std::sort(platform_dirs.begin(), platform_dirs.end());

    for (const std::filesystem::path& platform_dir : platform_dirs)
    {
        const std::filesystem::path bundles_dir = platform_dir / "Bundles";
        if (!std::filesystem::is_directory(bundles_dir, ec))
        {
            continue;
        }

        for (const auto& entry : std::filesystem::directory_iterator(platform_dir, ec))
        {
            if (!entry.is_regular_file(ec))
            {
                continue;
            }
            const std::filesystem::path candidate = entry.path();
            // "<project>.manifest.zasset": the manifest sits NEXT TO Bundles/,
            // not inside it.
            if (candidate.extension() == ".zasset" && candidate.stem().extension() == ".manifest")
            {
                out_bundles_dir = bundles_dir;
                out_manifest_path = candidate;
                return true;
            }
        }
    }
    return false;
}

int AssetBundleManager::MountBuild(const std::filesystem::path& build_root)
{
    std::filesystem::path bundles_dir;
    std::filesystem::path manifest_path;
    if (!FindPackagedBuild(build_root, bundles_dir, manifest_path))
    {
        LOG_INFO(ZPak,
                 "AssetBundleManager: no packaged content under {}; running from the source tree",
                 build_root.generic_string());
        return 0;
    }

    LOG_INFO(ZPak, "AssetBundleManager: mounting packaged content from {}", bundles_dir.generic_string());
    const int mounted = MountDirectory(bundles_dir, manifest_path);
    LOG_INFO(ZPak, "AssetBundleManager: {} bundle(s) mounted", mounted);
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
    RebuildGuidIndexLocked();
    if (m_Bundles.empty())
    {
        m_BundleDirectory.clear();
    }
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
    m_GuidToPath.clear();
    m_BundleDirectory.clear();
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

bool AssetBundleManager::TryResolveGuid(const std::string& guid, std::string& out_path) const
{
    if (guid.empty())
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_Mutex);
    const auto found = m_GuidToPath.find(guid);
    if (found == m_GuidToPath.end())
    {
        return false;
    }
    out_path = found->second;
    return true;
}

bool AssetBundleManager::EnsureBundleMountedForAsset(const std::string& asset_path)
{
    if (asset_path.empty())
    {
        return false;
    }
    if (ContainsAsset(asset_path))
    {
        return true;
    }

    std::string bundle_name;
    std::filesystem::path directory;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (m_Manifest == nullptr || m_BundleDirectory.empty())
        {
            return false;
        }
        directory = m_BundleDirectory;

        for (const AssetBundleInfo& info : m_Manifest->bundles)
        {
            for (const eastl::string& candidate : info.asset_paths)
            {
                if (EndsWithPath(asset_path, candidate.c_str()))
                {
                    bundle_name = info.name.c_str();
                    break;
                }
            }
            if (!bundle_name.empty())
            {
                break;
            }
        }
    }

    if (bundle_name.empty())
    {
        return false;
    }
    if (IsMounted(bundle_name))
    {
        return ContainsAsset(asset_path);
    }

    LOG_INFO(ZPak, "AssetBundleManager: '{}' needs bundle '{}'; mounting it now", asset_path, bundle_name);
    if (!MountBundle(directory / (bundle_name + ".zbundle")))
    {
        return false;
    }
    return ContainsAsset(asset_path);
}

bool AssetBundleManager::TryReadScriptModule(const std::string& module_id, std::string& out_source)
{
    const std::string entry_path = AssetBundles::MakeScriptEntryPath(module_id);
    if (entry_path.empty())
    {
        return false;
    }

    // A script bundle is only mounted on demand: it is nobody's dependency in
    // the asset sense, so a player that loaded no level yet has no reason to
    // have it open.
    EnsureBundleMountedForAsset(entry_path);

    std::vector<uint8_t> bytes;
    if (!TryReadAssetBytes(entry_path, bytes))
    {
        return false;
    }

    out_source.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return true;
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
