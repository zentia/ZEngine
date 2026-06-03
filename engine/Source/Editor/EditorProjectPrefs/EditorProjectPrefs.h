#pragma once

#include <string>

// Per-project editor preferences (Unity EditorPrefs analogue for project-scoped
// keys). Stored under <Project>/saved/config/editor_project_prefs.json.
//
// Unity splits session state two ways:
//   - EditorPrefs "LastOpenedScene" (machine registry; one-shot on explicit open)
//   - Library/LastSceneManagerSetup.txt (project-local multi-scene layout)
// ZEngine keeps a single project-local string for the last active .scene URL
// (Assets-relative, same shape as Level::getLevelResUrl()).
class EditorProjectPrefs
{
public:
    static std::string GetString(const char* key, const std::string& default_value = "");
    static void SetString(const char* key, const std::string& value);
    static void DeleteKey(const char* key);
};

// Shared key names (mirror Unity EditorApplication.kLastOpenedScene).
namespace EditorProjectPrefKeys
{
    inline constexpr const char* LastOpenedScene = "LastOpenedScene";
}
