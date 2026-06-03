#pragma once

#include "AssetImporterSettings.h"
#include "Runtime/Asset/AssetFile.h"
#include "Runtime/Core/Base/EngineSystem.h"

#include <filesystem>
#include <mutex>

// Forward declaration
class EditorAssetManager;

class AssetImporter : public Object
{
    REGISTER_CLASS_TRAITS(kTypeIsAbstract);
    REGISTER_CLASS(AssetImporter);

public:
    virtual ~AssetImporter() = default;

    virtual bool CanImport(const std::filesystem::path& file_path) const = 0;

    virtual std::vector<std::string> GetSupportedExtensions() const = 0;

    virtual bool Import(const std::filesystem::path& source_path,
                        const std::filesystem::path& output_path,
                        const AssetImporterSettings& import_settings,
                        AssetMetadata& out_metadata) = 0;

    virtual bool Reimport(const std::filesystem::path& zasset_path, const AssetImporterSettings& import_settings) = 0;

    // PR-AI3: source-path-aware reimport. Used by AutoReimport (focus-driven
    // reimport queue in EditorAssetManager) where we already know the
    // original source path because SourceAssetRegistry kept it. The default
    // implementation just calls back into `import` with a fresh
    // `AssetMetadata`, which is the right thing for any importer whose
    // `Reimport(zasset_path, settings)` would otherwise be a stub: TextureImporter
    // is the canonical case (its single-arg reimport explicitly returns false
    // because it has no metadata persistence layer to recover the source path
    // from).
    //
    // Importers that already implement source-discovery on their own
    // (DataTableImporter / XlsxImporter both walk <Project>/Data/ to find
    // their CSVs/XLSX, so they do NOT need the explicit source path) MAY
    // override this if they want to bypass that discovery and force a
    // specific source -- but the default is fine for them too.
    //
    // Returns true on a successful reimport. Implementations should NOT
    // delete the original `.zasset` on failure; the caller (AutoReimport)
    // wants the previous import to remain valid in that case.
    virtual bool Reimport(const std::filesystem::path& zasset_path,
                          const std::filesystem::path& source_path,
                          const AssetImporterSettings& import_settings)
    {
        AssetMetadata md;
        return Import(source_path, zasset_path, import_settings, md);
    }

    virtual std::unique_ptr<AssetImporterSettings> GetDefaultSettings() const = 0;
};

class AssetImportManager : public IEngineSystem
{
public:
    std::string GetName() const override { return GET_CLASS_NAME(AssetImpotManager); }
    SystemInitPhase GetInitPhase() const override { return SystemInitPhase::Resource; }
    std::vector<std::type_index> GetDependencies() const override;
    bool Initialize() override { return true; }
    void Shutdown() override {}
    void RegisterImporter(std::shared_ptr<AssetImporter> importer);

    std::shared_ptr<AssetImporter> FindImporter(const std::filesystem::path& file_path) const;

    bool ImportAsset(const std::filesystem::path& source_path,
                     const std::filesystem::path& output_path,
                     const AssetImporterSettings* import_settings = nullptr);

    void importSourceAssets(const std::filesystem::path& source_folder, const std::filesystem::path& output_folder);

    bool importAssets(const std::vector<std::filesystem::path>& source_paths,
                      const std::filesystem::path& output_folder,
                      const AssetImporterSettings* import_settings = nullptr);

    std::vector<std::shared_ptr<AssetImporter>> getAllImporters() const;

private:
    std::vector<std::shared_ptr<AssetImporter>> m_Importers;
    mutable std::mutex m_Mutex;
};