#pragma once
// ============================================================================
// GitTagStore — port of ClientProjBranches.CSharp/Git/GitTagStore.cs
//
// %USERPROFILE%\.client_proj_branches\git_tags.json is a bare array of
// {"Tag","ForeArgb","BackArgb"}. 0 means "unset" (fall back to the default
// row colors). Lookup is case-insensitive; the display casing is whatever the
// user typed first.
// ============================================================================
#include "Paths.h"
#include "JsonConfig.h"

#include <string>
#include <vector>
#include <algorithm>
#include <cctype>

namespace ClientProj {
namespace GitTags {

struct GitTagStyle {
    std::string Tag;
    int         ForeArgb = 0;   // 0 = unset
    int         BackArgb = 0;   // 0 = unset
};

inline std::string KeyOf(const std::string& tag)
{
    std::string key;
    key.reserve(tag.size());
    for (char c : tag) key += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return key;
}

inline std::string Trim(const std::string& value)
{
    size_t begin = 0;
    size_t end   = value.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(value[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;
    return value.substr(begin, end - begin);
}

inline std::vector<GitTagStyle> Load()
{
    std::vector<GitTagStyle> styles;

    for (const auto& value : Json::LoadArrayFromFile(Paths::GitTags()))
    {
        if (!value || !value->IsObject()) continue;
        const Json::Object& object = value->AsObject();
        GitTagStyle style;
        style.Tag      = Trim(Json::OptStringCI(object, "Tag"));
        style.ForeArgb = Json::OptInt(object, "ForeArgb", 0);
        style.BackArgb = Json::OptInt(object, "BackArgb", 0);
        if (!style.Tag.empty()) styles.push_back(style);
    }

    if (!styles.empty()) return styles;

    // A single object instead of an array.
    const Json::Object single = Json::LoadFromFile(Paths::GitTags());
    if (!single.empty())
    {
        GitTagStyle style;
        style.Tag      = Trim(Json::OptStringCI(single, "Tag"));
        style.ForeArgb = Json::OptInt(single, "ForeArgb", 0);
        style.BackArgb = Json::OptInt(single, "BackArgb", 0);
        if (!style.Tag.empty()) styles.push_back(style);
    }

    return styles;
}

inline void Save(const std::vector<GitTagStyle>& styles)
{
    Json::Array array;
    std::vector<std::string> seen;

    for (const GitTagStyle& style : styles)
    {
        const std::string name = Trim(style.Tag);
        if (name.empty()) continue;

        const std::string key = KeyOf(name);
        if (std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
        seen.push_back(key);

        Json::Object object;
        object["Tag"]      = std::make_shared<Json::Value>(name);
        object["ForeArgb"] = std::make_shared<Json::Value>(static_cast<double>(style.ForeArgb));
        object["BackArgb"] = std::make_shared<Json::Value>(static_cast<double>(style.BackArgb));
        array.push_back(std::make_shared<Json::Value>(object));
    }

    Json::SaveArrayToFile(Paths::GitTags(), array);
}

} // namespace GitTags
} // namespace ClientProj
