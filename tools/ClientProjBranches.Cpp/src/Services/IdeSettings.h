#pragma once
// ============================================================================
// IdeSettings — port of ClientProjBranches.CSharp/Services/IdeSettings.cs
//
// %USERPROFILE%\.client_proj_branches\ide_preference.json holds a bare JSON
// string: "CodeBuddy" or "Cursor". Older C++ builds wrote {"ide": "..."};
// both shapes are accepted on load, the C# shape is what gets written.
// ============================================================================
#include <string>
#include <algorithm>
#include <cctype>

#include "Paths.h"
#include "JsonConfig.h"

namespace ClientProj {
namespace IdeSettings {

inline std::string ToLower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

// Canonical internal value: "codebuddy" | "cursor"
inline std::string Normalize(const std::string& raw)
{
    const std::string lower = ToLower(raw);
    if (lower == "cursor") return "cursor";
    return "codebuddy";
}

// Display / on-disk value used by the C# build: "CodeBuddy" | "Cursor"
inline std::string CanonicalName(const std::string& internal)
{
    return Normalize(internal) == "cursor" ? "Cursor" : "CodeBuddy";
}

inline std::string Load()
{
    // Preferred (C#) shape: a bare string.
    if (auto value = Json::LoadValueFromFile(Paths::IdePreference()))
    {
        if (value->IsString()) return Normalize(value->AsString());
        if (value->IsObject()) return Normalize(Json::OptStringCI(value->AsObject(), "ide", "codebuddy"));
    }

    // Legacy object shape.
    const Json::Object legacy = Json::LoadFromFile(Paths::IdePreference());
    return Normalize(Json::OptStringCI(legacy, "ide", "codebuddy"));
}

inline void Save(const std::string& ide)
{
    // Written exactly like the C# build: "CodeBuddy"
    Json::SaveStringToFile(Paths::IdePreference(), CanonicalName(ide));
}

} // namespace IdeSettings
} // namespace ClientProj
