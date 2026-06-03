#include "UserPreferences.h"

#include "Runtime/Function/PlayerSettings/PlayerSettings.h"
#include "Runtime/Platform/Encoding/EncodingUtils.h"
#include "core/Log/LogSystem.h"
#include "rapidjson/istreamwrapper.h"
#include "rapidjson/prettywriter.h"

#include <cstdlib>
#include <fstream>

namespace
{
    void ensurePreferencesObject(rapidjson::Document& preferences)
    {
        if (!preferences.IsObject())
        {
            preferences.SetObject();
        }
    }

    const rapidjson::Value* findPreferenceValue(const rapidjson::Document& preferences, const std::string& key)
    {
        if (!preferences.IsObject())
        {
            return nullptr;
        }

        auto member = preferences.FindMember(key.c_str());
        if (member == preferences.MemberEnd())
        {
            return nullptr;
        }

        return &member->value;
    }

    rapidjson::Value& upsertPreferenceValue(rapidjson::Document& preferences, const std::string& key)
    {
        ensurePreferencesObject(preferences);

        auto member = preferences.FindMember(key.c_str());
        if (member != preferences.MemberEnd())
        {
            return member->value;
        }

        rapidjson::Value json_key;
        json_key.SetString(key.c_str(), static_cast<rapidjson::SizeType>(key.size()), preferences.GetAllocator());

        rapidjson::Value json_value;
        preferences.AddMember(json_key, json_value, preferences.GetAllocator());

        return preferences.FindMember(key.c_str())->value;
    }
}  // namespace

bool UserPreferences::Initialize()

{
    m_PreferencesFilePath =
        GetUserPreferencesDirectory(GET_SYSTEM(PlayerSettings)->m_ProductName) / "preferences.json";

    // Create directory if it doesn't exist
    std::filesystem::create_directories(m_PreferencesFilePath.parent_path());

    // Load existing preferences
    load();

    LOG_INFO(ZUserPreferences, "User preferences initialized: {}", m_PreferencesFilePath.generic_string());
    return true;
}

void UserPreferences::Shutdown()
{
    // Save preferences before shutdown
    if (m_Dirty)
    {
        save();
    }
}

void UserPreferences::save()
{
    try
    {
        // Create directory if it doesn't exist
        std::filesystem::create_directories(m_PreferencesFilePath.parent_path());

        // Write to temporary file first, then rename (atomic operation)
        std::filesystem::path temp_path = m_PreferencesFilePath;
        temp_path += ".tmp";

        std::ofstream file(temp_path, std::ios::binary);
        if (!file.is_open())
        {
            LOG_ERROR(ZUserPreferences, "Failed to open preferences file for writing: {}", temp_path.generic_string());
            return;
        }
        rapidjson::StringBuffer sb;
        rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(sb);
        m_Preferences.Accept(writer);
        file << sb.GetString();  // Pretty print with 4 spaces
        file.close();

        // Replace original file with temporary file
        std::filesystem::rename(temp_path, m_PreferencesFilePath);

        m_Dirty = false;
        LOG_DEBUG(ZUserPreferences, "User preferences saved: {}", m_PreferencesFilePath.generic_string());
    }
    catch (const std::exception& e)
    {
        LOG_ERROR(ZUserPreferences, "Failed to save preferences: {}", Encoding::GetExceptionMessage(e));
    }
}

void UserPreferences::load()
{
    if (!std::filesystem::exists(m_PreferencesFilePath))
    {
        // File doesn't exist, start with empty preferences
        m_Preferences.SetObject();
        return;
    }

    try
    {
        std::ifstream file(m_PreferencesFilePath);
        if (!file.is_open())
        {
            LOG_WARNING(
                ZUserPreferences, "Failed to open preferences file: {}", m_PreferencesFilePath.generic_string());
            m_Preferences.SetObject();
            return;
        }

        rapidjson::IStreamWrapper isw(file);
        m_Preferences.ParseStream(isw);
        if (m_Preferences.HasParseError() || !m_Preferences.IsObject())
        {
            LOG_WARNING(
                ZUserPreferences,
                "Failed to parse preferences file: {}, using defaults",
                m_PreferencesFilePath.generic_string());
            m_Preferences.SetObject();
            return;
        }

        LOG_DEBUG(ZUserPreferences, "User preferences loaded: {}", m_PreferencesFilePath.generic_string());
    }
    catch (const std::exception& e)
    {
        LOG_WARNING(
            ZUserPreferences, "Failed to load preferences: {}, using defaults", Encoding::GetExceptionMessage(e));
        m_Preferences.SetObject();
    }
}

std::string UserPreferences::GetString(const std::string& key, const std::string& default_value) const
{
    const auto* value = findPreferenceValue(m_Preferences, key);
    if (value && value->IsString())
    {
        return value->GetString();
    }

    return default_value;
}

void UserPreferences::SetString(const std::string& key, const std::string& value)
{
    auto& preference = upsertPreferenceValue(m_Preferences, key);
    preference.SetString(value.c_str(), static_cast<rapidjson::SizeType>(value.size()), m_Preferences.GetAllocator());
    m_Dirty = true;
}

int UserPreferences::GetInt(const std::string& key, int default_value) const
{
    const auto* value = findPreferenceValue(m_Preferences, key);
    if (value && value->IsInt())
    {
        return value->GetInt();
    }

    return default_value;
}

void UserPreferences::SetInt(const std::string& key, int value)
{
    upsertPreferenceValue(m_Preferences, key).SetInt(value);
    m_Dirty = true;
}

float UserPreferences::GetFloat(const std::string& key, float default_value) const
{
    const auto* value = findPreferenceValue(m_Preferences, key);
    if (value && value->IsNumber())
    {
        return value->GetFloat();
    }

    return default_value;
}

void UserPreferences::SetFloat(const std::string& key, float value)
{
    upsertPreferenceValue(m_Preferences, key).SetFloat(value);
    m_Dirty = true;
}

bool UserPreferences::GetBool(const std::string& key, bool default_value) const
{
    const auto* value = findPreferenceValue(m_Preferences, key);
    if (value && value->IsBool())
    {
        return value->GetBool();
    }

    return default_value;
}

void UserPreferences::SetBool(const std::string& key, bool value)
{
    upsertPreferenceValue(m_Preferences, key).SetBool(value);
    m_Dirty = true;
}

void UserPreferences::DeleteKey(const std::string& key)
{
    if (!m_Preferences.IsObject())
    {
        return;
    }

    if (m_Preferences.HasMember(key.c_str()))
    {
        m_Preferences.RemoveMember(key.c_str());
        m_Dirty = true;
    }
}

bool UserPreferences::HasKey(const std::string& key) const
{
    return m_Preferences.IsObject() && m_Preferences.HasMember(key.c_str());
}

std::filesystem::path UserPreferences::GetPreferencesFilePath() const
{
    return m_PreferencesFilePath;
}

std::filesystem::path UserPreferences::GetUserPreferencesDirectory(const std::string& app_name)
{
    // Unity-style per-user folder (same tree as LogSystem: LocalLow/ZEngine on Windows).
    static constexpr const char* kCompanyFolder = "ZEngine";
    const std::string folder_name = app_name.empty() ? kCompanyFolder : app_name;

#ifdef _WIN32
    if (const char* user_profile = std::getenv("USERPROFILE"))
    {
        return std::filesystem::path(user_profile) / "AppData" / "LocalLow" / kCompanyFolder;
    }
    if (const char* local_app_data = std::getenv("LOCALAPPDATA"))
    {
        return std::filesystem::path(local_app_data).parent_path() / "LocalLow" / kCompanyFolder;
    }
    return std::filesystem::current_path() / folder_name;
#elif defined(__APPLE__)
    const char* home = std::getenv("HOME");
    if (home)
    {
        return std::filesystem::path(home) / "Library" / "Application Support" / folder_name;
    }
    return std::filesystem::current_path() / folder_name;
#else
    // Linux and other Unix-like systems
    const char* home = std::getenv("HOME");
    if (home)
    {
        return std::filesystem::path(home) / ".config" / folder_name;
    }
    return std::filesystem::current_path() / folder_name;
#endif
}