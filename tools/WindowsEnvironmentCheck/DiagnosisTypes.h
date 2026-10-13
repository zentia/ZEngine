#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace zengine::envcheck::diagnosis
{
// UTC time in FILETIME units: 100-nanosecond intervals since 1601-01-01.
using UtcTicks = std::uint64_t;

constexpr UtcTicks TicksPerSecond = 10000000ull;
constexpr UtcTicks TicksPerMinute = 60ull * TicksPerSecond;
constexpr UtcTicks TicksPerHour = 60ull * TicksPerMinute;

// Windows event levels (winmeta.xml).
constexpr std::uint32_t EventLevelCritical = 1;
constexpr std::uint32_t EventLevelError = 2;
constexpr std::uint32_t EventLevelWarning = 3;

enum class EventChannel
{
    System,
    Application
};

// One event from the Windows event log, reduced to the fields used by the rules.
// Paths are sanitized (user profile replaced) before they are stored here.
struct NormalizedEvent
{
    EventChannel channel = EventChannel::System;
    std::string provider;
    std::uint32_t eventId = 0;
    std::uint32_t level = 0;
    UtcTicks time = 0;

    // "Application Error" / "Application Hang"
    std::string appName;
    std::string appPath;
    std::string moduleName;
    std::string modulePath;
    std::string exceptionCode;

    // "Windows Error Reporting" 1001
    std::string werEventName;
    std::string werP1;
};

struct Observability
{
    bool systemLogReadable = false;
    bool applicationLogReadable = false;
    bool systemOldestKnown = false;
    UtcTicks systemOldest = 0;
    bool budgetExceeded = false;
    bool eventCapReached = false;
    bool queryErrorOccurred = false;
};

// Raw material for the rules. Produced by EventLogReader (live) or by the
// events file parser (replay), so rules never touch the operating system.
struct EventCollection
{
    UtcTicks collectedAt = 0;
    UtcTicks rangeStart = 0;
    UtcTicks rangeEnd = 0;
    Observability observability;
    std::vector<UtcTicks> systemEventTimes;
    std::vector<NormalizedEvent> events;
};

enum class IncidentAnchor
{
    None,
    Launcher,
    WindowsErrorReporting
};

enum class IncidentKind
{
    Unknown,
    Crash,
    Hang
};

struct DiagnosisRequest
{
    bool hasLauncherIncidentTime = false;
    UtcTicks launcherIncidentTime = 0;
    IncidentKind launcherIncidentKind = IncidentKind::Unknown;
    std::string gameExecutableName;
    UtcTicks now = 0;
    UtcTicks lookbackStart = 0;
    std::uint32_t lookbackHours = 0;
};

struct Incident
{
    IncidentAnchor anchor = IncidentAnchor::None;
    IncidentKind kind = IncidentKind::Unknown;
    UtcTicks time = 0;
    UtcTicks nearStart = 0;
    UtcTicks nearEnd = 0;
    UtcTicks windowStart = 0;
    UtcTicks windowEnd = 0;
    bool faultDetailsAvailable = false;
    std::string faultModuleName;
    std::string faultModulePath;
    std::string exceptionCode;
};

enum class FindingLayer
{
    GpuDriver,
    Hardware,
    Storage,
    SystemStability,
    SystemPower,
    OtherProcess,
    SystemResource,
    ThirdParty,
    Game
};

enum class FindingStrength
{
    Weak,
    Medium,
    Strong
};

enum class Proximity
{
    Near,
    Nearby,
    Lookback
};

struct Finding
{
    std::string ruleId;
    std::string code;
    FindingLayer layer = FindingLayer::GpuDriver;
    FindingStrength strength = FindingStrength::Weak;
    Proximity proximity = Proximity::Lookback;
    UtcTicks time = 0;
    bool hasDelta = false;
    std::int64_t deltaSeconds = 0;
    std::uint32_t count = 1;
    std::string evidence;
};

enum class Verdict
{
    SystemCauseLikely,
    SystemIssuesNearby,
    NoSystemEvidence,
    Inconclusive
};

enum class Confidence
{
    Low,
    Medium,
    High
};

struct UserAction
{
    std::string code;
    std::string zh;
};

struct DiagnosisResult
{
    Incident incident;
    Observability observability;
    bool observabilityOk = false;
    std::vector<std::string> observabilityIssues;
    std::uint64_t systemEventsInWindow = 0;
    std::vector<Finding> findings;
    Verdict verdict = Verdict::Inconclusive;
    Confidence confidence = Confidence::Low;
    std::string userMessageCode;
    std::string userMessageZh;
    std::vector<UserAction> actions;
};

const char* ToString(IncidentAnchor value);
const char* ToString(IncidentKind value);
const char* ToString(FindingLayer value);
const char* ToString(FindingStrength value);
const char* ToString(Proximity value);
const char* ToString(Verdict value);
const char* ToString(Confidence value);
} // namespace zengine::envcheck::diagnosis
