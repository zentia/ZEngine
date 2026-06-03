#pragma once

#include "PackageResolver.h"
#include "PackageTypes.h"

#include "Runtime/Core/Base/EngineSystem.h"

#include <filesystem>
#include <mutex>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <vector>

class ProjectInfo;

/**
 * @brief Editor-only package manager (Unity UPM client analogue).
 *
 * Resolves <Project>/Packages/manifest.json against the engine built-in catalog
 * and optional embedded packages. Writes packages-lock.json and a CMake fragment
 * under Intermediate/Packages/.
 */
class PackageManager : public IEngineSystem
{
public:
    std::string GetName() const override { return "PackageManager"; }
    SystemInitPhase GetInitPhase() const override { return SystemInitPhase::Resource; }
    std::vector<std::type_index> GetDependencies() const override;
    bool Initialize() override;
    void Shutdown() override;

    /// Re-run resolution. Returns false on error (details logged).
    bool Resolve(bool force = false);

    const ResolvedPackage* FindPackage(const std::string& name) const;
    std::vector<const ResolvedPackage*> GetResolvedPackages() const;
    std::filesystem::path GetPackageRoot(const std::string& name) const;

    std::filesystem::path GetProjectManifestPath() const;
    std::filesystem::path GetProjectLockPath() const;

    /// Empty when the last Resolve() succeeded.
    std::string GetLastResolveError() const;

private:
    static std::filesystem::path FindEngineRoot();

    bool NeedsResolve() const;
    bool LoadProjectDependencies(std::unordered_map<std::string, std::string>& out_deps,
                                 std::string& out_error) const;

    mutable std::mutex m_Mutex;
    std::unordered_map<std::string, ResolvedPackage> m_Resolved;
    std::filesystem::path m_EngineRoot;
    std::filesystem::path m_ProjectRoot;
    std::filesystem::path m_ProjectManifest;
    std::filesystem::path m_ProjectLock;
    std::filesystem::path m_CMakeFragment;
    std::string m_LastResolveError;
};
