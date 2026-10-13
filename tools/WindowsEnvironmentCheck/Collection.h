#pragma once
#include "DiagnosisTypes.h"
#include <atomic>
#include <filesystem>
#include <functional>
#include <string>

namespace zengine::envcheck::collector
{
struct Profile
{
    std::string id = "generic";
    std::string displayName = "Windows 环境采集";
    std::string gameExecutable;
    unsigned lookbackHours = 336;
};
struct Options
{
    Profile profile;
    std::filesystem::path outputDirectory;
    diagnosis::UtcTicks incidentTime = 0;
    diagnosis::IncidentKind incidentKind = diagnosis::IncidentKind::Unknown;
    bool includeEvents = false;
    unsigned stageTimeoutSeconds = 45;
};
struct Result
{
    std::filesystem::path archive;
    std::filesystem::path directory;
    bool partial = false;
    bool cancelled = false;
};
std::filesystem::path ExecutableDirectory();
std::filesystem::path DefaultOutputDirectory();
Profile LoadProfile(const std::filesystem::path& file);
Result Collect(const Options&, const std::atomic_bool& cancel, const std::function<void(const std::string&, bool cancellable)>& progress);
bool IsCollectionCommand(int argc, wchar_t** argv);
int RunCollectionCommand(int argc, wchar_t** argv);
std::string JsonString(const std::string& value);
std::string CollectFirmwareJson(bool& complete);
std::string CollectHealthEventsJson(unsigned lookbackHours, bool& complete);
void WriteZip(const std::filesystem::path& directory, const std::filesystem::path& output,
              const std::vector<std::string>& files);
}
