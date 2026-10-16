#include "Collection.h"
#include "AttributionRules.h"
#include "DiagnosisReport.h"
#include "DiagnosisUtil.h"
#include "EnvCheckLog.h"
#include "EnvironmentProbe.h"
#include "EvidenceFiles.h"
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
struct Handle
{
    HANDLE h = nullptr;
    ~Handle() { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); }
};
// FindFirstFile handles must be released with FindClose, not CloseHandle.
struct FindHandle
{
    HANDLE h = INVALID_HANDLE_VALUE;
    ~FindHandle() { if (h != INVALID_HANDLE_VALUE) FindClose(h); }
};
std::string RunStageProcess(const Options& options, const std::filesystem::path& logDirectory,
                           const std::filesystem::path& directory,
                           const std::string& stage, const std::atomic_bool& cancel)
{
    const auto exe = ToolExecutablePath();
    if (!std::filesystem::is_regular_file(exe)) return "HELPER_MISSING";
    std::wstring command = Quote(exe.wstring()) + L" --collect-stage " + Utf8ToWide(stage)
        + L" --output-dir " + Quote(directory.wstring()) + L" --lookback-hours " + std::to_wstring(options.profile.lookbackHours)
        + L" --game-exe " + Quote(Utf8ToWide(options.profile.gameExecutable.empty() ? "__unspecified_game__.exe" : options.profile.gameExecutable))
        + L" --incident-kind " + Utf8ToWide(options.incidentKind == IncidentKind::Crash ? "crash" : options.incidentKind == IncidentKind::Hang ? "hang" : "unknown");
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
    auto deadline = GetTickCount64() + options.stageTimeoutSeconds * 1000ull;
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
        {
            std::ostringstream out; out << "{\"schemaVersion\":1,\"redacted\":true,\"replayCompatible\":false,\"events\":[";
            bool first = true;
            for (const auto& e : events.events)
            {
                if (!first) out << ','; first = false;
                auto app = EqualsIgnoreCaseAscii(e.appName, options.profile.gameExecutable) ? options.profile.gameExecutable
                    : EqualsIgnoreCaseAscii(e.appName, "dwm.exe") ? "dwm.exe" : e.appName.empty() ? "" : "OTHER_PROCESS";
                out << "{\"timeUtc\":" << JsonString(FormatUtcIso8601(e.time)) << ",\"provider\":" << JsonString(e.provider)
                    << ",\"id\":" << e.eventId << ",\"level\":" << e.level << ",\"application\":" << JsonString(app)
                    << ",\"module\":" << JsonString(app == "OTHER_PROCESS" ? "REDACTED" : FileNameOf(e.moduleName)) << ",\"exceptionCode\":" << JsonString(e.exceptionCode)
                    << ",\"werEvent\":" << JsonString(e.werEventName)
                    << ",\"liveKernelCode\":" << JsonString(EqualsIgnoreCaseAscii(e.werEventName, "LiveKernelEvent") ? e.werP1 : "") << '}';
            }
            out << "]}";
            WriteReportFile(options.outputDirectory / "events-redacted.json", out.str());
        }
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
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        {
            if (depth + 1 < MaxGameLogScanDepth && name != L"." && name != L"..")
                ScanLogFiles(directory / name, depth + 1, out);
            continue;
        }
        // A link inside the log folder could point anywhere; never follow it.
        if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
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
// Reads a log that may still be open for writing by the game; returns the copied byte count.
std::uint64_t CopyLogFile(const std::filesystem::path& source, std::uint64_t size, const std::filesystem::path& target)
{
    Handle input{CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    if (!input.h || input.h == INVALID_HANDLE_VALUE) return 0;
    FILE_ATTRIBUTE_TAG_INFO info{};
    if (!GetFileInformationByHandleEx(input.h, FileAttributeTagInfo, &info, sizeof(info)) ||
        (info.FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) return 0;
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
    return copied;
}
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
        if (result.archiveNames.size() >= MaxGameLogFiles) break;
        if (candidate.bytes > MaxGameLogFileBytes || total + candidate.bytes > MaxGameLogTotalBytes)
        {
            skipped.push_back({RelativeLogPath(candidate.path, logDirectory), candidate.bytes > MaxGameLogFileBytes ? "TOO_LARGE" : "TOTAL_LIMIT"});
            continue;
        }
        const auto name = ArchiveLogName(static_cast<std::uint32_t>(result.archiveNames.size()), candidate.path);
        const auto copied = CopyLogFile(candidate.path, candidate.bytes, outputDirectory / name);
        if (copied == 0) { skipped.push_back({RelativeLogPath(candidate.path, logDirectory), "UNREADABLE"}); continue; }
        total += copied;
        result.archiveNames.push_back(name);
        if (!firstFile) files << ','; firstFile = false;
        files << "{\"archiveName\":" << JsonString(name) << ",\"relativePath\":" << JsonString(RelativeLogPath(candidate.path, logDirectory))
            << ",\"bytes\":" << copied << ",\"modifiedUtc\":" << JsonString(FormatUtcIso8601(candidate.modified))
            << ",\"sha256\":" << JsonString(Sha256File(outputDirectory / name))
            << ",\"complete\":" << (copied == candidate.bytes ? "true" : "false") << '}';
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
        {"gpu-selection", "验证双显卡自动选卡规则", "gpu-selection.json"}};
    // Game logs only exist for profiles that declare where the game writes them.
    const auto logDirectory = options.logDirectory.empty() ? ResolveGameLogDirectory(options.profile) : options.logDirectory;
    if (!logDirectory.empty()) stages.push_back({"gamelogs", "复制游戏日志", "game-logs.json"});
    std::vector<std::string> files;
    std::ostringstream manifest, summary;
    manifest << "{\"schemaVersion\":1,\"toolVersion\":" << JsonString(ToolVersion) << ",\"profile\":" << JsonString(options.profile.id)
        << ",\"collectedAtUtc\":" << JsonString(FormatUtcIso8601(CurrentUtcTime()))
        << ",\"incidentTimeUtc\":" << JsonString(FormatUtcIso8601(options.incidentTime))
        << ",\"incidentKind\":" << JsonString(ToString(options.incidentKind)) << ",\"lookbackHours\":" << options.profile.lookbackHours
        << ",\"detailedEventsConsent\":" << (options.includeEvents ? "true" : "false") << ",\"stages\":[";
    summary << "Windows 环境采集报告\n项目: " << options.profile.displayName << "\n工具: " << ToolVersion
        << "\n不自动上传，不加载游戏DLL，不运行内存测试，不修改BIOS。\n未发现事件不等于系统正常；采集成功也不等于硬件健康。\n\n";
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
        else if (key == L"--game-exe") { options.profile.gameExecutable = WideToUtf8(value()); if (!SafeName(options.profile.gameExecutable)) throw std::invalid_argument("Invalid executable name"); }
        else if (key == L"--lookback-hours") options.profile.lookbackHours = Number(value(), 1, 720);
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
                "Safe read-only collection. No DLL loading, reboot, stress test or upload.\n"
                "Exit: 0=ZIP created (inspect manifest for partial data), 30=error.\n";
            return 0;
        }
        else throw std::invalid_argument("Unknown collection option");
    }
    if (!stage.empty()) return RunStage(stage, options);
    std::atomic_bool cancel{false};
    auto result = Collect(options, cancel, [](const std::string& message, bool) { std::cout << message << std::endl; });
    std::cout << "ZIP: " << WideToUtf8(result.archive.wstring()) << "\nPartial: " << result.partial << '\n';
    return 0;
}
}
