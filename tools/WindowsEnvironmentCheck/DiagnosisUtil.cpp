#include "DiagnosisUtil.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace zengine::envcheck::diagnosis
{
namespace
{
constexpr std::size_t MaxFractionDigits = 7;
constexpr std::size_t UsersDirectoryOffset = 3;
const std::string UsersDirectoryName = "users\\";
const std::string SanitizedUserProfile = "%USERPROFILE%";

bool ReadDigits(const std::string& text, std::size_t& position, std::size_t count, int& value)
{
    if (position + count > text.size())
    {
        return false;
    }
    int result = 0;
    for (std::size_t index = 0; index < count; ++index)
    {
        const char character = text[position + index];
        if (character < '0' || character > '9')
        {
            return false;
        }
        result = result * 10 + (character - '0');
    }
    position += count;
    value = result;
    return true;
}

bool Expect(const std::string& text, std::size_t& position, char expected)
{
    if (position >= text.size() || text[position] != expected)
    {
        return false;
    }
    ++position;
    return true;
}
} // namespace

std::string WideToUtf8(const std::wstring& value)
{
    if (value.empty())
    {
        return {};
    }
    const int byteCount = WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (byteCount <= 0)
    {
        return {};
    }
    std::string result(static_cast<std::size_t>(byteCount), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), byteCount, nullptr, nullptr);
    return result;
}

std::wstring Utf8ToWide(const std::string& value)
{
    if (value.empty())
    {
        return {};
    }
    const int characterCount = MultiByteToWideChar(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (characterCount <= 0)
    {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(characterCount), L'\0');
    MultiByteToWideChar(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), characterCount);
    return result;
}

std::string ToLowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character)
    {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

bool EqualsIgnoreCaseAscii(const std::string& left, const std::string& right)
{
    return left.size() == right.size() && ToLowerAscii(left) == ToLowerAscii(right);
}

bool StartsWith(const std::string& value, const std::string& prefix)
{
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

std::string FormatUtcIso8601(UtcTicks time)
{
    if (time == 0)
    {
        return {};
    }
    FILETIME fileTime{};
    fileTime.dwLowDateTime = static_cast<DWORD>(time & 0xFFFFFFFFull);
    fileTime.dwHighDateTime = static_cast<DWORD>(time >> 32);
    SYSTEMTIME systemTime{};
    if (FileTimeToSystemTime(&fileTime, &systemTime) == FALSE)
    {
        return {};
    }
    char buffer[32] = {};
    std::snprintf(
        buffer, sizeof(buffer), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
        static_cast<unsigned>(systemTime.wYear), static_cast<unsigned>(systemTime.wMonth),
        static_cast<unsigned>(systemTime.wDay), static_cast<unsigned>(systemTime.wHour),
        static_cast<unsigned>(systemTime.wMinute), static_cast<unsigned>(systemTime.wSecond),
        static_cast<unsigned>(systemTime.wMilliseconds));
    return buffer;
}

bool TryParseUtcIso8601(const std::string& text, UtcTicks& time)
{
    std::size_t position = 0;
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (!ReadDigits(text, position, 4, year) || !Expect(text, position, '-') ||
        !ReadDigits(text, position, 2, month) || !Expect(text, position, '-') ||
        !ReadDigits(text, position, 2, day))
    {
        return false;
    }
    if (position >= text.size() || (text[position] != 'T' && text[position] != 't' && text[position] != ' '))
    {
        return false;
    }
    ++position;
    if (!ReadDigits(text, position, 2, hour) || !Expect(text, position, ':') ||
        !ReadDigits(text, position, 2, minute) || !Expect(text, position, ':') ||
        !ReadDigits(text, position, 2, second))
    {
        return false;
    }

    UtcTicks fractionTicks = 0;
    if (position < text.size() && text[position] == '.')
    {
        ++position;
        std::size_t digits = 0;
        UtcTicks scale = TicksPerSecond;
        while (position < text.size() && text[position] >= '0' && text[position] <= '9')
        {
            if (digits < MaxFractionDigits)
            {
                scale /= 10;
                fractionTicks += static_cast<UtcTicks>(text[position] - '0') * scale;
            }
            ++digits;
            ++position;
        }
        if (digits == 0)
        {
            return false;
        }
    }

    std::int64_t offsetSeconds = 0;
    if (position < text.size() && (text[position] == 'Z' || text[position] == 'z'))
    {
        ++position;
    }
    else if (position < text.size() && (text[position] == '+' || text[position] == '-'))
    {
        const bool negative = text[position] == '-';
        ++position;
        int offsetHours = 0;
        int offsetMinutes = 0;
        if (!ReadDigits(text, position, 2, offsetHours) || !Expect(text, position, ':') ||
            !ReadDigits(text, position, 2, offsetMinutes))
        {
            return false;
        }
        offsetSeconds = static_cast<std::int64_t>(offsetHours) * 3600 + offsetMinutes * 60;
        if (negative)
        {
            offsetSeconds = -offsetSeconds;
        }
    }
    else
    {
        // A time without zone is ambiguous; the contract requires UTC.
        return false;
    }
    if (position != text.size())
    {
        return false;
    }

    SYSTEMTIME systemTime{};
    systemTime.wYear = static_cast<WORD>(year);
    systemTime.wMonth = static_cast<WORD>(month);
    systemTime.wDay = static_cast<WORD>(day);
    systemTime.wHour = static_cast<WORD>(hour);
    systemTime.wMinute = static_cast<WORD>(minute);
    systemTime.wSecond = static_cast<WORD>(second);
    FILETIME fileTime{};
    if (SystemTimeToFileTime(&systemTime, &fileTime) == FALSE)
    {
        return false;
    }
    std::int64_t ticks = static_cast<std::int64_t>(
        (static_cast<UtcTicks>(fileTime.dwHighDateTime) << 32) | fileTime.dwLowDateTime);
    ticks += static_cast<std::int64_t>(fractionTicks);
    ticks -= offsetSeconds * static_cast<std::int64_t>(TicksPerSecond);
    if (ticks <= 0)
    {
        return false;
    }
    time = static_cast<UtcTicks>(ticks);
    return true;
}

UtcTicks CurrentUtcTime()
{
    FILETIME fileTime{};
    GetSystemTimeAsFileTime(&fileTime);
    return (static_cast<UtcTicks>(fileTime.dwHighDateTime) << 32) | fileTime.dwLowDateTime;
}

UtcTicks SubtractClamped(UtcTicks value, UtcTicks amount)
{
    return value > amount ? value - amount : 0;
}

std::int64_t DeltaSeconds(UtcTicks value, UtcTicks reference)
{
    if (value >= reference)
    {
        return static_cast<std::int64_t>((value - reference) / TicksPerSecond);
    }
    return -static_cast<std::int64_t>((reference - value) / TicksPerSecond);
}

std::string SanitizePath(const std::string& path)
{
    std::string normalized = path;
    std::replace(normalized.begin(), normalized.end(), '/', '\\');
    const std::string lower = ToLowerAscii(normalized);
    if (lower.size() <= UsersDirectoryOffset || lower[1] != ':' || lower[2] != '\\' ||
        lower.compare(UsersDirectoryOffset, UsersDirectoryName.size(), UsersDirectoryName) != 0)
    {
        return normalized;
    }
    const std::size_t nameStart = UsersDirectoryOffset + UsersDirectoryName.size();
    const std::size_t nameEnd = normalized.find('\\', nameStart);
    if (nameEnd == std::string::npos)
    {
        return SanitizedUserProfile;
    }
    return SanitizedUserProfile + normalized.substr(nameEnd);
}

std::string FileNameOf(const std::string& path)
{
    const std::size_t separator = path.find_last_of("\\/");
    return separator == std::string::npos ? path : path.substr(separator + 1);
}

std::string LowerDirectoryOf(const std::string& path)
{
    std::string lower = ToLowerAscii(path);
    std::replace(lower.begin(), lower.end(), '/', '\\');
    const std::size_t separator = lower.find_last_of('\\');
    return separator == std::string::npos ? std::string{} : lower.substr(0, separator + 1);
}

const char* ToString(IncidentAnchor value)
{
    switch (value)
    {
    case IncidentAnchor::Launcher: return "launcher";
    case IncidentAnchor::WindowsErrorReporting: return "windows_error_reporting";
    case IncidentAnchor::None: break;
    }
    return "none";
}

const char* ToString(IncidentKind value)
{
    switch (value)
    {
    case IncidentKind::Crash: return "crash";
    case IncidentKind::Hang: return "hang";
    case IncidentKind::Unknown: break;
    }
    return "unknown";
}

const char* ToString(FindingLayer value)
{
    switch (value)
    {
    case FindingLayer::GpuDriver: return "gpu_driver";
    case FindingLayer::Hardware: return "hardware";
    case FindingLayer::Storage: return "storage";
    case FindingLayer::SystemStability: return "system_stability";
    case FindingLayer::SystemPower: return "system_power";
    case FindingLayer::OtherProcess: return "other_process";
    case FindingLayer::SystemResource: return "system_resource";
    case FindingLayer::ThirdParty: return "third_party";
    case FindingLayer::Game: return "game";
    }
    return "unknown";
}

const char* ToString(FindingStrength value)
{
    switch (value)
    {
    case FindingStrength::Strong: return "strong";
    case FindingStrength::Medium: return "medium";
    case FindingStrength::Weak: break;
    }
    return "weak";
}

const char* ToString(Proximity value)
{
    switch (value)
    {
    case Proximity::Near: return "near";
    case Proximity::Nearby: return "nearby";
    case Proximity::Lookback: break;
    }
    return "lookback";
}

const char* ToString(Verdict value)
{
    switch (value)
    {
    case Verdict::SystemCauseLikely: return "SYSTEM_CAUSE_LIKELY";
    case Verdict::SystemIssuesNearby: return "SYSTEM_ISSUES_NEARBY";
    case Verdict::NoSystemEvidence: return "NO_SYSTEM_EVIDENCE";
    case Verdict::Inconclusive: break;
    }
    return "INCONCLUSIVE";
}

const char* ToString(Confidence value)
{
    switch (value)
    {
    case Confidence::High: return "high";
    case Confidence::Medium: return "medium";
    case Confidence::Low: break;
    }
    return "low";
}
} // namespace zengine::envcheck::diagnosis
