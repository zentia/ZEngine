#include "Diagnosis.h"

#include "AttributionRules.h"
#include "DiagnosisReport.h"
#include "DiagnosisUtil.h"
#include "EnvironmentProbe.h"
#include "EventLogReader.h"
#include "HardwareProbe.h"
#include "ReportWriter.h"

#include <Windows.h>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>

namespace zengine::envcheck::diagnosis
{
namespace
{
constexpr std::uint32_t MinLookbackHours = 1;
constexpr std::uint32_t MaxLookbackHours = 720;
constexpr std::uint32_t MinBudgetSeconds = 1;
constexpr std::uint32_t MaxBudgetSeconds = 300;
constexpr std::uint64_t MillisecondsPerSecond = 1000;
// The launcher and this tool share the machine clock; allow only a small skew.
constexpr UtcTicks IncidentFutureTolerance = 5ull * TicksPerMinute;

const wchar_t* const DiagnoseSwitch = L"--diagnose";
const char* const DiagnosisFileName = "OSGameDiagnosis.json";
const char* const EventsFileName = "OSGameDiagnosisEvents.tsv";

std::uint32_t ParseBoundedUnsigned(const std::wstring& text, std::uint32_t minimum, std::uint32_t maximum, const char* option)
{
    const std::string message = std::string(option) + " must be an integer between " +
        std::to_string(minimum) + " and " + std::to_string(maximum) + ".";
    if (text.empty() || text.find_first_not_of(L"0123456789") != std::wstring::npos)
    {
        throw std::invalid_argument(message);
    }
    const unsigned long long value = std::stoull(text);
    if (value < minimum || value > maximum)
    {
        throw std::invalid_argument(message);
    }
    return static_cast<std::uint32_t>(value);
}

std::wstring RequireValue(int& index, int argumentCount, wchar_t** arguments, const char* option)
{
    if (++index >= argumentCount)
    {
        throw std::invalid_argument(std::string(option) + " requires a value.");
    }
    return arguments[index];
}

std::string ReadWholeFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        throw std::runtime_error("Failed to open events file: " + WideToUtf8(path.wstring()));
    }
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}
} // namespace

bool IsDiagnosisCommandLine(int argumentCount, wchar_t** arguments)
{
    for (int index = 1; index < argumentCount; ++index)
    {
        if (std::wstring(arguments[index]) == DiagnoseSwitch)
        {
            return true;
        }
    }
    return false;
}

DiagnosisOptions ParseDiagnosisCommandLine(int argumentCount, wchar_t** arguments)
{
    DiagnosisOptions options;
    for (int index = 1; index < argumentCount; ++index)
    {
        const std::wstring argument = arguments[index];
        if (argument == DiagnoseSwitch)
        {
            continue;
        }
        if (argument == L"--output-dir")
        {
            options.outputDirectory = RequireValue(index, argumentCount, arguments, "--output-dir");
        }
        else if (argument == L"--incident-time")
        {
            const std::string text = WideToUtf8(RequireValue(index, argumentCount, arguments, "--incident-time"));
            if (!TryParseUtcIso8601(text, options.incidentTime))
            {
                throw std::invalid_argument(
                    "--incident-time must be ISO 8601 with a zone, e.g. 2026-10-12T03:04:05Z.");
            }
            options.hasIncidentTime = true;
        }
        else if (argument == L"--incident-kind")
        {
            const std::wstring value = RequireValue(index, argumentCount, arguments, "--incident-kind");
            if (value == L"crash")
            {
                options.incidentKind = IncidentKind::Crash;
            }
            else if (value == L"hang")
            {
                options.incidentKind = IncidentKind::Hang;
            }
            else if (value == L"unknown")
            {
                options.incidentKind = IncidentKind::Unknown;
            }
            else
            {
                throw std::invalid_argument("--incident-kind must be crash, hang or unknown.");
            }
        }
        else if (argument == L"--game-exe")
        {
            const std::string value = FileNameOf(WideToUtf8(RequireValue(index, argumentCount, arguments, "--game-exe")));
            if (value.empty())
            {
                throw std::invalid_argument("--game-exe requires an executable file name.");
            }
            options.gameExecutable = value;
        }
        else if (argument == L"--lookback-hours")
        {
            options.lookbackHours = ParseBoundedUnsigned(
                RequireValue(index, argumentCount, arguments, "--lookback-hours"),
                MinLookbackHours, MaxLookbackHours, "--lookback-hours");
        }
        else if (argument == L"--budget-seconds")
        {
            options.budgetSeconds = ParseBoundedUnsigned(
                RequireValue(index, argumentCount, arguments, "--budget-seconds"),
                MinBudgetSeconds, MaxBudgetSeconds, "--budget-seconds");
        }
        else if (argument == L"--diagnose-detail")
        {
            options.writeDetail = true;
        }
        else if (argument == L"--diagnose-from-events")
        {
            options.replayEventsFile = RequireValue(index, argumentCount, arguments, "--diagnose-from-events");
        }
        else if (argument == L"--quiet")
        {
            options.quiet = true;
        }
        else if (argument == L"--help" || argument == L"-h" || argument == L"/?")
        {
            options.showHelp = true;
        }
        else
        {
            throw std::invalid_argument("Unknown option in --diagnose mode.");
        }
    }
    return options;
}

void PrintDiagnosisUsage()
{
    std::cout
        << "ZWindowsEnvironmentCheck --diagnose [options]\n\n"
        << "Checks Windows event records around a game crash or hang and writes one\n"
        << "conclusion file (OSGameDiagnosis.json) for the launcher.\n\n"
        << "Options:\n"
        << "  --output-dir <path>          Output directory. Default: current directory.\n"
        << "  --incident-time <utc>        Time of the crash/hang, ISO 8601 with zone,\n"
        << "                               e.g. 2026-10-12T03:04:05Z.\n"
        << "  --incident-kind <kind>       crash, hang or unknown. Default: unknown.\n"
        << "  --game-exe <name>            Game executable name. Default: WXQ.exe.\n"
        << "  --lookback-hours N           Search range when no incident time is given.\n"
        << "                               Default: 72, range 1-720.\n"
        << "  --budget-seconds N           Total time budget. Default: 20, range 1-300.\n"
        << "  --diagnose-detail            Also write OSGameDiagnosisEvents.tsv (local only).\n"
        << "  --diagnose-from-events <f>   Replay an events file instead of reading the\n"
        << "                               event logs (for rule regression tests).\n"
        << "  --quiet                      Do not print the summary.\n\n"
        << "Exit codes: 0=conclusion written, 30=tool error.\n";
}

int RunDiagnosis(const DiagnosisOptions& options)
{
    if (options.showHelp)
    {
        PrintDiagnosisUsage();
        return 0;
    }
    std::filesystem::create_directories(options.outputDirectory);

    const UtcTicks generatedAt = CurrentUtcTime();
    const bool replay = !options.replayEventsFile.empty();
    const UtcTicks lookbackSpan = static_cast<UtcTicks>(options.lookbackHours) * TicksPerHour;

    EventCollection collection;
    if (replay)
    {
        collection = ParseEventsFile(ReadWholeFile(options.replayEventsFile));
    }
    else
    {
        if (options.hasIncidentTime && options.incidentTime > generatedAt + IncidentFutureTolerance)
        {
            throw std::invalid_argument("--incident-time is in the future.");
        }
        UtcTicks rangeStart = SubtractClamped(generatedAt, lookbackSpan);
        if (options.hasIncidentTime)
        {
            rangeStart = std::min(rangeStart, SubtractClamped(options.incidentTime, IncidentWindowMargin));
        }
        EventLogReadOptions readOptions;
        readOptions.rangeStart = rangeStart;
        readOptions.rangeEnd = generatedAt;
        readOptions.deadlineTickCount =
            GetTickCount64() + static_cast<std::uint64_t>(options.budgetSeconds) * MillisecondsPerSecond;
        collection = ReadEventLogs(readOptions);
    }

    DiagnosisRequest request;
    request.hasLauncherIncidentTime = options.hasIncidentTime;
    request.launcherIncidentTime = options.incidentTime;
    request.launcherIncidentKind = options.incidentKind;
    request.gameExecutableName = options.gameExecutable;
    request.now = collection.rangeEnd != 0 ? collection.rangeEnd : collection.collectedAt;
    request.lookbackStart = SubtractClamped(request.now, lookbackSpan);
    request.lookbackHours = options.lookbackHours;
    const DiagnosisResult result = Diagnose(request, collection);

    DiagnosisReportContext context;
    context.source = replay ? "replay" : "live";
    context.generatedAt = generatedAt;
    context.gameExecutable = options.gameExecutable;
    context.lookbackHours = options.lookbackHours;

    // Write the conclusion before touching the graphics stack: if adapter
    // enumeration misbehaves on a broken driver, the launcher still gets a verdict.
    const auto diagnosisPath = options.outputDirectory / DiagnosisFileName;
    WriteReportFile(diagnosisPath, BuildDiagnosisJson(result, context));
    if (options.writeDetail)
    {
        WriteReportFile(options.outputDirectory / EventsFileName, BuildEventsFile(collection));
    }
    if (!replay)
    {
        EnvironmentSnapshot snapshot;
        try
        {
            CollectGraphicsInformation(snapshot);
            context.gpus = &snapshot.gpuAdapters;
            WriteReportFile(diagnosisPath, BuildDiagnosisJson(result, context));
        }
        catch (const std::exception&)
        {
            // Keep the conclusion already written without GPU details.
        }
    }

    if (!options.quiet)
    {
        std::cout << "Verdict: " << ToString(result.verdict)
                  << " (confidence: " << ToString(result.confidence) << ")\n"
                  << result.userMessageZh << '\n';
    }
    std::cout << "Diagnosis written to:\n  " << WideToUtf8(diagnosisPath.wstring()) << '\n';
    return 0;
}
} // namespace zengine::envcheck::diagnosis
