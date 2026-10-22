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
        // ".tag" 是 ".tga" 的笔误 —— 它让拖拽导入 / AssetImportManager 分发
        // 永远认不出 .tga（而 ImportProjectTextures 用的是另一份
        // isSourceImageExtension()，那份是对的，所以只有手动导入这一路坏掉）。
        return ext_lower == ".png" || ext_lower == ".jpg" || ext_lower == ".jpeg" || ext_lower == ".tga" ||
               ext_lower == ".bmp" || ext_lower == ".dds";
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
    // so references resolve identically in a player build. Returns the number of
    // textures actually cooked.
    //
    // `only_stale` makes it incremental: a texture whose source bytes, import
    // settings and platform are unchanged AND whose cooked output is still
    // there is left alone (and does not count towards the return value). The
    // default (false) keeps the historical "re-cook everything, always"
    // behaviour, which is what the Cook menu items and `asset.cook` want --
    // they are the deliberate "redo it" action. Packaging passes true.
    //
    // The state lives in `<Cooked>/<Platform>/cook_cache.json`; deleting the
    // platform directory is a full re-cook.
    static int CookProjectTextures(TextureImporterSettings::BuildTarget target, bool only_stale = false);
};