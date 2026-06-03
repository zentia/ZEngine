#include "EditorProjectPrefs.h"

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Project/ProjectInfo.h"
#include "core/Log/LogSystem.h"
#include "rapidjson/istreamwrapper.h"
#include "rapidjson/prettywriter.h"
#include "rapidjson/stringbuffer.h"

#include <fstream>

namespace
{
    struct EditorProjectPrefsState
    {
        rapidjson::Document document;
        bool dirty {false};
        bool loaded {false};
    };

    EditorProjectPrefsState& state()
    {
        static EditorProjectPrefsState s_state;
        return s_state;
    }

    std::filesystem::path prefsFilePath()
    {
        const ProjectInfo* project = GET_SYSTEM(ProjectInfo);
        if (project == nullptr)
        {
            return {};
        }

        const std::filesystem::path root = project->GetProjectRoot();
        if (root.empty())
        {
            return {};
        }

        const std::string saved = project->saved_dir.empty() ? std::string("saved") : project->saved_dir;
        return root / saved / "config" / "editor_project_prefs.json";
    }

    // One-time migration from the earlier last_scene.txt prototype.
    void migrateLegacyLastSceneTxt(rapidjson::Document& document)
    {
        if (document.HasMember(EditorProjectPrefKeys::LastOpenedScene))
        {
            return;
        }

        const ProjectInfo* project = GET_SYSTEM(ProjectInfo);
        if (project == nullptr)
        {
            return;
        }

        const std::string saved = project->saved_dir.empty() ? std::string("saved") : project->saved_dir;
        const std::filesystem::path legacy_path = project->GetProjectRoot() / saved / "config" / "last_scene.txt";
        if (legacy_path.empty())
        {
            return;
        }

        std::ifstream input(legacy_path);
        if (!input.is_open())
        {
            return;
        }

        std::string line;
        if (!std::getline(input, line))
        {
            return;
        }

        const auto begin = line.find_first_not_of(" \t\r\n");
        if (begin == std::string::npos)
        {
            return;
        }

        const auto end = line.find_last_not_of(" \t\r\n");
        std::string trimmed = line.substr(begin, end - begin + 1);
        if (trimmed.empty())
        {
            return;
        }

        rapidjson::Value key;
        key.SetString(EditorProjectPrefKeys::LastOpenedScene,
                      static_cast<rapidjson::SizeType>(std::char_traits<char>::length(EditorProjectPrefKeys::LastOpenedScene)),
                      document.GetAllocator());
        rapidjson::Value value;
        value.SetString(trimmed.c_str(), static_cast<rapidjson::SizeType>(trimmed.size()), document.GetAllocator());
        document.AddMember(key, value, document.GetAllocator());
    }

    void ensureLoaded()
    {
        EditorProjectPrefsState& prefs = state();
        if (prefs.loaded)
        {
            return;
        }

        prefs.loaded = true;
        prefs.document.SetObject();

        const std::filesystem::path file_path = prefsFilePath();
        if (file_path.empty())
        {
            return;
        }

        if (std::filesystem::exists(file_path))
        {
            std::ifstream file(file_path);
            if (file.is_open())
            {
                rapidjson::IStreamWrapper stream(file);
                prefs.document.ParseStream(stream);
                if (prefs.document.HasParseError() || !prefs.document.IsObject())
                {
                    prefs.document.SetObject();
                }
            }
        }

        migrateLegacyLastSceneTxt(prefs.document);
    }

    void saveIfDirty()
    {
        EditorProjectPrefsState& prefs = state();
        if (!prefs.dirty)
        {
            return;
        }

        const std::filesystem::path file_path = prefsFilePath();
        if (file_path.empty())
        {
            return;
        }

        std::error_code ec;
        std::filesystem::create_directories(file_path.parent_path(), ec);

        const std::filesystem::path temp_path = file_path.string() + ".tmp";
        {
            std::ofstream file(temp_path, std::ios::binary);
            if (!file.is_open())
            {
                LOG_WARNING(ZEditor, "EditorProjectPrefs: failed to write {}", temp_path.generic_string());
                return;
            }

            rapidjson::StringBuffer buffer;
            rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
            prefs.document.Accept(writer);
            file << buffer.GetString();
        }

        std::filesystem::rename(temp_path, file_path, ec);
        if (ec)
        {
            std::filesystem::copy_file(temp_path, file_path, std::filesystem::copy_options::overwrite_existing, ec);
            std::filesystem::remove(temp_path, ec);
        }

        prefs.dirty = false;
    }
}  // namespace

std::string EditorProjectPrefs::GetString(const char* key, const std::string& default_value)
{
    if (key == nullptr || key[0] == '\0')
    {
        return default_value;
    }

    ensureLoaded();
    const EditorProjectPrefsState& prefs = state();
    if (!prefs.document.IsObject())
    {
        return default_value;
    }

    const auto member = prefs.document.FindMember(key);
    if (member == prefs.document.MemberEnd() || !member->value.IsString())
    {
        return default_value;
    }

    return member->value.GetString();
}

void EditorProjectPrefs::SetString(const char* key, const std::string& value)
{
    if (key == nullptr || key[0] == '\0')
    {
        return;
    }

    ensureLoaded();
    EditorProjectPrefsState& prefs = state();
    if (!prefs.document.IsObject())
    {
        prefs.document.SetObject();
    }

    rapidjson::Value json_key;
    json_key.SetString(key, static_cast<rapidjson::SizeType>(std::char_traits<char>::length(key)), prefs.document.GetAllocator());

    rapidjson::Value json_value;
    json_value.SetString(value.c_str(), static_cast<rapidjson::SizeType>(value.size()), prefs.document.GetAllocator());

    auto member = prefs.document.FindMember(key);
    if (member == prefs.document.MemberEnd())
    {
        prefs.document.AddMember(json_key, json_value, prefs.document.GetAllocator());
    }
    else
    {
        member->value = json_value;
    }

    prefs.dirty = true;
    saveIfDirty();
}

void EditorProjectPrefs::DeleteKey(const char* key)
{
    if (key == nullptr || key[0] == '\0')
    {
        return;
    }

    ensureLoaded();
    EditorProjectPrefsState& prefs = state();
    if (!prefs.document.IsObject() || !prefs.document.HasMember(key))
    {
        return;
    }

    prefs.document.RemoveMember(key);
    prefs.dirty = true;
    saveIfDirty();
}
