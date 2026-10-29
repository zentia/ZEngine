#pragma once
// ============================================================================
// ZSlate localization -- one mechanism, host-owned tables
// ============================================================================
// English is the source of truth and lives in the code: every string is written
// as Loc("<id>", "English text"). English therefore cannot fall out of sync with
// a table (there is none), and a missing translation degrades to English instead
// of to an empty label.
//
// ZSlate owns the mechanism only. It ships no translations of its own and takes
// no position on where a host keeps them (embedded table, file, registry,
// resource) -- RegisterStrings() is the whole contract. Switching language is
// SetLanguage(); a host that remembers the choice persists LanguageId() and
// reads it back through LanguageFromId().
//
// Strings are resolved when a widget is built, not when it is painted, so a
// language switch means rebuilding the widget tree -- see Loc(). Not thread
// safe by design: the UI thread is the only caller and a lock in the lookup
// path would cost more than it protects.
// ============================================================================

#include <cstddef>

namespace ZSlate
{

enum class ELanguage : int
{
    English = 0,          // the literals in the source; no table needed
    ChineseSimplified = 1,
};

// One translated string. A table is a plain array of these, which keeps a host
// that stores its strings elsewhere (a file, the registry) free to build the
// array however it likes.
struct FLocalizationEntry
{
    const char* id;
    const char* text;
};

// The active language. English until a host says otherwise.
void SetLanguage(ELanguage language);
ELanguage GetLanguage();

// Add (or replace) translations for one language. English needs no table.
void RegisterStrings(ELanguage language, const FLocalizationEntry* entries, std::size_t count);

// Resolve `id` in the active language; `english` is the fallback, so a string
// with no translation still reads correctly. An unknown id while a non-English
// language is active is reported once through ZSlate's log sink -- that is how
// a typo in an id gets noticed instead of silently showing English.
const char* Loc(const char* id, const char* english);

// For a language menu: the name in the language itself ("English", "简体中文"),
// and a stable id ("en", "zh-CN") a host can persist.
const char* LanguageName(ELanguage language);
const char* LanguageId(ELanguage language);
bool LanguageFromId(const char* id, ELanguage& out_language);

} // namespace ZSlate
