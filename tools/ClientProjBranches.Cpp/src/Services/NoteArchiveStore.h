#pragma once
// ============================================================================
// NoteArchiveStore — port of the C# NoteArchiveStore (Svn/NoteEntry.cs) plus
// the weekly-report copy template (Services/NoteArchiveCopyTemplate.cs).
//
// %USERPROFILE%\.client_proj_branches\notes_archive.json is a bare array of
// {"Priority","Text","Url","Created","CompletedAt","WeekTag"} — PascalCase,
// no "Done" field (CompletedAt carries that meaning).
// ============================================================================
#include "Paths.h"
#include "JsonConfig.h"

#include <string>
#include <vector>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>

namespace ClientProj {
namespace NoteArchive {

struct NoteArchiveEntry {
    std::string Priority    = "Normal";
    std::string Text;
    std::string Url;
    std::string Created;
    std::string CompletedAt;
    std::string WeekTag;
};

inline std::string NormalizePriority(const std::string& priority)
{
    std::string lower;
    lower.reserve(priority.size());
    for (char c : priority) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower == "high") return "High";
    if (lower == "low")  return "Low";
    return "Normal";
}

inline std::string NowString()
{
    const std::time_t now = std::time(nullptr);
    std::tm local {};
    localtime_s(&local, &now);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &local);
    return buf;
}

// ISO-8601 week tag, e.g. "2026-W42". The week belongs to the year that
// contains its Thursday; week 1 is the one containing January 4th.
inline std::string IsoWeekTag(std::time_t timeValue)
{
    std::tm local {};
    localtime_s(&local, &timeValue);

    const int weekday = (local.tm_wday == 0) ? 7 : local.tm_wday;   // Mon=1..Sun=7

    std::tm thursday = local;
    thursday.tm_mday += (4 - weekday);
    thursday.tm_isdst = -1;
    std::mktime(&thursday);

    const int isoYear = thursday.tm_year + 1900;

    std::tm jan4th {};
    jan4th.tm_year  = isoYear - 1900;
    jan4th.tm_mon   = 0;
    jan4th.tm_mday  = 4;
    jan4th.tm_isdst = -1;
    std::mktime(&jan4th);

    const int jan4Weekday = (jan4th.tm_wday == 0) ? 7 : jan4th.tm_wday;
    std::tm week1Monday = jan4th;
    week1Monday.tm_mday -= (jan4Weekday - 1);
    week1Monday.tm_isdst = -1;
    std::mktime(&week1Monday);

    const double days = std::difftime(std::mktime(&thursday), std::mktime(&week1Monday)) / 86400.0;
    int week = static_cast<int>(days) / 7 + 1;
    if (week < 1) week = 1;

    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-W%02d", isoYear, week);
    return buf;
}

inline std::string WeekTagOf(const std::string& completedAt)
{
    // "yyyy-MM-dd HH:mm:ss" -> parse, fall back to now when unparsable.
    std::tm parsed {};
    if (completedAt.size() >= 10)
    {
        parsed.tm_year = std::atoi(completedAt.substr(0, 4).c_str()) - 1900;
        parsed.tm_mon  = std::atoi(completedAt.substr(5, 2).c_str()) - 1;
        parsed.tm_mday = std::atoi(completedAt.substr(8, 2).c_str());
        if (completedAt.size() >= 19)
        {
            parsed.tm_hour = std::atoi(completedAt.substr(11, 2).c_str());
            parsed.tm_min  = std::atoi(completedAt.substr(14, 2).c_str());
            parsed.tm_sec  = std::atoi(completedAt.substr(17, 2).c_str());
        }
        parsed.tm_isdst = -1;
        const std::time_t value = std::mktime(&parsed);
        if (value != static_cast<std::time_t>(-1)) return IsoWeekTag(value);
    }
    return IsoWeekTag(std::time(nullptr));
}

inline std::vector<NoteArchiveEntry> Load()
{
    std::vector<NoteArchiveEntry> entries;

    for (const auto& value : Json::LoadArrayFromFile(Paths::NotesArchive()))
    {
        if (!value || !value->IsObject()) continue;
        const Json::Object& object = value->AsObject();

        NoteArchiveEntry entry;
        entry.Priority    = NormalizePriority(Json::OptStringCI(object, "Priority", "Normal"));
        entry.Text        = Json::OptStringCI(object, "Text");
        entry.Url         = Json::OptStringCI(object, "Url");
        entry.Created     = Json::OptStringCI(object, "Created");
        entry.CompletedAt = Json::OptStringCI(object, "CompletedAt");
        entry.WeekTag     = Json::OptStringCI(object, "WeekTag");

        if (entry.Text.empty()) continue;
        if (entry.WeekTag.empty()) entry.WeekTag = "(no-week)";
        entries.push_back(entry);
    }
    return entries;
}

inline void Save(const std::vector<NoteArchiveEntry>& entries)
{
    Json::Array array;
    array.reserve(entries.size());
    for (const NoteArchiveEntry& entry : entries)
    {
        if (entry.Text.empty()) continue;
        Json::Object object;
        object["Priority"]    = std::make_shared<Json::Value>(NormalizePriority(entry.Priority));
        object["Text"]        = std::make_shared<Json::Value>(entry.Text);
        object["Url"]         = std::make_shared<Json::Value>(entry.Url);
        object["Created"]     = std::make_shared<Json::Value>(entry.Created);
        object["CompletedAt"] = std::make_shared<Json::Value>(entry.CompletedAt);
        object["WeekTag"]     = std::make_shared<Json::Value>(entry.WeekTag);
        array.push_back(std::make_shared<Json::Value>(object));
    }
    Json::SaveArrayToFile(Paths::NotesArchive(), array);
}

// ---------------------------------------------------------------------------
// Weekly-report copy template
// ---------------------------------------------------------------------------

inline const char* DefaultTemplate()
{
    return "二组周报：\r\n本周工作：\r\n${Content1}\r\n下周计划：\r\n${Content2}";
}

inline const char* PlaceholderContent1() { return "${Content1}"; }
inline const char* PlaceholderContent2() { return "${Content2}"; }

inline std::string LoadTemplate()
{
    std::ifstream file(Paths::NotesArchiveCopyTemplate(), std::ios::binary);
    if (!file.is_open()) return DefaultTemplate();
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string text = buffer.str();
    if (text.empty()) return DefaultTemplate();
    return text;
}

inline void SaveTemplate(const std::string& text)
{
    std::ofstream file(Paths::NotesArchiveCopyTemplate(), std::ios::binary | std::ios::trunc);
    if (file.is_open()) file << text;
}

inline std::string ReplaceAll(std::string text, const std::string& from, const std::string& to)
{
    if (from.empty()) return text;
    size_t pos = 0;
    while ((pos = text.find(from, pos)) != std::string::npos)
    {
        text.replace(pos, from.size(), to);
        pos += to.size();
    }
    return text;
}

inline std::string ApplyTemplate(const std::string& content1, const std::string& content2)
{
    return ReplaceAll(ReplaceAll(LoadTemplate(), PlaceholderContent2(), content2),
                      PlaceholderContent1(), content1);
}

// First line gets "- ", continuation lines are indented by two spaces.
inline std::string FormatBullet(const std::string& text)
{
    std::string cleaned;
    for (char c : text) { if (c != '\r') cleaned += c; }

    std::string result;
    size_t pos = 0;
    bool first = true;
    while (pos <= cleaned.size())
    {
        const size_t newline = cleaned.find('\n', pos);
        std::string line = cleaned.substr(pos, (newline == std::string::npos) ? std::string::npos : newline - pos);

        while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.pop_back();
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.erase(line.begin());

        if (!line.empty())
        {
            result += first ? "- " : "  ";
            result += line;
            result += "\r\n";
            first = false;
        }

        if (newline == std::string::npos) break;
        pos = newline + 1;
    }
    return result;
}

inline std::string FormatArchiveEntries(const std::vector<NoteArchiveEntry>& entries)
{
    std::vector<NoteArchiveEntry> sorted = entries;
    std::stable_sort(sorted.begin(), sorted.end(),
        [](const NoteArchiveEntry& a, const NoteArchiveEntry& b) { return a.CompletedAt > b.CompletedAt; });

    std::string result;
    for (const NoteArchiveEntry& entry : sorted) result += FormatBullet(entry.Text);
    return result;
}

} // namespace NoteArchive
} // namespace ClientProj
