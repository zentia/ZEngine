#pragma once

#include "Editor/AssetPipeline/AssetImporter.h"
#include "TextureImporterSettings.h"

class TextureImporter : public AssetImporter
{
public:
    bool CanImport(const std::filesystem::path& file_path) const override
    {
        auto ext = file_path.extension().string();
        std::string ext_lower = ext;
        std::transform(ext_lower.begin(), ext_lower.end(), ext_lower.begin(), ::tolower);
        return ext_lower == ".png" || ext_lower == ".jpg" || ext_lower == ".jpeg" || ext_lower == ".tag" ||
               ext_lower == ".dds";
    }

    std::vector<std::string> GetSupportedExtensions() const override;

    bool Import(const std::filesystem::path& source_path,
                const std::filesystem::path& output_path,
                const AssetImporterSettings& import_settings,
                AssetMetadata& out_metadata) override;

    bool Reimport(const std::filesystem::path& zasset_path, const AssetImporterSettings& import_settings) override;

    std::unique_ptr<AssetImporterSettings> GetDefaultSettings() const override;
};