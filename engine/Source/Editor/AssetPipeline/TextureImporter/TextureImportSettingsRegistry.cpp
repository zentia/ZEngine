#include "TextureImportSettingsRegistry.h"

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Project/ProjectInfo.h"
#include "rapidjson/document.h"
#include "rapidjson/istreamwrapper.h"
#include "rapidjson/prettywriter.h"
#include "rapidjson/stringbuffer.h"

#include <algorithm>
#include <fstream>

namespace
{
    std::string normalisePath(const std::filesystem::path& p)
    {
        std::string s = p.generic_string();
#ifdef _WIN32
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
        return s;
    }
}  // namespace

bool TextureImportSettingsRegistry::Initialize()
{
    ProjectInfo* project_info = GET_SYSTEM(ProjectInfo);
    if (project_info == nullptr)
    {
        return true;
    }
    const std::filesystem::path registry_root = project_info->GetAssetRegistryRoot();
    if (registry_root.empty())
    {
        return true;
    }
    m_RegistryFile = registry_root / "texture_import_settings.json";
    return LoadFromDisk();
}

void TextureImportSettingsRegistry::Shutdown()
{
    SaveToDisk();
    std::lock_guard<std::mutex> lk(m_Mutex);
    m_Entries.clear();
    m_RegistryFile.clear();
}

std::filesystem::path TextureImportSettingsRegistry::GetRegistryFilePath() const
{
    return m_RegistryFile;
}

TextureImporterSettings
TextureImportSettingsRegistry::GetOrCreate(const std::filesystem::path& zasset_abs_path)
{
    if (zasset_abs_path.empty())
    {
        TextureImporterSettings defaults;
        return defaults;
    }

    const std::string key = NormaliseKey(zasset_abs_path);
    {
        std::lock_guard<std::mutex> lk(m_Mutex);
        auto it = m_Entries.find(key);
        if (it != m_Entries.end())
        {
            return it->second;
        }
    }

    TextureImporterSettings created;
    Set(zasset_abs_path, created);
    return created;
}

std::optional<TextureImporterSettings>
TextureImportSettingsRegistry::Lookup(const std::filesystem::path& zasset_abs_path) const
{
    if (zasset_abs_path.empty())
    {
        return std::nullopt;
    }
    std::lock_guard<std::mutex> lk(m_Mutex);
    auto it = m_Entries.find(NormaliseKey(zasset_abs_path));
    if (it == m_Entries.end())
    {
        return std::nullopt;
    }
    return it->second;
}

void TextureImportSettingsRegistry::Set(const std::filesystem::path& zasset_abs_path,
                                        const TextureImporterSettings& settings)
{
    if (zasset_abs_path.empty())
    {
        return;
    }
    {
        std::lock_guard<std::mutex> lk(m_Mutex);
        m_Entries[NormaliseKey(zasset_abs_path)] = settings;
    }
    SaveToDisk();
}

void TextureImportSettingsRegistry::RemoveEntry(const std::filesystem::path& zasset_abs_path)
{
    bool changed = false;
    {
        std::lock_guard<std::mutex> lk(m_Mutex);
        auto it = m_Entries.find(NormaliseKey(zasset_abs_path));
        if (it != m_Entries.end())
        {
            m_Entries.erase(it);
            changed = true;
        }
    }
    if (changed)
    {
        SaveToDisk();
    }
}

std::string TextureImportSettingsRegistry::NormaliseKey(const std::filesystem::path& zasset_abs_path)
{
    return normalisePath(zasset_abs_path);
}

bool TextureImportSettingsRegistry::LoadFromDisk()
{
    std::lock_guard<std::mutex> lk(m_Mutex);
    m_Entries.clear();

    if (m_RegistryFile.empty() || !std::filesystem::exists(m_RegistryFile))
    {
        return true;
    }

    std::ifstream ifs(m_RegistryFile);
    if (!ifs.is_open())
    {
        LOG_WARNING(ZEditor, "TextureImportSettingsRegistry: cannot open {}", m_RegistryFile.generic_string());
        return false;
    }

    rapidjson::IStreamWrapper wrapper(ifs);
    rapidjson::Document doc;
    doc.ParseStream(wrapper);
    if (doc.HasParseError() || !doc.IsObject())
    {
        LOG_WARNING(ZEditor, "TextureImportSettingsRegistry: JSON parse error in {}", m_RegistryFile.generic_string());
        return false;
    }

    if (!doc.HasMember("entries") || !doc["entries"].IsArray())
    {
        return true;
    }

    for (const auto& entry_val : doc["entries"].GetArray())
    {
        if (!entry_val.IsObject() || !entry_val.HasMember("zasset") || !entry_val["zasset"].IsString())
        {
            continue;
        }
        const std::string zasset_key = entry_val["zasset"].GetString();
        if (zasset_key.empty())
        {
            continue;
        }

        TextureImporterSettings settings;
        if (entry_val.HasMember("settings") && entry_val["settings"].IsObject())
        {
            rapidjson::StringBuffer buffer;
            rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
            entry_val["settings"].Accept(writer);
            (void)settings.LoadFromJsonString(buffer.GetString());
        }
        m_Entries[zasset_key] = settings;
    }

    return true;
}

bool TextureImportSettingsRegistry::SaveToDisk() const
{
    if (m_RegistryFile.empty())
    {
        return true;
    }

    std::unordered_map<std::string, TextureImporterSettings> snapshot;
    {
        std::lock_guard<std::mutex> lk(m_Mutex);
        snapshot = m_Entries;
    }

    rapidjson::Document doc;
    doc.SetObject();
    auto& alloc = doc.GetAllocator();
    doc.AddMember("version", 1, alloc);

    rapidjson::Value entries(rapidjson::kArrayType);
    for (const auto& [key, settings] : snapshot)
    {
        rapidjson::Value entry_obj(rapidjson::kObjectType);
        entry_obj.AddMember("zasset", rapidjson::Value(key.c_str(), alloc), alloc);

        const std::string settings_json = settings.SaveToJsonString();
        rapidjson::Document settings_doc;
        settings_doc.Parse(settings_json.c_str());
        if (!settings_doc.HasParseError() && settings_doc.IsObject())
        {
            rapidjson::Value settings_val;
            settings_val.CopyFrom(settings_doc, alloc);
            entry_obj.AddMember("settings", settings_val, alloc);
        }
        entries.PushBack(entry_obj, alloc);
    }
    doc.AddMember("entries", entries, alloc);

    rapidjson::StringBuffer buffer;
    rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
    doc.Accept(writer);

    const std::filesystem::path tmp_path = m_RegistryFile.string() + ".tmp";
    {
        std::ofstream ofs(tmp_path, std::ios::binary | std::ios::trunc);
        if (!ofs.is_open())
        {
            LOG_WARNING(ZEditor,
                        "TextureImportSettingsRegistry: cannot write tmp {}",
                        tmp_path.generic_string());
            return false;
        }
        ofs.write(buffer.GetString(), static_cast<std::streamsize>(buffer.GetSize()));
    }

    std::error_code ec;
    std::filesystem::rename(tmp_path, m_RegistryFile, ec);
    if (ec)
    {
        std::filesystem::copy_file(tmp_path, m_RegistryFile, std::filesystem::copy_options::overwrite_existing, ec);
        std::filesystem::remove(tmp_path, ec);
    }
    return true;
}
