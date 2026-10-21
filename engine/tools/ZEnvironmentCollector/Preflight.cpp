#include "Preflight.h"

#include "DiagnosisUtil.h"
#include "HardwareProbe.h"
#include "JsonEscape.h"
#include "ReportWriter.h"
#include "ToolVersion.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>

namespace zengine::envcheck::preflight
{
namespace
{
using diagnosis::UtcTicks;

constexpr std::uint32_t PreflightSchemaVersion = 1;
const char* const PreflightFileNameValue = "OSGamePreflight.json";
const wchar_t* const PreflightSwitch = L"--preflight";

constexpr std::uint32_t MinBudgetSeconds = 1;
constexpr std::uint32_t MaxBudgetSeconds = 30;
constexpr std::uint64_t MillisecondsPerSecond = 1000;

// Below this, a patch cannot land and the game's own log writer starts failing.
constexpr std::uint64_t LowFreeSpaceBytes = 2ull * 1024ull * 1024ull * 1024ull;
// "The high-performance GPU has under this much dedicated memory" is the signal
// that the preference resolved to an integrated part.
constexpr std::uint64_t IntegratedVideoMemoryCeiling = 1ull * 1024ull * 1024ull * 1024ull;
constexpr std::uint64_t DiscreteVideoMemoryFloor = 2ull * 1024ull * 1024ull * 1024ull;

// SMBIOS enclosure types that mean "a box with a monitor someone can plug into
// the wrong port". Laptops are absent on purpose -- see the chassis note in
// Preflight.h: on switchable graphics a laptop looks exactly like a mis-cabled
// desktop, and there the state is correct.
bool IsDesktopChassis(unsigned int chassisType)
{
    switch (chassisType & 0x7F)
    {
    case 3:  // Desktop
    case 4:  // Low Profile Desktop
    case 5:  // Pizza Box
    case 6:  // Mini Tower
    case 7:  // Tower
    case 13: // All In One
    case 15: // Space-saving
    case 16: // Lunch Box
    case 17: // Main Server Chassis
    case 24: // Sealed-case PC
    case 35: // Stick PC
        return true;
    default:
        return false;
    }
}

// Which GPU the picture is actually coming out of. Nothing here infers from the
// adapter name: it is EnumOutputs plus AttachedToDesktop, which is the only
// honest answer Windows gives.
struct MonitorAttachment
{
    bool known = false;               // every candidate adapter enumerated cleanly
    bool discretePresent = false;
    bool discreteHasAttachedOutput = false;
    const GpuAdapterInfo* discrete = nullptr;
    const GpuAdapterInfo* displayAdapter = nullptr;  // integrated part carrying the display
};

MonitorAttachment InspectMonitorAttachment(const PreflightInputs& inputs)
{
    MonitorAttachment attachment;
    bool anyUnenumerated = false;
    bool sawCandidate = false;

    for (const auto& gpu : inputs.gpus)
    {
        if (gpu.softwareAdapter)
        {
            continue;
        }
        if (!gpu.outputsEnumerated)
        {
            // An adapter we could not ask is not an adapter with no monitor.
            anyUnenumerated = true;
            continue;
        }
        sawCandidate = true;
        if (gpu.dedicatedVideoMemoryBytes >= DiscreteVideoMemoryFloor)
        {
            attachment.discrete = &gpu;
            attachment.discretePresent = true;
            if (gpu.attachedOutputCount != 0)
            {
                attachment.discreteHasAttachedOutput = true;
            }
        }
        else if (gpu.dedicatedVideoMemoryBytes < IntegratedVideoMemoryCeiling &&
                 gpu.attachedOutputCount != 0 &&
                 attachment.displayAdapter == nullptr)
        {
            attachment.displayAdapter = &gpu;
        }
    }

    attachment.known = sawCandidate && !anyUnenumerated;
    return attachment;
}

std::string JsonString(const std::string& value)
{
    return "\"" + EscapeJson(value) + "\"";
}

const char* BoolText(bool value)
{
    return value ? "true" : "false";
}

std::wstring RequireValue(int& index, int argumentCount, wchar_t** arguments, const char* option)
{
    if (++index >= argumentCount)
    {
        throw std::invalid_argument(std::string(option) + " requires a value.");
    }
    return arguments[index];
}

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

// Mutex names cannot contain backslashes and a fixed name would coalesce two
// products that happen to share a machine, so the output directory -- the thing
// that actually decides which report file is being contended -- is folded into
// the name instead.
std::wstring PreflightInstanceName(const std::filesystem::path& outputDirectory)
{
    const std::wstring text = std::filesystem::absolute(outputDirectory).wstring();
    std::uint64_t hash = 1469598103934665603ull;
    for (const wchar_t character : text)
    {
        const wchar_t folded = (character >= L'A' && character <= L'Z')
            ? static_cast<wchar_t>(character - L'A' + L'a')
            : character;
        for (int byte = 0; byte < 2; ++byte)
        {
            hash ^= static_cast<std::uint64_t>((folded >> (byte * 8)) & 0xff);
            hash *= 1099511628211ull;
        }
    }
    std::wostringstream stream;
    stream << L"Local\\ZEnvironmentCheck.Preflight." << std::hex << hash;
    return stream.str();
}

void CollectGameVolume(PreflightInputs& inputs, const std::filesystem::path& gameDirectory)
{
    if (gameDirectory.empty())
    {
        return;
    }
    const std::wstring target = std::filesystem::absolute(gameDirectory).wstring();
    wchar_t root[MAX_PATH]{};
    if (::GetVolumePathNameW(target.c_str(), root, MAX_PATH) == FALSE)
    {
        return;
    }
    ULARGE_INTEGER callerAvailable{};
    ULARGE_INTEGER total{};
    ULARGE_INTEGER totalFree{};
    if (::GetDiskFreeSpaceExW(root, &callerAvailable, &total, &totalFree) == FALSE)
    {
        return;
    }
    inputs.gameVolumeRoot = diagnosis::WideToUtf8(root);
    // What this user can actually use, quotas included -- the number that decides
    // whether a patch can land, not the number the disk reports.
    inputs.gameVolumeFreeBytes = callerAvailable.QuadPart;
    inputs.gameVolumeKnown = true;
}

// An Insider build is worth naming because it is a stability problem the player
// can actually leave. Detected from BuildLabEx, which reads
// "...amd64fre.ge_release..." on a retail build and "...rs_prerelease..." on a
// flighted one. Build numbers alone cannot do this: 26200 is both a retail and a
// Dev-channel build.
bool ReadInsiderPreview(bool& known)
{
    known = false;
    HKEY key = nullptr;
    const LONG openResult = ::RegOpenKeyExW(
        HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
        0, KEY_READ, &key);
    if (openResult != ERROR_SUCCESS)
    {
        return false;
    }
    wchar_t buffer[256]{};
    DWORD size = sizeof(buffer);
    DWORD type = 0;
    const LONG queryResult = ::RegQueryValueExW(
        key, L"BuildLabEx", nullptr, &type,
        reinterpret_cast<LPBYTE>(buffer), &size);
    ::RegCloseKey(key);
    if (queryResult != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ))
    {
        return false;
    }
    known = true;
    return std::wstring(buffer).find(L"rs_prerelease") != std::wstring::npos;
}

PreflightInputs CollectPreflightInputs(const std::filesystem::path& gameDirectory)
{
    PreflightInputs inputs;

    // includeDevices=false: CPU, memory and OS only. The disk and display walks
    // are deliberately left out -- see the note in RunPreflight about what a
    // per-launch check is allowed to touch.
    const EnvironmentSnapshot snapshot = CollectEnvironment(false);
    inputs.cpu = snapshot.cpu;
    inputs.system = snapshot.system;

    try
    {
        EnvironmentSnapshot graphics;
        CollectGraphicsInformation(graphics);
        inputs.gpus = graphics.gpuAdapters;
        inputs.gpusKnown = !graphics.gpuAdapters.empty();
    }
    catch (const std::exception&)
    {
        inputs.gpusKnown = false;
    }

    try
    {
        inputs.gpuSelection = ProbeRealGpuSelection();
        inputs.gpuSelectionKnown = inputs.gpuSelection.dxgiFactoryCreated;
    }
    catch (const std::exception&)
    {
        inputs.gpuSelectionKnown = false;
    }

    // Firmware table read only: no registry, no microcode, no SMBIOS memory
    // walk. One syscall-sized copy out of the firmware table.
    try
    {
        inputs.chassisType = ReadSmbiosChassisType(inputs.chassisKnown);
    }
    catch (const std::exception&)
    {
        inputs.chassisKnown = false;
    }

    CollectGameVolume(inputs, gameDirectory);
    inputs.insiderPreview = ReadInsiderPreview(inputs.insiderPreviewKnown);
    return inputs;
}

// One code -> one thing the player can do. The launcher shows these verbatim, so
// they are sentences rather than identifiers with a table on the other side.
diagnosis::UserAction ActionFor(const std::string& findingCode)
{
    if (findingCode == "GPU_DRIVER_MIXED")
    {
        return {"UPDATE_GPU_DRIVER", "到显卡厂商官网把两张显卡的驱动更新到同一个版本。"};
    }
    if (findingCode == "GPU_INTEGRATED_SELECTED")
    {
        return {"SELECT_DISCRETE_GPU", "在显卡控制面板里把游戏指定为使用高性能（独立）显卡。"};
    }
    if (findingCode == "GPU_SOFTWARE_RENDERER")
    {
        return {"INSTALL_GPU_DRIVER", "安装显卡厂商官方驱动后重启电脑。"};
    }
    if (findingCode == "MONITOR_ON_INTEGRATED_GPU")
    {
        return {"PLUG_MONITOR_INTO_DISCRETE_GPU",
                "把显示器的视频线接到独立显卡的输出口上，不要接主板上的接口。"};
    }
    if (findingCode == "STORAGE_LOW_FREE_SPACE")
    {
        return {"FREE_DISK_SPACE", "清理游戏所在磁盘，至少保留 2 GB 可用空间。"};
    }
    if (findingCode == "WINDOWS_INSIDER_PREVIEW")
    {
        return {"LEAVE_INSIDER", "退出 Windows 预览体验计划，回到正式版系统。"};
    }
    if (findingCode == "PHYSICAL_MEMORY_INSUFFICIENT")
    {
        return {"FREE_MEMORY", "关闭其它占用内存的程序，或升级内存后再启动游戏。"};
    }
    if (findingCode == "WINDOWS_X64_REQUIRED")
    {
        return {"UPGRADE_OS", "游戏需要 64 位 Windows 系统，请更换系统后再启动。"};
    }
    return {"CONTACT_SUPPORT", "如果问题持续，请提交环境报告联系客服。"};
}

void AppendAction(std::vector<diagnosis::UserAction>& actions, const diagnosis::UserAction& action)
{
    for (const auto& existing : actions)
    {
        if (existing.code == action.code)
        {
            return;
        }
    }
    actions.push_back(action);
}

// Mirrors the hard requirements of EvaluateEnvironment(). Written out instead of
// calling it because that function collapses every failure into one issueCode,
// and the launcher needs a separate code -- and a separate action -- for each
// condition it is going to show.
void AppendBlockingFindings(PreflightResult& result, const PreflightInputs& inputs, const Requirements& requirements)
{
    if (!inputs.system.is64BitOs || !inputs.system.is64BitProcess)
    {
        result.findings.push_back({"WINDOWS_X64_REQUIRED", "system",
            "当前系统不是 64 位 Windows，游戏无法运行。", ""});
    }

    std::string missing;
    const auto note = [&missing](bool required, bool available, const char* name)
    {
        if (required && !available)
        {
            if (!missing.empty())
            {
                missing += ", ";
            }
            missing += name;
        }
    };
    note(requirements.avx, inputs.cpu.avx, "AVX");
    note(requirements.avx2, inputs.cpu.avx2, "AVX2");
    note(requirements.fma, inputs.cpu.fma, "FMA");
    note(requirements.f16c, inputs.cpu.f16c, "F16C");
    if (!missing.empty())
    {
        result.findings.push_back({"CPU_ISA_UNSUPPORTED", "cpu",
            "这台电脑的 CPU 不支持游戏需要的指令集，游戏无法运行。", missing});
    }

    if (inputs.system.physicalMemoryBytes != 0 &&
        inputs.system.physicalMemoryBytes < requirements.minimumMemoryBytes)
    {
        result.findings.push_back({"PHYSICAL_MEMORY_INSUFFICIENT", "memory",
            "内存低于游戏的最低要求，进入游戏后很可能频繁卡顿或闪退。",
            std::to_string(inputs.system.physicalMemoryBytes / (1024 * 1024)) + " MB installed"});
    }
}

// Everything here is advisory. A preflight that blocks a launch is a preflight
// that will one day block a paying player over a false positive, and the
// launcher is the one that decides how loudly to say it.
void AppendAdvisoryFindings(PreflightResult& result, const PreflightInputs& inputs)
{
    if (inputs.gpusKnown)
    {
        // Same vendor, two different driver versions: what a mixed Intel Arc +
        // UHD install looks like, and the shape behind the black-screen reports
        // where the driver stack resets on an Alt-Tab.
        for (std::size_t outer = 0; outer < inputs.gpus.size(); ++outer)
        {
            for (std::size_t inner = outer + 1; inner < inputs.gpus.size(); ++inner)
            {
                const auto& left = inputs.gpus[outer];
                const auto& right = inputs.gpus[inner];
                if (left.vendorId != 0 && left.vendorId == right.vendorId &&
                    !left.driverVersion.empty() && left.driverVersion != right.driverVersion)
                {
                    result.findings.push_back({"GPU_DRIVER_MIXED", "gpu",
                        "检测到同一厂商的两张显卡驱动版本不一致，可能导致黑屏或掉驱动。",
                        left.name + " [" + left.driverVersion + "] / " +
                        right.name + " [" + right.driverVersion + "]"});
                    outer = inputs.gpus.size();
                    break;
                }
            }
        }

        bool anyHardware = false;
        for (const auto& gpu : inputs.gpus)
        {
            if (!gpu.softwareAdapter)
            {
                anyHardware = true;
            }
        }
        if (!anyHardware)
        {
            result.findings.push_back({"GPU_SOFTWARE_RENDERER", "gpu",
                "没有检测到可用的硬件显卡，游戏可能只能软件渲染，画面和帧率都会很差。", ""});
        }
    }

    if (inputs.gpuSelectionKnown && inputs.gpuSelection.gpuPreferencePicked)
    {
        const auto& picked = inputs.gpuSelection.preferredAdapter;
        std::uint64_t best = 0;
        for (const auto& adapter : inputs.gpuSelection.allAdapters)
        {
            best = std::max(best, adapter.dedicatedVideoMemoryBytes);
        }
        if (picked.dedicatedVideoMemoryBytes < IntegratedVideoMemoryCeiling &&
            best >= DiscreteVideoMemoryFloor)
        {
            result.findings.push_back({"GPU_INTEGRATED_SELECTED", "gpu",
                "游戏很可能跑在集成显卡上，帧率会明显偏低。",
                picked.name + " with " +
                std::to_string(picked.dedicatedVideoMemoryBytes / (1024 * 1024)) + " MB, " +
                "this machine has " + std::to_string(best / (1024 * 1024)) + " MB available"});
        }
    }

    // The discrete GPU is installed but the picture is coming out of the
    // integrated one. Gated on a desktop chassis because a laptop in this exact
    // state is working as designed, and gated on `known` because an adapter
    // whose outputs could not be enumerated is not evidence of anything.
    if (inputs.gpusKnown && inputs.chassisKnown && IsDesktopChassis(inputs.chassisType))
    {
        const auto attachment = InspectMonitorAttachment(inputs);
        if (attachment.known && attachment.discretePresent && !attachment.discreteHasAttachedOutput &&
            attachment.displayAdapter != nullptr)
        {
            result.findings.push_back({"MONITOR_ON_INTEGRATED_GPU", "gpu",
                "显示器接在集成显卡上，独立显卡没有接显示器，游戏可能跑不到独立显卡的性能。",
                "display on " + attachment.displayAdapter->name + " (" +
                std::to_string(attachment.displayAdapter->attachedOutputCount) + " attached), " +
                attachment.discrete->name + " has no attached output"});
        }
    }

    if (inputs.gameVolumeKnown && inputs.gameVolumeFreeBytes < LowFreeSpaceBytes)
    {
        result.findings.push_back({"STORAGE_LOW_FREE_SPACE", "storage",
            "游戏所在磁盘可用空间不足，更新和日志写入可能失败。",
            inputs.gameVolumeRoot + " free " +
            std::to_string(inputs.gameVolumeFreeBytes / (1024 * 1024)) + " MB"});
    }

    if (inputs.insiderPreviewKnown && inputs.insiderPreview)
    {
        result.findings.push_back({"WINDOWS_INSIDER_PREVIEW", "system",
            "当前系统是 Windows 预览体验版，稳定性没有保证。", ""});
    }
}

bool IsBlocking(const std::string& findingCode)
{
    return findingCode == "WINDOWS_X64_REQUIRED" ||
           findingCode == "CPU_ISA_UNSUPPORTED" ||
           findingCode == "PHYSICAL_MEMORY_INSUFFICIENT";
}

std::filesystem::path DefaultGameDirectory()
{
    wchar_t buffer[MAX_PATH]{};
    const DWORD length = ::GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length == 0 || length >= static_cast<DWORD>(MAX_PATH))
    {
        return {};
    }
    return std::filesystem::path(buffer).parent_path();
}

// One probe job, owned by shared_ptr so that detaching the worker on timeout
// cannot leave it writing into a stack frame that has already returned. That is
// the whole reason this is not a plain local.
struct ProbeJob
{
    std::shared_ptr<PreflightInputs> inputs = std::make_shared<PreflightInputs>();
    std::atomic_bool finished{false};
};

void PrintPreflightUsage()
{
    std::cout
        << "ZEnvironmentCollector -batchmode --preflight [options]\n\n"
        << "Cheap machine check for a launcher to run before (or while) the game\n"
        << "starts. Writes one conclusion file (OSGamePreflight.json) with the same\n"
        << "verdict / confidence / userMessage / actions keys OSGameDiagnosis.json\n"
        << "uses, and never blocks the game: on timeout it publishes Inconclusive.\n\n"
        << "Options:\n"
        << "  --output-dir <path>          Output directory. Default: %LOCALAPPDATA%\\OSGame\\EnvCheck.\n"
        << "                               Never the current directory -- a launcher\n"
        << "                               passes the game install directory as cwd.\n"
        << "  --game-dir <path>            Directory whose free space is checked.\n"
        << "                               Default: the directory this tool runs from.\n"
        << "  --game-exe <name>            Game executable name. Default: WXQ.exe.\n"
        << "  --budget-seconds N           Hard probe budget. Default: 3, range 1-30.\n"
        << "  --quiet                      Do not print the summary.\n\n"
        << "Exit codes: 0=conclusion written (or another preflight already owns the\n"
        << "output file, in which case nothing is written), 30=tool error.\n"
        << "A non-zero verdict is reported in the file, not in the exit code: the\n"
        << "launcher reads the file, so `exe && next` in a .bat stays correct.\n";
}

} // namespace

const char* PreflightFileName()
{
    return PreflightFileNameValue;
}

const char* ToString(PreflightVerdict value)
{
    switch (value)
    {
    case PreflightVerdict::Pass: return "Pass";
    case PreflightVerdict::Advise: return "Advise";
    case PreflightVerdict::BelowMinimum: return "BelowMinimum";
    case PreflightVerdict::Inconclusive: return "Inconclusive";
    }
    return "Inconclusive";
}

std::filesystem::path DefaultPreflightOutputDirectory()
{
    wchar_t buffer[MAX_PATH]{};
    const DWORD length = ::GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH);
    std::filesystem::path root = (length != 0 && length < static_cast<DWORD>(MAX_PATH))
        ? std::filesystem::path(buffer)
        : std::filesystem::temp_directory_path();
    return root / L"OSGame" / L"EnvCheck";
}

Requirements DefaultPreflightRequirements()
{
    Requirements requirements;
    // AVX2 is the one instruction set the game binary actually refuses to run
    // without (the local predictor's fully-fused MLP path); the rest are checked
    // by the full compatibility check, not by a per-launch preflight.
    requirements.avx2 = true;
    requirements.minimumMemoryBytes = 4ull * 1024ull * 1024ull * 1024ull;
    return requirements;
}

PreflightResult EvaluatePreflight(
    const PreflightInputs& inputs,
    const Requirements& requirements,
    bool probesComplete)
{
    PreflightResult result;
    result.probesComplete = probesComplete;
    result.budgetExceeded = !probesComplete;

    if (!probesComplete)
    {
        result.verdict = PreflightVerdict::Inconclusive;
        result.confidence = diagnosis::Confidence::Low;
        result.userMessageCode = "PREFLIGHT_INCOMPLETE";
        result.userMessageZh = "本次环境检查没有跑完，未得出结论。可以直接启动游戏。";
        result.notes.push_back("the probes did not finish inside the budget");
        return result;
    }

    AppendBlockingFindings(result, inputs, requirements);
    AppendAdvisoryFindings(result, inputs);

    std::size_t blocking = 0;
    for (const auto& finding : result.findings)
    {
        if (IsBlocking(finding.code))
        {
            ++blocking;
        }
        AppendAction(result.actions, ActionFor(finding.code));
    }

    if (blocking != 0)
    {
        result.verdict = PreflightVerdict::BelowMinimum;
        result.confidence = diagnosis::Confidence::High;
        result.userMessageCode = result.findings.front().code;
        result.userMessageZh = result.findings.front().zh;
        return result;
    }
    if (!result.findings.empty())
    {
        result.verdict = PreflightVerdict::Advise;
        result.confidence = diagnosis::Confidence::Medium;
        result.userMessageCode = "ENVIRONMENT_ADVICE";
        result.userMessageZh = result.findings.size() == 1
            ? result.findings.front().zh
            : "检测到 " + std::to_string(result.findings.size()) +
              " 项可能影响游戏体验的环境问题：" + result.findings.front().zh;
        return result;
    }
    result.verdict = PreflightVerdict::Pass;
    result.confidence = diagnosis::Confidence::High;
    result.userMessageCode = "NO_ISSUES_FOUND";
    result.userMessageZh = "未发现明显的环境问题，可以正常启动游戏。";
    return result;
}

std::string BuildPreflightJson(
    const PreflightResult& result,
    const PreflightInputs& inputs,
    const PreflightReportContext& context)
{
    std::ostringstream stream;
    stream << "{\n"
           << "  \"schemaVersion\": " << PreflightSchemaVersion << ",\n"
           << "  \"toolVersion\": " << JsonString(ToolVersion) << ",\n"
           << "  \"mode\": \"preflight\",\n"
           << "  \"source\": " << JsonString(context.source) << ",\n"
           << "  \"generatedAtUtc\": " << JsonString(diagnosis::FormatUtcIso8601(context.generatedAt)) << ",\n"
           << "  \"gameExecutable\": " << JsonString(context.gameExecutable) << ",\n"
           // The four keys the launcher reads. Same names, same shapes and the
           // same order as OSGameDiagnosis.json, so one parser serves both files.
           << "  \"verdict\": " << JsonString(ToString(result.verdict)) << ",\n"
           << "  \"confidence\": " << JsonString(diagnosis::ToString(result.confidence)) << ",\n"
           << "  \"userMessage\": {\"code\": " << JsonString(result.userMessageCode)
           << ", \"zh\": " << JsonString(result.userMessageZh) << "},\n";

    stream << "  \"actions\": [";
    for (std::size_t index = 0; index < result.actions.size(); ++index)
    {
        stream << (index == 0 ? "\n" : ",\n")
               << "    {\"code\": " << JsonString(result.actions[index].code)
               << ", \"zh\": " << JsonString(result.actions[index].zh) << "}";
    }
    stream << (result.actions.empty() ? "" : "\n  ") << "],\n";

    stream << "  \"findings\": [";
    for (std::size_t index = 0; index < result.findings.size(); ++index)
    {
        const auto& finding = result.findings[index];
        stream << (index == 0 ? "\n" : ",\n")
               << "    {\"code\": " << JsonString(finding.code)
               << ", \"layer\": " << JsonString(finding.layer)
               << ", \"zh\": " << JsonString(finding.zh)
               << ", \"evidence\": " << JsonString(finding.evidence) << "}";
    }
    stream << (result.findings.empty() ? "" : "\n  ") << "],\n";

    stream << "  \"budget\": {\"seconds\": " << context.budgetSeconds
           << ", \"exceeded\": " << BoolText(result.budgetExceeded)
           << ", \"probesComplete\": " << BoolText(result.probesComplete) << "},\n";

    stream << "  \"notes\": [";
    for (std::size_t index = 0; index < result.notes.size(); ++index)
    {
        stream << (index == 0 ? "" : ", ") << JsonString(result.notes[index]);
    }
    stream << "],\n";

    stream << "  \"environment\": {\n"
           << "    \"cpu\": {\"brand\": " << JsonString(inputs.cpu.brand)
           << ", \"logicalProcessors\": " << inputs.cpu.logicalProcessorCount
           << ", \"avx\": " << BoolText(inputs.cpu.avx)
           << ", \"avx2\": " << BoolText(inputs.cpu.avx2)
           << ", \"fma\": " << BoolText(inputs.cpu.fma)
           << ", \"f16c\": " << BoolText(inputs.cpu.f16c) << "},\n"
           << "    \"system\": {\"windowsVersion\": " << JsonString(inputs.system.windowsVersion)
           << ", \"is64BitOs\": " << BoolText(inputs.system.is64BitOs)
           << ", \"physicalMemoryBytes\": " << inputs.system.physicalMemoryBytes
           << ", \"availablePhysicalMemoryBytes\": " << inputs.system.availablePhysicalMemoryBytes
           << ", \"insiderPreview\": " << BoolText(inputs.insiderPreview) << "},\n";

    stream << "    \"gpuAdapters\": [";
    for (std::size_t index = 0; index < inputs.gpus.size(); ++index)
    {
        const auto& gpu = inputs.gpus[index];
        stream << (index == 0 ? "\n" : ",\n")
               << "      {\"name\": " << JsonString(gpu.name)
               << ", \"vendorId\": " << gpu.vendorId
               << ", \"driverVersion\": " << JsonString(gpu.driverVersion)
               << ", \"driverDate\": " << JsonString(gpu.driverDate)
               << ", \"dedicatedVideoMemoryBytes\": " << gpu.dedicatedVideoMemoryBytes
               << ", \"outputCount\": " << gpu.outputCount
               << ", \"attachedOutputCount\": " << gpu.attachedOutputCount
               << ", \"softwareAdapter\": " << BoolText(gpu.softwareAdapter) << "}";
    }
    stream << (inputs.gpus.empty() ? "" : "\n    ") << "],\n";

    const auto attachment = InspectMonitorAttachment(inputs);
    stream << "    \"monitorAttachment\": {\"known\": " << BoolText(attachment.known)
           << ", \"discretePresent\": " << BoolText(attachment.discretePresent)
           << ", \"discreteHasAttachedOutput\": " << BoolText(attachment.discreteHasAttachedOutput)
           << ", \"displayAdapter\": " << JsonString(attachment.displayAdapter != nullptr ? attachment.displayAdapter->name : "")
           << ", \"discreteAdapter\": " << JsonString(attachment.discrete != nullptr ? attachment.discrete->name : "")
           << "},\n";

    stream << "    \"chassis\": {\"known\": " << BoolText(inputs.chassisKnown)
           << ", \"smbiosType\": " << inputs.chassisType
           << ", \"desktop\": " << BoolText(inputs.chassisKnown && IsDesktopChassis(inputs.chassisType)) << "},\n";

    stream << "    \"gpuPreference\": {\"known\": " << BoolText(inputs.gpuSelectionKnown)
           << ", \"picked\": " << BoolText(inputs.gpuSelection.gpuPreferencePicked)
           << ", \"adapterName\": " << JsonString(inputs.gpuSelection.preferredAdapter.name)
           << ", \"dedicatedVideoMemoryBytes\": " << inputs.gpuSelection.preferredAdapter.dedicatedVideoMemoryBytes
           << "},\n"
           << "    \"gameVolume\": {\"known\": " << BoolText(inputs.gameVolumeKnown)
           << ", \"root\": " << JsonString(inputs.gameVolumeRoot)
           << ", \"availableBytes\": " << inputs.gameVolumeFreeBytes << "}\n"
           << "  }\n"
           << "}\n";
    return stream.str();
}

PreflightInstance::PreflightInstance(const std::filesystem::path& outputDirectory)
{
    const std::wstring name = PreflightInstanceName(outputDirectory);
    handle_ = ::CreateMutexW(nullptr, FALSE, name.c_str());
    if (handle_ == nullptr)
    {
        // No lock available means no protection, and the only safe reading of
        // that is "we are not the one that should write".
        return;
    }
    // Zero timeout: a second instance is a duplicate, not a caller to wait for.
    // WAIT_ABANDONED counts as acquired -- the previous owner died holding it and
    // there is nobody else left to publish the report.
    const DWORD waitResult = ::WaitForSingleObject(handle_, 0);
    acquired_ = waitResult == WAIT_OBJECT_0 || waitResult == WAIT_ABANDONED;
}

PreflightInstance::~PreflightInstance()
{
    if (handle_ == nullptr)
    {
        return;
    }
    if (acquired_)
    {
        ::ReleaseMutex(handle_);
    }
    ::CloseHandle(handle_);
}

bool WaitForProbeWorker(std::thread& worker, std::uint32_t milliseconds)
{
    const HANDLE handle = reinterpret_cast<HANDLE>(worker.native_handle());
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    return ::WaitForSingleObject(handle, milliseconds) == WAIT_OBJECT_0;
}

bool IsPreflightCommandLine(int argumentCount, wchar_t** arguments)
{
    for (int index = 1; index < argumentCount; ++index)
    {
        if (std::wstring(arguments[index]) == PreflightSwitch)
        {
            return true;
        }
    }
    return false;
}

PreflightOptions ParsePreflightCommandLine(int argumentCount, wchar_t** arguments)
{
    PreflightOptions options;
    for (int index = 1; index < argumentCount; ++index)
    {
        const std::wstring argument = arguments[index];
        if (argument == PreflightSwitch)
        {
            continue;
        }
        if (argument == L"--output-dir")
        {
            options.outputDirectory = RequireValue(index, argumentCount, arguments, "--output-dir");
        }
        else if (argument == L"--game-dir")
        {
            options.gameDirectory = RequireValue(index, argumentCount, arguments, "--game-dir");
        }
        else if (argument == L"--game-exe")
        {
            const std::string value = diagnosis::FileNameOf(
                diagnosis::WideToUtf8(RequireValue(index, argumentCount, arguments, "--game-exe")));
            if (value.empty())
            {
                throw std::invalid_argument("--game-exe requires an executable file name.");
            }
            options.gameExecutable = value;
        }
        else if (argument == L"--budget-seconds")
        {
            options.budgetSeconds = ParseBoundedUnsigned(
                RequireValue(index, argumentCount, arguments, "--budget-seconds"),
                MinBudgetSeconds, MaxBudgetSeconds, "--budget-seconds");
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
            throw std::invalid_argument("Unknown option in --preflight mode.");
        }
    }
    return options;
}

int RunPreflight(const PreflightOptions& options)
{
    if (options.showHelp)
    {
        PrintPreflightUsage();
        return 0;
    }

    const std::filesystem::path outputDirectory = options.outputDirectory.empty()
        ? DefaultPreflightOutputDirectory()
        : options.outputDirectory;
    std::filesystem::create_directories(outputDirectory);

    PreflightInstance instance(outputDirectory);
    if (!instance.Acquired())
    {
        // Writes nothing on purpose. The instance holding the lock is about to
        // publish, and an Inconclusive from here would rename itself over that
        // report -- the launcher would then see "no idea" where a moment earlier
        // it could have seen a real verdict.
        if (!options.quiet)
        {
            std::cout << "Preflight already running for this output directory; nothing written.\n";
        }
        return 0;
    }

    // The player is starting a game; this process is the least important one on
    // the machine. Below-normal is the floor -- a launcher that wants IO priority
    // too should spawn this with CREATE_BELOW_NORMAL (or lower) itself.
    ::SetPriorityClass(::GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);

    const UtcTicks generatedAt = diagnosis::CurrentUtcTime();
    const std::filesystem::path gameDirectory = options.gameDirectory.empty()
        ? DefaultGameDirectory()
        : options.gameDirectory;

    auto job = std::make_shared<ProbeJob>();
    std::thread worker([job, gameDirectory]()
    {
        *job->inputs = CollectPreflightInputs(gameDirectory);
        job->finished.store(true);
    });

    const bool withinBudget = WaitForProbeWorker(
        worker, options.budgetSeconds * static_cast<std::uint32_t>(MillisecondsPerSecond));
    if (withinBudget)
    {
        worker.join();
    }
    else
    {
        // Detach rather than join: joining would wait for a probe that is wedged
        // in a driver call, which is exactly the failure this budget exists for.
        // The process exits immediately afterwards, and that is what ends the
        // thread. job is held by the lambda's own shared_ptr copy, so the worker
        // never touches a frame this function has already left.
        worker.detach();
    }

    // Deliberately not the collected inputs when the budget was exceeded: they
    // are being written by a thread we just detached, and a half-filled adapter
    // list would read as "this machine has no graphics card".
    const PreflightInputs inputs = withinBudget ? *job->inputs : PreflightInputs{};
    const PreflightResult result = EvaluatePreflight(inputs, DefaultPreflightRequirements(), withinBudget);

    PreflightReportContext context;
    context.generatedAt = generatedAt;
    context.gameExecutable = options.gameExecutable;
    context.budgetSeconds = options.budgetSeconds;
    const auto path = outputDirectory / PreflightFileNameValue;
    WriteReportFile(path, BuildPreflightJson(result, inputs, context));

    if (!options.quiet)
    {
        std::cout << "Verdict: " << ToString(result.verdict)
                  << " (confidence: " << diagnosis::ToString(result.confidence) << ")\n"
                  << result.userMessageZh << '\n';
    }
    std::cout << "Preflight written to:\n  " << diagnosis::WideToUtf8(path.wstring()) << '\n';
    return 0;
}
} // namespace zengine::envcheck::preflight
