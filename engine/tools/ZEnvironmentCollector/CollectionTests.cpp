#include "Collection.h"
#include "DiagnosisUtil.h"
#include "DiagnosisReport.h"
#include "AttributionRules.h"
#include "EventLogReader.h"
#include "GpuSelectionProbe.h"
#include "ReportWriter.h"
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace zengine::envcheck;
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
int main()
{
    try
    {
        using namespace collector;
        Check(JsonString("a\n\"\\") == "\"a\\u000a\\\"\\\\\"", "JSON escaping");

        // Attribution anchor. A package that is missing any of the three inputs
        // cannot answer "was it the game" -- and three field packages in
        // October 2026 came back exactly like that, with the discovery left to
        // the developer. It has to be detectable, not just documented.
        {
            collector::Options bare;
            const auto none = collector::DescribeAttributionAnchor(bare);
            Check(!none.Usable() && !none.incidentTime && !none.incidentKind && !none.gameExecutable,
                  "options with nothing supplied are unattributable");
            Check(!collector::BuildAttributionWarning(none).empty(), "a warning is produced for summary.txt");
            const auto manifestNone = collector::BuildAttributionManifestFields(none);
            Check(manifestNone.find("\"attributionUsable\":false") != std::string::npos, "manifest reports unusable");
            Check(manifestNone.find("\"gameExecutable\"") != std::string::npos, "manifest names the missing game exe");

            collector::Options full = bare;
            full.incidentTime = diagnosis::CurrentUtcTime();
            full.incidentKind = diagnosis::IncidentKind::Crash;
            full.profile.gameExecutable = "Game.exe";
            const auto complete = collector::DescribeAttributionAnchor(full);
            Check(complete.Usable(), "all three supplied is attributable");
            Check(collector::BuildAttributionWarning(complete).empty(), "no warning when the anchor is complete");
            Check(collector::BuildAttributionManifestFields(complete)
                      == "\"attributionUsable\":true,\"missingAttributionInputs\":[]",
                  "manifest reports usable with an empty missing list");

            // Half-filled is what actually happens in the field: someone fills
            // in the time, leaves the type on "Unsure".
            collector::Options partial = full;
            partial.incidentKind = diagnosis::IncidentKind::Unknown;
            Check(!collector::DescribeAttributionAnchor(partial).Usable(), "'Unsure' kind is still unattributable");

            // A stage worker always receives --game-exe with a value, so "no
            // game" travels as a placeholder. It must not read as a name: that
            // is exactly how events-redacted.json ended up claiming the game was
            // specified and answering isTargetGame=false instead of null.
            collector::Options placeholder = full;
            placeholder.profile.gameExecutable = collector::UnspecifiedGamePlaceholder;
            const auto withPlaceholder = collector::DescribeAttributionAnchor(placeholder);
            Check(!withPlaceholder.gameExecutable && !withPlaceholder.Usable(),
                  "the placeholder exe name is not a specified game");

            // The shareable event file replaces process names. Application Error
            // carries the faulting module in a field of its own, and for a crash
            // in the executable itself that module IS the process -- so keeping
            // it for an event whose application name never resolved hands over
            // exactly the name OTHER_PROCESS exists to hide.
            diagnosis::NormalizedEvent unnamed;
            unnamed.appName.clear();
            unnamed.moduleName = "C:\\Program Files\\Browser\\browser.exe";
            unnamed.provider = "Application Error";
            const auto redacted = collector::BuildRedactedEventsJson({unnamed}, true, "Game.exe");
            Check(redacted.find("\"application\":\"\"") != std::string::npos, "an unresolved application name stays empty");
            Check(redacted.find("browser.exe") == std::string::npos, "the module of an unnamed process is redacted too");

            // A process this file does name keeps its module: the game is named
            // by whoever supplied the exe, and dwm.exe is a system process.
            diagnosis::NormalizedEvent gameCrash;
            gameCrash.appName = "Game.exe";
            gameCrash.moduleName = "C:\\Game\\Game.exe";
            const auto gameJson = collector::BuildRedactedEventsJson({gameCrash}, true, "Game.exe");
            Check(gameJson.find("\"module\":\"Game.exe\"") != std::string::npos, "the game's own module is readable");

            diagnosis::NormalizedEvent other;
            other.appName = "Other.exe";
            other.moduleName = "C:\\Other\\helper.dll";
            const auto otherJson = collector::BuildRedactedEventsJson({other}, true, "Game.exe");
            Check(otherJson.find("\"application\":\"OTHER_PROCESS\"") != std::string::npos, "other processes stay unnamed");
            Check(otherJson.find("helper.dll") == std::string::npos, "and so does their module");
        }
        diagnosis::UtcTicks a = 0, b = 0;
        Check(diagnosis::TryParseUtcIso8601("2026-10-12T08:00:00+08:00", a), "parse zoned time");
        Check(diagnosis::TryParseUtcIso8601("2026-10-12T00:00:00Z", b) && a == b, "timezone equivalence");
        Check(!diagnosis::TryParseUtcIso8601("2026-10-12T08:00:00", a), "reject no zone");
        diagnosis::EventCollection collection;
        collection.rangeEnd = diagnosis::CurrentUtcTime(); collection.collectedAt = collection.rangeEnd;
        collection.rangeStart = collection.rangeEnd - diagnosis::TicksPerHour;
        collection.observability.systemLogReadable = true;
        collection.observability.applicationLogReadable = true;
        collection.observability.systemOldestKnown = true;
        collection.observability.systemOldest = collection.rangeStart - diagnosis::TicksPerHour;
        collection.systemEventTimes.push_back(collection.rangeStart);
        auto replay = diagnosis::ParseEventsFile(diagnosis::BuildEventsFile(collection));
        Check(replay.rangeEnd == collection.rangeEnd, "event replay roundtrip");
        diagnosis::DiagnosisRequest request;
        request.now = collection.rangeEnd; request.lookbackStart = collection.rangeStart;
        request.lookbackHours = 1; request.gameExecutableName = "test.exe";
        auto before = diagnosis::Diagnose(request, collection);
        auto after = diagnosis::Diagnose(request, replay);
        Check(before.verdict == after.verdict && before.confidence == after.confidence, "pure diagnosis replay");

        // The incident window must be scanned before the rest of the lookback,
        // otherwise a machine that logs heavily spends the event cap on old
        // records and never reaches the incident (observed in the field).
        {
            const diagnosis::UtcTicks now = 1000 * diagnosis::TicksPerHour;
            const diagnosis::UtcTicks incident = now - 3 * diagnosis::TicksPerHour;
            diagnosis::EventLogReadOptions read;
            read.rangeEnd = now;
            read.rangeStart = now - 336 * diagnosis::TicksPerHour;
            read.hasPriorityWindow = true;
            read.priorityStart = incident - diagnosis::IncidentWindowMargin;
            read.priorityEnd = incident + diagnosis::IncidentWindowMargin;
            const auto order = diagnosis::BuildEventScanOrder(read);
            Check(order.size() == 3, "priority window splits the range in three");
            Check(order[0].start == read.priorityStart && order[0].end == read.priorityEnd,
                "incident window scanned first");
            Check(order[1].start == read.priorityEnd + 1 && order[1].end == read.rangeEnd,
                "records written after the incident scanned second");
            Check(order[2].start == read.rangeStart && order[2].end == read.priorityStart - 1,
                "older context scanned last");
            for (const auto& range : order) Check(!diagnosis::IsEmptyEventScanRange(range), "no empty sub-range");
            // Inclusive bounds: a gap of exactly one tick means no event is read twice.
            Check(order[1].start > order[0].end && order[0].start > order[2].end, "sub-ranges do not overlap");

            diagnosis::EventLogReadOptions plain;
            plain.rangeEnd = now;
            plain.rangeStart = read.rangeStart;
            const auto single = diagnosis::BuildEventScanOrder(plain);
            Check(single.size() == 1 && single[0].start == plain.rangeStart && single[0].end == now,
                "no incident time keeps a single range");

            Check(order[0].priority && !order[1].priority && !order[2].priority, "only the incident window is priority");
            Check(!single[0].priority, "single range is never priority");

            diagnosis::EventLogReadOptions outside = read;
            outside.priorityStart = read.rangeStart - 10 * diagnosis::TicksPerHour;
            outside.priorityEnd = read.rangeStart - 5 * diagnosis::TicksPerHour;
            const auto clamped = diagnosis::BuildEventScanOrder(outside);
            Check(clamped.size() == 1 && clamped[0].end == now, "window outside the range falls back to one pass");
            Check(!clamped[0].priority, "fallback pass is not reported as the incident window");
        }

        // A truncated scan must not weaken the verdict when the incident window
        // itself was fully scanned: that is the whole point of scanning it first.
        {
            const diagnosis::UtcTicks now = diagnosis::CurrentUtcTime();
            const diagnosis::UtcTicks incident = now - 3 * diagnosis::TicksPerHour;
            diagnosis::EventCollection noisy;
            noisy.collectedAt = now;
            noisy.rangeEnd = now;
            noisy.rangeStart = now - 336 * diagnosis::TicksPerHour;
            noisy.observability.systemLogReadable = true;
            noisy.observability.applicationLogReadable = true;
            noisy.observability.systemOldestKnown = true;
            noisy.observability.systemOldest = noisy.rangeStart;
            // Cap hit while reading older context, after the incident window.
            noisy.observability.eventCapReached = true;
            noisy.observability.priorityWindowRequested = true;
            noisy.observability.priorityWindowComplete = true;
            noisy.systemEventTimes.push_back(incident);

            diagnosis::DiagnosisRequest noisyRequest;
            noisyRequest.now = now;
            noisyRequest.lookbackStart = noisy.rangeStart;
            noisyRequest.lookbackHours = 336;
            noisyRequest.gameExecutableName = "test.exe";
            noisyRequest.hasLauncherIncidentTime = true;
            noisyRequest.launcherIncidentTime = incident;
            noisyRequest.launcherIncidentKind = diagnosis::IncidentKind::Hang;

            const auto contextOnly = diagnosis::Diagnose(noisyRequest, noisy);
            Check(contextOnly.verdict == diagnosis::Verdict::NoSystemEvidence,
                "context-only truncation keeps the no-evidence verdict");
            Check(std::find(contextOnly.observabilityIssues.begin(), contextOnly.observabilityIssues.end(),
                      "SCAN_CONTEXT_TRUNCATED") != contextOnly.observabilityIssues.end(),
                "context-only truncation is still reported");

            // Same truncation, but this time it cut the incident window short.
            noisy.observability.priorityWindowComplete = false;
            const auto windowLost = diagnosis::Diagnose(noisyRequest, noisy);
            Check(windowLost.verdict == diagnosis::Verdict::Inconclusive,
                "truncated incident window stays inconclusive");

            // Version 1 samples predate the flags and must stay replayable.
            auto legacy = diagnosis::BuildEventsFile(noisy);
            const auto versionTag = legacy.find("\t2\n");
            Check(versionTag != std::string::npos, "events file advertises version 2");
            legacy.replace(versionTag, 3, "\t1\n");
            legacy.erase(legacy.find("O\tpriorityWindowRequested"),
                std::string("O\tpriorityWindowRequested\t1\nO\tpriorityWindowComplete\t0\n").size());
            const auto legacyParsed = diagnosis::ParseEventsFile(legacy);
            Check(!legacyParsed.observability.priorityWindowRequested &&
                      !legacyParsed.observability.priorityWindowComplete,
                "version 1 replay defaults the new flags to false");
        }
        auto directory = std::filesystem::temp_directory_path() / ("envcollector-test-" + std::to_string(GetCurrentProcessId()));
        Check(std::filesystem::create_directory(directory), "unique test folder");
        std::ofstream(directory / "sample.json", std::ios::binary) << "{\"sample\":true}";
        WriteZip(directory, directory / "test.zip", {"sample.json"});
        Check(std::filesystem::file_size(directory / "test.zip") > 40, "ZIP output");
        bool rejected = false;
        try { WriteZip(directory, directory / "unsafe.zip", {"../sample.json"}); } catch (...) { rejected = true; }
        Check(rejected, "archive traversal rejected");
        std::ofstream(directory / "occupied.zip.partial", std::ios::binary) << "must survive";
        rejected = false;
        try { WriteZip(directory, directory / "occupied.zip", {"sample.json"}); } catch (...) { rejected = true; }
        Check(rejected && std::filesystem::file_size(directory / "occupied.zip.partial") == 12, "never truncate existing output");
        Options cancelledOptions; cancelledOptions.outputDirectory = directory / "cancelled";
        std::atomic_bool cancelled{true};
        auto partial = Collect(cancelledOptions, cancelled, [](const std::string&, bool) {});
        Check(partial.cancelled && partial.partial && std::filesystem::is_regular_file(partial.archive), "cancel retains partial ZIP");

        // Game logs travel with the report: they are the only record of what the
        // game itself printed before the incident.
        {
            auto logs = directory / "GameLogs";
            std::filesystem::create_directories(logs / "nested");
            std::ofstream(logs / "2026-01-01_000000_d1_g1_r1.logcompr", std::ios::binary) << "session\n";
            std::ofstream(logs / "nested" / "weird name@.log", std::ios::binary) << "nested\n";
            std::ofstream(logs / "ignored.bin", std::ios::binary) << "binary";
            const auto collected = CollectGameLogs(logs, directory / "log-out", 336, 0);
            Check(collected.status == GameLogStatus::Ok, "game logs collected");
            // Compressed per-session logs (.logcompr) are the game's own record; they must not be filtered out.
            Check(collected.archiveNames.size() == 2, "only .log/.logcompr/.txt files are packaged");
            for (const auto& name : collected.archiveNames)
            {
                Check(IsArchivedGameLogName(name), "archive name stays whitelisted");
                Check(std::filesystem::is_regular_file(directory / "log-out" / name), "log copy written next to the reports");
            }
            Check(collected.json.find("weird") != std::string::npos, "index keeps the relative path");
            Check(collected.json.find("ignored.bin") == std::string::npos, "other extensions skipped");
            Check(collected.json.find("\"sha256\"") != std::string::npos, "index hashes each copy");

            const auto missing = CollectGameLogs(directory / "NoSuchLogs", directory / "log-out2", 336, 0);
            Check(missing.status == GameLogStatus::NoLogs && missing.archiveNames.empty()
                && missing.json.find("LOG_DIRECTORY_MISSING") != std::string::npos, "missing log folder is reported, not blamed");
            std::filesystem::create_directories(directory / "EmptyLogs");
            const auto empty = CollectGameLogs(directory / "EmptyLogs", directory / "log-out3", 336, 0);
            Check(empty.status == GameLogStatus::NoLogs && empty.json.find("NO_LOG_FILES_IN_WINDOW") != std::string::npos,
                "empty log folder is reported");

            // The index says it does not follow links. A junction inside the log
            // folder is a link to somewhere else, and following it would package
            // logs that are not the game's into a report somebody sends -- carrying
            // a relativePath that quietly claims they came from the game's folder.
            {
                const auto outside = directory / "OutsideLogs";
                std::filesystem::create_directories(outside);
                std::ofstream(outside / "elsewhere.log", std::ios::binary) << "not the game\n";
                const auto linked = directory / "JunctionLogs";
                std::filesystem::create_directories(linked);
                // A junction rather than a directory symlink: creating one needs no
                // privilege, so this case runs on any machine that can run the tool.
                const std::string link = "cmd /c mklink /J \"" + (linked / "pointed").string()
                    + "\" \"" + outside.string() + "\" >nul";
                if (std::system(link.c_str()) == 0)
                {
                    const auto junction = CollectGameLogs(linked, directory / "log-out4", 336, 0);
                    Check(junction.archiveNames.empty(), "a junction inside the log folder is not followed");
                    Check(junction.json.find("elsewhere.log") == std::string::npos,
                          "logs reachable only through a link stay out of the report");
                }
            }
            // Every other cap says why a log was left out. The count cap was the
            // only one that dropped files without a word, which left the index
            // reading like a complete list of what the window held.
            {
                const auto many = directory / "ManyLogs";
                std::filesystem::create_directories(many);
                for (std::uint32_t i = 0; i < MaxGameLogFiles + 2; ++i)
                {
                    std::ofstream(many / ("session" + std::to_string(i) + ".log"), std::ios::binary) << "line\n";
                }
                const auto capped = CollectGameLogs(many, directory / "log-out5", 336, 0);
                Check(capped.archiveNames.size() == MaxGameLogFiles, "the file cap holds");
                Check(capped.json.find("FILE_LIMIT") != std::string::npos,
                      "and the logs it had to drop are named, not silently omitted");
            }

            Profile configured;
            configured.logSubPath = "Tencent\\osgameCNOfficial\\Cache\\Logs";
            Check(diagnosis::WideToUtf8(ResolveGameLogDirectory(configured).wstring()).find("osgameCNOfficial") != std::string::npos,
                "profile log sub path resolves under LocalLow");
            Check(ResolveGameLogDirectory(Profile{}).empty(), "no log stage without a configured path");
            Check(!IsArchivedGameLogName("../gamelog-00-x.log") && IsArchivedGameLogName("gamelog-00-x.log"), "archive name guard");
        }
        std::filesystem::remove_all(directory);
        // GPU auto-selection is part of the environment snapshot now, not a
        // separate step the compatibility check bolts on, so a plain
        // CollectEnvironment() has to carry it.
        const auto envSnapshot = CollectEnvironment();
        Check(!envSnapshot.gpuSelectionMockResults.empty(), "GPU mock cases inside CollectEnvironment");
        Check(AllGpuSelectionMockCasesPassed(envSnapshot.gpuSelectionMockResults), "GPU mock regression via CollectEnvironment");
        // The real probe is machine-dependent (headless CI has no adapters), so
        // assert it *ran*, not that it found hardware.
        Check(envSnapshot.gpuSelectionRealProbe.dxgiFactoryCreated || !envSnapshot.gpuSelectionRealProbe.errorMessage.empty(),
            "real GPU probe ran inside CollectEnvironment");
        // includeDevices=false is the "basic" stage: no DXGI at all, mocks still run.
        const auto basicSnapshot = CollectEnvironment(false);
        Check(!basicSnapshot.gpuSelectionMockResults.empty(), "GPU mock cases survive includeDevices=false");
        Check(basicSnapshot.gpuSelectionRealProbe.allAdapters.empty()
            && !basicSnapshot.gpuSelectionRealProbe.dxgiFactoryCreated
            && basicSnapshot.gpuSelectionRealProbe.errorMessage.empty(), "no DXGI probe under includeDevices=false");
        // One serializer for the report and for the collected stage file.
        const std::string gpuJson = BuildGpuSelectionJson(envSnapshot.gpuSelectionMockResults, envSnapshot.gpuSelectionRealProbe);
        Check(gpuJson.find("\"allMockCasesPassed\": true") != std::string::npos, "GPU JSON reports mock verdict");
        Check(gpuJson.find("\"mockCases\": [") != std::string::npos && gpuJson.find("\"realProbe\": {") != std::string::npos,
            "GPU JSON has both sections");
        GpuSelectionMockCaseResult escaping;
        escaping.caseName = "quote\" back\\slash";
        GpuSelectionAdapter escapingAdapter;
        escapingAdapter.name = "a\"b\\c";
        escaping.adapters.push_back(escapingAdapter);
        const std::string escapedJson = BuildGpuSelectionJson({escaping}, GpuSelectionRealProbeResult{});
        Check(escapedJson.find("quote\\\" back\\\\slash") != std::string::npos, "GPU JSON escapes case name");
        Check(escapedJson.find("a\\\"b\\\\c") != std::string::npos, "GPU JSON escapes adapter name");
        // Reports are published by rename, not written in place. The launcher
        // reads OSGameDiagnosis.json asynchronously -- it polls the output
        // directory while this tool is still running -- so the old
        // truncate-then-stream write let it observe an empty or half-written
        // JSON. Publishing has to be observable as atomic from a reader.
        {
            const auto reportDirectory = std::filesystem::temp_directory_path()
                / ("envcollector-report-" + std::to_string(GetCurrentProcessId()));
            std::filesystem::remove_all(reportDirectory);
            Check(std::filesystem::create_directory(reportDirectory), "report test folder");
            const auto report = reportDirectory / "OSGameDiagnosis.json";
            // Multi-megabyte payloads: a torn read of a 1 KiB report is
            // unlikely enough that the test would pass even without the fix.
            const std::string first = std::string(3u << 20, 'a') + "}";
            const std::string second = std::string(5u << 20, 'b') + "}";

            WriteReportFile(report, first);
            Check(std::filesystem::file_size(report) == first.size(), "report written");
            // A second run reuses the same output directory, so an existing
            // report has to be replaced, not kept or appended to.
            WriteReportFile(report, second);
            Check(std::filesystem::file_size(report) == second.size(), "existing report replaced");

            std::atomic_bool stopReader{false};
            std::atomic_int tornReads{0};
            std::atomic_int completedReads{0};
            // The reader shares delete, which is what WriteReportFile asks the
            // launcher to do (see ReportWriter.h). Sharing it is also what lets
            // this reader hammer the file continuously without starving the
            // publish: a handle that does not share delete blocks the rename for
            // as long as it stays open.
            std::thread reader([&]
            {
                std::vector<char> buffer(std::max(first.size(), second.size()));
                while (!stopReader.load())
                {
                    const HANDLE handle = CreateFileW(report.c_str(), GENERIC_READ,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                    // The rename can deny an open for an instant; that is a
                    // transient error, not evidence of torn content.
                    if (handle == INVALID_HANDLE_VALUE) { std::this_thread::yield(); continue; }
                    LARGE_INTEGER size{};
                    DWORD read = 0;
                    const bool sized = GetFileSizeEx(handle, &size) != FALSE;
                    const bool complete = sized
                        && static_cast<std::uint64_t>(size.QuadPart) <= buffer.size()
                        && ReadFile(handle, buffer.data(), static_cast<DWORD>(size.QuadPart), &read, nullptr) != FALSE
                        && read == static_cast<DWORD>(size.QuadPart);
                    CloseHandle(handle);
                    if (!complete) { std::this_thread::yield(); continue; }
                    const std::string content(buffer.data(), static_cast<std::size_t>(read));
                    if (content != first && content != second) ++tornReads;
                    ++completedReads;
                }
            });
            // Everything below can throw (a publish that never gets through
            // does), and unwinding past a joinable std::thread calls
            // std::terminate -- which turns an ordinary failure into a fast-fail
            // with no message at all. Join before anything escapes.
            struct JoinOnExit
            {
                std::thread* thread;
                std::atomic_bool* stop;
                ~JoinOnExit()
                {
                    stop->store(true);
                    if (thread->joinable()) thread->join();
                }
            } joinOnExit{&reader, &stopReader};

            for (int iteration = 0; iteration < 12; ++iteration)
            {
                WriteReportFile(report, iteration % 2 == 0 ? second : first);
            }
            stopReader.store(true);
            reader.join();
            Check(completedReads.load() > 0, "the polling reader observed the report");
            Check(tornReads.load() == 0, "a polling reader never sees a half-written report");

            // A leftover temporary next to the report is what makes a support
            // engineer wonder which of the two files is authoritative.
            bool leftover = false;
            for (std::filesystem::directory_iterator entry(reportDirectory), end; entry != end; ++entry)
            {
                if (entry->path().filename().string().find(".tmp") != std::string::npos) leftover = true;
            }
            Check(!leftover, "no temporary left next to the report");

            bool threw = false;
            try { WriteReportFile(reportDirectory / "missing" / "report.json", first); }
            catch (const std::exception&) { threw = true; }
            Check(threw, "writing into a missing directory throws");
            Check(!std::filesystem::exists(reportDirectory / "missing"), "a failed write creates nothing");

            std::filesystem::remove_all(reportDirectory);
        }
        std::cout << "PASS JSON, timestamps, diagnosis replay, event scan order, ZIP whitelist, "
                     "report publishing, GPU mock invocation\n";
        return 0;
    }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
