#pragma once

#include "Runtime/Core/Base/EngineSystem.h"
#include "Runtime/Function/PlayerSettings/PlayerSettings.h"
#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

#include <filesystem>
#include <string>

/**
 * @brief User preferences manager, similar to Unity's PlayerPrefs or Unreal Engine's GConfig
 *
 * This class provides a simple interface for saving and loading user preferences
 * that persist across application sessions. Preferences are stored in JSON format
 * in the user's application data directory.
 */
class UserPreferences : public IEngineSystem
{
public:
    std::string GetName() const override { return GET_CLASS_NAME(UserPreferences); }
    SystemInitPhase GetInitPhase() const override { return SystemInitPhase::PreInit; }
    std::vector<std::type_index> GetDependencies() const override { return {GET_SYSTEM_TYPE(PlayerSettings)}; }
    /**
     * @brief Initialize the user preferences system
     * @param app_name Application name (used for directory naming)
     */
    bool Initialize() override;

    /**
     * @brief Shutdown and save all preferences
     */
    void Shutdown() override;

    /**
     * @brief Save preferences to disk
     */
    void save();

    /**
     * @brief Load preferences from disk
     */
    void load();

    /**
     * @brief Get a string preference value
     * @param key Preference key
     * @param default_value Default value if key doesn't exist
     * @return Preference value or default
     */
    std::string GetString(const std::string& key, const std::string& default_value = "") const;

    /**
     * @brief Set a string preference value
     * @param key Preference key
     * @param value Preference value
     */
    void SetString(const std::string& key, const std::string& value);

    /**
     * @brief Get an integer preference value
     * @param key Preference key
     * @param default_value Default value if key doesn't exist
     * @return Preference value or default
     */
    int GetInt(const std::string& key, int default_value = 0) const;

    /**
     * @brief Set an integer preference value
     * @param key Preference key
     * @param value Preference value
     */
    void SetInt(const std::string& key, int value);

    /**
     * @brief Get a float preference value
     * @param key Preference key
     * @param default_value Default value if key doesn't exist
     * @return Preference value or default
     */
    float GetFloat(const std::string& key, float default_value = 0.0f) const;

    /**
     * @brief Set a float preference value
     * @param key Preference key
     * @param value Preference value
     */
    void SetFloat(const std::string& key, float value);

    /**
     * @brief Get a boolean preference value
     * @param key Preference key
     * @param default_value Default value if key doesn't exist
     * @return Preference value or default
     */
    bool GetBool(const std::string& key, bool default_value = false) const;

    /**
     * @brief Set a boolean preference value
     * @param key Preference key
     * @param value Preference value
     */
    void SetBool(const std::string& key, bool value);

    /**
     * @brief Delete a preference
     * @param key Preference key
     */
    void DeleteKey(const std::string& key);

    /**
     * @brief Check if a preference key exists
     * @param key Preference key
     * @return True if key exists
     */
    bool HasKey(const std::string& key) const;

    /**
     * @brief Get the preferences file path
     * @return Path to preferences file
     */
    std::filesystem::path GetPreferencesFilePath() const;

private:
    std::filesystem::path m_PreferencesFilePath;
    bool m_Dirty {false};
    rapidjson::Document m_Preferences;
    /**
     * @brief Get the user preferences directory path
     * @param app_name Application name
     * @return Path to preferences directory
     */
    static std::filesystem::path GetUserPreferencesDirectory(const std::string& app_name);
};