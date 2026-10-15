#include "Collection.h"
#include "DiagnosisUtil.h"
#include "DiagnosisReport.h"
#include "AttributionRules.h"
#include "EventLogReader.h"
#include "GpuSelectionProbe.h"
#include <Windows.h>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace zengine::envcheck;
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
int main()
{
    try
    {
        using namespace collector;
        Check(JsonString("a\n\"\\") == "\"a\\u000a\\\"\\\\\"", "JSON escaping");
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
        std::cout << "PASS JSON, timestamps, diagnosis replay, event scan order, ZIP whitelist, GPU mock invocation\n";
        return 0;
    }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
