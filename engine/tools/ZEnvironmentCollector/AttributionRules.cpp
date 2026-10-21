#include "AttributionRules.h"

#include "DiagnosisUtil.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace zengine::envcheck::diagnosis
{
namespace
{
// Proximity windows. Initial values; calibrate with field samples before tightening.
constexpr UtcTicks LauncherNearBefore = 120ull * TicksPerSecond;
constexpr UtcTicks LauncherNearAfter = 30ull * TicksPerSecond;
// AppCrash/AppHang records are written when WER handles the report, which can lag the fault.
constexpr UtcTicks WerNearBefore = 300ull * TicksPerSecond;
constexpr UtcTicks WerNearAfter = 60ull * TicksPerSecond;
constexpr UtcTicks NearbyBefore = IncidentWindowMargin;
constexpr UtcTicks NearbyAfter = IncidentWindowMargin;
// Maximum distance between a launcher-reported time and the game's AppCrash/AppHang record.
constexpr UtcTicks LauncherFaultMatchTolerance = 5ull * TicksPerMinute;
constexpr std::size_t MaxReportedFindings = 10;

// System channel. Provider names are compared lowercase.
const std::string ProviderDisplay = "display";
constexpr std::uint32_t EventDisplayTdrRecovered = 4101;
const std::array<const char*, 4> GpuKernelDriverProviderPrefixes = {"nvlddmkm", "amdkm", "amdwddm", "igfx"};
const std::string ProviderWhea = "microsoft-windows-whea-logger";
const std::array<const char*, 9> StorageProviders = {
    "disk", "ntfs", "microsoft-windows-ntfs", "stornvme", "storahci",
    "iastora", "iastorac", "iastoravc", "iastorv"};
const std::string ProviderWerSystemErrorReporting = "microsoft-windows-wer-systemerrorreporting";
const std::string ProviderBugCheck = "bugcheck";
constexpr std::uint32_t EventBugCheck = 1001;
const std::string ProviderKernelPower = "microsoft-windows-kernel-power";
constexpr std::uint32_t EventKernelPowerUnexpectedReboot = 41;
constexpr std::uint32_t EventKernelPowerEnterSleep = 42;
constexpr std::uint32_t EventKernelPowerResumeFromSleep = 107;
const std::string ProviderClassicEventLog = "eventlog";
constexpr std::uint32_t EventUnexpectedShutdown = 6008;
const std::string ProviderPowerTroubleshooter = "microsoft-windows-power-troubleshooter";
constexpr std::uint32_t EventReturnedFromLowPower = 1;
const std::string ProviderResourceExhaustion = "microsoft-windows-resource-exhaustion-detector";
constexpr std::uint32_t EventLowVirtualMemory = 2004;
const std::string ProviderEventLog = "microsoft-windows-eventlog";
constexpr std::uint32_t EventLogCleared = 104;

// Application channel.
const std::string ProviderApplicationError = "application error";
constexpr std::uint32_t EventApplicationError = 1000;
const std::string ProviderApplicationHang = "application hang";
constexpr std::uint32_t EventApplicationHang = 1002;
const std::string ProviderWindowsErrorReporting = "windows error reporting";
constexpr std::uint32_t EventWindowsErrorReporting = 1001;
const std::string WerLiveKernelEvent = "livekernelevent";
// GPU hang / engine timeout codes documented under WDDM timeout detection and recovery.
const std::array<const char*, 3> GpuLiveKernelEventCodes = {"117", "141", "142"};
const std::string DesktopWindowManager = "dwm.exe";

// Vendor user-mode graphics driver modules (they also live under DriverStore).
const std::array<const char*, 14> GpuUserModeDriverPrefixes = {
    "nvwgf2um", "nvd3dum", "nvoglv", "nvldumd", "nvwgf",
    "atidxx", "atiumd", "aticfx", "amdxx", "amdxc",
    "igd10", "igd11", "igd12", "igdumd"};
const std::string DriverStoreMarker = "\\driverstore\\filerepository\\";
const std::string WindowsDirectoryMarker = "\\windows\\";
constexpr std::size_t DriveLetterLength = 2;
const std::string UnknownModuleName = "unknown";

struct Classification
{
    bool matched = false;
    const char* ruleId = "";
    std::string code;
    FindingLayer layer = FindingLayer::GpuDriver;
    FindingStrength strength = FindingStrength::Weak;
    // The record is written after the fault (for example at the next boot).
    bool reportedAfterFault = false;
};

struct ObservabilityAssessment
{
    bool fatal = false;
    bool downgrade = false;
    bool scanIncomplete = false;
};

template <std::size_t Count>
bool EqualsAny(const std::string& value, const std::array<const char*, Count>& candidates)
{
    return std::any_of(candidates.begin(), candidates.end(), [&value](const char* candidate)
    {
        return value == candidate;
    });
}

template <std::size_t Count>
bool StartsWithAny(const std::string& value, const std::array<const char*, Count>& prefixes)
{
    return std::any_of(prefixes.begin(), prefixes.end(), [&value](const char* prefix)
    {
        return StartsWith(value, prefix);
    });
}

bool IsErrorLevel(std::uint32_t level)
{
    return level != 0 && level <= EventLevelError;
}

bool IsWarningOrWorse(std::uint32_t level)
{
    return level != 0 && level <= EventLevelWarning;
}

// Forwarded: the definition sits below NormalizeWerCode, which the Windows Error
// Reporting branch needs, and both classifiers are defined above that.
bool MatchesGpuDriverReset(EventChannel channel, const std::string& providerLower, std::uint32_t eventId,
                           const std::string& werEventName, const std::string& werP1);

Classification Make(
    const char* ruleId,
    std::string code,
    FindingLayer layer,
    FindingStrength strength,
    bool reportedAfterFault = false)
{
    Classification classification;
    classification.matched = true;
    classification.ruleId = ruleId;
    classification.code = std::move(code);
    classification.layer = layer;
    classification.strength = strength;
    classification.reportedAfterFault = reportedAfterFault;
    return classification;
}

Classification ClassifySystemEvent(const std::string& provider, std::uint32_t eventId, std::uint32_t level)
{
    if (MatchesGpuDriverReset(EventChannel::System, provider, eventId, std::string{}, std::string{}))
    {
        return Make("GPU_TDR", "DISPLAY_TDR_RECOVERED", FindingLayer::GpuDriver, FindingStrength::Strong);
    }
    if (StartsWithAny(provider, GpuKernelDriverProviderPrefixes) && IsErrorLevel(level))
    {
        return Make("GPU_DRIVER_ERROR", "GPU_KERNEL_DRIVER_ERROR", FindingLayer::GpuDriver, FindingStrength::Medium);
    }
    if (provider == ProviderWhea && IsWarningOrWorse(level))
    {
        return IsErrorLevel(level)
            ? Make("WHEA_ERROR", "WHEA_UNCORRECTED", FindingLayer::Hardware, FindingStrength::Strong)
            : Make("WHEA_ERROR", "WHEA_CORRECTED", FindingLayer::Hardware, FindingStrength::Medium);
    }
    if (EqualsAny(provider, StorageProviders) && IsWarningOrWorse(level))
    {
        return IsErrorLevel(level)
            ? Make("STORAGE_ERROR", "STORAGE_ERROR", FindingLayer::Storage, FindingStrength::Strong)
            : Make("STORAGE_ERROR", "STORAGE_WARNING", FindingLayer::Storage, FindingStrength::Medium);
    }
    if ((provider == ProviderWerSystemErrorReporting || provider == ProviderBugCheck) && eventId == EventBugCheck)
    {
        return Make("BUGCHECK", "SYSTEM_BUGCHECK", FindingLayer::SystemStability, FindingStrength::Strong, true);
    }
    if ((provider == ProviderKernelPower && eventId == EventKernelPowerUnexpectedReboot) ||
        (provider == ProviderClassicEventLog && eventId == EventUnexpectedShutdown))
    {
        // Weak on purpose: a frozen game followed by a hard reset produces the same record.
        return Make("UNEXPECTED_SHUTDOWN", "UNEXPECTED_SHUTDOWN", FindingLayer::SystemStability,
            FindingStrength::Weak, true);
    }
    if ((provider == ProviderKernelPower &&
            (eventId == EventKernelPowerEnterSleep || eventId == EventKernelPowerResumeFromSleep)) ||
        (provider == ProviderPowerTroubleshooter && eventId == EventReturnedFromLowPower))
    {
        return Make("SLEEP_TRANSITION", "SYSTEM_SLEEP_RESUME", FindingLayer::SystemPower, FindingStrength::Medium);
    }
    if (provider == ProviderResourceExhaustion && eventId == EventLowVirtualMemory)
    {
        // Weak: the game itself may be the process that exhausted memory.
        return Make("RESOURCE_EXHAUSTION", "LOW_VIRTUAL_MEMORY", FindingLayer::SystemResource, FindingStrength::Weak);
    }
    return {};
}

bool IsLogClearedEvent(const std::string& provider, std::uint32_t eventId)
{
    return provider == ProviderEventLog && eventId == EventLogCleared;
}

bool IsApplicationFaultEvent(const std::string& provider, std::uint32_t eventId)
{
    return (provider == ProviderApplicationError && eventId == EventApplicationError) ||
        (provider == ProviderApplicationHang && eventId == EventApplicationHang);
}

bool IsGameFaultEvent(const NormalizedEvent& event, const std::string& gameExecutableLower)
{
    return event.channel == EventChannel::Application &&
        IsApplicationFaultEvent(ToLowerAscii(event.provider), event.eventId) &&
        !gameExecutableLower.empty() &&
        ToLowerAscii(event.appName) == gameExecutableLower;
}

IncidentKind KindOf(const NormalizedEvent& event)
{
    return event.eventId == EventApplicationHang ? IncidentKind::Hang : IncidentKind::Crash;
}

std::string NormalizeWerCode(std::string value)
{
    value = ToLowerAscii(value);
    if (StartsWith(value, "0x"))
    {
        value.erase(0, 2);
    }
    const std::size_t firstNonZero = value.find_first_not_of('0');
    return firstNonZero == std::string::npos ? std::string{} : value.substr(firstNonZero);
}

std::string ToUpperAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character)
    {
        return static_cast<char>(std::toupper(character));
    });
    return value;
}

// The two shapes a graphics driver reset takes in the log: the display subsystem
// recovering one, and Windows Error Reporting filing a LiveKernelEvent for one.
// Both live here so the rule table and the preflight count the same events.
bool MatchesGpuDriverReset(EventChannel channel, const std::string& providerLower, std::uint32_t eventId,
                           const std::string& werEventName, const std::string& werP1)
{
    if (channel == EventChannel::System)
    {
        return providerLower == ProviderDisplay && eventId == EventDisplayTdrRecovered;
    }
    if (providerLower == ProviderWindowsErrorReporting && eventId == EventWindowsErrorReporting)
    {
        return ToLowerAscii(werEventName) == WerLiveKernelEvent &&
               EqualsAny(NormalizeWerCode(werP1), GpuLiveKernelEventCodes);
    }
    return false;
}



bool IsUnderGameDirectory(const std::string& pathLower, const std::string& gameDirectoryLower)
{
    return !gameDirectoryLower.empty() && StartsWith(pathLower, gameDirectoryLower);
}

bool IsUnderWindowsDirectory(const std::string& pathLower)
{
    return pathLower.size() > DriveLetterLength + WindowsDirectoryMarker.size() &&
        pathLower[1] == ':' &&
        pathLower.compare(DriveLetterLength, WindowsDirectoryMarker.size(), WindowsDirectoryMarker) == 0;
}

bool IsGpuDriverModule(const std::string& moduleLower, const std::string& pathLower)
{
    return pathLower.find(DriverStoreMarker) != std::string::npos ||
        StartsWithAny(moduleLower, GpuUserModeDriverPrefixes);
}

Classification ClassifyApplicationEvent(
    const NormalizedEvent& event,
    const Incident& incident,
    const std::string& gameDirectoryLower)
{
    const std::string provider = ToLowerAscii(event.provider);
    if (provider == ProviderWindowsErrorReporting && event.eventId == EventWindowsErrorReporting)
    {
        if (MatchesGpuDriverReset(EventChannel::Application, provider, event.eventId,
                                  event.werEventName, event.werP1))
        {
            return Make("GPU_TDR", "LIVE_KERNEL_EVENT_" + ToUpperAscii(NormalizeWerCode(event.werP1)),
                FindingLayer::GpuDriver, FindingStrength::Strong);
        }
        return {};
    }
    if (!IsApplicationFaultEvent(provider, event.eventId))
    {
        return {};
    }

    const bool crash = event.eventId == EventApplicationError;
    if (ToLowerAscii(event.appName) == DesktopWindowManager)
    {
        return Make("OTHER_PROCESS_FAULT", crash ? "DWM_CRASH" : "DWM_HANG",
            FindingLayer::OtherProcess, FindingStrength::Strong);
    }
    if (crash && incident.faultDetailsAvailable && !event.moduleName.empty())
    {
        const std::string gameModuleLower = ToLowerAscii(incident.faultModuleName);
        const std::string gameModulePathLower = ToLowerAscii(incident.faultModulePath);
        if (gameModuleLower != UnknownModuleName &&
            ToLowerAscii(event.moduleName) == gameModuleLower &&
            !IsUnderGameDirectory(gameModulePathLower, gameDirectoryLower))
        {
            return Make("OTHER_PROCESS_FAULT", "SAME_MODULE_CRASH",
                FindingLayer::OtherProcess, FindingStrength::Strong);
        }
    }
    // Weak: unrelated background applications crash on many healthy machines.
    return Make("OTHER_PROCESS_FAULT", crash ? "OTHER_APP_CRASH" : "OTHER_APP_HANG",
        FindingLayer::OtherProcess, FindingStrength::Weak);
}

bool ComputeProximity(const Incident& incident, UtcTicks time, bool reportedAfterFault, Proximity& proximity)
{
    if (incident.anchor == IncidentAnchor::None)
    {
        if (time >= incident.windowStart && time <= incident.windowEnd)
        {
            proximity = Proximity::Lookback;
            return true;
        }
        return false;
    }
    const UtcTicks nearEnd = reportedAfterFault ? incident.windowEnd : incident.nearEnd;
    if (time >= incident.nearStart && time <= nearEnd)
    {
        proximity = Proximity::Near;
        return true;
    }
    if (time >= incident.windowStart && time <= incident.windowEnd)
    {
        proximity = Proximity::Nearby;
        return true;
    }
    return false;
}

int StrengthRank(FindingStrength strength)
{
    switch (strength)
    {
    case FindingStrength::Strong: return 3;
    case FindingStrength::Medium: return 2;
    case FindingStrength::Weak: break;
    }
    return 1;
}

int ProximityRank(Proximity proximity)
{
    switch (proximity)
    {
    case Proximity::Near: return 3;
    case Proximity::Nearby: return 2;
    case Proximity::Lookback: break;
    }
    return 1;
}

int LayerRank(FindingLayer layer)
{
    switch (layer)
    {
    case FindingLayer::Hardware: return 8;
    case FindingLayer::SystemStability: return 7;
    case FindingLayer::GpuDriver: return 6;
    case FindingLayer::Storage: return 5;
    case FindingLayer::SystemPower: return 4;
    case FindingLayer::OtherProcess: return 3;
    case FindingLayer::SystemResource: return 2;
    case FindingLayer::ThirdParty: return 1;
    case FindingLayer::Game: break;
    }
    return 0;
}

std::int64_t AbsoluteDelta(const Finding& finding)
{
    return finding.deltaSeconds < 0 ? -finding.deltaSeconds : finding.deltaSeconds;
}

bool IsBetterOccurrence(const Finding& candidate, const Finding& existing)
{
    if (ProximityRank(candidate.proximity) != ProximityRank(existing.proximity))
    {
        return ProximityRank(candidate.proximity) > ProximityRank(existing.proximity);
    }
    if (candidate.hasDelta)
    {
        return AbsoluteDelta(candidate) < AbsoluteDelta(existing);
    }
    return candidate.time > existing.time;
}

bool RanksBefore(const Finding& left, const Finding& right)
{
    if (StrengthRank(left.strength) != StrengthRank(right.strength))
    {
        return StrengthRank(left.strength) > StrengthRank(right.strength);
    }
    if (ProximityRank(left.proximity) != ProximityRank(right.proximity))
    {
        return ProximityRank(left.proximity) > ProximityRank(right.proximity);
    }
    if (LayerRank(left.layer) != LayerRank(right.layer))
    {
        return LayerRank(left.layer) > LayerRank(right.layer);
    }
    return AbsoluteDelta(left) < AbsoluteDelta(right);
}

void MergeFinding(std::vector<Finding>& findings, Finding finding)
{
    for (auto& existing : findings)
    {
        if (existing.ruleId == finding.ruleId && existing.code == finding.code)
        {
            const std::uint32_t count = existing.count + 1;
            if (IsBetterOccurrence(finding, existing))
            {
                existing = std::move(finding);
            }
            existing.count = count;
            return;
        }
    }
    findings.push_back(std::move(finding));
}

const char* ChannelName(EventChannel channel)
{
    return channel == EventChannel::System ? "System" : "Application";
}

std::string BuildEvidence(const NormalizedEvent& event, const Classification& classification)
{
    std::string evidence = std::string(ChannelName(event.channel)) + "/" + event.provider + "/" +
        std::to_string(event.eventId);
    // Other application names are not reported, except the desktop compositor.
    if (classification.code == "DWM_CRASH" || classification.code == "DWM_HANG" ||
        classification.code == "SAME_MODULE_CRASH")
    {
        evidence += classification.code == "SAME_MODULE_CRASH" ? ":OTHER_PROCESS" : ":dwm.exe";
    }
    return evidence;
}

Incident ResolveIncident(
    const DiagnosisRequest& request,
    const std::vector<const NormalizedEvent*>& gameFaults,
    const NormalizedEvent*& anchorEvent)
{
    Incident incident;
    anchorEvent = nullptr;
    if (request.hasLauncherIncidentTime)
    {
        const UtcTicks time = request.launcherIncidentTime;
        incident.anchor = IncidentAnchor::Launcher;
        incident.kind = request.launcherIncidentKind;
        incident.time = time;
        incident.nearStart = SubtractClamped(time, LauncherNearBefore);
        incident.nearEnd = time + LauncherNearAfter;
        UtcTicks bestDistance = std::numeric_limits<UtcTicks>::max();
        for (const auto* event : gameFaults)
        {
            const UtcTicks distance = event->time > time ? event->time - time : time - event->time;
            if (distance <= LauncherFaultMatchTolerance && distance < bestDistance)
            {
                bestDistance = distance;
                anchorEvent = event;
            }
        }
        if (incident.kind == IncidentKind::Unknown && anchorEvent != nullptr)
        {
            incident.kind = KindOf(*anchorEvent);
        }
    }
    else
    {
        for (const auto* event : gameFaults)
        {
            if (event->time >= request.lookbackStart && event->time <= request.now &&
                (anchorEvent == nullptr || event->time > anchorEvent->time))
            {
                anchorEvent = event;
            }
        }
        if (anchorEvent != nullptr)
        {
            incident.anchor = IncidentAnchor::WindowsErrorReporting;
            incident.kind = KindOf(*anchorEvent);
            incident.time = anchorEvent->time;
            incident.nearStart = SubtractClamped(incident.time, WerNearBefore);
            incident.nearEnd = incident.time + WerNearAfter;
        }
    }

    if (incident.anchor != IncidentAnchor::None)
    {
        incident.windowStart = SubtractClamped(incident.time, NearbyBefore);
        incident.windowEnd = std::min(incident.time + NearbyAfter, request.now);
        incident.nearEnd = std::min(incident.nearEnd, request.now);
    }
    else
    {
        incident.windowStart = request.lookbackStart;
        incident.windowEnd = request.now;
    }

    if (anchorEvent != nullptr)
    {
        incident.faultModuleName = anchorEvent->moduleName;
        incident.faultModulePath = anchorEvent->modulePath;
        incident.exceptionCode = anchorEvent->exceptionCode;
        incident.faultDetailsAvailable = !anchorEvent->moduleName.empty();
    }
    return incident;
}

ObservabilityAssessment AssessObservability(
    const EventCollection& collection,
    const Incident& incident,
    DiagnosisResult& result)
{
    ObservabilityAssessment assessment;
    const Observability& observability = collection.observability;
    auto addIssue = [&result](const char* code)
    {
        result.observabilityIssues.emplace_back(code);
    };

    if (!observability.systemLogReadable)
    {
        addIssue("SYSTEM_LOG_UNREADABLE");
        assessment.fatal = true;
    }
    else
    {
        const bool anchored = incident.anchor != IncidentAnchor::None;
        const UtcTicks requiredStart = anchored ? incident.nearStart : incident.windowStart;
        if (!observability.systemOldestKnown)
        {
            addIssue("SYSTEM_LOG_OLDEST_UNKNOWN");
            assessment.downgrade = true;
        }
        else if (observability.systemOldest > requiredStart)
        {
            if (anchored)
            {
                addIssue("SYSTEM_LOG_NOT_COVERING_INCIDENT");
                assessment.fatal = true;
            }
            else
            {
                addIssue("SYSTEM_LOG_PARTIAL_LOOKBACK");
                assessment.downgrade = true;
            }
        }
        else if (anchored && observability.systemOldest > incident.windowStart)
        {
            addIssue("SYSTEM_LOG_PARTIAL_WINDOW");
            assessment.downgrade = true;
        }

        result.systemEventsInWindow = static_cast<std::uint64_t>(std::count_if(
            collection.systemEventTimes.begin(), collection.systemEventTimes.end(),
            [&incident](UtcTicks time)
            {
                return time >= incident.windowStart && time <= incident.windowEnd;
            }));
        if (result.systemEventsInWindow == 0)
        {
            addIssue("SYSTEM_LOG_SILENT_WINDOW");
            assessment.downgrade = true;
        }

        const bool cleared = std::any_of(collection.events.begin(), collection.events.end(),
            [&incident](const NormalizedEvent& event)
            {
                return event.channel == EventChannel::System &&
                    IsLogClearedEvent(ToLowerAscii(event.provider), event.eventId) &&
                    event.time >= incident.windowStart;
            });
        if (cleared)
        {
            addIssue("SYSTEM_LOG_CLEARED");
            assessment.downgrade = true;
        }
    }

    if (!observability.applicationLogReadable)
    {
        addIssue("APPLICATION_LOG_UNREADABLE");
        assessment.downgrade = true;
    }
    if (observability.budgetExceeded)
    {
        addIssue("SCAN_BUDGET_EXCEEDED");
    }
    if (observability.eventCapReached)
    {
        addIssue("SCAN_EVENT_CAP_REACHED");
    }
    if (observability.queryErrorOccurred)
    {
        addIssue("SCAN_QUERY_ERROR");
    }
    if (observability.budgetExceeded || observability.eventCapReached || observability.queryErrorOccurred)
    {
        // The incident window is scanned first. If it completed, the truncation
        // happened afterwards and only cost context, so the verdict still rests
        // on a fully scanned window and must not be weakened to "inconclusive".
        if (observability.priorityWindowRequested && observability.priorityWindowComplete)
        {
            addIssue("SCAN_CONTEXT_TRUNCATED");
        }
        else
        {
            assessment.scanIncomplete = true;
        }
    }
    if ((collection.rangeStart != 0 && collection.rangeStart > incident.windowStart) ||
        collection.rangeEnd < incident.windowEnd)
    {
        addIssue("COLLECTION_RANGE_INCOMPLETE");
        assessment.scanIncomplete = true;
    }

    result.observabilityOk = !assessment.fatal;
    return assessment;
}

void AddFaultModuleFinding(
    const Incident& incident,
    const std::string& gameDirectoryLower,
    const std::string& gameExecutableLower,
    std::vector<Finding>& findings)
{
    if (!incident.faultDetailsAvailable)
    {
        return;
    }
    const std::string moduleLower = ToLowerAscii(incident.faultModuleName);
    const std::string pathLower = ToLowerAscii(incident.faultModulePath);
    if (moduleLower.empty() || moduleLower == UnknownModuleName)
    {
        return;
    }

    Finding finding;
    finding.strength = FindingStrength::Weak;
    finding.proximity = Proximity::Near;
    finding.time = incident.time;
    finding.hasDelta = true;
    finding.deltaSeconds = 0;
    finding.evidence = "Application/Application Error/1000:" + incident.faultModuleName;
    if (IsGpuDriverModule(moduleLower, pathLower))
    {
        // Location only: a driver module on the stack does not tell who caused the fault.
        finding.ruleId = "FAULT_MODULE";
        finding.code = "FAULT_MODULE_GPU_DRIVER";
        finding.layer = FindingLayer::GpuDriver;
    }
    else if (IsUnderGameDirectory(pathLower, gameDirectoryLower) ||
        (pathLower.empty() && moduleLower == gameExecutableLower))
    {
        finding.ruleId = "FAULT_MODULE";
        finding.code = "FAULT_MODULE_GAME";
        finding.layer = FindingLayer::Game;
    }
    else if (!pathLower.empty() && !IsUnderWindowsDirectory(pathLower))
    {
        finding.ruleId = "FAULT_MODULE";
        finding.code = "FAULT_MODULE_THIRD_PARTY";
        finding.layer = FindingLayer::ThirdParty;
    }
    else
    {
        // Windows system modules (ntdll, KERNELBASE, d3d11...) are ambiguous; no finding.
        return;
    }
    findings.push_back(std::move(finding));
}

UserAction MakeAction(const char* code)
{
    struct ActionText
    {
        const char* code;
        const char* zh;
    };
    static const ActionText Actions[] = {
        {"UPDATE_GPU_DRIVER", "从显卡厂商官网下载并安装最新的显卡驱动"},
        {"CLEAN_INSTALL_GPU_DRIVER", "如问题依旧，使用驱动安装程序的“清洁安装”重新安装显卡驱动"},
        {"RESTORE_DEFAULT_CLOCKS", "如对显卡、处理器或内存做过超频，请恢复默认设置后再试"},
        {"CHECK_COOLING", "检查电脑散热是否良好（风扇、灰尘、温度）"},
        {"INSTALL_WINDOWS_UPDATES", "安装最新的 Windows 更新"},
        {"CONTACT_HARDWARE_VENDOR", "如问题频繁出现，建议联系电脑或硬件厂商检测"},
        {"CHECK_DISK_HEALTH", "检查游戏所在磁盘的健康状态"},
        {"REPAIR_GAME_FILES", "在启动器中修复游戏文件"},
        {"CHECK_POWER_SUPPLY", "检查电源和供电是否稳定"},
        {"AVOID_SLEEP_WHILE_PLAYING", "游戏时避免让电脑进入睡眠，可在系统电源设置中调整"},
        {"RESTART_COMPUTER", "重启电脑后再试"},
        {"CLOSE_BACKGROUND_SOFTWARE", "关闭不必要的后台程序，释放内存"},
        {"CLOSE_OVERLAY_SOFTWARE", "关闭录屏、悬浮窗、加速器等可能注入游戏的软件后再试"},
        {"SUBMIT_FEEDBACK", "提交问题反馈"},
    };
    for (const auto& action : Actions)
    {
        if (std::string(action.code) == code)
        {
            return {action.code, action.zh};
        }
    }
    return {code, ""};
}

std::vector<const char*> ActionCodesFor(FindingLayer layer)
{
    switch (layer)
    {
    case FindingLayer::GpuDriver:
        return {"UPDATE_GPU_DRIVER", "CLEAN_INSTALL_GPU_DRIVER", "RESTORE_DEFAULT_CLOCKS", "CHECK_COOLING"};
    case FindingLayer::Hardware:
        return {"RESTORE_DEFAULT_CLOCKS", "CHECK_COOLING", "INSTALL_WINDOWS_UPDATES", "CONTACT_HARDWARE_VENDOR"};
    case FindingLayer::Storage:
        return {"CHECK_DISK_HEALTH", "REPAIR_GAME_FILES"};
    case FindingLayer::SystemStability:
        return {"INSTALL_WINDOWS_UPDATES", "UPDATE_GPU_DRIVER", "RESTORE_DEFAULT_CLOCKS", "CHECK_POWER_SUPPLY"};
    case FindingLayer::SystemPower:
        return {"AVOID_SLEEP_WHILE_PLAYING"};
    case FindingLayer::OtherProcess:
        return {"RESTART_COMPUTER", "INSTALL_WINDOWS_UPDATES"};
    case FindingLayer::SystemResource:
        return {"CLOSE_BACKGROUND_SOFTWARE"};
    case FindingLayer::ThirdParty:
        return {"CLOSE_OVERLAY_SOFTWARE"};
    case FindingLayer::Game:
        break;
    }
    return {};
}

void SetActions(DiagnosisResult& result, const Finding* primary)
{
    if (primary != nullptr)
    {
        for (const char* code : ActionCodesFor(primary->layer))
        {
            result.actions.push_back(MakeAction(code));
        }
    }
    result.actions.push_back(MakeAction("SUBMIT_FEEDBACK"));
}

const char* CauseMessageCode(FindingLayer layer)
{
    switch (layer)
    {
    case FindingLayer::GpuDriver: return "SYS_GPU_DRIVER_RESET";
    case FindingLayer::Hardware: return "SYS_HARDWARE_ERROR";
    case FindingLayer::Storage: return "SYS_STORAGE_ERROR";
    case FindingLayer::SystemStability: return "SYS_BUGCHECK";
    case FindingLayer::OtherProcess: return "SYS_MULTI_APP_FAULT";
    default: break;
    }
    return "SYS_ISSUE";
}

const char* CauseMessageText(FindingLayer layer)
{
    switch (layer)
    {
    case FindingLayer::GpuDriver:
        return "检测到游戏出现问题的同一时间，显卡驱动发生了异常（被系统重置或报错）。这类问题通常与显卡驱动或显卡工作状态有关。";
    case FindingLayer::Hardware:
        return "检测到游戏出现问题的同一时间，系统记录到硬件错误。这类问题通常与处理器、内存、主板或相关固件设置有关。";
    case FindingLayer::Storage:
        return "检测到游戏出现问题的同一时间，系统记录到磁盘读写错误，可能导致游戏文件读取失败。";
    case FindingLayer::SystemStability:
        return "检测到游戏出现问题的同一时间，电脑发生了系统级崩溃（蓝屏）并重新启动。";
    case FindingLayer::OtherProcess:
        return "检测到游戏出现问题的同一时间，系统桌面组件或其他程序也发生了同类异常，说明系统当时可能处于不稳定状态。";
    default:
        break;
    }
    return "检测到游戏出现问题的同一时间，系统记录到异常。";
}

const char* LayerMessageCode(FindingLayer layer)
{
    switch (layer)
    {
    case FindingLayer::GpuDriver: return "GPU_DRIVER";
    case FindingLayer::Hardware: return "HARDWARE";
    case FindingLayer::Storage: return "STORAGE";
    case FindingLayer::SystemStability: return "SYSTEM_STABILITY";
    case FindingLayer::SystemPower: return "SYSTEM_POWER";
    case FindingLayer::OtherProcess: return "OTHER_PROCESS";
    case FindingLayer::SystemResource: return "SYSTEM_RESOURCE";
    case FindingLayer::ThirdParty: return "THIRD_PARTY";
    case FindingLayer::Game: break;
    }
    return "UNKNOWN";
}

const char* LayerPhrase(FindingLayer layer)
{
    switch (layer)
    {
    case FindingLayer::GpuDriver: return "系统记录到显卡驱动异常";
    case FindingLayer::Hardware: return "系统记录到硬件错误";
    case FindingLayer::Storage: return "系统记录到磁盘读写异常";
    case FindingLayer::SystemStability: return "电脑发生过蓝屏或非正常关机";
    case FindingLayer::SystemPower: return "电脑进入过睡眠或从睡眠中恢复";
    case FindingLayer::OtherProcess: return "其他程序也发生过崩溃或无响应";
    case FindingLayer::SystemResource: return "系统出现过内存不足";
    case FindingLayer::ThirdParty: return "崩溃发生在第三方软件的模块中";
    case FindingLayer::Game: break;
    }
    return "系统记录到异常";
}

std::string TimeScopePrefix(bool anchored, std::uint32_t lookbackHours)
{
    if (anchored)
    {
        return "在游戏出现问题前后的一段时间内，";
    }
    return "最近 " + std::to_string(lookbackHours) + " 小时内，";
}

const std::string FeedbackSuffix = "建议提交问题反馈，我们会结合游戏日志进一步分析。";

void DecideVerdict(
    const DiagnosisRequest& request,
    const ObservabilityAssessment& assessment,
    DiagnosisResult& result)
{
    const bool anchored = result.incident.anchor != IncidentAnchor::None;
    const Finding* primary = nullptr;
    bool strongNear = false;
    bool mediumOrStrong = false;
    for (const auto& finding : result.findings)
    {
        if (finding.layer == FindingLayer::Game)
        {
            continue;
        }
        if (primary == nullptr)
        {
            primary = &finding;
        }
        if (finding.strength == FindingStrength::Strong && finding.proximity == Proximity::Near)
        {
            strongNear = true;
        }
        if (finding.strength != FindingStrength::Weak)
        {
            mediumOrStrong = true;
        }
    }

    if (assessment.fatal)
    {
        result.verdict = Verdict::Inconclusive;
        result.confidence = Confidence::Low;
        result.userMessageCode = "SYSTEM_CHECK_UNAVAILABLE";
        result.userMessageZh = "无法读取问题发生时段的系统记录（系统日志不可用、已被清理或已被覆盖），"
            "暂时无法判断是否与电脑系统有关。建议提交问题反馈。";
        SetActions(result, nullptr);
        return;
    }
    if (anchored && strongNear)
    {
        result.verdict = Verdict::SystemCauseLikely;
        result.confidence = result.incident.anchor == IncidentAnchor::Launcher ? Confidence::High : Confidence::Medium;
        result.userMessageCode = CauseMessageCode(primary->layer);
        result.userMessageZh = CauseMessageText(primary->layer);
        SetActions(result, primary);
        return;
    }
    if (mediumOrStrong)
    {
        result.verdict = Verdict::SystemIssuesNearby;
        const bool closeEnough = anchored &&
            (primary->proximity == Proximity::Near || primary->strength == FindingStrength::Strong);
        result.confidence = closeEnough ? Confidence::Medium : Confidence::Low;
        result.userMessageCode = std::string("NEARBY_") + LayerMessageCode(primary->layer);
        result.userMessageZh = TimeScopePrefix(anchored, request.lookbackHours) + LayerPhrase(primary->layer);
        // Without an incident time the record may be days away from the problem.
        // Saying it "may be related" then reads as a diagnosis the data does not support.
        result.userMessageZh += closeEnough
            ? "，可能与你遇到的问题有关。"
            : "。该记录不在问题发生的时间点附近，无法确定与本次问题有关。" + FeedbackSuffix;
        SetActions(result, primary);
        return;
    }
    if (assessment.scanIncomplete)
    {
        result.verdict = Verdict::Inconclusive;
        result.confidence = Confidence::Low;
        result.userMessageCode = "SYSTEM_CHECK_INCOMPLETE";
        result.userMessageZh = "系统检测未能完整完成，暂时无法给出判断。建议稍后重试，或直接提交问题反馈。";
        SetActions(result, nullptr);
        return;
    }
    if (primary != nullptr)
    {
        result.verdict = Verdict::Inconclusive;
        result.confidence = Confidence::Low;
        result.userMessageCode = "WEAK_SIGNALS";
        result.userMessageZh = std::string("发现了少量可能相关的线索（") + LayerPhrase(primary->layer) +
            "），但不足以判断问题是否由电脑系统引起。" + FeedbackSuffix;
        SetActions(result, primary);
        return;
    }

    result.verdict = Verdict::NoSystemEvidence;
    result.confidence = anchored && !assessment.downgrade ? Confidence::Medium : Confidence::Low;
    result.userMessageCode = "NO_SYSTEM_EVIDENCE";
    result.userMessageZh = anchored
        ? "在游戏出现问题的时段内，未发现电脑系统层面的异常记录。" + FeedbackSuffix
        : "最近 " + std::to_string(request.lookbackHours) + " 小时内未发现电脑系统层面的异常记录。" + FeedbackSuffix;
    SetActions(result, nullptr);
}
} // namespace

bool IsGpuDriverResetEvent(const NormalizedEvent& event)
{
    // MatchesGpuDriverReset has internal linkage, which is fine: it is the same
    // translation unit the rule table above lives in, so the preflight and the
    // GPU_TDR finding are counting from one definition.
    return MatchesGpuDriverReset(event.channel, ToLowerAscii(event.provider), event.eventId,
                                 event.werEventName, event.werP1);
}

bool IsRelevantEvent(EventChannel channel, const std::string& providerLower, std::uint32_t eventId, std::uint32_t level)
{
    if (channel == EventChannel::System)
    {
        return ClassifySystemEvent(providerLower, eventId, level).matched ||
            IsLogClearedEvent(providerLower, eventId);
    }
    return IsApplicationFaultEvent(providerLower, eventId) ||
        (providerLower == ProviderWindowsErrorReporting && eventId == EventWindowsErrorReporting);
}

DiagnosisResult Diagnose(const DiagnosisRequest& request, const EventCollection& collection)
{
    DiagnosisResult result;
    result.observability = collection.observability;
    const std::string gameExecutableLower = ToLowerAscii(request.gameExecutableName);

    std::vector<const NormalizedEvent*> gameFaults;
    for (const auto& event : collection.events)
    {
        if (IsGameFaultEvent(event, gameExecutableLower))
        {
            gameFaults.push_back(&event);
        }
    }

    const NormalizedEvent* anchorEvent = nullptr;
    result.incident = ResolveIncident(request, gameFaults, anchorEvent);
    const Incident& incident = result.incident;
    const std::string gameDirectoryLower =
        anchorEvent != nullptr ? LowerDirectoryOf(anchorEvent->appPath) : std::string{};

    const ObservabilityAssessment assessment = AssessObservability(collection, incident, result);

    std::vector<Finding> findings;
    for (const auto& event : collection.events)
    {
        if (IsGameFaultEvent(event, gameExecutableLower))
        {
            continue;
        }
        const Classification classification = event.channel == EventChannel::System
            ? ClassifySystemEvent(ToLowerAscii(event.provider), event.eventId, event.level)
            : ClassifyApplicationEvent(event, incident, gameDirectoryLower);
        if (!classification.matched)
        {
            continue;
        }
        Proximity proximity = Proximity::Lookback;
        if (!ComputeProximity(incident, event.time, classification.reportedAfterFault, proximity))
        {
            continue;
        }

        Finding finding;
        finding.ruleId = classification.ruleId;
        finding.code = classification.code;
        finding.layer = classification.layer;
        finding.strength = classification.strength;
        finding.proximity = proximity;
        finding.time = event.time;
        finding.hasDelta = incident.anchor != IncidentAnchor::None;
        finding.deltaSeconds = finding.hasDelta ? DeltaSeconds(event.time, incident.time) : 0;
        finding.evidence = BuildEvidence(event, classification);
        MergeFinding(findings, std::move(finding));
    }
    AddFaultModuleFinding(incident, gameDirectoryLower, gameExecutableLower, findings);

    std::stable_sort(findings.begin(), findings.end(), RanksBefore);
    if (findings.size() > MaxReportedFindings)
    {
        findings.resize(MaxReportedFindings);
    }
    result.findings = std::move(findings);

    DecideVerdict(request, assessment, result);
    return result;
}
} // namespace zengine::envcheck::diagnosis
