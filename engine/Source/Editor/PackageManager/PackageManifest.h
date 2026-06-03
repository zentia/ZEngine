#pragma once

#include "PackageTypes.h"

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>

namespace PackageManifest
{

/// Parse a package.json or catalog package entry object.
bool ParseFromFile(const std::filesystem::path& path, PackageManifestData& out, std::string& out_error);

/// Project manifest: { "dependencies": { "name": "version" } }.
bool ParseProjectManifest(const std::filesystem::path& path,
                          std::unordered_map<std::string, std::string>& out_deps,
                          std::string& out_error);

/// Engine catalog: engine/Packages/manifest.json.
bool ParseEngineCatalog(const std::filesystem::path& catalog_path,
                        std::unordered_map<std::string, PackageManifestData>& out_packages,
                        std::string& out_error);

/// True if `resolved_version` satisfies `constraint` (exact or >=x.y.z).
bool VersionSatisfies(const std::string& resolved_version, const std::string& constraint);

/// Reject registry/git specifiers early (V1).
bool IsUnsupportedSourceSpecifier(const std::string& specifier, std::string& out_reason);

}  // namespace PackageManifest
