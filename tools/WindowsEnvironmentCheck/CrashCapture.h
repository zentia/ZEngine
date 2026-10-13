#pragma once
#include "Collection.h"
#include "EvidenceFiles.h"

namespace zengine::envcheck::collector
{
struct CaptureOptions
{
    Options environment;
    std::filesystem::path procDump;
    std::filesystem::path gameExecutable;
    bool memoryConsent = false;
    bool licenseAccepted = false;
    unsigned waitSeconds = 3600;
};
struct CaptureResult
{
    Result report;
    bool dumpCaptured = false;
    std::string message;
};
std::string ValidateProcDump(const std::filesystem::path& file);
CaptureResult CaptureCrash(const CaptureOptions& options, const std::atomic_bool& stop,
                           const std::function<void(const std::string&, bool)>& progress);
std::filesystem::path LoadProcDumpPreference();
void SaveProcDumpPreference(const std::filesystem::path& file);
}
