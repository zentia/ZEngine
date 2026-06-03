#pragma once

#include "Editor/AssetPipeline/AssetImporter.h"
#include "ShaderImporterSetting.h"

namespace Runtime
{
    class ShaderImporter : public AssetImporter
    {
    public:
        bool CanImport(const std::filesystem::path& file_path) const override
        {
            auto&& ext = file_path.extension().string();
            std::string ext_lower = ext;
            std::transform(ext_lower.begin(), ext_lower.end(), ext_lower.begin(), ::towlower);
            return ext_lower == ".shader" || ext_lower == ".vert" || ext_lower == ".frag";
        }

        std::vector<std::string> GetSupportedExtensions() const override;

        bool Import(const std::filesystem::path& source_path,
                    const std::filesystem::path& output_path,
                    const AssetImporterSettings& import_settings,
                    AssetMetadata& out_metadata) override;

        bool Reimport(const std::filesystem::path& zasset_path, const AssetImporterSettings& import_settings) override;

        std::unique_ptr<AssetImporterSettings> GetDefaultSettings() const override;

        // PR-SE3b: project-level first-time import. Scans <project>/Shaders/
        // for .shader files that don't have a corresponding ShaderRes .zasset
        // under Assets/_Generated/Shaders/, parses each one via ShaderLabParser,
        // converts the result to a ShaderRes, and writes it as .zasset.
        // Returns the number of newly imported shaders.
        static int ImportProjectShaders();

        /// Warm DX12 DXIL cache for all `.shader` under Shaders/ (Win64 editor).
        static int PrecompileProjectShaderVariants();

        /// Warm DX12 DXIL cache for one `.shader` source (no-op on non-Win).
        static void PrecompileShaderVariants(const std::filesystem::path& source_shader_path);
    };
}  // namespace Runtime
