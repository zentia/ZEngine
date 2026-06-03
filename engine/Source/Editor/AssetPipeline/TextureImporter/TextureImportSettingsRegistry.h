#pragma once

#include "TextureImporterSettings.h"

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

// Persists per-texture import settings at
// `<Project>/AssetRegistry/texture_import_settings.json` (VCS-checked-in,
// same policy as source_registry.json / script_registry.json).
class TextureImportSettingsRegistry
{
public:
    bool Initialize();
    void Shutdown();

    std::filesystem::path GetRegistryFilePath() const;

    TextureImporterSettings GetOrCreate(const std::filesystem::path& zasset_abs_path);
    std::optional<TextureImporterSettings> Lookup(const std::filesystem::path& zasset_abs_path) const;
    void Set(const std::filesystem::path& zasset_abs_path, const TextureImporterSettings& settings);
    void RemoveEntry(const std::filesystem::path& zasset_abs_path);

private:
    static std::string NormaliseKey(const std::filesystem::path& zasset_abs_path);

    bool LoadFromDisk();
    bool SaveToDisk() const;

    std::unordered_map<std::string, TextureImporterSettings> m_Entries;
    std::filesystem::path m_RegistryFile;
    mutable std::mutex m_Mutex;
};
