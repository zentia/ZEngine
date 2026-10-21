#pragma once

#include "DiagnosisTypes.h"
#include "EnvironmentProbe.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace zengine::envcheck::preflight
{
// Why a verdict set of its own, and not the diagnosis one: a diagnosis answers
// "was the system involved in a crash that already happened", a question with no
// "everything is fine" state. A preflight answers "will this machine have a bad
// time", and the launcher has to tell three things apart that a single
// "Inconclusive" cannot carry -- nothing to report, something worth mentioning,
// and no idea because the check did not finish.
enum class PreflightVerdict
{
    Pass,          // nothing worth telling the player
    Advise,        // something worth mentioning; the launcher must not block on it
    BelowMinimum,  // the machine cannot run the game as shipped
    Inconclusive   // the check did not finish: fail open
};
const char* ToString(PreflightVerdict value);

struct PreflightOptions
{
    // Never defaults to the current directory. A launcher spawns this tool with
    // the game install directory as the working directory, and dropping a new
    // file there is what makes the launcher's own integrity scan decide the
    // install changed and start repairing.
    std::filesystem::path outputDirectory;
    // Which volume's free space to check. Defaults to the directory this tool
    // runs from, which ships next to the game executable.
    std::filesystem::path gameDirectory;
    // Where the game's minimum spec is read from. Empty means
    // <gameDirectory>/min-spec.json, and a game that ships no such file gets
    // the built-in fallback rather than an error.
    std::filesystem::path minSpecPath;
    std::string gameExecutable = "WXQ.exe";
    std::uint32_t budgetSeconds = 3;
    bool quiet = false;
    bool showHelp = false;
};

// One thing the preflight noticed. `layer` is the same vocabulary the diagnosis
// findings use, so a launcher that groups by layer can reuse its own code.
struct PreflightFinding
{
    std::string code;
    std::string layer;
    std::string zh;
    std::string evidence;
};

struct PreflightResult
{
    PreflightVerdict verdict = PreflightVerdict::Inconclusive;
    // The diagnosis types are reused on purpose: the launcher reads these four
    // keys out of two different files, and a second spelling of "confidence" is
    // one more thing for it to get wrong.
    diagnosis::Confidence confidence = diagnosis::Confidence::Low;
    std::string userMessageCode;
    std::string userMessageZh;
    std::vector<diagnosis::UserAction> actions;
    std::vector<PreflightFinding> findings;
    // False when the probes did not finish inside the budget. The verdict is
    // Inconclusive in that case, and this is the flag that says why -- the
    // launcher logs it, because "the check timed out on this machine" is itself
    // worth knowing about.
    bool probesComplete = false;
    bool budgetExceeded = false;
    std::vector<std::string> notes;
};

// Everything the preflight is allowed to look at. Kept separate from the probe
// The game renders through Direct3D 11, so "can the adapter this machine is
// going to pick actually initialise D3D11" is the one graphics question whose
// answer is launch-blocking. There is no cheap way to ask it:
// IDXGIAdapter::CheckInterfaceSupport returns DXGI_ERROR_UNSUPPORTED for D3D11
// on every adapter measured, so a driver has to be started to get a truthful
// answer. That costs tens of milliseconds, which is why this is collected
// inside the probe budget rather than after it.
struct Direct3D11Probe
{
    // False when the question was never asked: no d3d11.dll, no adapter, or the
    // probe did not finish. "Did not ask" must never read as "cannot".
    bool known = false;
    bool deviceCreated = false;
    // True when something could create a device but only the software adapter
    // could -- i.e. no GPU driver is installed. Reported as "unusable", never as
    // "cannot start": Unity does start on WARP, at a few frames a second.
    bool softwareOnlyAdapter = false;
    // True when the adapter that produced the device is the one the game picks.
    // A machine where only some *other* adapter can do D3D11 may still start, so
    // it advises instead of blocking -- blocking a player who can actually play
    // is the one mistake here that cannot be walked back.
    bool onPreferredAdapter = false;
    std::uint32_t featureLevel = 0;
    std::string adapterName;
    // Why the preferred adapter said no. Worth keeping: "the driver does not
    // support this" and "the driver crashed" are different phone calls.
    std::uint32_t failureHresult = 0;
};

// Feature levels are named in min-spec.json ("11_0") and compared as the raw
// D3D_FEATURE_LEVEL number the runtime hands back, so both spellings live here
// and no DirectX header has to be pulled into EnvironmentProbe.h.
std::uint32_t ParseFeatureLevelName(const std::string& name);
std::string FeatureLevelName(std::uint32_t value);
// HRESULTs land in the report the way support reads them, not the way C++ does.
std::string HexText(std::uint32_t value);

// calls so the rules can be regression-tested with a synthetic machine: a CI
// agent has no GPU, and a rule that cannot be tested is a rule nobody trusts.
struct PreflightInputs
{
    CpuFeatures cpu;
    SystemInfo system;
    std::vector<GpuAdapterInfo> gpus;
    GpuSelectionRealProbeResult gpuSelection;

    // "Known" flags matter as much as the values: an empty adapter list means
    // either "no GPU" or "enumeration failed", and those are different answers.
    bool gpusKnown = false;
    bool gpuSelectionKnown = false;

    std::string gameVolumeRoot;
    std::uint64_t gameVolumeFreeBytes = 0;
    bool gameVolumeKnown = false;

    // SMBIOS system-enclosure type. It is here for one rule only: telling a
    // desktop apart from a laptop. On a switchable-graphics laptop the panel is
    // driven by the integrated part by design, so "the monitor is on the
    // integrated GPU" is the normal state there and saying otherwise would fire
    // on most gaming laptops sold.
    unsigned int chassisType = 0;
    bool chassisKnown = false;

    bool insiderPreview = false;
    bool insiderPreviewKnown = false;

    // The game's DLLs link the Visual C++ runtime dynamically unless they were
    // built /MT. Without it the process dies with 0xc0000135 before WinMain and
    // the player sees nothing at all -- the classic "double-click, no reaction"
    // report. Read from the runtime's own registry key and from an app-local
    // copy beside the game, because both are legitimate ways to have it.
    bool vcRuntimePresent = false;
    bool vcRuntimeKnown = false;

    Direct3D11Probe direct3d11;
};

struct PreflightReportContext
{
    std::string source = "live";
    diagnosis::UtcTicks generatedAt = 0;
    std::string gameExecutable;
    std::uint32_t budgetSeconds = 0;
    // What the verdict was measured against, and where those numbers came from.
    // A player whose "Pass" looks wrong is a support ticket, and the answer is
    // the spec that was applied -- not the one this tool happened to ship with.
    Requirements requirements;
    std::string requirementsSource = "default";
};

Requirements DefaultPreflightRequirements();

// A game's minimum spec is data about the game, not about this tool, so it does
// not live in the binary: it ships beside the game in min-spec.json and is read
// at every launch. What stays compiled in is only a fallback for a game that
// ships no file.
std::filesystem::path DefaultMinSpecPath(const std::filesystem::path& gameDirectory);

// Pure: file text in, requirements out. Exposed so reading a hand-written file
// is tested without one on disk.
bool ParseRequirementsJson(const std::string& text, Requirements& requirements,
                           std::vector<std::string>& problems);

struct LoadedRequirements
{
    Requirements requirements;
    // Verbatim in the report -- see the note on PreflightReportContext.
    std::string source = "default";
    // Empty on the happy path. Populated only when a file was found and could
    // not be used: a game that ships no file is a supported configuration, and
    // reporting it would put a line in every report for nothing.
    std::vector<std::string> problems;
};

// Reads `overridePath` when given, else <gameDirectory>/min-spec.json. Never
// fails and never throws: absent, unreadable or malformed all fall back to
// DefaultPreflightRequirements(). A preflight that refuses to run because of a
// config file is a preflight that stops a launch over a typo.
LoadedRequirements LoadRequirements(const std::filesystem::path& gameDirectory,
                                    const std::filesystem::path& overridePath);

// Pure: inputs in, conclusion out. The launcher contract lives here, not in the
// probes, so "what counts as below minimum" is one function you can read.
PreflightResult EvaluatePreflight(
    const PreflightInputs& inputs,
    const Requirements& requirements,
    bool probesComplete);

std::string BuildPreflightJson(
    const PreflightResult& result,
    const PreflightInputs& inputs,
    const PreflightReportContext& context);

// One preflight per output directory at a time. Two instances racing on the same
// report file is not a theoretical worry: a player who double-clicks a launcher,
// or a launcher that retries, gets two processes, and the slower one renames its
// stale conclusion over the newer one. The loser exits without writing instead.
//
// Held for the lifetime of the object; the handle is void* so this header does
// not have to drag <Windows.h> into every translation unit that includes it.
class PreflightInstance
{
public:
    explicit PreflightInstance(const std::filesystem::path& outputDirectory);
    PreflightInstance(const PreflightInstance&) = delete;
    PreflightInstance& operator=(const PreflightInstance&) = delete;
    ~PreflightInstance();

    bool Acquired() const { return acquired_; }

private:
    void* handle_ = nullptr;
    bool acquired_ = false;
};

// Waits for the probe worker, at most `milliseconds`. Returns false when the
// worker is still running, which is the signal to detach it and publish an
// Inconclusive report: a probe wedged in a driver call must never keep a process
// alive on a player's machine. Exposed so the timeout itself is under test.
bool WaitForProbeWorker(std::thread& worker, std::uint32_t milliseconds);

bool IsPreflightCommandLine(int argumentCount, wchar_t** arguments);
PreflightOptions ParsePreflightCommandLine(int argumentCount, wchar_t** arguments);
int RunPreflight(const PreflightOptions& options);

const char* PreflightFileName();
std::filesystem::path DefaultPreflightOutputDirectory();
} // namespace zengine::envcheck::preflight
