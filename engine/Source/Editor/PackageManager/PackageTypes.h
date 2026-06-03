#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

/// How a package was resolved (Unity PackageSource subset).
enum class PackageSource
{
    Unknown,
    Builtin,   ///< engine/Packages/manifest.json
    Embedded,  ///< <Project>/Packages/<name>/package.json
    File,      ///< file: relative path in project manifest
    Registry,  ///< reserved; V1 rejects
    Git        ///< reserved; V1 rejects
};

inline const char* PackageSourceToString(PackageSource s)
{
    switch (s)
    {
        case PackageSource::Builtin:
            return "builtin";
        case PackageSource::Embedded:
            return "embedded";
        case PackageSource::File:
            return "file";
        case PackageSource::Registry:
            return "registry";
        case PackageSource::Git:
            return "git";
        default:
            return "unknown";
    }
}

/// Parsed fields from a single package.json (or catalog entry).
struct PackageManifestData
{
    std::string name;
    std::string version;
    std::string display_name;
    std::string description;
    std::unordered_map<std::string, std::string> dependencies;
    std::unordered_map<std::string, std::string> optional_dependencies;
    /// Module name -> relative path inside package root (CMake hint).
    std::unordered_map<std::string, std::string> modules;
};

/// One node in the resolved dependency graph (lock file + runtime lookup).
struct ResolvedPackage
{
    std::string name;
    std::string version;
    int depth {0};
    PackageSource source {PackageSource::Unknown};
    std::filesystem::path root_path;  ///< absolute package root on disk
    std::unordered_map<std::string, std::string> dependencies;  ///< name -> constraint string
    PackageManifestData manifest;
};
