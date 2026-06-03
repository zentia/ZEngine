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

    // Texture cook (Phase 5) startup pass. Walks <Project>/Assets/ for source
    // images (.png/.jpg/.jpeg/.tga/.bmp) and, for any lacking a sibling
    // <stem>.zasset, cooks the editor-platform variant (mips + BC encode) so
    // RenderResourceBase::LoadTexture can resolve the compressed Texture2D.
    // A2 first-time seeding -- existing .zasset siblings are skipped, so this
    // is idempotent and cheap on warm restarts. Mirrors
    // ShaderImporter::ImportProjectShaders. Returns the number imported.
    static int ImportProjectTextures();

    // Texture cook (Phase 6). Walks <Project>/Assets/ for source images, cooks
    // the variant for `target` (mips + BC on desktop/WebGL, ASTC on mobile),
    // caches it in the DDC, and writes a cooked Texture2D .zasset to
    // <Project>/Intermediate/Cooked/<Platform>/<rel>.zasset. The cooked asset
    // reuses the SOURCE asset's GUID (read from the Assets/<stem>.zasset header)
    // so references resolve identically in a player build. Always re-cooks
    // (overwrites) -- this is the deliberate "build" action, not first-time
    // seeding. Returns the number of textures cooked.
    static int CookProjectTextures(TextureImporterSettings::BuildTarget target);
};