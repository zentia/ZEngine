#include "Preflight.h"

#include "DiagnosisUtil.h"

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

using namespace zengine::envcheck;
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

namespace
{
// A machine with nothing wrong with it. Every rule test starts here and breaks
// exactly one thing, so a test that fails can only be about the thing it broke.
preflight::PreflightInputs HealthyInputs()
{
    preflight::PreflightInputs inputs;
    inputs.cpu.brand = "Test CPU";
    inputs.cpu.logicalProcessorCount = 8;
    inputs.cpu.avx = true;
    inputs.cpu.avx2 = true;
    inputs.cpu.fma = true;
    inputs.cpu.f16c = true;
    inputs.system.is64BitOs = true;
    inputs.system.is64BitProcess = true;
    inputs.system.windowsVersion = "Windows 11 (build 26100)";
    inputs.system.physicalMemoryBytes = 16ull << 30;
    inputs.system.availablePhysicalMemoryBytes = 8ull << 30;
    // ullTotalPageFile is the commit limit -- RAM plus page file -- so a healthy
    // machine reports more than it has RAM.
    inputs.system.totalPageFileBytes = (16ull << 30) + (8ull << 30);
    inputs.system.availablePageFileBytes = 20ull << 30;

    GpuAdapterInfo gpu;
    gpu.name = "Test Discrete GPU";
    gpu.vendorId = 0x8086;
    gpu.driverVersion = "32.0.101.9150";
    gpu.dedicatedVideoMemoryBytes = 8ull << 30;
    inputs.gpus.push_back(gpu);
    inputs.gpusKnown = true;

    inputs.gpuSelection.dxgiFactoryCreated = true;
    inputs.gpuSelection.factory6Available = true;
    inputs.gpuSelection.gpuPreferencePicked = true;
    inputs.gpuSelection.preferredAdapter.name = gpu.name;
    inputs.gpuSelection.preferredAdapter.dedicatedVideoMemoryBytes = gpu.dedicatedVideoMemoryBytes;
    inputs.gpuSelection.allAdapters.push_back(inputs.gpuSelection.preferredAdapter);
    inputs.gpuSelectionKnown = true;

    inputs.gameVolumeRoot = "C:\\";
    inputs.gameVolumeFreeBytes = 100ull << 30;
    inputs.gameVolumeKnown = true;

    inputs.insiderPreview = false;
    inputs.insiderPreviewKnown = true;

    inputs.vcRuntimePresent = true;
    inputs.vcRuntimeKnown = true;
    return inputs;
}

// A desktop whose panel is wired to the integrated part while a discrete GPU
// sits there unused: the "cable in the motherboard port" build. Every variant
// below starts here and changes exactly one thing.
preflight::PreflightInputs DisplayOnIntegratedInputs()
{
    auto inputs = HealthyInputs();
    inputs.gpus.front().outputsEnumerated = true;
    inputs.gpus.front().attachedOutputCount = 0;
    GpuAdapterInfo integrated;
    integrated.name = "Test Integrated GPU";
    integrated.vendorId = inputs.gpus.front().vendorId;
    integrated.driverVersion = inputs.gpus.front().driverVersion;
    integrated.dedicatedVideoMemoryBytes = 512ull << 20;
    integrated.outputsEnumerated = true;
    integrated.attachedOutputCount = 1;
    inputs.gpus.push_back(integrated);
    inputs.chassisType = 3; // SMBIOS: Desktop
    inputs.chassisKnown = true;
    return inputs;
}

preflight::PreflightResult Evaluate(const preflight::PreflightInputs& inputs, bool probesComplete = true)
{
    return preflight::EvaluatePreflight(inputs, preflight::DefaultPreflightRequirements(), probesComplete);
}

bool HasFinding(const preflight::PreflightResult& result, const std::string& code)
{
    for (const auto& finding : result.findings)
    {
        if (finding.code == code) return true;
    }
    return false;
}
} // namespace

int main()
{
    try
    {
        Check(std::string(preflight::ToString(preflight::PreflightVerdict::Pass)) == "Pass"
              && std::string(preflight::ToString(preflight::PreflightVerdict::Advise)) == "Advise"
              && std::string(preflight::ToString(preflight::PreflightVerdict::BelowMinimum)) == "BelowMinimum"
              && std::string(preflight::ToString(preflight::PreflightVerdict::Inconclusive)) == "Inconclusive",
              "verdict strings are the four the launcher parses");

        // ---- the healthy machine says nothing ----
        {
            const auto result = Evaluate(HealthyInputs());
            Check(result.verdict == preflight::PreflightVerdict::Pass, "a healthy machine passes");
            Check(result.findings.empty(), "a healthy machine produces no findings");
            Check(result.actions.empty(), "a healthy machine asks for nothing");
            Check(result.userMessageCode == "NO_ISSUES_FOUND", "pass carries a message code");
        }

        // ---- below minimum: the three things that make the game unplayable ----
        {
            auto inputs = HealthyInputs();
            inputs.cpu.avx2 = false;
            const auto result = Evaluate(inputs);
            Check(result.verdict == preflight::PreflightVerdict::BelowMinimum, "a missing ISA is below minimum");
            Check(result.userMessageCode == "CPU_ISA_UNSUPPORTED", "the ISA result names the cause");
            Check(!result.actions.empty(), "a below-minimum result still offers an action");
        }
        {
            auto inputs = HealthyInputs();
            inputs.system.physicalMemoryBytes = 2ull << 30;
            const auto result = Evaluate(inputs);
            Check(result.verdict == preflight::PreflightVerdict::BelowMinimum, "memory under the minimum is below minimum");
            Check(HasFinding(result, "PHYSICAL_MEMORY_INSUFFICIENT"), "the memory finding is named");
        }
        {
            auto inputs = HealthyInputs();
            inputs.system.is64BitOs = false;
            const auto result = Evaluate(inputs);
            Check(result.verdict == preflight::PreflightVerdict::BelowMinimum, "a 32-bit OS is below minimum");
            Check(HasFinding(result, "WINDOWS_X64_REQUIRED"), "the OS finding is named");
        }
        // A machine that is merely old must not be reported as unplayable: the
        // distinction is the whole reason Advise exists.
        {
            auto inputs = HealthyInputs();
            inputs.cpu.logicalProcessorCount = 4;
            const auto result = Evaluate(inputs);
            Check(result.verdict == preflight::PreflightVerdict::Pass, "few cores alone is not a failure");
        }

        // ---- advisory: worth mentioning, never worth blocking a launch ----
        {
            // The shape behind the black-screen reports: one vendor, two driver
            // versions in the same machine.
            auto inputs = HealthyInputs();
            GpuAdapterInfo second = inputs.gpus.front();
            second.name = "Test Integrated GPU";
            second.driverVersion = "32.0.101.7088";
            inputs.gpus.push_back(second);
            const auto result = Evaluate(inputs);
            Check(result.verdict == preflight::PreflightVerdict::Advise, "mixed driver versions advise");
            Check(HasFinding(result, "GPU_DRIVER_MIXED"), "the mixed-driver finding is named");
            Check(result.userMessageCode == "ENVIRONMENT_ADVICE", "advice carries one summary code");
        }
        {
            // The high-performance preference resolving to a sub-1GB part while a
            // 2GB+ part is installed is what "running on the integrated GPU" looks
            // like without having to guess from the adapter name.
            auto inputs = HealthyInputs();
            inputs.gpuSelection.preferredAdapter.name = "Test Integrated GPU";
            inputs.gpuSelection.preferredAdapter.dedicatedVideoMemoryBytes = 512ull << 20;
            const auto result = Evaluate(inputs);
            Check(result.verdict == preflight::PreflightVerdict::Advise, "an integrated pick advises");
            Check(HasFinding(result, "GPU_INTEGRATED_SELECTED"), "the integrated pick is named");
        }
        {
            // The discrete part is installed but nothing is plugged into it.
            auto inputs = DisplayOnIntegratedInputs();
            const auto result = Evaluate(inputs);
            Check(result.verdict == preflight::PreflightVerdict::Advise, "a mis-cabled desktop advises");
            Check(HasFinding(result, "MONITOR_ON_INTEGRATED_GPU"), "the mis-cabled finding is named");
        }
        {
            // Exactly the same GPU layout on a laptop is working as designed:
            // switchable graphics drive the panel through the integrated part,
            // so the discrete part legitimately reports no attached output.
            auto inputs = DisplayOnIntegratedInputs();
            inputs.chassisType = 9; // SMBIOS: Laptop
            const auto result = Evaluate(inputs);
            Check(!HasFinding(result, "MONITOR_ON_INTEGRATED_GPU"), "a laptop in the same state is not reported");
        }
        {
            // No usable enclosure type (VM, odd OEM table): guessing would make
            // this fire on machines where it is not true.
            auto inputs = DisplayOnIntegratedInputs();
            inputs.chassisKnown = false;
            const auto result = Evaluate(inputs);
            Check(!HasFinding(result, "MONITOR_ON_INTEGRATED_GPU"), "an unknown chassis says nothing about the monitor");
        }
        {
            // Unknown is not absent. "Could not enumerate the discrete card's
            // outputs" is not "the discrete card has no monitor".
            auto inputs = DisplayOnIntegratedInputs();
            inputs.gpus.front().outputsEnumerated = false;
            const auto result = Evaluate(inputs);
            Check(!HasFinding(result, "MONITOR_ON_INTEGRATED_GPU"), "a failed output enumeration is not a mis-cabled verdict");
        }
        {
            // The cable in the right place.
            auto inputs = DisplayOnIntegratedInputs();
            inputs.gpus.front().attachedOutputCount = 1;
            const auto result = Evaluate(inputs);
            Check(!HasFinding(result, "MONITOR_ON_INTEGRATED_GPU"), "a display on the discrete GPU is not reported");
        }
        {
            // 16 GB installed, 512 MB free: passes the minimum-spec rule and is
            // still the machine that crashes. Installed size says nothing here.
            auto inputs = HealthyInputs();
            inputs.system.availablePhysicalMemoryBytes = 512ull << 20;
            const auto result = Evaluate(inputs);
            Check(result.verdict == preflight::PreflightVerdict::Advise, "a full machine advises");
            Check(HasFinding(result, "MEMORY_AVAILABLE_LOW"), "low available memory is named");
        }
        {
            // Below the minimum the machine already reports as blocking; adding
            // "also low right now" on top of it is noise.
            auto inputs = HealthyInputs();
            inputs.system.physicalMemoryBytes = 2ull << 30;
            inputs.system.availablePhysicalMemoryBytes = 512ull << 20;
            const auto result = Evaluate(inputs);
            Check(!HasFinding(result, "MEMORY_AVAILABLE_LOW"),
                  "a machine already below the minimum is not also told it is full");
        }
        {
            // No page file at all: commit limit == RAM.
            auto inputs = HealthyInputs();
            inputs.system.physicalMemoryBytes = 8ull << 30;
            inputs.system.availablePhysicalMemoryBytes = 4ull << 30;
            inputs.system.totalPageFileBytes = 8ull << 30;
            inputs.system.availablePageFileBytes = 4ull << 30;
            const auto result = Evaluate(inputs);
            Check(HasFinding(result, "PAGE_FILE_TOO_SMALL"), "no page file on a small-RAM machine is named");
        }
        {
            // The same absence above the RAM ceiling is a considered choice.
            auto inputs = HealthyInputs();
            inputs.system.totalPageFileBytes = 16ull << 30;
            inputs.system.availablePageFileBytes = 8ull << 30;
            const auto result = Evaluate(inputs);
            Check(!HasFinding(result, "PAGE_FILE_TOO_SMALL"),
                  "a large-RAM machine without a page file is left alone");
        }
        {
            // Not installed, not shipped beside the game.
            auto inputs = HealthyInputs();
            inputs.vcRuntimePresent = false;
            inputs.vcRuntimeKnown = true;
            const auto result = Evaluate(inputs);
            Check(result.verdict == preflight::PreflightVerdict::Advise, "a missing runtime advises, never blocks");
            Check(HasFinding(result, "VC_RUNTIME_MISSING"), "the missing runtime is named");
        }
        {
            // Could not read the registry key is not "the runtime is absent".
            auto inputs = HealthyInputs();
            inputs.vcRuntimePresent = false;
            inputs.vcRuntimeKnown = false;
            const auto result = Evaluate(inputs);
            Check(!HasFinding(result, "VC_RUNTIME_MISSING"), "an unknown runtime state says nothing");
        }
        {
            auto inputs = HealthyInputs();
            inputs.gpus.front().softwareAdapter = true;
            const auto result = Evaluate(inputs);
            Check(result.verdict == preflight::PreflightVerdict::Advise, "software rendering advises");
            Check(HasFinding(result, "GPU_SOFTWARE_RENDERER"), "the software renderer is named");
        }
        {
            auto inputs = HealthyInputs();
            inputs.gameVolumeFreeBytes = 500ull << 20;
            const auto result = Evaluate(inputs);
            Check(result.verdict == preflight::PreflightVerdict::Advise, "a nearly full disk advises");
            Check(HasFinding(result, "STORAGE_LOW_FREE_SPACE"), "the disk finding is named");
        }
        {
            auto inputs = HealthyInputs();
            inputs.insiderPreview = true;
            const auto result = Evaluate(inputs);
            Check(result.verdict == preflight::PreflightVerdict::Advise, "an Insider build advises");
            Check(HasFinding(result, "WINDOWS_INSIDER_PREVIEW"), "the Insider finding is named");
        }
        // Unknown is not the same as absent. An empty adapter list after a failed
        // enumeration must not be reported as "this machine has no GPU".
        {
            auto inputs = HealthyInputs();
            inputs.gpus.clear();
            inputs.gpusKnown = false;
            const auto result = Evaluate(inputs);
            Check(result.verdict == preflight::PreflightVerdict::Pass, "a failed GPU enumeration says nothing");
            Check(!HasFinding(result, "GPU_SOFTWARE_RENDERER"), "no GPU data is not a software-renderer verdict");
        }

        // ---- fail open ----
        {
            // Advisory findings were already collected, but the probes did not
            // finish: the honest answer is still "no idea", because a partial
            // scan has not ruled out the thing that actually blocks the game.
            auto inputs = HealthyInputs();
            inputs.gameVolumeFreeBytes = 500ull << 20;
            const auto result = Evaluate(inputs, false);
            Check(result.verdict == preflight::PreflightVerdict::Inconclusive, "an unfinished check is inconclusive");
            Check(result.budgetExceeded && !result.probesComplete, "the unfinished result says why");
            Check(result.findings.empty(), "an unfinished check reports no findings");
            Check(result.userMessageCode == "PREFLIGHT_INCOMPLETE", "the inconclusive result carries a message code");
        }

        // ---- the launcher contract ----
        {
            preflight::PreflightReportContext context;
            context.generatedAt = diagnosis::CurrentUtcTime();
            context.gameExecutable = "WXQ.exe";
            context.budgetSeconds = 3;
            auto inputs = HealthyInputs();
            inputs.gameVolumeFreeBytes = 100ull << 20;
            const std::string json = preflight::BuildPreflightJson(Evaluate(inputs), inputs, context);
            // The four keys OSGameDiagnosis.json exposes, same names and shapes.
            Check(json.find("\"verdict\": \"") != std::string::npos, "verdict key");
            Check(json.find("\"confidence\": \"") != std::string::npos, "confidence key");
            Check(json.find("\"userMessage\": {\"code\": ") != std::string::npos, "userMessage key");
            Check(json.find("\"actions\": [") != std::string::npos, "actions key");
            Check(json.find("\"mode\": \"preflight\"") != std::string::npos, "mode discriminates the two files");
            Check(json.find("\"monitorAttachment\": {") != std::string::npos, "monitorAttachment is reported");
            Check(json.find("\"pageFile\": {") != std::string::npos, "pageFile is reported");
            Check(json.find("\"runtime\": {") != std::string::npos, "runtime is reported");
            Check(json.find("\"attachedOutputCount\": ") != std::string::npos, "each adapter reports its attached outputs");
            Check(json.find("\"code\": \"FREE_DISK_SPACE\", \"zh\": ") != std::string::npos, "actions carry Chinese text");
            Check(json.find('\t') == std::string::npos, "no raw control characters in the JSON");
        }

        // ---- hard timeout ----
        {
            std::thread quick([] {});
            Check(preflight::WaitForProbeWorker(quick, 5000), "a finished worker is reported as finished");
            quick.join();

            std::thread slow([] { std::this_thread::sleep_for(std::chrono::seconds(30)); });
            Check(!preflight::WaitForProbeWorker(slow, 50), "a worker still running is reported as such");
            // Detached on purpose: this is what a probe wedged in a driver call
            // looks like, and the process must be able to leave without it.
            slow.detach();
        }

        // ---- one preflight per output directory ----
        {
            const auto directory = std::filesystem::temp_directory_path()
                / ("envpreflight-" + std::to_string(GetCurrentProcessId()));
            const auto other = directory / "other";
            std::filesystem::create_directories(other);

            std::atomic_bool firstAcquired{false};
            std::atomic_bool secondAcquired{true};
            std::atomic_bool release{false};
            // Held from another thread: a mutex is recursive for the thread that
            // owns it, so testing this on one thread would prove nothing.
            std::thread holder([&]
            {
                preflight::PreflightInstance instance(directory);
                firstAcquired.store(instance.Acquired());
                while (!release.load()) std::this_thread::yield();
            });
            // Sleeps, not yields: a bare yield loop can burn through its whole
            // budget before the thread has even been scheduled, and then this
            // test measures nothing at all -- it would run both instances on the
            // same thread's timeline and "pass" by accident.
            for (int spin = 0; spin < 5000 && !firstAcquired.load(); ++spin)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            {
                preflight::PreflightInstance second(directory);
                secondAcquired.store(second.Acquired());
            }
            release.store(true);
            holder.join();
            Check(firstAcquired.load(), "the first preflight acquires the lock");
            Check(!secondAcquired.load(), "a second preflight on the same directory is refused");

            // A different output directory is a different report file, so those
            // two must not suppress each other.
            preflight::PreflightInstance elsewhere(other);
            Check(elsewhere.Acquired(), "a different output directory has its own lock");

            std::filesystem::remove_all(directory);
        }

        // ---- where the report lands ----
        {
            const std::filesystem::path defaultDirectory = preflight::DefaultPreflightOutputDirectory();
            Check(defaultDirectory.is_absolute(), "the default output directory is absolute");
            // A launcher passes the game install directory as the working
            // directory; writing the report there is what triggers a repair scan.
            Check(defaultDirectory != std::filesystem::current_path(), "the default output directory is not the cwd");
        }

        // ---- end to end ----
        {
            const auto directory = std::filesystem::temp_directory_path()
                / ("envpreflight-run-" + std::to_string(GetCurrentProcessId()));
            std::filesystem::remove_all(directory);
            preflight::PreflightOptions options;
            options.outputDirectory = directory;
            options.budgetSeconds = 5;
            options.quiet = true;
            const int exitCode = preflight::RunPreflight(options);
            Check(exitCode == 0, "a preflight run exits 0 even when it finds something");

            const auto path = directory / preflight::PreflightFileName();
            Check(std::filesystem::is_regular_file(path), "the preflight report is written");
            std::string content;
            {
                // Closed before the cleanup below: an open handle is what makes
                // remove_all fail on Windows, and the failure would look like a
                // problem with the report rather than with this test.
                std::ifstream input(path);
                content.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
            }
            Check(content.find("\"mode\": \"preflight\"") != std::string::npos, "the report is a preflight report");
            const bool knownVerdict =
                content.find("\"verdict\": \"Pass\"") != std::string::npos ||
                content.find("\"verdict\": \"Advise\"") != std::string::npos ||
                content.find("\"verdict\": \"BelowMinimum\"") != std::string::npos ||
                content.find("\"verdict\": \"Inconclusive\"") != std::string::npos;
            Check(knownVerdict, "the report carries one of the four verdicts");
            // The file is published by rename (see ReportWriter), so a launcher
            // polling it mid-write can never see a partial document -- and there
            // is no temporary left behind to confuse anyone.
            for (std::filesystem::directory_iterator entry(directory), end; entry != end; ++entry)
            {
                Check(entry->path().filename().string().find(".tmp") == std::string::npos,
                      "no temporary left next to the report");
            }
            std::filesystem::remove_all(directory);
        }

        std::cout << "PASS preflight verdicts, advisory rules, fail-open timeout, single instance, launcher contract\n";
        return 0;
    }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
