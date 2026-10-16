#pragma once
// ============================================================================
// UiLocalization -- the collector's own UI strings
// ============================================================================
// The mechanism lives in ZSlate (ZSlate/Core/SlateLocalization.h): English is
// the text written in the source, every other language is a table of
// id -> text registered at start-up. This module holds that table, and the
// persisted language choice.
//
// Deliberately ZSlate-free and Windows-free in its declarations: the CLI-only
// build (EnvironmentCheckGuiFallback.cpp, no ZSlate at all) links the same
// object, so nothing here may reach into ZSlate headers. Registering the table
// is the GUI's job.
//
// Report file content stays Chinese on purpose -- those files go to support,
// not to the player -- so this table covers the interface only.
// ============================================================================

#include <cstddef>
#include <string>

namespace zengine::envcheck::ui
{

struct UiStringEntry
{
    const char* id;
    const char* text;
};

// The Simplified Chinese table. nullptr when the build has no translations.
const UiStringEntry* ChineseSimplifiedStrings(std::size_t& out_count);

// The remembered language, as one of ZSlate's language ids ("en", "zh-CN").
// Empty means "not chosen yet", which the caller treats as the default.
// HKCU\Software\ZEngine\EnvironmentCollector, same key as the ProcDump path.
std::string LoadLanguagePreference();
void SaveLanguagePreference(const std::string& languageId);

} // namespace zengine::envcheck::ui
