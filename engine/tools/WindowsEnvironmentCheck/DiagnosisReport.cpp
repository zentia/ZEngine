#include "DiagnosisReport.h"

#include "DiagnosisUtil.h"
#include "ToolVersion.h"

#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace zengine::envcheck::diagnosis
{
namespace
{
constexpr int DiagnosisSchemaVersion = 1;
const std::string EventsFileHeader = "#OSGameDiagnosisEvents";
// Version 2 adds the priorityWindow* observability keys. Version 1 files stay
// replayable: the missing keys simply keep their defaults.
constexpr int EventsFileVersion = 2;
constexpr int OldestSupportedEventsFileVersion = 1;
constexpr std::size_t EventLineFieldCount = 14;
constexpr std::size_t HexIdWidth = 4;

const char* BoolText(bool value)
{
    return value ? "true" : "false";
}

std::string EscapeJson(const std::string& value)
{
    std::ostringstream stream;
    for (const unsigned char character : value)
    {
        switch (character)
        {
        case '\\': stream << "\\\\"; break;
        case '"': stream << "\\\""; break;
        case '\b': stream << "\\b"; break;
        case '\f': stream << "\\f"; break;
        case '\n': stream << "\\n"; break;
        case '\r': stream << "\\r"; break;
        case '\t': stream << "\\t"; break;
        default:
            if (character < 0x20)
            {
                stream << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                       << static_cast<int>(character) << std::dec;
            }
            else
            {
                stream << static_cast<char>(character);
            }
            break;
        }
    }
    return stream.str();
}

std::string JsonString(const std::string& value)
{
    return "\"" + EscapeJson(value) + "\"";
}

std::string JsonTime(UtcTicks time)
{
    const std::string text = FormatUtcIso8601(time);
    return text.empty() ? std::string("null") : JsonString(text);
}

std::string HexId(std::uint32_t value)
{
    std::ostringstream stream;
    stream << "0x" << std::uppercase << std::hex << std::setw(static_cast<int>(HexIdWidth))
           << std::setfill('0') << value;
    return stream.str();
}

std::string CleanField(std::string value)
{
    for (char& character : value)
    {
        if (character == '\t' || character == '\r' || character == '\n')
        {
            character = ' ';
        }
    }
    return value;
}

std::vector<std::string> SplitTabs(const std::string& line)
{
    std::vector<std::string> fields;
    std::size_t start = 0;
    while (true)
    {
        const std::size_t tab = line.find('\t', start);
        if (tab == std::string::npos)
        {
            fields.push_back(line.substr(start));
            break;
        }
        fields.push_back(line.substr(start, tab - start));
        start = tab + 1;
    }
    return fields;
}

std::uint64_t ParseUnsigned(const std::string& text, std::size_t lineNumber)
{
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
    {
        throw std::runtime_error("Events file line " + std::to_string(lineNumber) + ": invalid number '" + text + "'.");
    }
    return std::stoull(text);
}

bool ParseFlag(const std::string& text, std::size_t lineNumber)
{
    if (text == "1")
    {
        return true;
    }
    if (text == "0")
    {
        return false;
    }
    throw std::runtime_error("Events file line " + std::to_string(lineNumber) + ": invalid flag '" + text + "'.");
}

void WriteActions(std::ostringstream& stream, const std::vector<UserAction>& actions)
{
    stream << "  \"actions\": [";
    for (std::size_t index = 0; index < actions.size(); ++index)
    {
        stream << (index == 0 ? "\n" : ",\n")
               << "    {\"code\": " << JsonString(actions[index].code)
               << ", \"zh\": " << JsonString(actions[index].zh) << "}";
    }
    stream << (actions.empty() ? "],\n" : "\n  ],\n");
}

void WriteFindings(std::ostringstream& stream, const std::vector<Finding>& findings)
{
    stream << "  \"findings\": [";
    for (std::size_t index = 0; index < findings.size(); ++index)
    {
        const Finding& finding = findings[index];
        stream << (index == 0 ? "\n" : ",\n")
               << "    {\"ruleId\": " << JsonString(finding.ruleId)
               << ", \"code\": " << JsonString(finding.code)
               << ", \"layer\": " << JsonString(ToString(finding.layer))
               << ", \"strength\": " << JsonString(ToString(finding.strength))
               << ", \"proximity\": " << JsonString(ToString(finding.proximity))
               << ", \"timeUtc\": " << JsonTime(finding.time)
               << ", \"deltaSeconds\": ";
        if (finding.hasDelta)
        {
            stream << finding.deltaSeconds;
        }
        else
        {
            stream << "null";
        }
        stream << ", \"count\": " << finding.count
               << ", \"evidence\": " << JsonString(finding.evidence) << "}";
    }
    stream << (findings.empty() ? "],\n" : "\n  ],\n");
}

void WriteGpus(std::ostringstream& stream, const std::vector<GpuAdapterInfo>& gpus)
{
    stream << "    \"gpus\": [";
    for (std::size_t index = 0; index < gpus.size(); ++index)
    {
        const GpuAdapterInfo& gpu = gpus[index];
        stream << (index == 0 ? "\n" : ",\n")
               << "      {\"name\": " << JsonString(gpu.name)
               << ", \"vendorId\": " << JsonString(HexId(gpu.vendorId))
               << ", \"deviceId\": " << JsonString(HexId(gpu.deviceId))
               << ", \"driverVersion\": " << JsonString(gpu.driverVersion)
               << ", \"driverDate\": " << JsonString(gpu.driverDate)
               << ", \"softwareAdapter\": " << BoolText(gpu.softwareAdapter) << "}";
    }
    stream << (gpus.empty() ? "]\n" : "\n    ]\n");
}
} // namespace

std::string BuildDiagnosisJson(const DiagnosisResult& result, const DiagnosisReportContext& context)
{
    const Incident& incident = result.incident;
    const Observability& observability = result.observability;
    std::ostringstream stream;
    stream << "{\n"
           << "  \"schemaVersion\": " << DiagnosisSchemaVersion << ",\n"
           << "  \"toolVersion\": " << JsonString(ToolVersion) << ",\n"
           << "  \"mode\": \"diagnose\",\n"
           << "  \"source\": " << JsonString(context.source) << ",\n"
           << "  \"generatedAtUtc\": " << JsonTime(context.generatedAt) << ",\n"
           << "  \"verdict\": " << JsonString(ToString(result.verdict)) << ",\n"
           << "  \"confidence\": " << JsonString(ToString(result.confidence)) << ",\n"
           << "  \"userMessage\": {\"code\": " << JsonString(result.userMessageCode)
           << ", \"zh\": " << JsonString(result.userMessageZh) << "},\n";
    WriteActions(stream, result.actions);

    stream << "  \"incident\": {\n"
           << "    \"anchor\": " << JsonString(ToString(incident.anchor)) << ",\n"
           << "    \"kind\": " << JsonString(ToString(incident.kind)) << ",\n"
           << "    \"timeUtc\": " << JsonTime(incident.time) << ",\n"
           << "    \"nearWindowStartUtc\": " << JsonTime(incident.nearStart) << ",\n"
           << "    \"nearWindowEndUtc\": " << JsonTime(incident.nearEnd) << ",\n"
           << "    \"windowStartUtc\": " << JsonTime(incident.windowStart) << ",\n"
           << "    \"windowEndUtc\": " << JsonTime(incident.windowEnd) << ",\n"
           << "    \"faultModule\": " << JsonString(FileNameOf(incident.faultModuleName)) << ",\n"
           << "    \"exceptionCode\": " << JsonString(incident.exceptionCode) << "\n"
           << "  },\n";

    stream << "  \"observability\": {\n"
           << "    \"ok\": " << BoolText(result.observabilityOk) << ",\n"
           << "    \"issues\": [";
    for (std::size_t index = 0; index < result.observabilityIssues.size(); ++index)
    {
        stream << (index == 0 ? "" : ", ") << JsonString(result.observabilityIssues[index]);
    }
    stream << "],\n"
           << "    \"systemLogReadable\": " << BoolText(observability.systemLogReadable) << ",\n"
           << "    \"applicationLogReadable\": " << BoolText(observability.applicationLogReadable) << ",\n"
           << "    \"systemLogOldestUtc\": "
           << (observability.systemOldestKnown ? JsonTime(observability.systemOldest) : std::string("null")) << ",\n"
           << "    \"systemEventsInWindow\": " << result.systemEventsInWindow << ",\n"
           << "    \"budgetExceeded\": " << BoolText(observability.budgetExceeded) << ",\n"
           << "    \"eventCapReached\": " << BoolText(observability.eventCapReached) << ",\n"
           << "    \"queryErrorOccurred\": " << BoolText(observability.queryErrorOccurred) << ",\n"
           // Added in 1.7.3: tells whether a truncated scan actually lost
           // incident evidence or only context. Purely additive, so launchers
           // that read verdict/confidence/userMessage/actions are unaffected.
           << "    \"priorityWindowRequested\": " << BoolText(observability.priorityWindowRequested) << ",\n"
           << "    \"priorityWindowComplete\": " << BoolText(observability.priorityWindowComplete) << "\n"
           << "  },\n";

    WriteFindings(stream, result.findings);

    stream << "  \"parameters\": {\"gameExecutable\": " << JsonString(context.gameExecutable)
           << ", \"lookbackHours\": " << context.lookbackHours << "},\n"
           << "  \"environment\": {\n";
    WriteGpus(stream, context.gpus != nullptr ? *context.gpus : std::vector<GpuAdapterInfo>{});
    stream << "  }\n"
           << "}\n";
    return stream.str();
}

std::string BuildEventsFile(const EventCollection& collection)
{
    const Observability& observability = collection.observability;
    std::ostringstream stream;
    stream << EventsFileHeader << '\t' << EventsFileVersion << '\n'
           << "C\tcollectedAt\t" << collection.collectedAt << '\n'
           << "C\trangeStart\t" << collection.rangeStart << '\n'
           << "C\trangeEnd\t" << collection.rangeEnd << '\n'
           << "O\tsystemLogReadable\t" << (observability.systemLogReadable ? 1 : 0) << '\n'
           << "O\tapplicationLogReadable\t" << (observability.applicationLogReadable ? 1 : 0) << '\n'
           << "O\tsystemOldestKnown\t" << (observability.systemOldestKnown ? 1 : 0) << '\n'
           << "O\tsystemOldest\t" << observability.systemOldest << '\n'
           << "O\tbudgetExceeded\t" << (observability.budgetExceeded ? 1 : 0) << '\n'
           << "O\teventCapReached\t" << (observability.eventCapReached ? 1 : 0) << '\n'
           << "O\tqueryErrorOccurred\t" << (observability.queryErrorOccurred ? 1 : 0) << '\n'
           << "O\tpriorityWindowRequested\t" << (observability.priorityWindowRequested ? 1 : 0) << '\n'
           << "O\tpriorityWindowComplete\t" << (observability.priorityWindowComplete ? 1 : 0) << '\n';
    for (const UtcTicks time : collection.systemEventTimes)
    {
        stream << "T\t" << time << '\n';
    }
    for (const auto& event : collection.events)
    {
        stream << "E\t" << (event.channel == EventChannel::System ? 'S' : 'A')
               << '\t' << event.time
               << '\t' << FormatUtcIso8601(event.time)
               << '\t' << CleanField(event.provider)
               << '\t' << event.eventId
               << '\t' << event.level
               << '\t' << CleanField(event.appName)
               << '\t' << CleanField(event.appPath)
               << '\t' << CleanField(event.moduleName)
               << '\t' << CleanField(event.modulePath)
               << '\t' << CleanField(event.exceptionCode)
               << '\t' << CleanField(event.werEventName)
               << '\t' << CleanField(event.werP1)
               << '\n';
    }
    return stream.str();
}

EventCollection ParseEventsFile(const std::string& content)
{
    EventCollection collection;
    Observability& observability = collection.observability;
    std::istringstream input(content);
    std::string line;
    std::size_t lineNumber = 0;
    bool headerSeen = false;
    while (std::getline(input, line))
    {
        ++lineNumber;
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        if (line.empty())
        {
            continue;
        }
        const auto fields = SplitTabs(line);
        if (!headerSeen)
        {
            if (fields.size() < 2 || fields[0] != EventsFileHeader)
            {
                throw std::runtime_error("Events file has an unsupported header.");
            }
            const std::uint64_t version = ParseUnsigned(fields[1], lineNumber);
            // Older samples stay replayable so recorded regressions keep working.
            if (version < static_cast<std::uint64_t>(OldestSupportedEventsFileVersion) ||
                version > static_cast<std::uint64_t>(EventsFileVersion))
            {
                throw std::runtime_error("Events file version " + std::to_string(version) + " is not supported.");
            }
            headerSeen = true;
            continue;
        }

        const std::string& kind = fields[0];
        if ((kind == "C" || kind == "O") && fields.size() == 3)
        {
            const std::string& key = fields[1];
            const std::string& value = fields[2];
            if (key == "collectedAt") collection.collectedAt = ParseUnsigned(value, lineNumber);
            else if (key == "rangeStart") collection.rangeStart = ParseUnsigned(value, lineNumber);
            else if (key == "rangeEnd") collection.rangeEnd = ParseUnsigned(value, lineNumber);
            else if (key == "systemLogReadable") observability.systemLogReadable = ParseFlag(value, lineNumber);
            else if (key == "applicationLogReadable") observability.applicationLogReadable = ParseFlag(value, lineNumber);
            else if (key == "systemOldestKnown") observability.systemOldestKnown = ParseFlag(value, lineNumber);
            else if (key == "systemOldest") observability.systemOldest = ParseUnsigned(value, lineNumber);
            else if (key == "budgetExceeded") observability.budgetExceeded = ParseFlag(value, lineNumber);
            else if (key == "eventCapReached") observability.eventCapReached = ParseFlag(value, lineNumber);
            else if (key == "queryErrorOccurred") observability.queryErrorOccurred = ParseFlag(value, lineNumber);
            else if (key == "priorityWindowRequested") observability.priorityWindowRequested = ParseFlag(value, lineNumber);
            else if (key == "priorityWindowComplete") observability.priorityWindowComplete = ParseFlag(value, lineNumber);
            else throw std::runtime_error("Events file line " + std::to_string(lineNumber) + ": unknown key '" + key + "'.");
        }
        else if (kind == "T" && fields.size() == 2)
        {
            collection.systemEventTimes.push_back(ParseUnsigned(fields[1], lineNumber));
        }
        else if (kind == "E" && fields.size() == EventLineFieldCount)
        {
            NormalizedEvent event;
            if (fields[1] == "S")
            {
                event.channel = EventChannel::System;
            }
            else if (fields[1] == "A")
            {
                event.channel = EventChannel::Application;
            }
            else
            {
                throw std::runtime_error("Events file line " + std::to_string(lineNumber) + ": unknown channel.");
            }
            event.time = ParseUnsigned(fields[2], lineNumber);
            // fields[3] is the human-readable time and is ignored on input.
            event.provider = fields[4];
            event.eventId = static_cast<std::uint32_t>(ParseUnsigned(fields[5], lineNumber));
            event.level = static_cast<std::uint32_t>(ParseUnsigned(fields[6], lineNumber));
            event.appName = fields[7];
            event.appPath = fields[8];
            event.moduleName = fields[9];
            event.modulePath = fields[10];
            event.exceptionCode = fields[11];
            event.werEventName = fields[12];
            event.werP1 = fields[13];
            collection.events.push_back(std::move(event));
        }
        else
        {
            throw std::runtime_error("Events file line " + std::to_string(lineNumber) + ": malformed record.");
        }
    }
    if (!headerSeen)
    {
        throw std::runtime_error("Events file is empty.");
    }
    return collection;
}
} // namespace zengine::envcheck::diagnosis
