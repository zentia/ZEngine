#pragma once

#include <filesystem>

enum ArtifactPathType
{
    kArtifactPathNone,
};

ArtifactPathType GetArtifactPathType(std::filesystem::path& path);