#pragma once
// ============================================================================
// BranchPathsStore — port of ClientProjBranches.CSharp/Svn/BranchPathsStore.cs
//
// %USERPROFILE%\.client_proj_branches\paths.json is a bare JSON array of
// strings, shared with the C# build. The older C++ shape
// ({"paths":[...]}) is still read for backwards compatibility but is never
// written again.
// ============================================================================
#include "Paths.h"
#include "JsonConfig.h"

#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace ClientProj {
namespace BranchPathsStore {

inline std::vector<std::string> AutoScan()
{
    std::vector<std::string> found;
    std::error_code ec;
    for (char drive = 'C'; drive <= 'Z'; ++drive)
    {
        const std::string candidate = std::string(1, drive) + ":\\OSGame\\Client_proj";
        if (std::filesystem::exists(candidate, ec)) found.push_back(candidate);
    }
    return found;
}

inline std::vector<std::string> Load()
{
    std::vector<std::string> paths;

    for (const auto& value : Json::LoadArrayFromFile(Paths::BranchPaths()))
    {
        if (value && value->IsString()) paths.push_back(value->AsString());
    }

    if (paths.empty())
    {
        // Legacy shape written by earlier C++ builds.
        const Json::Object legacy = Json::LoadFromFile(Paths::BranchPaths());
        if (const Json::Array* arr = Json::OptArray(legacy, "paths"))
        {
            for (const auto& value : *arr)
            {
                if (value && value->IsString()) paths.push_back(value->AsString());
            }
        }
    }

    return paths;
}

inline void Save(const std::vector<std::string>& paths)
{
    Json::Array array;
    array.reserve(paths.size());
    for (const std::string& path : paths)
    {
        array.push_back(std::make_shared<Json::Value>(path));
    }
    Json::SaveArrayToFile(Paths::BranchPaths(), array);
}

// Paths actually worth scanning: configured directories that still exist,
// falling back to an automatic drive scan when nothing is configured.
inline std::vector<std::string> CandidatePaths()
{
    std::vector<std::string> result;
    std::error_code ec;
    for (const std::string& path : Load())
    {
        if (std::filesystem::is_directory(path, ec)) result.push_back(path);
    }
    if (result.empty()) result = AutoScan();
    return result;
}

} // namespace BranchPathsStore
} // namespace ClientProj
