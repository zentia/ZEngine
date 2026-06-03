#pragma once

#include <filesystem>
#include <string>

class Path
{
public:
    static const std::filesystem::path GetRelativePath(const std::filesystem::path& directory, const std::filesystem::path& file_path);

    static const std::vector<eastl::string> GetPathSegments(const std::filesystem::path& file_path);

    static const std::tuple<std::string, std::string, std::string> GetFileExtensions(const std::filesystem::path& file_path);

    static const eastl::string GetFilePureName(const eastl::string);
};