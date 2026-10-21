#pragma once
#include "DiagnosisTypes.h"
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace zengine::envcheck::collector
{
struct Profile
{
    std::string id = "generic";
    std::string displayName = "Windows 环境采集";
    std::string gameExecutable;
    unsigned lookbackHours = 336;
    // Game logs are optional: an empty logSubPath disables the gamelogs stage.
    std::string logRoot = "localLow"; // localLow | localAppData | roaming | programData
    std::string logSubPath; // e.g. "Tencent\osgameCNOfficial\Cache\Logs", relative to logRoot
};
struct Options
{
    Profile profile;
    std::filesystem::path outputDirectory;
    // Resolved game log directory; overrides Profile when set from the command line.
    std::filesystem::path logDirectory;
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

// ---------------------------------------------------------------------------
// Incident anchor -- the three inputs that decide whether a package can be
// attributed at all.
// ---------------------------------------------------------------------------
// Without an incident time, an incident kind and the game exe name, a package
// can only answer "what does this machine look like" -- it cannot answer "was
// that the game". Three real cases (20261013 r809053, 20261015 #45665,
// 20261016 Lenovo 82RC) came back with one or more of these missing and were
// useless for attribution, which is why the tool now says so loudly instead of
// shipping a package that looks complete.
//
// Not an error: a pure machine-health sweep is a legitimate use, so a missing
// anchor warns and is recorded, never refuses to collect.
struct AttributionAnchor
{
    bool incidentTime = false;     // options.incidentTime != 0
    bool incidentKind = false;     // options.incidentKind != Unknown
    bool gameExecutable = false;   // profile.gameExecutable non-empty
    bool Usable() const { return incidentTime && incidentKind && gameExecutable; }
};
AttributionAnchor DescribeAttributionAnchor(const Options& options);

// summary.txt header, in Chinese on purpose: report files are a deliverable for
// support, not a player-facing UI, so they never follow the language switch.
// Empty when the anchor is complete.
std::string BuildAttributionWarning(const AttributionAnchor& anchor);

// manifest.json fragment: "\"attributionUsable\":false,\"missingAttributionInputs\":[...]".
// Machine-readable so a triage script can sort incoming packages before a human
// opens any of them.
std::string BuildAttributionManifestFields(const AttributionAnchor& anchor);
// Game log packaging (stage "gamelogs").
constexpr std::uint32_t MaxGameLogFiles = 8;
constexpr std::uint64_t MaxGameLogFileBytes = 16ull << 20; // matches the report archive per-file cap
constexpr std::uint64_t MaxGameLogTotalBytes = 48ull << 20;
constexpr int MaxGameLogScanDepth = 3; // 1 = the log directory itself, no subdirectories
constexpr const char* GameLogNamePrefix = "gamelog-";

enum class GameLogStatus { Ok, NoLogs };
struct GameLogCollection
{
    GameLogStatus status = GameLogStatus::NoLogs;
    std::string json;
    std::vector<std::string> archiveNames;
};
// Copies the newest .log/.logcompr/.txt files of logDirectory into outputDirectory and returns the index JSON.
// Files are never modified or deleted; oversized ones are skipped and recorded in the index.
GameLogCollection CollectGameLogs(const std::filesystem::path& logDirectory, const std::filesystem::path& outputDirectory,
                                  unsigned lookbackHours, diagnosis::UtcTicks incidentTime);
std::filesystem::path ResolveGameLogDirectory(const Profile& profile);
// True for names this tool can have written into the report directory (archive whitelist).
bool IsArchivedGameLogName(const std::string& name);

// The shareable copy of the event list: process names are replaced and only the
// boolean "is this our game" survives. Split out of the events stage so the
// redaction itself can be checked without reading the live event log.
std::string BuildRedactedEventsJson(const std::vector<diagnosis::NormalizedEvent>& events, bool gameExecutableSpecified,
                                    const std::string& gameExecutable);

// Stand-in passed to a stage worker when no game exe was specified -- --game-exe
// needs a value, and an empty one is indistinguishable from a missing argument.
// It is never a real exe name: the worker turns it back into "unset" the moment
// it receives it, because a placeholder that reaches the attribution rules (or
// events-redacted.json's gameExecutableSpecified) reads as a real game and
// silently turns "we were never told" into "definitely not the game".
constexpr const char* UnspecifiedGamePlaceholder = "__unspecified_game__.exe";

// File name of the single distributed tool executable. After the collector GUI,
// the GPU inspector GUI and the CLI merged into one shell, this is the only name
// the package ships; the collector still re-launches *itself* under it, once as
// an isolated stage worker (RunStageProcess) and once elevated for WER registry
// changes (WerRegistry.cpp). Both call sites read this constant so renaming the
// output can never leave one of them pointing at a file that no longer exists.
constexpr const wchar_t* ToolExecutableName = L"ZEnvironmentCollector.exe";
std::filesystem::path ToolExecutablePath();

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
