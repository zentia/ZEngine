#include "PackageManager.h"

#include "PackageManifest.h"
#include "PackageScaffoldTemplates.h"

#include "Runtime/Core/Base/EngineSystem.h"
#include "Runtime/Project/ProjectInfo.h"
#include "core/Log/LogSystem.h"

#include <chrono>
#include <cstdlib>
#include <fstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace
{

    bool HasEnginePackageCatalog(const std::filesystem::path& root)
    {
        if (root.empty())
        {
            return false;
        }
        return std::filesystem::exists(root / "engine" / "Packages" / "manifest.json");
    }

    int64_t FileMTimeNs(const std::filesystem::path& p)
    {
        std::error_code ec;
        const auto ft = std::filesystem::last_write_time(p, ec);
        if (ec)
        {
            return 0;
        }
        const auto sctp = std::chrono::time_point_cast<std::chrono::nanoseconds>(
            ft - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
        return sctp.time_since_epoch().count();
    }

    std::filesystem::path FindEngineRootFromExecutable()
    {
#ifdef _WIN32
        char exe_path[MAX_PATH] = {};
        const DWORD len = GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
        if (len == 0 || len >= MAX_PATH)
        {
            return {};
        }
        std::filesystem::path cur = std::filesystem::path(exe_path).parent_path();
        for (int i = 0; i < 10 && !cur.empty(); ++i)
        {
            if (HasEnginePackageCatalog(cur))
            {
                return cur;
            }
            const auto parent = cur.parent_path();
            if (parent == cur)
            {
                break;
            }
            cur = parent;
        }
#endif
        return {};
    }

}  // namespace

std::vector<std::type_index> PackageManager::GetDependencies() const
{
    return {GET_SYSTEM_TYPE(ProjectInfo)};
}

std::filesystem::path PackageManager::FindEngineRoot()
{
    if (const char* root = std::getenv("ZENGINE_ROOT"))
    {
        if (root[0] != '\0')
        {
            const std::filesystem::path env_root(root);
            if (HasEnginePackageCatalog(env_root))
            {
                return env_root;
            }
            // Some setups point ZENGINE_ROOT at the engine/ subfolder.
            const std::filesystem::path parent = env_root.parent_path();
            if (HasEnginePackageCatalog(parent))
            {
                return parent;
            }
            LOG_WARNING(ZPackage,
                        "ZENGINE_ROOT={} has no engine/Packages/manifest.json; "
                        "falling back to executable search",
                        env_root.generic_string());
        }
    }

    if (const std::filesystem::path from_exe = FindEngineRootFromExecutable(); !from_exe.empty())
    {
        return from_exe;
    }

    std::error_code ec;
    std::filesystem::path cur = std::filesystem::current_path(ec);
    for (int i = 0; i < 12 && !cur.empty(); ++i)
    {
        if (HasEnginePackageCatalog(cur))
        {
            return cur;
        }
        const auto parent = cur.parent_path();
        if (parent == cur)
        {
            break;
        }
        cur = parent;
    }

    return {};
}

bool PackageManager::Initialize()
{
    const auto project = GET_SYSTEM(ProjectInfo);
    if (project == nullptr || project->project_path.empty())
    {
        return true;
    }

    m_ProjectRoot = project->project_path;
    m_ProjectManifest = PackageResolver::GetProjectManifestPath(m_ProjectRoot);
    m_ProjectLock = PackageResolver::GetProjectLockPath(m_ProjectRoot);

    const std::string interm =
        project->intermediate_dir.empty() ? "Intermediate" : project->intermediate_dir.c_str();
    m_CMakeFragment = m_ProjectRoot / interm / "Packages" / "zpackages.cmake";

    m_EngineRoot = FindEngineRoot();
    if (m_EngineRoot.empty())
    {
        LOG_WARNING(ZPackage,
                    "PackageManager: ZENGINE_ROOT not set and engine catalog not found; "
                    "skipping package resolve");
        return true;
    }

    if (!Resolve(true))
    {
        // Degraded mode: UGUI and other built-ins are still linked via CMake.
        // Do not fail SystemRegistry startup (LOG_FATAL) on resolve errors.
        LOG_ERROR(ZPackage,
                  "PackageManager: resolve failed; editor continues without ZPM graph");
        return true;
    }
    return true;
}

void PackageManager::Shutdown()
{
    std::lock_guard<std::mutex> lk(m_Mutex);
    m_Resolved.clear();
    m_ProjectRoot.clear();
    m_EngineRoot.clear();
}

bool PackageManager::NeedsResolve() const
{
    if (!std::filesystem::exists(m_ProjectManifest))
    {
        return false;
    }
    if (!std::filesystem::exists(m_ProjectLock))
    {
        return true;
    }
    return FileMTimeNs(m_ProjectManifest) > FileMTimeNs(m_ProjectLock);
}

bool PackageManager::LoadProjectDependencies(std::unordered_map<std::string, std::string>& out_deps,
                                             std::string& out_error) const
{
    if (!std::filesystem::exists(m_ProjectManifest))
    {
        out_deps.clear();
        return true;
    }
    return PackageManifest::ParseProjectManifest(m_ProjectManifest, out_deps, out_error);
}

bool PackageManager::Resolve(bool force)
{
    if (m_ProjectRoot.empty() || m_EngineRoot.empty())
    {
        return true;
    }

    if (!force && !NeedsResolve())
    {
        // Lock file exists and is newer than manifest - still load into memory if empty.
        std::lock_guard<std::mutex> lk(m_Mutex);
        if (!m_Resolved.empty())
        {
            return true;
        }
    }

    std::unordered_map<std::string, std::string> deps;
    std::string err;
    if (!LoadProjectDependencies(deps, err))
    {
        m_LastResolveError = err;
        LOG_ERROR(ZPackage, "PackageManager: failed to read project manifest: {}", err);
        return false;
    }

    if (deps.empty())
    {
        m_LastResolveError.clear();
        LOG_INFO(ZPackage, "PackageManager: no dependencies in {}", m_ProjectManifest.generic_string());
        return true;
    }

    PackageResolver resolver;
    PackageResolver::Result result = resolver.Resolve(m_EngineRoot, m_ProjectRoot, deps);
    if (!result.success)
    {
        m_LastResolveError = result.error_message;
        LOG_ERROR(ZPackage, "PackageManager: resolve failed: {}", result.error_message);
        return false;
    }
    m_LastResolveError.clear();

    std::filesystem::create_directories(m_ProjectLock.parent_path());
    if (!PackageResolver::WriteLockFile(m_ProjectLock, result))
    {
        LOG_WARNING(ZPackage, "PackageManager: failed to write {}", m_ProjectLock.generic_string());
    }
    if (!PackageResolver::WriteCMakeFragment(m_CMakeFragment, result))
    {
        LOG_WARNING(ZPackage,
                    "PackageManager: failed to write {}",
                    m_CMakeFragment.generic_string());
    }

    {
        std::lock_guard<std::mutex> lk(m_Mutex);
        m_Resolved = std::move(result.packages);
    }

    for (const auto& kv : result.packages)
    {
        const ResolvedPackage& p = kv.second;
        LOG_INFO(ZPackage,
                 "resolved {}@{} [{}] {}",
                 p.name,
                 p.version,
                 PackageSourceToString(p.source),
                 p.root_path.generic_string());
    }

    return true;
}

const ResolvedPackage* PackageManager::FindPackage(const std::string& name) const
{
    std::lock_guard<std::mutex> lk(m_Mutex);
    auto it = m_Resolved.find(name);
    if (it == m_Resolved.end())
    {
        return nullptr;
    }
    return &it->second;
}

std::vector<const ResolvedPackage*> PackageManager::GetResolvedPackages() const
{
    std::lock_guard<std::mutex> lk(m_Mutex);
    std::vector<const ResolvedPackage*> out;
    out.reserve(m_Resolved.size());
    for (const auto& kv : m_Resolved)
    {
        out.push_back(&kv.second);
    }
    return out;
}

std::filesystem::path PackageManager::GetPackageRoot(const std::string& name) const
{
    const ResolvedPackage* p = FindPackage(name);
    if (p == nullptr)
    {
        return {};
    }
    return p->root_path;
}

std::filesystem::path PackageManager::GetProjectManifestPath() const
{
    return m_ProjectManifest;
}

std::filesystem::path PackageManager::GetProjectLockPath() const
{
    return m_ProjectLock;
}

std::string PackageManager::GetLastResolveError() const
{
    return m_LastResolveError;
}
