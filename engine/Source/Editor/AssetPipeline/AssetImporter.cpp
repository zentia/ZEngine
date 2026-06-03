#include "AssetImporter.h"

#include "Editor/EditorAsset/EditorAssetManager.h"

#include <algorithm>
#include <mutex>

IMPLEMENT_REGISTER_CLASS(AssetImporter)

std::vector<std::type_index> AssetImportManager::GetDependencies() const
{
    return {GET_SYSTEM_TYPE(EditorAssetManager)};
}

void AssetImportManager::RegisterImporter(std::shared_ptr<AssetImporter> importer)
{
    if (!importer)
        return;

    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Importers.push_back(importer);
}

std::shared_ptr<AssetImporter> AssetImportManager::FindImporter(const std::filesystem::path& file_path) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    for (const auto& importer : m_Importers)
    {
        if (importer && importer->CanImport(file_path))
        {
            return importer;
        }
    }

    return nullptr;
}

bool AssetImportManager::ImportAsset(const std::filesystem::path& source_path,
                                     const std::filesystem::path& output_path,
                                     const AssetImporterSettings* import_settings)
{
    // Check if source file exists
    if (!std::filesystem::exists(source_path))
    {
        return false;
    }

    // Find appropriate importer
    auto importer = FindImporter(source_path);
    if (!importer)
    {
        return false;
    }

    // Create output directory if it doesn't exist
    std::filesystem::path output_dir = output_path.parent_path();
    if (!output_dir.empty() && !std::filesystem::exists(output_dir))
    {
        try
        {
            std::filesystem::create_directories(output_dir);
        }
        catch (...)
        {
            return false;
        }
    }

    // Get default settings if none provided
    std::unique_ptr<AssetImporterSettings> default_settings;
    const AssetImporterSettings* settings_to_use = import_settings;

    if (!settings_to_use)
    {
        default_settings = importer->GetDefaultSettings();
        if (!default_settings)
        {
            return false;
        }
        settings_to_use = default_settings.get();
    }

    // Create metadata object
    AssetMetadata metadata;

    // Call importer's import method
    bool success = importer->Import(source_path, output_path, *settings_to_use, metadata);

    return success;
}