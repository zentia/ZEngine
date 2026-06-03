#pragma once

enum ArtifactPathType
{
    kArtifactPathNone,
};

ArtifactPathType GetArtifactPathType(std::filesystem::path& path);