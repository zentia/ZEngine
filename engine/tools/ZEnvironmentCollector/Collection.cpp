#include "Collection.h"
#include "AttributionRules.h"
#include "DiagnosisReport.h"
#include "DiagnosisUtil.h"
#include "EnvCheckLog.h"
#include "DllProbe.h"
#include "EnvironmentCheckCommand.h"
#include "EnvironmentProbe.h"
#include "EvidenceFiles.h"
#include "PeInspector.h"
#include "HardwareProbe.h"
#include "EventLogReader.h"
#include "ReportWriter.h"
#include "ToolVersion.h"
#include <Windows.h>
#include <KnownFolders.h>
#include <ShlObj.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace zengine::envcheck::collector
{
using namespace diagnosis;
std::string JsonString(const std::string& value)
{
    std::string out = "\"";
    const char* hex = "0123456789abcdef";
    for (unsigned char c : value)
    {
        if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c < 32) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
        else out += static_cast<char>(c);
    }
    return out + '"';
}
AttributionAnchor DescribeAttributionAnchor(const Options& options)
{
    AttributionAnchor anchor;
    anchor.incidentTime = options.incidentTime != 0;
    anchor.incidentKind = options.incidentKind != IncidentKind::Unknown;
    // The placeholder counts as "not specified": it only ever reaches a stage
    // worker on the command line, and treating it as a name would turn "we were
    // never told which game" into "a game was named".
    anchor.gameExecutable = !options.profile.gameExecutable.empty()
        && options.profile.gameExecutable != UnspecifiedGamePlaceholder;
    return anchor;
}

std::string BuildAttributionWarning(const AttributionAnchor& anchor)
{
    if (anchor.Usable()) return {};
    std::vector<std::string> missing;
    if (!anchor.incidentTime) missing.push_back("事故发生时间（manifest 的 incidentTimeUtc）");
    if (!anchor.incidentKind) missing.push_back("现象类型：崩溃还是卡死（manifest 的 incidentKind）");
    if (!anchor.gameExecutable) missing.push_back("游戏 exe 名（profile 的 gameExecutable）");
    std::ostringstream out;
    out << "================================================================\n"
        << "警告：本包缺少事故锚点，不能用于事故归因。\n"
        << "它只能回答「这台机器近期系统层面有什么」，不能回答「是不是游戏导致的」，\n"
        << "同样也不能用来证明「与游戏无关」。\n"
        << "缺失项：";
    for (std::size_t index = 0; index < missing.size(); ++index)
    {
        if (index) out << "、";
        out << missing[index];
    }
    out << "\n补齐以上信息后重新采集，这份包才具备归因价值。\n"
        << "================================================================\n\n";
    return out.str();
}

std::string BuildAttributionManifestFields(const AttributionAnchor& anchor)
{
    std::vector<std::string> missing;
    if (!anchor.incidentTime) missing.push_back("incidentTimeUtc");
    if (!anchor.incidentKind) missing.push_back("incidentKind");
    if (!anchor.gameExecutable) missing.push_back("gameExecutable");
    std::ostringstream out;
    out << "\"attributionUsable\":" << (anchor.Usable() ? "true" : "false") << ",\"missingAttributionInputs\":[";
    for (std::size_t index = 0; index < missing.size(); ++index)
    {
        if (index) out << ',';
        out << JsonString(missing[index]);
    }
    return out.str() + ']';
}

std::filesystem::path ExecutableDirectory()
{
    std::vector<wchar_t> buffer(32768);
    const DWORD count = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (!count || count == buffer.size()) throw std::runtime_error("Executable path unavailable");
    return std::filesystem::path(std::wstring(buffer.data(), count)).parent_path();
}
std::filesystem::path ToolExecutablePath()
{
    return ExecutableDirectory() / ToolExecutableName;
}
std::filesystem::path DefaultOutputDirectory()
{
    PWSTR path = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &path)))
        throw std::runtime_error("Desktop directory unavailable; specify --output-dir");
    std::filesystem::path result = std::filesystem::path(path) / L"EnvironmentReports";
    CoTaskMemFree(path);
    return result;
}
// Closes on scope exit. Outside the anonymous namespace below because the log
// copy it belongs to is exported for tests.
struct Handle
{
    HANDLE h = nullptr;
    ~Handle() { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); }
};
namespace
{
bool SafeName(const std::string& name)
{
    return !name.empty() && name.size() <= 80 && name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") == std::string::npos
        && name != "." && name != "..";
}
// A profile-declared game log path: relative, no drive, no "..", no empty segment.
bool SafeRelativePath(const std::string& value)
{
    if (value.empty() || value.size() > 200 || value.front() == '\\' || value.front() == '/') return false;
    std::string current;
    auto reject = [&current]() {
        bool bad = current.empty() || current == "." || current == ".." ||
            current.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._- ") != std::string::npos;
        current.clear();
        return bad;
    };
    for (char c : value)
    {
        if (c == '\\' || c == '/') { if (reject()) return false; continue; }
        current += c;
    }
    return !reject();
}
std::filesystem::path KnownFolderPath(const KNOWNFOLDERID& id)
{
    PWSTR path = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &path))) return {};
    std::filesystem::path result = path;
    CoTaskMemFree(path);
    return result;
}
unsigned Number(const std::wstring& value, unsigned minimum, unsigned maximum)
{
    if (value.empty() || value.find_first_not_of(L"0123456789") != std::wstring::npos)
        throw std::invalid_argument("Expected unsigned integer");
    auto number = std::stoul(value);
    if (number < minimum || number > maximum) throw std::invalid_argument("Integer outside supported range");
    return static_cast<unsigned>(number);
}
std::wstring Quote(const std::wstring& value)
{
    std::wstring out = L"\"";
    unsigned slash = 0;
    for (auto c : value)
    {
        if (c == L'\\') { ++slash; continue; }
        out.append(c == L'"' ? slash * 2 + 1 : slash, L'\\');
        out += c; slash = 0;
    }
    out.append(slash * 2, L'\\');
    return out + L'"';
}
// FindFirstFile handles must be released with FindClose, not CloseHandle.
struct FindHandle
{
    HANDLE h = INVALID_HANDLE_VALUE;
    ~FindHandle() { if (h != INVALID_HANDLE_VALUE) FindClose(h); }
};
// Back to the spelling --require takes, so a stage worker measures against the
// same minimums its caller was given.
std::string RequiredFeaturesText(const Requirements& requirements)
{
    std::string text;
    if (requirements.avx) text += "avx,";
    if (requirements.avx2) text += "avx2,";
    if (requirements.fma) text += "fma,";
    if (requirements.f16c) text += "f16c,";
    if (!text.empty()) text.pop_back();
    return text.empty() ? "none" : text;
}
std::string RunStageProcess(const Options& options, const std::filesystem::path& logDirectory,
                           const std::filesystem::path& directory,
                           const std::string& stage, const std::atomic_bool& cancel)
{
    const auto exe = ToolExecutablePath();
    if (!std::filesystem::is_regular_file(exe)) return "HELPER_MISSING";
    std::wstring command = Quote(exe.wstring()) + L" --collect-stage " + Utf8ToWide(stage)
        + L" --output-dir " + Quote(directory.wstring()) + L" --lookback-hours " + std::to_wstring(options.profile.lookbackHours)
        + L" --require " + Utf8ToWide(RequiredFeaturesText(options.requirements))
        + L" --min-memory-gb " + std::to_wstring(options.requirements.minimumMemoryBytes / (1024ull * 1024ull * 1024ull))
        + L" --game-exe " + Quote(Utf8ToWide(options.profile.gameExecutable.empty() ? UnspecifiedGamePlaceholder : options.profile.gameExecutable))
        + L" --incident-kind " + Utf8ToWide(options.incidentKind == IncidentKind::Crash ? "crash" : options.incidentKind == IncidentKind::Hang ? "hang" : "unknown");
    if (!options.gameDirectory.empty()) command += L" --game-dir " + Quote(options.gameDirectory.wstring());
    if (options.incidentTime) command += L" --incident-time " + Quote(Utf8ToWide(FormatUtcIso8601(options.incidentTime)));
    if (options.includeEvents) command += L" --include-events";
    if (!logDirectory.empty()) command += L" --log-dir " + Quote(logDirectory.wstring());
    Handle job{CreateJobObjectW(nullptr, nullptr)};
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job.h || !SetInformationJobObject(job.h, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        return "JOB_SETUP_FAILED";
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                        nullptr, ExecutableDirectory().c_str(), &startup, &process))
        return "HELPER_START_FAILED_" + std::to_string(GetLastError());
    Handle processHandle{process.hProcess}, threadHandle{process.hThread};
    if (!AssignProcessToJobObject(job.h, process.hProcess))
    {
        TerminateProcess(process.hProcess, 30); WaitForSingleObject(process.hProcess, 5000);
        return "JOB_ASSIGN_FAILED";
    }
    if (ResumeThread(process.hThread) == static_cast<DWORD>(-1))
    {
        TerminateJobObject(job.h, 30); WaitForSingleObject(process.hProcess, 5000); return "HELPER_RESUME_FAILED";
    }
    // The compatibility stage walks a directory and launches one isolated process
    // per GameCore DLL, so the shared per-stage budget is a floor for it rather
    // than a ceiling: 45 s is not enough for a real game directory, and being
    // killed halfway would lose the whole section rather than shorten it.
    const auto timeoutSeconds = stage == "compatibility"
        ? (std::max)(options.stageTimeoutSeconds, 180u) : options.stageTimeoutSeconds;
    auto deadline = GetTickCount64() + timeoutSeconds * 1000ull;
    for (;;)
    {
        DWORD wait = WaitForSingleObject(process.hProcess, 100);
        if (wait == WAIT_OBJECT_0)
        {
            DWORD code = 30;
            if (!GetExitCodeProcess(process.hProcess, &code)) return "EXIT_CODE_UNAVAILABLE";
            // 3 = the stage ran but had nothing to collect (not a failure of the machine or the tool).
            return code == 0 ? "OK" : code == 2 ? "INCOMPLETE" : code == 3 ? "NO_LOGS" : "HELPER_FAILED_" + std::to_string(code);
        }
        if (cancel.load() || GetTickCount64() >= deadline || wait == WAIT_FAILED)
        {
            TerminateJobObject(job.h, 30); WaitForSingleObject(process.hProcess, 5000);
            return cancel.load() ? "CANCELLED" : wait == WAIT_FAILED ? "WAIT_FAILED" : "TIMEOUT";
        }
    }
}
std::string EnvironmentJson(const EnvironmentSnapshot& s)
{
    std::ostringstream out;
    out << "{\"schemaVersion\":1,\"toolVersion\":" << JsonString(ToolVersion)
        << ",\"cpu\":{\"brand\":" << JsonString(s.cpu.brand) << ",\"vendor\":" << JsonString(s.cpu.vendor)
        << ",\"family\":" << s.cpu.family << ",\"model\":" << s.cpu.model << ",\"stepping\":" << s.cpu.stepping
        << ",\"logicalProcessors\":" << s.cpu.logicalProcessorCount << ",\"avx\":" << (s.cpu.avx ? "true" : "false")
        << ",\"avx2\":" << (s.cpu.avx2 ? "true" : "false") << "},\"system\":{\"windows\":" << JsonString(s.system.windowsVersion)
        << ",\"physicalMemoryBytes\":" << s.system.physicalMemoryBytes << ",\"availablePhysicalMemoryBytes\":" << s.system.availablePhysicalMemoryBytes
        << ",\"commitLimitBytes\":" << s.system.totalPageFileBytes << ",\"commitAvailableBytes\":" << s.system.availablePageFileBytes
        << ",\"uptimeMilliseconds\":" << s.system.uptimeMilliseconds << "},\"gpus\":[";
    bool first = true;
    for (const auto& g : s.gpuAdapters)
    {
        if (!first) out << ','; first = false;
        out << "{\"name\":" << JsonString(g.name) << ",\"driverVersion\":" << JsonString(g.driverVersion)
            << ",\"driverDate\":" << JsonString(g.driverDate) << ",\"vendorId\":" << g.vendorId << ",\"deviceId\":" << g.deviceId
            << ",\"dedicatedVideoMemoryBytes\":" << g.dedicatedVideoMemoryBytes << '}';
    }
    out << "],\"displays\":["; first = true;
    for (const auto& d : s.displays)
    {
        if (!first) out << ','; first = false;
        out << "{\"width\":" << d.width << ",\"height\":" << d.height << ",\"refreshRate\":" << d.refreshRate << '}';
    }
    out << "],\"disks\":["; first = true;
    for (const auto& d : s.physicalDisks)
    {
        if (!first) out << ','; first = false;
        out << "{\"model\":" << JsonString(d.model) << ",\"busType\":" << JsonString(d.busType) << ",\"capacityBytes\":" << d.capacityBytes << '}';
    }
    out << "],\"volumes\":["; first = true;
    for (const auto& v : s.volumes)
    {
        if (!first) out << ','; first = false;
        out << "{\"fileSystem\":" << JsonString(v.fileSystem) << ",\"totalBytes\":" << v.totalBytes << ",\"availableBytes\":" << v.availableBytes << '}';
    }
    out << "],\"note\":\"Only this stage's fields are populated; empty/default values in other sections are not measurements.\"}";
    return out.str();
}
bool IsGameCoreDllName(const std::filesystem::path& path)
{
    const auto name = ToLowerAscii(WideToUtf8(path.filename().wstring()));
    return name == "gamecore.dll" || name == "gamecore_standalone.dll";
}
int RunStage(const std::string& stage, const Options& options)
{
    bool complete = true;
    std::filesystem::create_directories(options.outputDirectory);
    if (stage == "basic")
    {
        const auto snapshot = CollectEnvironment(false);
        complete = snapshot.system.physicalMemoryBytes != 0 && !snapshot.cpu.brand.empty();
        WriteReportFile(options.outputDirectory / "basic.json", EnvironmentJson(snapshot));
    }
    else if (stage == "graphics" || stage == "storage")
    {
        EnvironmentSnapshot snapshot;
        if (stage == "graphics")
        {
            CollectGraphicsInformation(snapshot);
            complete = !snapshot.gpuAdapters.empty();
        }
        else
        {
            CollectStorageInformation(snapshot);
            complete = !snapshot.physicalDisks.empty() && !snapshot.volumes.empty();
        }
        WriteReportFile(options.outputDirectory / (stage + ".json"), EnvironmentJson(snapshot));
    }
    else if (stage == "gpu-selection")
    {
        // Dual-GPU auto-select verification: the offline mock regression cases
        // (pure arithmetic, always available) plus the real DXGI probe on this
        // machine. Deliberately its own stage instead of a field on
        // "graphics": the mocks say whether the shipped heuristic in
        // GfxDeviceSetup.cpp still behaves, which is a different question from
        // "what hardware is installed", and a graphics-stage failure must not
        // silently drop it from the package.
        //
        // The JSON comes from the same serializer the compatibility report
        // uses, so a collected ZIP and a -batchmode report always agree.
        const auto mockResults = RunAllGpuSelectionMockCases();
        const auto realResult = ProbeRealGpuSelection();
        complete = !mockResults.empty() && !realResult.allAdapters.empty();
        std::ostringstream out;
        out << "{\"schemaVersion\":1,\"toolVersion\":" << JsonString(ToolVersion)
            << ",\"gpuAutoSelection\":{\n" << BuildGpuSelectionJson(mockResults, realResult) << "\n}}\n";
        WriteReportFile(options.outputDirectory / "gpu-selection.json", out.str());
    }
    else if (stage == "compatibility")
    {
        // What the compatibility tab used to produce, now inside the collected
        // package: the static PE checks and the isolated load probe, next to the
        // hardware snapshot they are judged against. Same CollectEnvironment()
        // and BuildJsonReport() the -batchmode report uses, so the two paths
        // cannot drift into disagreeing about one machine.
        auto snapshot = CollectEnvironment();
        const auto targets = ResolveDllCheckTargets(options.gameDirectory);
        for (const auto& dll : targets.inspect) snapshot.inspectedImages.push_back(InspectPeImage(dll));
        bool truncated = targets.truncated;
        if (!targets.probe.empty())
        {
            DllProbeOptions probeOptions;
            probeOptions.probeExecutable = ExecutableDirectory() / ProbeExecutableName;
            probeOptions.timeoutMilliseconds = 60000; // matches the -batchmode default
            for (const auto& dll : targets.probe)
            {
                probeOptions.exportName = IsGameCoreDllName(dll) ? "OSG_LocalPredictHardwareCapable" : std::string();
                snapshot.dllProbeResults.push_back(RunDllProbe(dll, probeOptions));
            }
        }
        const auto evaluation = EvaluateEnvironment(snapshot, options.requirements);
        auto json = BuildJsonReport(snapshot, options.requirements, evaluation);
        // The tail of the list is the part that is least likely to matter, so
        // that is what goes when the document gets too big: one file in the
        // archive is capped at 16 MiB and exceeding it fails the whole package.
        while (json.size() > MaxDllCheckJsonBytes && !snapshot.inspectedImages.empty())
        {
            snapshot.inspectedImages.pop_back();
            truncated = true;
            json = BuildJsonReport(snapshot, options.requirements, evaluation);
        }
        // A capped list is not a complete one, and the manifest says so: the
        // package still packs, but nobody should read it as "every DLL here is
        // fine".
        complete = !truncated;
        WriteReportFile(options.outputDirectory / "compatibility.json", json);
    }
    else if (stage == "firmware") WriteReportFile(options.outputDirectory / "firmware.json", CollectFirmwareJson(complete));
    else if (stage == "health") WriteReportFile(options.outputDirectory / "health-events.json", CollectHealthEventsJson(options.profile.lookbackHours, complete));
    else if (stage == "events")
    {
        auto now = CurrentUtcTime();
        EventLogReadOptions read;
        read.rangeEnd = now;
        read.rangeStart = SubtractClamped(now, options.profile.lookbackHours * TicksPerHour);
        if (options.incidentTime)
        {
            // Read the incident window before the rest of the lookback: a noisy
            // machine fills the event cap long before the scan gets there.
            read.hasPriorityWindow = true;
            read.priorityStart = SubtractClamped(options.incidentTime, IncidentWindowMargin);
            read.priorityEnd = std::min(options.incidentTime + IncidentWindowMargin, now);
            read.rangeStart = std::min(read.rangeStart, read.priorityStart);
        }
        read.deadlineTickCount = GetTickCount64() + 20000;
        auto events = ReadEventLogs(read);
        DiagnosisRequest request;
        request.now = now; request.lookbackStart = read.rangeStart; request.lookbackHours = options.profile.lookbackHours;
        request.gameExecutableName = options.profile.gameExecutable;
        request.hasLauncherIncidentTime = options.incidentTime != 0; request.launcherIncidentTime = options.incidentTime;
        request.launcherIncidentKind = options.incidentKind;
        auto result = Diagnose(request, events);
        complete = result.observabilityOk && !events.observability.budgetExceeded && !events.observability.eventCapReached && !events.observability.queryErrorOccurred;
        DiagnosisReportContext context;
        context.source = "collector"; context.generatedAt = now; context.gameExecutable = options.profile.gameExecutable;
        context.lookbackHours = options.profile.lookbackHours;
        WriteReportFile(options.outputDirectory / "diagnosis.json", BuildDiagnosisJson(result, context));
        if (options.includeEvents)
            WriteReportFile(options.outputDirectory / "events-redacted.json",
                BuildRedactedEventsJson(events.events, DescribeAttributionAnchor(options).gameExecutable,
                    options.profile.gameExecutable));
    }
    else if (stage == "gamelogs")
    {
        const auto logs = CollectGameLogs(options.logDirectory, options.outputDirectory, options.profile.lookbackHours, options.incidentTime);
        WriteReportFile(options.outputDirectory / "game-logs.json", logs.json);
        return logs.status == GameLogStatus::Ok ? 0 : 3;
    }
    else throw std::invalid_argument("Unknown collection stage");
    return complete ? 0 : 2;
}
struct LogCandidate
{
    std::filesystem::path path;
    std::uint64_t bytes = 0;
    diagnosis::UtcTicks modified = 0;
};
diagnosis::UtcTicks FileTimeToTicks(const FILETIME& value)
{
    return (static_cast<diagnosis::UtcTicks>(value.dwHighDateTime) << 32) | value.dwLowDateTime;
}
bool IsLogExtension(const std::filesystem::path& name)
{
    const auto extension = ToLowerAscii(WideToUtf8(name.extension().wstring()));
    // ".logcompr" is the compressed per-session log the game writes into Cache\Logs;
    // skipping it would leave the report without the game's own record.
    return extension == ".log" || extension == ".logcompr" || extension == ".txt";
}
void ScanLogFiles(const std::filesystem::path& directory, int depth, std::vector<LogCandidate>& out)
{
    WIN32_FIND_DATAW data{};
    FindHandle find{FindFirstFileW((directory.wstring() + L"\\*").c_str(), &data)};
    if (find.h == INVALID_HANDLE_VALUE) return;
    do
    {
        const std::wstring name = data.cFileName;
        const bool isLink = (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        {
            // A link can point anywhere: another volume, the whole system drive, or
            // back up the tree. Recursing into one would package logs that are not
            // the game's, into a report someone sends, behind a relativePath that
            // quietly claims they came from the game's own folder. Depth alone does
            // not save us -- one level of a junction onto C: is already the machine.
            if (!isLink && depth + 1 < MaxGameLogScanDepth && name != L"." && name != L"..")
                ScanLogFiles(directory / name, depth + 1, out);
            continue;
        }
        // A link inside the log folder could point anywhere; never follow it.
        if (isLink) continue;
        std::filesystem::path file = directory / name;
        if (!IsLogExtension(name)) continue;
        const auto bytes = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        if (bytes == 0) continue;
        out.push_back({std::move(file), bytes, FileTimeToTicks(data.ftLastWriteTime)});
    } while (FindNextFileW(find.h, &data));
}
std::string ArchiveLogName(std::uint32_t index, const std::filesystem::path& source)
{
    std::string name = WideToUtf8(source.filename().wstring());
    for (char& c : name)
        if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') && !(c >= '0' && c <= '9') && c != '.' && c != '_' && c != '-') c = '_';
    if (!name.empty() && name.front() == '.') name.front() = '_';
    for (size_t at = name.find(".."); at != std::string::npos; at = name.find("..", at)) name.replace(at, 2, "__");
    if (name.empty()) name = "log";
    if (name.size() > 96) name.erase(96);
    char digits[3] = {static_cast<char>('0' + index / 10), static_cast<char>('0' + index % 10), '\0'};
    return std::string(GameLogNamePrefix) + digits + "-" + name;
}
std::string RelativeLogPath(const std::filesystem::path& file, const std::filesystem::path& root)
{
    const auto text = file.wstring(), base = root.wstring();
    if (text.size() > base.size() + 1 && text.compare(0, base.size(), base) == 0 && (text[base.size()] == L'\\' || text[base.size()] == L'/'))
        return WideToUtf8(text.substr(base.size() + 1));
    return WideToUtf8(file.filename().wstring());
}
}
// Reads a log that may still be open for writing by the game. `size` is what the
// scan measured, and the copy stops there on purpose: a writer we cannot outrun
// would otherwise keep it going forever.
//
// Exported for tests. The case this function exists for -- the game appending
// while we read -- needs the file to change between the scan and the copy, and
// inside one CollectGameLogs() call that can only be produced by racing a
// thread. Calling it with a `size` below the file's real size is the same
// arithmetic, and it is deterministic.
GameLogCopy CopyLogFile(const std::filesystem::path& source, std::uint64_t size, const std::filesystem::path& target)
{
    Handle input{CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    if (!input.h || input.h == INVALID_HANDLE_VALUE) return {};
    FILE_ATTRIBUTE_TAG_INFO info{};
    if (!GetFileInformationByHandleEx(input.h, FileAttributeTagInfo, &info, sizeof(info)) ||
        (info.FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) return {};
    Handle output{CreateFileW(target.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (!output.h || output.h == INVALID_HANDLE_VALUE) throw std::runtime_error("无法写入日志副本");
    std::vector<char> buffer(65536);
    std::uint64_t copied = 0;
    while (copied < size)
    {
        DWORD wanted = static_cast<DWORD>(std::min<std::uint64_t>(buffer.size(), size - copied));
        DWORD got = 0, wrote = 0;
        if (!ReadFile(input.h, buffer.data(), wanted, &got, nullptr) || got == 0) break;
        if (!WriteFile(output.h, buffer.data(), got, &wrote, nullptr) || wrote != got) throw std::runtime_error("日志副本写入失败");
        copied += got;
    }
    if (!FlushFileBuffers(output.h)) throw std::runtime_error("日志副本未能落盘");
    // Ask the file how big it is now. The scan measured it before we opened it,
    // so a log the game kept writing comes back exactly `size` bytes long and
    // looks like a faithful copy -- while the tail, the newest lines, the part a
    // crash shows up in, is still in the original and not in here.
    LARGE_INTEGER now{};
    const bool knownSize = GetFileSizeEx(input.h, &now) != FALSE;
    return {copied, copied == size && (!knownSize || static_cast<std::uint64_t>(now.QuadPart) <= copied)};
}
DllCheckTargets ResolveDllCheckTargets(const std::filesystem::path& gameDirectory)
{
    DllCheckTargets result;
    std::vector<std::filesystem::path> gameDlls, otherDlls;
    std::set<std::wstring> seen;
    std::vector<std::filesystem::path> roots{ExecutableDirectory()};
    if (!gameDirectory.empty()) roots.push_back(gameDirectory);
    std::error_code ignored;
    for (const auto& root : roots)
    {
        if (!std::filesystem::is_directory(root, ignored)) continue;
        for (std::filesystem::recursive_directory_iterator it(root, ignored), end; it != end; ++it)
        {
            const auto path = it->path();
            if (!it->is_regular_file(ignored) || ToLowerAscii(WideToUtf8(path.extension().wstring())) != ".dll") continue;
            auto key = path.lexically_normal().wstring();
            std::transform(key.begin(), key.end(), key.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
            if (!seen.insert(key).second) continue;
            ++result.found;
            (IsGameCoreDllName(path) ? gameDlls : otherDlls).push_back(path);
        }
    }
    // The game's own DLLs are the reason this stage exists, so they go in first.
    // A cap that dropped GameCore.dll while keeping sixty unrelated plugins
    // would report "no load probe" for a machine that merely had a big directory.
    if (gameDlls.size() > MaxDllCheckFiles)
    {
        gameDlls.resize(MaxDllCheckFiles);
        result.truncated = true;
    }
    result.inspect = gameDlls;
    for (const auto& dll : otherDlls)
    {
        if (result.inspect.size() >= MaxDllCheckFiles) { result.truncated = true; break; }
        result.inspect.push_back(dll);
    }
    for (const auto& dll : result.inspect)
        if (IsGameCoreDllName(dll)) result.probe.push_back(dll);
    return result;
}
std::string BuildRedactedEventsJson(const std::vector<diagnosis::NormalizedEvent>& events, bool gameSpecified,
    const std::string& gameExecutable)
{
    // schemaVersion 2 adds isTargetGame. Until now a crash whose process
    // was redacted to OTHER_PROCESS carried no answer at all to "is that
    // our game crashing?" -- the one question an incoming package exists
    // to answer. The boolean answers it while keeping the name hidden.
    // Same predicate the anchor uses, so a placeholder can never make
    // this file claim the game was named when it was not.
    std::ostringstream out;
    out << "{\"schemaVersion\":2,\"redacted\":true,\"replayCompatible\":false"
        << ",\"gameExecutableSpecified\":" << (gameSpecified ? "true" : "false")
        << ",\"events\":[";
    bool first = true;
    for (const auto& e : events)
    {
        if (!first) out << ','; first = false;
        const bool isGame = gameSpecified && EqualsIgnoreCaseAscii(e.appName, gameExecutable);
        auto app = isGame ? gameExecutable
            : EqualsIgnoreCaseAscii(e.appName, "dwm.exe") ? "dwm.exe" : e.appName.empty() ? "" : "OTHER_PROCESS";
        // The module is only readable for a process this file already names.
        // Application Error carries the faulting module in a field of its own,
        // and when the application name never resolved, that module is often the
        // crashing executable itself -- printing it would hand over exactly the
        // name OTHER_PROCESS exists to hide.
        const bool namedProcess = isGame || app == "dwm.exe";
        out << "{\"timeUtc\":" << JsonString(FormatUtcIso8601(e.time)) << ",\"provider\":" << JsonString(e.provider)
            << ",\"id\":" << e.eventId << ",\"level\":" << e.level << ",\"application\":" << JsonString(app)
            // null, never false, when no game exe was supplied: "we were
            // never told which game" and "this is definitely not that
            // game" are different answers and must stay distinguishable.
            << ",\"isTargetGame\":" << (gameSpecified ? (isGame ? "true" : "false") : "null")
            << ",\"module\":" << JsonString(namedProcess ? FileNameOf(e.moduleName) : "REDACTED") << ",\"exceptionCode\":" << JsonString(e.exceptionCode)
            << ",\"werEvent\":" << JsonString(e.werEventName)
            << ",\"liveKernelCode\":" << JsonString(EqualsIgnoreCaseAscii(e.werEventName, "LiveKernelEvent") ? e.werP1 : "") << '}';
    }
    out << "]}";
    return out.str();
}
Profile LoadProfile(const std::filesystem::path& file)
{
    if (!std::filesystem::is_regular_file(file)) throw std::invalid_argument("Profile file not found");
    auto read = [&](const wchar_t* key, const wchar_t* fallback) {
        wchar_t data[512]{};
        auto size = GetPrivateProfileStringW(L"project", key, fallback, data, 512, std::filesystem::absolute(file).c_str());
        if (size >= 510) throw std::invalid_argument("Profile field too long");
        return std::wstring(data, size);
    };
    Profile p;
    p.id = WideToUtf8(read(L"id", L"generic"));
    p.displayName = WideToUtf8(read(L"displayName", L"Windows Environment Collector"));
    p.gameExecutable = WideToUtf8(read(L"gameExecutable", L""));
    p.lookbackHours = Number(read(L"lookbackHours", L"336"), 1, 720);
    p.logRoot = WideToUtf8(read(L"logRoot", L"localLow"));
    p.logSubPath = WideToUtf8(read(L"logSubPath", L""));
    if (!SafeName(p.id) || (!p.gameExecutable.empty() && !SafeName(p.gameExecutable)))
        throw std::invalid_argument("Invalid profile id or executable name");
    if (p.logRoot != "localLow" && p.logRoot != "localAppData" && p.logRoot != "roaming" && p.logRoot != "programData")
        throw std::invalid_argument("Invalid log root");
    if (!p.logSubPath.empty() && !SafeRelativePath(p.logSubPath)) throw std::invalid_argument("Invalid log sub path");
    return p;
}
std::filesystem::path ResolveGameLogDirectory(const Profile& profile)
{
    if (profile.logSubPath.empty()) return {};
    const KNOWNFOLDERID* folder = &FOLDERID_LocalAppDataLow;
    if (profile.logRoot == "localAppData") folder = &FOLDERID_LocalAppData;
    else if (profile.logRoot == "roaming") folder = &FOLDERID_RoamingAppData;
    else if (profile.logRoot == "programData") folder = &FOLDERID_ProgramData;
    const auto base = KnownFolderPath(*folder);
    if (base.empty()) return {};
    return (base / Utf8ToWide(profile.logSubPath)).lexically_normal();
}
bool IsArchivedGameLogName(const std::string& name)
{
    const std::string prefix = GameLogNamePrefix;
    if (name.size() <= prefix.size() || name.size() > 128 || name.compare(0, prefix.size(), prefix) != 0) return false;
    if (name.find("..") != std::string::npos) return false;
    return name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") == std::string::npos;
}
GameLogCollection CollectGameLogs(const std::filesystem::path& logDirectory, const std::filesystem::path& outputDirectory,
                                  unsigned lookbackHours, diagnosis::UtcTicks incidentTime)
{
    GameLogCollection result;
    const auto now = CurrentUtcTime();
    std::ostringstream out;
    out << "{\"schemaVersion\":1,\"toolVersion\":" << JsonString(ToolVersion)
        << ",\"collectedAtUtc\":" << JsonString(FormatUtcIso8601(now))
        << ",\"lookbackHours\":" << lookbackHours
        << ",\"logDirectory\":" << JsonString(SanitizePath(WideToUtf8(logDirectory.wstring())))
        << ",\"maxFiles\":" << MaxGameLogFiles << ",\"maxFileBytes\":" << MaxGameLogFileBytes
        << ",\"maxTotalBytes\":" << MaxGameLogTotalBytes;
    std::string reason;
    std::error_code error;
    if (logDirectory.empty()) reason = "NOT_CONFIGURED";
    else if (!std::filesystem::is_directory(logDirectory, error)) reason = "LOG_DIRECTORY_MISSING";
    if (!reason.empty())
    {
        out << ",\"status\":\"NO_LOGS\",\"reason\":" << JsonString(reason) << ",\"files\":[],\"skipped\":[]"
            << ",\"note\":\"No game log was packaged; the machine is not blamed for a missing log folder.\"}";
        result.json = out.str();
        return result;
    }
    std::vector<LogCandidate> candidates;
    ScanLogFiles(logDirectory, 1, candidates);
    const auto oldest = SubtractClamped(now, static_cast<diagnosis::UtcTicks>(lookbackHours) * TicksPerHour);
    candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
        [&](const LogCandidate& c) { return c.modified < oldest; }), candidates.end());
    // Newest first, but logs written inside the incident window are the ones that matter.
    std::sort(candidates.begin(), candidates.end(), [&](const LogCandidate& a, const LogCandidate& b) {
        if (incidentTime)
        {
            const bool nearA = a.modified >= SubtractClamped(incidentTime, IncidentWindowMargin) && a.modified <= incidentTime + IncidentWindowMargin;
            const bool nearB = b.modified >= SubtractClamped(incidentTime, IncidentWindowMargin) && b.modified <= incidentTime + IncidentWindowMargin;
            if (nearA != nearB) return nearA;
        }
        return a.modified > b.modified;
    });
    std::filesystem::create_directories(outputDirectory);
    std::uint64_t total = 0;
    std::vector<std::pair<std::string, std::string>> skipped; // relative path, reason
    std::ostringstream files, skippedJson;
    bool firstFile = true, firstSkipped = true;
    for (const auto& candidate : candidates)
    {
        if (result.archiveNames.size() >= MaxGameLogFiles)
        {
            // Every other cap says why a log was left out. Dropping these in silence
            // left the index reading like a complete list of what the window held,
            // and the cap bites hardest exactly when it matters -- when there are
            // more logs near the incident than the cap allows.
            skipped.push_back({RelativeLogPath(candidate.path, logDirectory), "FILE_LIMIT"});
            continue;
        }
        if (candidate.bytes > MaxGameLogFileBytes || total + candidate.bytes > MaxGameLogTotalBytes)
        {
            skipped.push_back({RelativeLogPath(candidate.path, logDirectory), candidate.bytes > MaxGameLogFileBytes ? "TOO_LARGE" : "TOTAL_LIMIT"});
            continue;
        }
        const auto name = ArchiveLogName(static_cast<std::uint32_t>(result.archiveNames.size()), candidate.path);
        const auto copy = CopyLogFile(candidate.path, candidate.bytes, outputDirectory / name);
        if (copy.bytes == 0) { skipped.push_back({RelativeLogPath(candidate.path, logDirectory), "UNREADABLE"}); continue; }
        total += copy.bytes;
        result.archiveNames.push_back(name);
        if (!firstFile) files << ','; firstFile = false;
        files << "{\"archiveName\":" << JsonString(name) << ",\"relativePath\":" << JsonString(RelativeLogPath(candidate.path, logDirectory))
            << ",\"bytes\":" << copy.bytes << ",\"modifiedUtc\":" << JsonString(FormatUtcIso8601(candidate.modified))
            << ",\"sha256\":" << JsonString(Sha256File(outputDirectory / name))
            << ",\"complete\":" << (copy.complete ? "true" : "false") << '}';
    }
    for (const auto& entry : skipped)
    {
        if (!firstSkipped) skippedJson << ','; firstSkipped = false;
        skippedJson << "{\"relativePath\":" << JsonString(entry.first) << ",\"reason\":" << JsonString(entry.second) << '}';
    }
    result.status = result.archiveNames.empty() ? GameLogStatus::NoLogs : GameLogStatus::Ok;
    out << ",\"status\":" << JsonString(result.status == GameLogStatus::Ok ? "OK" : "NO_LOGS");
    if (result.archiveNames.empty()) out << ",\"reason\":" << JsonString(skipped.empty() ? "NO_LOG_FILES_IN_WINDOW" : "NO_LOG_FILES_COLLECTED");
    out << ",\"files\":[" << files.str() << "],\"skipped\":[" << skippedJson.str() << ']'
        << ",\"note\":\"Copies of game logs; only the newest files inside the lookback window are packaged. "
           "Logs may contain account or chat data; originals are never modified or deleted.\"}";
    result.json = out.str();
    return result;
}
Result Collect(const Options& options, const std::atomic_bool& cancel, const std::function<void(const std::string&, bool)>& progress)
{
    if (!SafeName(options.profile.id) || options.profile.lookbackHours < 1 || options.profile.lookbackHours > 720
        || options.stageTimeoutSeconds < 1 || options.stageTimeoutSeconds > 300)
        throw std::invalid_argument("Invalid collection options");
    if (options.incidentTime > CurrentUtcTime() + 5 * TicksPerMinute)
        throw std::invalid_argument("Incident time is in the future");
    auto root = std::filesystem::absolute(options.outputDirectory.empty() ? DefaultOutputDirectory() : options.outputDirectory);
    std::filesystem::create_directories(root);
    auto stamp = FormatUtcIso8601(CurrentUtcTime());
    stamp.erase(std::remove_if(stamp.begin(), stamp.end(), [](char c) { return c == ':' || c == '.'; }), stamp.end());
    auto prefix = options.profile.id + "-" + stamp + "-" + std::to_string(GetCurrentProcessId());
    Result result;
    for (unsigned n = 0; n < 100; ++n)
    {
        result.directory = root / (prefix + "-" + std::to_string(n));
        if (std::filesystem::create_directory(result.directory)) break;
        if (n == 99) throw std::runtime_error("Cannot create unique report directory");
    }
    struct Stage { const char* id; const char* label; const char* file; };
    std::vector<Stage> stages = {{"basic", "采集CPU与内存", "basic.json"}, {"firmware", "采集BIOS与内存配置", "firmware.json"},
        {"events", "分析系统事件", "diagnosis.json"}, {"health", "采集WHEA与内存诊断历史", "health-events.json"},
        {"graphics", "采集显卡与驱动", "graphics.json"}, {"storage", "采集存储信息", "storage.json"},
        {"gpu-selection", "验证双显卡自动选卡规则", "gpu-selection.json"},
        {"compatibility", "静态检查DLL并隔离加载GameCore", "compatibility.json"}};
    // Game logs only exist for profiles that declare where the game writes them.
    const auto logDirectory = options.logDirectory.empty() ? ResolveGameLogDirectory(options.profile) : options.logDirectory;
    if (!logDirectory.empty()) stages.push_back({"gamelogs", "复制游戏日志", "game-logs.json"});
    std::vector<std::string> files;
    std::ostringstream manifest, summary;
    // Computed once, from what the caller actually supplied, and echoed into
    // both outputs: summary.txt so the person who collected it sees it, and
    // manifest.json so triage can filter packages without opening any of them.
    const AttributionAnchor anchor = DescribeAttributionAnchor(options);
    manifest << "{\"schemaVersion\":1,\"toolVersion\":" << JsonString(ToolVersion) << ",\"profile\":" << JsonString(options.profile.id)
        << ",\"collectedAtUtc\":" << JsonString(FormatUtcIso8601(CurrentUtcTime()))
        << ",\"incidentTimeUtc\":" << JsonString(FormatUtcIso8601(options.incidentTime))
        << ",\"incidentKind\":" << JsonString(ToString(options.incidentKind)) << ",\"lookbackHours\":" << options.profile.lookbackHours
        << ",\"detailedEventsConsent\":" << (options.includeEvents ? "true" : "false")
        << ',' << BuildAttributionManifestFields(anchor) << ",\"stages\":[";
    // The warning goes FIRST, above the boilerplate: a package that cannot be
    // attributed has to say so before anything that makes it look complete.
    summary << BuildAttributionWarning(anchor);
    summary << "Windows 环境采集报告\n项目: " << options.profile.displayName << "\n工具: " << ToolVersion
        << "\n不自动上传，不压测，不运行内存测试，不重启，不修改BIOS。"
        << "\n兼容性检查（compatibility.json）会静态扫描 DLL，并在隔离子进程中加载 GameCore；不影响本进程与游戏本身。"
        << "\n未发现事件不等于系统正常；采集成功也不等于硬件健康。\n\n";
    bool first = true;
    for (const auto& stage : stages)
    {
        progress(stage.label, true);
        std::string status = cancel.load() ? "CANCELLED" : RunStageProcess(options, logDirectory, result.directory, stage.id, cancel);
        // NO_LOGS is an expected outcome (game never ran here), not a failed collection.
        const bool noLogs = status == "NO_LOGS";
        bool usable = status == "OK" || status == "INCOMPLETE" || noLogs;
        if (usable && (!std::filesystem::is_regular_file(result.directory / stage.file) || std::filesystem::file_size(result.directory / stage.file) == 0))
        { status = "OUTPUT_MISSING"; usable = false; }
        if (usable)
        {
            files.push_back(stage.file);
            if (std::string(stage.id) == "events" && options.includeEvents && std::filesystem::is_regular_file(result.directory / "events-redacted.json"))
                files.push_back("events-redacted.json");
        }
        if (status != "OK" && !noLogs) result.partial = true;
        if (!first) manifest << ','; first = false;
        manifest << "{\"id\":" << JsonString(stage.id) << ",\"status\":" << JsonString(status) << '}';
        summary << stage.label << ": " << status << '\n';
    }
    // The gamelogs worker copies the logs itself; pick up whatever it produced.
    std::vector<std::string> logCopies;
    {
        std::error_code error;
        for (std::filesystem::directory_iterator entry(result.directory, error), end; !error && entry != end; entry.increment(error))
        {
            const auto name = entry->path().filename().string();
            if (entry->is_regular_file() && IsArchivedGameLogName(name)) logCopies.push_back(name);
        }
        std::sort(logCopies.begin(), logCopies.end());
    }
    files.insert(files.end(), logCopies.begin(), logCopies.end());
    progress("正在完成ZIP（此阶段不可取消）", false);
    result.cancelled = cancel.load();
    manifest << "],\"partial\":" << (result.partial ? "true" : "false") << ",\"cancelled\":" << (result.cancelled ? "true" : "false")
        << ",\"healthInterpretation\":\"See each report's observability and status; successful collection is not a health verdict\""
        // Where the collector's own 扁鹊 log lives -- a stage that reported a
        // failure usually has the reason there, and it is not part of the ZIP.
        << ",\"toolLog\":" << JsonString(LogFilePath())
        << ",\"files\":[";
    files.push_back("summary.txt"); files.push_back("manifest.json");
    for (size_t i = 0; i < files.size(); ++i) { if (i) manifest << ','; manifest << JsonString(files[i]); }
    manifest << "]}";
    if (!logCopies.empty())
        summary << "\n本次包含 " << logCopies.size() << " 个游戏日志副本（窗口内最新的日志），可能含账号等信息，确认后再发送。\n";
    summary << "\n已保留本地报告。请确认内容后手动发送ZIP给研发。\n内存检测需用户保存工作并主动安排；本工具只读取已有检测结果。\n";
    WriteReportFile(result.directory / "summary.txt", summary.str());
    WriteReportFile(result.directory / "manifest.json", manifest.str());
    result.archive = std::filesystem::path(result.directory.wstring() + L".zip");
    WriteZip(result.directory, result.archive, files);
    return result;
}
bool IsCollectionCommand(int argc, wchar_t** argv)
{
    for (int i = 1; i < argc; ++i)
        if (std::wstring(argv[i]) == L"--collect" || std::wstring(argv[i]) == L"--collect-stage") return true;
    return false;
}
int RunCollectionCommand(int argc, wchar_t** argv)
{
    Options options;
    std::string stage;
    for (int i = 1; i < argc; ++i)
    {
        std::wstring key = argv[i];
        auto value = [&]() -> std::wstring { if (++i == argc) throw std::invalid_argument("Missing option value"); return argv[i]; };
        if (key == L"--collect") continue;
        if (key == L"--collect-stage") stage = WideToUtf8(value());
        else if (key == L"--profile") options.profile = LoadProfile(value());
        else if (key == L"--output-dir") options.outputDirectory = value();
        else if (key == L"--log-dir") options.logDirectory = value();
        else if (key == L"--game-exe")
        {
            options.profile.gameExecutable = WideToUtf8(value());
            // The stage worker always receives a value, so "no game specified"
            // arrives as the placeholder. Restore it to unset before anything
            // reads it: seen as a real name it would make events-redacted.json
            // claim the game was specified (and answer isTargetGame=false
            // instead of null) when in fact nobody said which game it was.
            if (options.profile.gameExecutable == UnspecifiedGamePlaceholder) options.profile.gameExecutable.clear();
            if (!options.profile.gameExecutable.empty() && !SafeName(options.profile.gameExecutable)) throw std::invalid_argument("Invalid executable name");
        }
        else if (key == L"--lookback-hours") options.profile.lookbackHours = Number(value(), 1, 720);
        else if (key == L"--game-dir") options.gameDirectory = value();
        else if (key == L"--require") ApplyRequiredFeatures(options.requirements, WideToUtf8(value()));
        else if (key == L"--min-memory-gb")
            options.requirements.minimumMemoryBytes = static_cast<std::uint64_t>(Number(value(), 1, 1024)) << 30;
        else if (key == L"--stage-timeout-seconds") options.stageTimeoutSeconds = Number(value(), 1, 300);
        else if (key == L"--include-events") options.includeEvents = true;
        else if (key == L"--incident-time") { if (!TryParseUtcIso8601(WideToUtf8(value()), options.incidentTime)) throw std::invalid_argument("Incident time requires ISO8601 timezone"); }
        else if (key == L"--incident-kind")
        {
            auto kind = value();
            if (kind == L"crash") options.incidentKind = IncidentKind::Crash;
            else if (kind == L"hang") options.incidentKind = IncidentKind::Hang;
            else if (kind != L"unknown") throw std::invalid_argument("Invalid incident kind");
        }
        else if (key == L"--help")
        {
            std::cout << "--collect [--profile file.ini] [--output-dir path] [--game-exe game.exe]\n"
                "  [--incident-time ISO8601-with-zone] [--incident-kind crash|hang|unknown]\n"
                "  [--lookback-hours 1..720] [--include-events] [--stage-timeout-seconds 1..300]\n"
                "  [--log-dir path]  # override the profile's game log directory\n"
                "  [--game-dir path] # where the game's DLLs live: static PE checks and\n"
                "                    # the isolated load probe look there\n"
                "  [--require avx,avx2,fma,f16c|none] [--min-memory-gb 1..1024]\n"
                "One package with everything: hardware, dual-GPU selection, event\n"
                "diagnosis, and the DLL checks (compatibility.json). No reboot, stress\n"
                "test or upload; the load probe runs GameCore in an isolated child\n"
                "process. A big --game-dir can exceed the per-stage timeout: that\n"
                "stage gets at least 180 s, and --stage-timeout-seconds raises it.\n"
                "Exit: 0=ZIP created (inspect manifest for partial data), 30=error.\n";
            return 0;
        }
        else throw std::invalid_argument("Unknown collection option");
    }
    // Stage workers are separate processes; only the orchestrating run prints.
    if (!stage.empty()) return RunStage(stage, options);
    // Same warning the GUI shows and summary.txt carries. A CLI run is even
    // easier to fire off with no anchor, and stdout is the only place anyone
    // will see it before the ZIP is handed over.
    if (const auto anchor = DescribeAttributionAnchor(options); !anchor.Usable())
        std::cout << BuildAttributionWarning(anchor);
    std::atomic_bool cancel{false};
    auto result = Collect(options, cancel, [](const std::string& message, bool) { std::cout << message << std::endl; });
    std::cout << "ZIP: " << WideToUtf8(result.archive.wstring()) << "\nPartial: " << result.partial << '\n';
    return 0;
}
}
