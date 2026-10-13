#include "Collection.h"
#include "DiagnosisUtil.h"
#include "DiagnosisReport.h"
#include "AttributionRules.h"
#include "GpuSelectionProbe.h"
#include <Windows.h>
#include <fstream>
#include <iostream>
#include <stdexcept>

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
        std::filesystem::remove_all(directory);
        const auto mocks = RunAllGpuSelectionMockCases();
        Check(!mocks.empty(), "GPU mock cases preserved");
        for (const auto& mock : mocks) Check(mock.passed, "GPU mock regression");
        std::cout << "PASS JSON, timestamps, diagnosis replay, ZIP whitelist, GPU mock invocation\n";
        return 0;
    }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
