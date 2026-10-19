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

    bool insiderPreview = false;
    bool insiderPreviewKnown = false;
};

struct PreflightReportContext
{
    std::string source = "live";
    diagnosis::UtcTicks generatedAt = 0;
    std::string gameExecutable;
    std::uint32_t budgetSeconds = 0;
};

Requirements DefaultPreflightRequirements();

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
