#pragma once

#include "DiagnosisTypes.h"
#include "EnvironmentProbe.h"

#include <cstdint>
#include <string>
#include <vector>

namespace zengine::envcheck::diagnosis
{
struct DiagnosisReportContext
{
    std::string source;
    UtcTicks generatedAt = 0;
    std::string gameExecutable;
    std::uint32_t lookbackHours = 0;
    const std::vector<GpuAdapterInfo>* gpus = nullptr;
};

// OSGameDiagnosis.json: the only file the launcher needs to read.
std::string BuildDiagnosisJson(const DiagnosisResult& result, const DiagnosisReportContext& context);

// Tab-separated events file. Written by --diagnose-detail, read back by
// --diagnose-from-events so rules can be regression-tested without a live machine.
std::string BuildEventsFile(const EventCollection& collection);
EventCollection ParseEventsFile(const std::string& content);
} // namespace zengine::envcheck::diagnosis
