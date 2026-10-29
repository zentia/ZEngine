// ============================================================================
// SlateLocalization -- see SlateLocalization.h
// ============================================================================

#include "ZSlate/Core/SlateLocalization.h"

#include "ZSlate/Core/SlateLog.h"

#include <array>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace ZSlate
{

namespace
{
    using FTable = std::unordered_map<std::string, std::string>;

    // Function-local statics: no static-initialisation order to get wrong, which
    // matters because a host may register its tables from its own static init.
    constexpr std::size_t kLanguageCount = 2;

    std::array<FTable, kLanguageCount>& Tables()
    {
        static std::array<FTable, kLanguageCount> tables;
        return tables;
    }

    ELanguage& ActiveLanguage()
    {
        static ELanguage language = ELanguage::English;
        return language;
    }

    void WarnMissing(const char* id)
    {
        static std::unordered_set<std::string> warned;
        if (!warned.insert(id).second) return;
        Log(ELogLevel::Warning, "[SlateLocalization] no %s text for '%s'",
            LanguageId(ActiveLanguage()), id);
    }
}

void SetLanguage(ELanguage language)
{
    const int index = static_cast<int>(language);
    if (index < 0 || index >= static_cast<int>(kLanguageCount)) return;
    ActiveLanguage() = language;
}

ELanguage GetLanguage()
{
    return ActiveLanguage();
}

void RegisterStrings(ELanguage language, const FLocalizationEntry* entries, std::size_t count)
{
    const int index = static_cast<int>(language);
    if (index < 0 || index >= static_cast<int>(kLanguageCount)) return;
    if (language == ELanguage::English) return;   // English is the source text
    if (!entries) return;

    FTable& table = Tables()[static_cast<std::size_t>(index)];
    for (std::size_t i = 0; i < count; ++i)
    {
        if (entries[i].id && entries[i].text) table[entries[i].id] = entries[i].text;
    }
}

const char* Loc(const char* id, const char* english)
{
    const ELanguage language = ActiveLanguage();
    if (language == ELanguage::English || !id) return english;

    const FTable& table = Tables()[static_cast<std::size_t>(language)];
    const auto found = table.find(id);
    if (found != table.end()) return found->second.c_str();

    WarnMissing(id);
    return english;
}

const char* LanguageName(ELanguage language)
{
    switch (language)
    {
    case ELanguage::ChineseSimplified: return "简体中文";
    case ELanguage::English: break;
    }
    return "English";
}

const char* LanguageId(ELanguage language)
{
    switch (language)
    {
    case ELanguage::ChineseSimplified: return "zh-CN";
    case ELanguage::English: break;
    }
    return "en";
}

bool LanguageFromId(const char* id, ELanguage& out_language)
{
    if (!id) return false;
    if (std::strcmp(id, "en") == 0) { out_language = ELanguage::English; return true; }
    if (std::strcmp(id, "zh-CN") == 0 || std::strcmp(id, "zh-Hans") == 0)
    {
        out_language = ELanguage::ChineseSimplified;
        return true;
    }
    return false;
}

} // namespace ZSlate
