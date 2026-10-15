#pragma once

#include "DiagnosisTypes.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace zengine::envcheck::diagnosis
{
struct DiagnosisOptions
{
    std::filesystem::path outputDirectory = std::filesystem::current_path();
    bool hasIncidentTime = false;
    UtcTicks incidentTime = 0;
    IncidentKind incidentKind = IncidentKind::Unknown;
    std::string gameExecutable = "WXQ.exe";
    std::uint32_t lookbackHours = 72;
    std::uint32_t budgetSeconds = 20;
    bool writeDetail = false;
    std::filesystem::path replayEventsFile;
    bool quiet = false;
    bool showHelp = false;
};

bool IsDiagnosisCommandLine(int argumentCount, wchar_t** arguments);
DiagnosisOptions ParseDiagnosisCommandLine(int argumentCount, wchar_t** arguments);
void PrintDiagnosisUsage();

// Writes OSGameDiagnosis.json (and OSGameDiagnosisEvents.tsv with --diagnose-detail).
// Returns 0 when a conclusion was written; throws on tool errors.
int RunDiagnosis(const DiagnosisOptions& options);
} // namespace zengine::envcheck::diagnosis
