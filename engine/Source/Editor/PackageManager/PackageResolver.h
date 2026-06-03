#pragma once

#include "PackageTypes.h"

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

/// Resolves a project dependency graph (Unity project:resolve-packages analogue).
class PackageResolver
{
public:
    struct Result
    {
        bool success {false};
        std::string error_message;
        std::unordered_map<std::string, ResolvedPackage> packages;
    };

    /// @param engine_root  ZENGINE_ROOT (repo root containing engine/).
    /// @param project_root Open .zproject directory.
    Result Resolve(const std::filesystem::path& engine_root,
                   const std::filesystem::path& project_root,
                   const std::unordered_map<std::string, std::string>& project_dependencies);

    static std::filesystem::path GetEngineCatalogPath(const std::filesystem::path& engine_root);
    static std::filesystem::path GetProjectPackagesDir(const std::filesystem::path& project_root);
    static std::filesystem::path GetProjectManifestPath(const std::filesystem::path& project_root);
    static std::filesystem::path GetProjectLockPath(const std::filesystem::path& project_root);

    static bool WriteLockFile(const std::filesystem::path& lock_path, const Result& result);
    static bool WriteCMakeFragment(const std::filesystem::path& cmake_path, const Result& result);

private:
    std::filesystem::path m_EngineRoot;
    std::filesystem::path m_ProjectRoot;
    std::filesystem::path m_PackagesDir;
    std::unordered_map<std::string, PackageManifestData> m_EngineCatalog;

    bool LoadEngineCatalog(std::string& out_error);

    bool ResolveOne(const std::string& name,
                    const std::string& constraint,
                    int depth,
                    std::unordered_map<std::string, ResolvedPackage>& resolved,
                    std::vector<std::string>& stack,
                    std::string& out_error);

    bool LocatePackage(const std::string& name,
                       const std::string& constraint,
                       ResolvedPackage& out_pkg,
                       std::string& out_error) const;
};
