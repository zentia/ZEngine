#include "FontImporter.h"

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Core/Memory/MemoryManager.h"
#include "Runtime/UI/Core/Font.h"
#include "Runtime/Project/ProjectInfo.h"
#include "Runtime/BaseClasses/ObjectManager.h"
#include "Runtime/Resource/Asset/AssetManager.h"
#include "core/Log/LogSystem.h"

#include "Editor/EditorAsset/EditorAssetManager.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <random>
#include <sstream>

namespace
{
    std::string ToLowerExtension(std::string ext)
    {
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return ext;
    }

    std::string GenerateGuid()
    {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, 15);
        std::uniform_int_distribution<> dis2(8, 11);

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
        ss << "-";
        ss << dis2(gen);
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

    eastl::string MakeSourceRelPath(const std::filesystem::path& source_path)
    {
        if (const auto project_info = GET_SYSTEM(ProjectInfo))
        {
            std::error_code ec;
            const auto rel = std::filesystem::relative(source_path, project_info->GetProjectRoot(), ec);
            if (!ec)
            {
                return eastl::string(rel.generic_string().c_str());
            }
        }
        return eastl::string(source_path.generic_string().c_str());
    }
}  // namespace

bool FontImporter::CanImport(const std::filesystem::path& file_path) const
{
    const std::string ext = ToLowerExtension(file_path.extension().string());
    return ext == ".ttf" || ext == ".otf" || ext == ".ttc";
}

std::vector<std::string> FontImporter::GetSupportedExtensions() const
{
    return {".ttf", ".otf", ".ttc"};
}

bool FontImporter::Import(const std::filesystem::path& source_path,
                          const std::filesystem::path& output_path,
                          const AssetImporterSettings& import_settings,
                          AssetMetadata& out_metadata)
{
    if (!std::filesystem::exists(source_path))
    {
        LOG_ERROR(ZEditor, "FontImporter: source does not exist: {}", source_path.generic_string());
        return false;
    }

    const FontImporterSettings* font_settings = dynamic_cast<const FontImporterSettings*>(&import_settings);
    if (font_settings == nullptr)
    {
        LOG_ERROR(ZEditor, "FontImporter: invalid import settings type");
        return false;
    }

    auto* object_manager = GET_SYSTEM(ObjectManager).get();
    if (object_manager == nullptr)
    {
        LOG_ERROR(ZEditor, "FontImporter: ObjectManager unavailable");
        return false;
    }

    Object* produced = object_manager->Produce(TypeOf<Font>(), /*instanceID=*/0);
    if (produced == nullptr)
    {
        LOG_ERROR(ZEditor, "FontImporter: failed to allocate Font");
        return false;
    }

    auto* font = static_cast<Font*>(produced);
    font->m_SourceRelPath = MakeSourceRelPath(source_path);
    font->m_DefaultSize = font_settings->default_size > 0 ? font_settings->default_size : 16;

    const auto asset_manager = GET_SYSTEM(AssetManager);
    if (asset_manager == nullptr)
    {
        MemoryManager::DestroyObject(produced);
        LOG_ERROR(ZEditor, "FontImporter: AssetManager unavailable");
        return false;
    }

    {
        std::error_code ec;
        if (!output_path.parent_path().empty())
        {
            std::filesystem::create_directories(output_path.parent_path(), ec);
        }
    }

    const int imported_default_size = font->m_DefaultSize;
    const bool ok = asset_manager->WriteObjectToDiskThreadSafe(output_path, *font);

    if (import_settings.generate_guid && !import_settings.custom_guid.empty())
    {
        out_metadata.guid = import_settings.custom_guid;
    }
    else
    {
        out_metadata.guid = GenerateGuid();
    }
    out_metadata.source_file_path = source_path.generic_string();
    {
        std::error_code ec;
        out_metadata.source_file_time = std::filesystem::last_write_time(source_path, ec);
    }
    out_metadata.dependencies.clear();
    out_metadata.custom_metadata.clear();

    MemoryManager::DestroyObject(produced);

    if (ok)
    {
        LOG_INFO(ZEditor,
                 "FontImporter: imported {} -> {} (default_size={})",
                 source_path.generic_string(),
                 output_path.generic_string(),
                 imported_default_size);
    }
    else
    {
        LOG_ERROR(ZEditor, "FontImporter: failed to write zasset: {}", output_path.generic_string());
    }

    return ok;
}

bool FontImporter::Reimport(const std::filesystem::path& zasset_path, const AssetImporterSettings& import_settings)
{
    if (auto editor_mgr = std::dynamic_pointer_cast<EditorAssetManager>(GET_SYSTEM(AssetManager)))
    {
        return editor_mgr->reimportAsset(zasset_path.generic_string(), &import_settings);
    }
    return false;
}

std::unique_ptr<AssetImporterSettings> FontImporter::GetDefaultSettings() const
{
    return std::make_unique<FontImporterSettings>();
}
