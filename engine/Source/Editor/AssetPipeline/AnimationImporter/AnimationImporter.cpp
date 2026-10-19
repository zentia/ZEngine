#include "AnimationImporter.h"

#include "Editor/AssetPipeline/FbxAnimation.h"
#include "Editor/EditorAsset/EditorAssetManager.h"
#include "Runtime/Core/Base/Macro.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <random>
#include <sstream>
#include <string>

#if defined(ZENGINE_HAS_UFBX)

    #include <ufbx.h>

#endif

namespace
{
    std::string ToLowerExtension(const std::filesystem::path& in_path)
    {
        std::string ext = in_path.extension().generic_string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return ext;
    }

    /// Same shape as MeshImporter's generator. TODO(PR-SK2): share one
    /// implementation -- duplicated GUID code must not drift apart.
    std::string GenerateGuid()
    {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, 15);
        std::stringstream ss;
        ss << std::hex;
        for (int i = 0; i < 8; ++i)
        {
            ss << dis(gen);
        }
        ss << "-";
        for (int i = 0; i < 4; ++i)
        {
            ss << dis(gen);
        }
        ss << "-4";
        for (int i = 0; i < 3; ++i)
        {
            ss << dis(gen);
        }
        ss << "-a";
        for (int i = 0; i < 3; ++i)
        {
            ss << dis(gen);
        }
        ss << "-";
        for (int i = 0; i < 12; ++i)
        {
            ss << dis(gen);
        }
        return ss.str();
    }
}  // namespace

bool AnimationImporter::CanImport(const std::filesystem::path& file_path) const
{
    return ToLowerExtension(file_path) == ".fbx";
}

std::vector<std::string> AnimationImporter::GetSupportedExtensions() const
{
    return {".fbx"};
}

bool AnimationImporter::Import(const std::filesystem::path& source_path,
                               const std::filesystem::path& output_path,
                               const AssetImporterSettings& import_settings,
                               AssetMetadata& out_metadata)
{
    if (!std::filesystem::exists(source_path))
    {
        LOG_ERROR(ZEditor, "AnimationImporter: source does not exist: {}", source_path.string());
        return false;
    }

    const auto* anim_settings = dynamic_cast<const AnimationImporterSettings*>(&import_settings);
    if (anim_settings == nullptr)
    {
        LOG_ERROR(ZEditor, "AnimationImporter: invalid import settings type");
        return false;
    }

    if (ToLowerExtension(source_path) != ".fbx")
    {
        LOG_ERROR(ZEditor, "AnimationImporter: {} is not supported (FBX only for now)", source_path.string());
        return false;
    }

#if !defined(ZENGINE_HAS_UFBX)

    LOG_ERROR(ZEditor, "AnimationImporter: FBX animation import requires ufbx");
    return false;

#else

    ufbx_load_opts opts = {};
    ufbx_error err = {};
    ufbx_scene* scene = ufbx_load_file(source_path.generic_string().c_str(), &opts, &err);
    if (scene == nullptr)
    {
        char buf[512] = {};
        ufbx_format_error(buf, sizeof(buf), &err);
        LOG_ERROR(ZEditor, "AnimationImporter: failed to load {}: {}", source_path.string(), buf);
        return false;
    }

    // Strip ".zasset": every clip produced from this source shares the base name.
    std::filesystem::path base = output_path;
    base.replace_extension();

    const int written = FbxAnimation::BakeAllStacks(scene, base, anim_settings->sample_rate, anim_settings->max_frames);
    ufbx_free_scene(scene);

    if (written == 0)
    {
        LOG_ERROR(ZEditor, "AnimationImporter: no animation stack could be baked from {}", source_path.string());
        return false;
    }

    out_metadata.guid = (import_settings.generate_guid && !import_settings.custom_guid.empty())
                            ? import_settings.custom_guid
                            : GenerateGuid();
    out_metadata.source_file_path = source_path.generic_string();
    {
        std::error_code ec;
        out_metadata.source_file_time = std::filesystem::last_write_time(source_path, ec);
    }
    out_metadata.dependencies.clear();
    out_metadata.custom_metadata.clear();

    LOG_INFO(ZEditor, "AnimationImporter: wrote {} clip(s) from {}", written, source_path.generic_string());
    return true;

#endif
}

bool AnimationImporter::Reimport(const std::filesystem::path& zasset_path, const AssetImporterSettings& import_settings)
{
    if (auto* editor_mgr = dynamic_cast<EditorAssetManager*>(GET_SYSTEM(AssetManager)))
    {
        return editor_mgr->reimportAsset(zasset_path.generic_string(), &import_settings);
    }

    LOG_WARNING(ZEditor, "AnimationImporter::Reimport({}): EditorAssetManager unavailable", zasset_path.string());
    return false;
}

std::unique_ptr<AssetImporterSettings> AnimationImporter::GetDefaultSettings() const
{
    return std::make_unique<AnimationImporterSettings>();
}
