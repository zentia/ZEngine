#pragma once

#include "DiagnosisTypes.h"

#include <string>

namespace zengine::envcheck::diagnosis
{
std::string WideToUtf8(const std::wstring& value);
std::wstring Utf8ToWide(const std::string& value);
std::string ToLowerAscii(std::string value);
bool EqualsIgnoreCaseAscii(const std::string& left, const std::string& right);
bool StartsWith(const std::string& value, const std::string& prefix);

// "2026-10-12T03:04:05.123Z"; empty string for 0.
std::string FormatUtcIso8601(UtcTicks time);
// Accepts "YYYY-MM-DDTHH:MM:SS[.fff](Z|+hh:mm|-hh:mm)".
bool TryParseUtcIso8601(const std::string& text, UtcTicks& time);
UtcTicks CurrentUtcTime();

UtcTicks SubtractClamped(UtcTicks value, UtcTicks amount);
std::int64_t DeltaSeconds(UtcTicks value, UtcTicks reference);

// Replaces "X:\Users\<name>\" with "%USERPROFILE%\" so reports carry no user name.
std::string SanitizePath(const std::string& path);
std::string FileNameOf(const std::string& path);
// Lowercase directory with trailing backslash, or empty.
std::string LowerDirectoryOf(const std::string& path);
} // namespace zengine::envcheck::diagnosis
