#pragma once

#include "Editor/AssetPipeline/AssetImporter.h"
#include "MeshImporterSettings.h"

class MeshImporter : public AssetImporter
{
public:
    bool CanImport(const std::filesystem::path& file_path) const override;

    std::vector<std::string> GetSupportedExtensions() const override;

    bool Import(const std::filesystem::path& source_path,
                const std::filesystem::path& output_path,
                const AssetImporterSettings& import_settings,
                AssetMetadata& out_metadata) override;

    bool Reimport(const std::filesystem::path& zasset_path, const AssetImporterSettings& import_settings) override;

    std::unique_ptr<AssetImporterSettings> GetDefaultSettings() const override;
};
