#include "Preflight.h"

#include "AttributionRules.h"
#include "DiagnosisUtil.h"
#include "EventLogReader.h"
#include "HardwareProbe.h"
#include "JsonEscape.h"
#include "ReportWriter.h"
#include "ToolVersion.h"

#include <Windows.h>

#include <d3d11.h>
// Only for the adapter enumeration and the feature-level constants: d3d11.dll is
// loaded at runtime, so nothing here adds a link-time dependency on DirectX.
#include <dxgi1_6.h>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <iostream>
#include <iterator>
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

// The free-space floor is part of the minimum spec now (minimumFreeDiskSpaceBytes)
// rather than a constant here, so a game can move it without a rebuild of this
// tool.
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

// Visual C++ 2015-2022 redistributable, x64. Read from the runtime's own
// registration rather than from the uninstall list: the uninstall entry is
// per-installer and disappears after some in-place upgrades, while this key is
// written by the runtime itself.
bool ReadVcRuntimeInstalled(bool& known)
{
    known = false;
    static const wchar_t* kRuntimeKeys[] = {
        L"SOFTWARE\\Microsoft\\VisualStudio\\14.0\\VC\\Runtimes\\x64",
        L"SOFTWARE\\WOW6432Node\\Microsoft\\VisualStudio\\14.0\\VC\\Runtimes\\x64",
    };
    for (const wchar_t* subKey : kRuntimeKeys)
    {
        HKEY key = nullptr;
        if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, subKey, 0, KEY_READ, &key) != ERROR_SUCCESS)
        {
            continue;
        }
        DWORD installed = 0;
        DWORD size = sizeof(installed);
        DWORD type = 0;
        const LONG queryResult = ::RegQueryValueExW(
            key, L"Installed", nullptr, &type,
            reinterpret_cast<LPBYTE>(&installed), &size);
        ::RegCloseKey(key);
        if (queryResult != ERROR_SUCCESS || type != REG_DWORD)
        {
            continue;
        }
        known = true;
        if (installed != 0)
        {
            return true;
        }
    }
    return false;
}

// Plenty of games ship the runtime DLLs beside the executable instead of
// installing them. Counting that as present is the whole difference between a
// useful hint and a false alarm on a machine that runs the game fine.
bool HasAppLocalVcRuntime(const std::filesystem::path& gameDirectory)
{
    if (gameDirectory.empty())
    {
        return false;
    }
    static const char* kRuntimeDlls[] = {"vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll"};
    for (const char* dll : kRuntimeDlls)
    {
        std::error_code ignored;
        if (std::filesystem::exists(gameDirectory / dll, ignored))
        {
            return true;
        }
    }
    return false;
}

// Minimal COM holder, mirroring the one already local to HardwareProbe.cpp and
// GpuSelectionProbe.cpp. Kept in this translation unit so asking one DirectX
// question does not put a COM helper into a header every module includes.
template <typename T>
class ComPtr
{
public:
    ComPtr() = default;
    ~ComPtr() { Reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    T** Put() { return &pointer_; }
    T* Get() const { return pointer_; }
    T* operator->() const { return pointer_; }
    void Reset()
    {
        if (pointer_ != nullptr)
        {
            pointer_->Release();
            pointer_ = nullptr;
        }
    }

private:
    T* pointer_ = nullptr;
};

using PFN_D3D11_CREATE_DEVICE = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
                                                 const D3D_FEATURE_LEVEL*, UINT, UINT,
                                                 ID3D11Device**, D3D_FEATURE_LEVEL*,
                                                 ID3D11DeviceContext**);

// Same yardstick HardwareProbe.cpp uses for GpuInfo::softwareAdapter, so the
// "only software rendering" advisory and this check never disagree about which
// adapter is software. The vendor/device pair is Microsoft Basic Render Driver,
// which is what the check falls back on when an older driver stack does not set
// the flag.
bool IsSoftwareAdapter(const DXGI_ADAPTER_DESC1& description)
{
    if ((description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
    {
        return true;
    }
    return description.VendorId == 0x1414 && description.DeviceId == 0x8c;
}

bool TryCreateD3D11Device(PFN_D3D11_CREATE_DEVICE create, IDXGIAdapter1* adapter,
                          std::uint32_t& featureLevel, std::uint32_t& failureHresult)
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL granted = D3D_FEATURE_LEVEL_9_1;
    // The whole ladder rather than one level: that is how you learn the highest
    // level a driver will give, instead of whether one specific level exists.
    const D3D_FEATURE_LEVEL ladder[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };
    const HRESULT result = create(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
                                  ladder, static_cast<UINT>(std::size(ladder)), D3D11_SDK_VERSION,
                                  device.Put(), &granted, context.Put());
    if (FAILED(result) || device.Get() == nullptr)
    {
        failureHresult = static_cast<std::uint32_t>(result);
        return false;
    }
    featureLevel = static_cast<std::uint32_t>(granted);
    return true;
}

Direct3D11Probe ProbeDirect3D11()
{
    Direct3D11Probe probe;

    // Loaded, never linked: a hard dependency on d3d11.lib would turn "this
    // machine has no d3d11.dll" into a process that cannot start at all, and a
    // preflight that cannot start is worse than one that answers nothing.
    HMODULE library = ::LoadLibraryW(L"d3d11.dll");
    if (library == nullptr)
    {
        return probe;
    }
    const auto create = reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(
        ::GetProcAddress(library, "D3D11CreateDevice"));
    if (create == nullptr)
    {
        return probe;
    }

    ComPtr<IDXGIFactory1> factory;
    if (FAILED(::CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(factory.Put()))) ||
        factory.Get() == nullptr)
    {
        return probe;
    }

    // The adapter the game is going to pick, chosen by the same call the game
    // makes, so the answer is about that card and not about whichever one
    // happens to be first in the list.
    ComPtr<IDXGIFactory6> factory6;
    ComPtr<IDXGIAdapter1> preferred;
    if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory6), reinterpret_cast<void**>(factory6.Put()))) &&
        factory6.Get() != nullptr)
    {
        factory6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                             __uuidof(IDXGIAdapter1),
                                             reinterpret_cast<void**>(preferred.Put()));
    }

    if (preferred.Get() != nullptr)
    {
        DXGI_ADAPTER_DESC1 description{};
        preferred->GetDesc1(&description);
        probe.known = true;

        // A software adapter winning the preference means there is no GPU driver
        // installed at all. It is not allowed to answer for the machine: the game
        // would start on WARP and run at a few frames a second, and saying "it
        // cannot start" about a machine that does start is the kind of thing a
        // player reports back verbatim.
        if (!IsSoftwareAdapter(description))
        {
            std::uint32_t level = 0;
            std::uint32_t failure = 0;
            if (TryCreateD3D11Device(create, preferred.Get(), level, failure))
            {
                probe.deviceCreated = true;
                probe.onPreferredAdapter = true;
                probe.featureLevel = level;
                probe.adapterName = diagnosis::WideToUtf8(description.Description);
                return probe;
            }
            probe.failureHresult = failure;
        }
    }

    // The card the game would pick said no. Before telling the player it cannot
    // run, ask the rest: production code falls back through the adapter list, and
    // blocking a player whose second card works is the one mistake here that
    // cannot be walked back afterwards.
    for (UINT index = 0;; ++index)
    {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(index, adapter.Put()) == DXGI_ERROR_NOT_FOUND)
        {
            break;
        }
        if (adapter.Get() == nullptr)
        {
            continue;
        }
        DXGI_ADAPTER_DESC1 description{};
        if (FAILED(adapter->GetDesc1(&description)))
        {
            continue;
        }
        probe.known = true;
        std::uint32_t level = 0;
        std::uint32_t failure = 0;
        if (TryCreateD3D11Device(create, adapter.Get(), level, failure))
        {
            // The software adapter is tried but never accepted as an answer.
            // Recording that it *could* create a device is what lets the rule say
            // "no GPU driver installed" instead of "no Direct3D 11"; the loop
            // keeps going in case a real card is still to come.
            if (IsSoftwareAdapter(description))
            {
                probe.softwareOnlyAdapter = true;
                if (probe.adapterName.empty())
                {
                    probe.adapterName = diagnosis::WideToUtf8(description.Description);
                }
                continue;
            }
            probe.deviceCreated = true;
            probe.onPreferredAdapter = false;
            probe.featureLevel = level;
            probe.adapterName = diagnosis::WideToUtf8(description.Description);
            return probe;
        }
        if (probe.failureHresult == 0)
        {
            probe.failureHresult = failure;
        }
    }
    return probe;
}

// RegOpenKeyEx reports a key that is not there as ERROR_FILE_NOT_FOUND, which is
// an answer and not a failure. Only an unexpected error leaves `asked` false, so
// that "could not look" never reads as "nothing is waiting".
bool RebootKeyPresent(const wchar_t* subKey, bool& asked)
{
    HKEY key = nullptr;
    const LONG result = ::RegOpenKeyExW(HKEY_LOCAL_MACHINE, subKey, 0, KEY_READ, &key);
    if (result == ERROR_FILE_NOT_FOUND || result == ERROR_PATH_NOT_FOUND)
    {
        asked = true;
        return false;
    }
    if (result != ERROR_SUCCESS)
    {
        return false;
    }
    ::RegCloseKey(key);
    asked = true;
    return true;
}

// A pending rename is worth a reboot prompt only when it is replacing a driver:
// \DriverStore\ is where every display driver lands and \System32\drivers\ where
// its kernel-mode half lands. The rest of this value is ordinary file churn, and
// entries in it survive reboots -- a machine one day after boot was still
// carrying three temp renames from desktop security software, so taking the value
// at face value would have told that player to reboot for nothing.
bool NamesDriverPath(const std::wstring& value)
{
    std::wstring lower;
    lower.reserve(value.size());
    for (const wchar_t character : value)
    {
        lower.push_back(character < 128 ? static_cast<wchar_t>(std::tolower(character)) : character);
    }
    return lower.find(L"\\driverstore\\") != std::wstring::npos ||
           lower.find(L"\\system32\\drivers\\") != std::wstring::npos;
}

bool PendingDriverReplacementPresent(bool& asked)
{
    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager",
                        0, KEY_READ, &key) != ERROR_SUCCESS)
    {
        return false;
    }
    DWORD size = 0;
    LONG query = ::RegQueryValueExW(key, L"PendingFileRenameOperations", nullptr, nullptr, nullptr, &size);
    if (query == ERROR_FILE_NOT_FOUND)
    {
        ::RegCloseKey(key);
        asked = true;
        return false;
    }
    if (query != ERROR_SUCCESS || size < sizeof(wchar_t))
    {
        ::RegCloseKey(key);
        return false;
    }
    std::vector<wchar_t> buffer(size / sizeof(wchar_t) + 2, L'\0');
    DWORD stored = size;
    query = ::RegQueryValueExW(key, L"PendingFileRenameOperations", nullptr, nullptr,
                               reinterpret_cast<LPBYTE>(buffer.data()), &stored);
    ::RegCloseKey(key);
    if (query != ERROR_SUCCESS)
    {
        return false;
    }
    asked = true;
    return NamesDriverPath(std::wstring(buffer.data(), stored / sizeof(wchar_t)));
}

// The places Windows records that something has finished installing and is
// waiting for the next boot. Read as existence checks: the interesting part is
// that the key or value is there at all, and none of them carry a state worth
// parsing.
bool ReadRebootPending(bool& known)
{
    known = false;
    bool asked = false;
    bool pending = false;
    if (RebootKeyPresent(
            L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Component Based Servicing\\RebootPending", asked))
    {
        pending = true;
    }
    if (RebootKeyPresent(
            L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\WindowsUpdate\\Auto Update\\RebootRequired", asked))
    {
        pending = true;
    }
    if (PendingDriverReplacementPresent(asked))
    {
        pending = true;
    }
    known = asked;
    return pending;
}

// How far back the driver-reset scan looks, and how long it may take. Both are
// small on purpose: this runs at every launch, and a machine that reset its
// graphics driver three months ago does not have a graphics problem today.
constexpr std::uint32_t GpuResetsLookbackDays = 7;
constexpr std::uint32_t GpuResetsScanDeadlineMilliseconds = 600;

// Counts graphics driver resets (TDR) in the last few days. The reader walks the
// log newest first, so a scan the cap or the deadline cut short still holds the
// most recent events: an incomplete scan can understate the count, never invent
// one. Stays `known = false` when neither channel could be read, because "could
// not look" must not become "nothing happened".
void ScanRecentGpuDriverResets(PreflightInputs& inputs)
{
    const UtcTicks now = diagnosis::CurrentUtcTime();
    diagnosis::EventLogReadOptions options;
    options.rangeEnd = now;
    options.rangeStart = diagnosis::SubtractClamped(
        now, static_cast<UtcTicks>(GpuResetsLookbackDays) * 24ull * diagnosis::TicksPerHour);
    options.deadlineTickCount = ::GetTickCount64() + GpuResetsScanDeadlineMilliseconds;

    const diagnosis::EventCollection collection = diagnosis::ReadEventLogs(options);
    if (!collection.observability.systemLogReadable && !collection.observability.applicationLogReadable)
    {
        return;
    }
    inputs.gpuTdrKnown = true;
    for (const auto& event : collection.events)
    {
        if (!diagnosis::IsGpuDriverResetEvent(event))
        {
            continue;
        }
        ++inputs.gpuTdrCount;
        if (event.time > inputs.gpuTdrLatestAt)
        {
            inputs.gpuTdrLatestAt = event.time;
        }
    }
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

    // Two registry reads and at most three file-exists checks: cheap enough to
    // sit inside a three-second budget.
    inputs.vcRuntimePresent = ReadVcRuntimeInstalled(inputs.vcRuntimeKnown);
    if (!inputs.vcRuntimePresent && HasAppLocalVcRuntime(gameDirectory))
    {
        inputs.vcRuntimePresent = true;
        inputs.vcRuntimeKnown = true;
    }
    inputs.rebootPending = ReadRebootPending(inputs.rebootPendingKnown);

    // This one starts a graphics driver and costs tens of milliseconds, which
    // makes it the most expensive thing the preflight does -- and also the only
    // thing in it that can answer "the game will not start at all". It belongs
    // with the probes, inside the budget, so that a driver that hangs turns into
    // an Inconclusive verdict instead of a hung process.
    try
    {
        inputs.direct3d11 = ProbeDirect3D11();
    }
    catch (const std::exception&)
    {
        inputs.direct3d11 = Direct3D11Probe{};
    }

    // Deliberately last: it is the one part of the collection that can be slow on
    // a machine with a busy event log, and the D3D11 probe above is the one that
    // decides whether the game starts at all. A scan that eats the rest of the
    // budget must not cost that answer.
    try
    {
        ScanRecentGpuDriverResets(inputs);
    }
    catch (const std::exception&)
    {
        inputs.gpuTdrKnown = false;
        inputs.gpuTdrCount = 0;
        inputs.gpuTdrLatestAt = 0;
    }
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
    if (findingCode == "D3D11_DEVICE_UNAVAILABLE")
    {
        return {"INSTALL_GPU_DRIVER", "安装显卡厂商官方驱动后重启电脑。"};
    }
    if (findingCode == "D3D11_ONLY_SOFTWARE_ADAPTER")
    {
        return {"INSTALL_GPU_DRIVER", "安装显卡厂商官方驱动后重启电脑。"};
    }
    if (findingCode == "D3D_FEATURE_LEVEL_BELOW_MINIMUM")
    {
        return {"UPGRADE_GPU", "显卡不支持游戏需要的功能级别，需要升级显卡或更新驱动后重启电脑。"};
    }
    if (findingCode == "D3D11_NOT_ON_PREFERRED_ADAPTER")
    {
        return {"SELECT_DISCRETE_GPU", "在显卡控制面板里把游戏指定为使用高性能（独立）显卡。"};
    }
    if (findingCode == "REBOOT_PENDING" || findingCode == "UPTIME_VERY_LONG")
    {
        return {"RESTART_COMPUTER", "保存好手头的工作，重启电脑后再启动游戏。"};
    }
    if (findingCode == "GPU_DRIVER_RESET_RECENT")
    {
        // The same sentence as a machine that cannot create a device at all: both
        // are "the graphics driver on this machine is not one you want to play on".
        return {"INSTALL_GPU_DRIVER", "安装显卡厂商官方驱动后重启电脑。"};
    }
    if (findingCode == "WINDOWS_INSIDER_PREVIEW")
    {
        return {"LEAVE_INSIDER", "退出 Windows 预览体验计划，回到正式版系统。"};
    }
    if (findingCode == "PHYSICAL_MEMORY_INSUFFICIENT")
    {
        return {"FREE_MEMORY", "关闭其它占用内存的程序，或升级内存后再启动游戏。"};
    }
    if (findingCode == "MEMORY_AVAILABLE_LOW")
    {
        return {"CLOSE_BACKGROUND_APPS", "关闭占用内存的程序（浏览器、录屏、下载工具等）后再启动游戏。"};
    }
    if (findingCode == "PAGE_FILE_TOO_SMALL")
    {
        return {"ENABLE_SYSTEM_MANAGED_PAGE_FILE",
                "在“系统属性 → 高级 → 性能 → 虚拟内存”里交回给系统管理，然后重启电脑。"};
    }
    if (findingCode == "VC_RUNTIME_MISSING")
    {
        return {"INSTALL_VC_REDIST", "安装 Microsoft Visual C++ 2015-2022 运行库（x64）后重启电脑。"};
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

    // The game renders through Direct3D 11, so a machine where no real adapter
    // can initialise it is a machine where the game does not start at all --
    // the "double-click and nothing happens" report that no other rule here
    // catches. Blocking is only honest because the probe asks the other adapters
    // first and only reports this when none of them could do it.
    if (inputs.direct3d11.known && !inputs.direct3d11.deviceCreated)
    {
        // Two machines, two sentences. "Only the software adapter could create a
        // device" means the driver is missing; the game does start there, on
        // WARP, and telling a player it cannot start about a game that does is
        // the one thing they will quote back at support.
        if (inputs.direct3d11.softwareOnlyAdapter)
        {
            result.findings.push_back({"D3D11_ONLY_SOFTWARE_ADAPTER", "gpu",
                "这台电脑上只有软件渲染器可以运行 Direct3D 11（显卡驱动没有装好），画面和帧率会非常差。",
                "only the software adapter created a device: " + inputs.direct3d11.adapterName});
        }
        else
        {
            result.findings.push_back({"D3D11_DEVICE_UNAVAILABLE", "gpu",
                "这台电脑的显卡无法初始化 Direct3D 11，游戏无法启动。",
                "D3D11CreateDevice failed, hr=" + HexText(inputs.direct3d11.failureHresult)});
        }
    }

    // Only when the game states a level of its own. A D3D11 game running at
    // 10_0 usually runs, so inventing a floor here would block machines that
    // work -- which is the one mistake a preflight cannot undo.
    if (inputs.direct3d11.known && inputs.direct3d11.deviceCreated &&
        requirements.minimumFeatureLevel != 0 &&
        inputs.direct3d11.featureLevel < requirements.minimumFeatureLevel)
    {
        result.findings.push_back({"D3D_FEATURE_LEVEL_BELOW_MINIMUM", "gpu",
            "显卡支持的功能级别低于游戏要求，游戏无法启动。",
            FeatureLevelName(inputs.direct3d11.featureLevel) + " on " +
            inputs.direct3d11.adapterName + ", " +
            FeatureLevelName(requirements.minimumFeatureLevel) + " required"});
    }
}

// Everything here is advisory. A preflight that blocks a launch is a preflight
// that will one day block a paying player over a false positive, and the
// launcher is the one that decides how loudly to say it.
void AppendAdvisoryFindings(PreflightResult& result, const PreflightInputs& inputs,
                            const Requirements& requirements)
{
    // D3D11 works somewhere on this machine, but not on the card the game is
    // going to pick. Advising rather than blocking: production code walks the
    // adapter list too, and the game may well start on the other one.
    if (inputs.direct3d11.known && inputs.direct3d11.deviceCreated &&
        !inputs.direct3d11.onPreferredAdapter)
    {
        result.findings.push_back({"D3D11_NOT_ON_PREFERRED_ADAPTER", "gpu",
            "游戏默认使用的显卡无法初始化 Direct3D 11，游戏可能会改用另一张显卡或无法启动。",
            "D3D11 available on " + inputs.direct3d11.adapterName});
    }

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

    // Installed memory below the minimum is reported as blocking above. This one
    // is for the machine that qualifies on paper and is simply full right now --
    // which is what the crash reports actually look like.
    if (inputs.system.availablePhysicalMemoryBytes != 0 &&
        inputs.system.physicalMemoryBytes >= requirements.minimumMemoryBytes &&
        inputs.system.availablePhysicalMemoryBytes < requirements.minimumAvailableMemoryBytes)
    {
        result.findings.push_back({"MEMORY_AVAILABLE_LOW", "memory",
            "当前可用内存很少，游戏启动后很可能卡顿或被系统直接终止。",
            std::to_string(inputs.system.availablePhysicalMemoryBytes / (1024 * 1024)) + " MB available of " +
            std::to_string(inputs.system.physicalMemoryBytes / (1024 * 1024)) + " MB installed"});
    }

    // ullTotalPageFile is the commit limit: RAM plus page file, so subtracting
    // RAM is how much page file there is. None at all turns a memory spike into
    // an out-of-memory kill instead of a slow swap.
    //
    // Two ways to be wrong here, and they are not the same machine. The absolute
    // floor catches the machine with almost none. The share of RAM catches the
    // 32 GB laptop with a 3.9 GB page file that exhausted its commit limit with
    // 17 GB of physical memory still free: it clears any absolute floor, because
    // the ceiling it keeps hitting is one nobody can see from the outside.
    // No page file at all on a large machine is still left alone -- that is a
    // choice its owner made deliberately and can defend.
    if (inputs.system.totalPageFileBytes != 0 && inputs.system.physicalMemoryBytes != 0)
    {
        const std::uint64_t pageFileBytes =
            inputs.system.totalPageFileBytes > inputs.system.physicalMemoryBytes
                ? inputs.system.totalPageFileBytes - inputs.system.physicalMemoryBytes
                : 0;
        const bool deliberatelyWithoutOne =
            pageFileBytes == 0 &&
            inputs.system.physicalMemoryBytes >= requirements.pageFileRuleMaxMemoryBytes;
        const bool belowFloor = pageFileBytes < requirements.minimumPageFileBytes;
        std::uint64_t requiredByShare = 0;
        if (requirements.minimumPageFilePercentOfRam != 0)
        {
            requiredByShare =
                inputs.system.physicalMemoryBytes * requirements.minimumPageFilePercentOfRam / 100;
            if (requirements.pageFileShareCapBytes != 0)
            {
                requiredByShare = std::min(requiredByShare, requirements.pageFileShareCapBytes);
            }
        }
        const bool belowShare = requiredByShare != 0 && pageFileBytes < requiredByShare;
        if (!deliberatelyWithoutOne && (belowFloor || belowShare))
        {
            result.findings.push_back({"PAGE_FILE_TOO_SMALL", "memory",
                "系统页面文件过小或未启用，内存吃紧时游戏会被系统终止而不是变慢。",
                "page file " + std::to_string(pageFileBytes / (1024 * 1024)) + " MB, RAM " +
                std::to_string(inputs.system.physicalMemoryBytes / (1024 * 1024)) + " MB (" +
                std::to_string(pageFileBytes * 100 / inputs.system.physicalMemoryBytes) + "% of RAM)"});
        }
    }

    // Advisory and never blocking, on purpose: whether the game needs the
    // runtime at all depends on how it was linked, and this tool cannot tell
    // that from outside the process. "Might be why nothing happens" is honest;
    // "cannot run" would be a guess.
    if (inputs.vcRuntimeKnown && !inputs.vcRuntimePresent)
    {
        result.findings.push_back({"VC_RUNTIME_MISSING", "runtime",
            "没有检测到 Visual C++ 运行库，如果游戏点开后没有任何反应，需要先安装它。", ""});
    }

    if (inputs.gameVolumeKnown && inputs.gameVolumeFreeBytes < requirements.minimumFreeDiskSpaceBytes)
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

    // A driver or an update that finished installing and is waiting for a reboot.
    // Every other number here describes the version on disk while the machine is
    // still running the one it booted with, so this is the state that makes the
    // rest of the report read healthier than the machine actually is.
    if (inputs.rebootPendingKnown && inputs.rebootPending)
    {
        result.findings.push_back({"REBOOT_PENDING", "system",
            "系统里有更新或驱动装完了但还没重启，重启之后才会真正生效。", ""});
    }

    // Long uptime on its own proves little, which is why the threshold is weeks
    // and the finding is advisory. Measured in powered-on time, not calendar days:
    // a machine that sleeps every night is not a stale machine.
    if (requirements.maximumUptimeDays != 0 && inputs.system.uptimeMilliseconds != 0)
    {
        const std::uint64_t uptimeDays = inputs.system.uptimeMilliseconds / (1000ull * 60 * 60 * 24);
        if (uptimeDays >= requirements.maximumUptimeDays)
        {
            result.findings.push_back({"UPTIME_VERY_LONG", "system",
                "这台电脑已经很久没有重启，建议重启后再启动游戏。",
                std::to_string(uptimeDays) + " days since boot"});
        }
    }

    // Graphics driver resets in the last week. One is already worth saying: a
    // reset is never a normal event, and a player who came here because the game
    // went black or froze is usually looking at exactly this. The events are read
    // newest first, so a truncated scan can understate the count, never invent it.
    if (inputs.gpuTdrKnown && inputs.gpuTdrCount != 0)
    {
        result.findings.push_back({"GPU_DRIVER_RESET_RECENT", "gpu",
            "最近显卡驱动崩溃并重置过，游戏可能出现黑屏、画面卡住或闪退。",
            std::to_string(inputs.gpuTdrCount) + " resets, latest at " +
            diagnosis::FormatUtcIso8601(inputs.gpuTdrLatestAt)});
    }
}

bool IsBlocking(const std::string& findingCode)
{
    return findingCode == "WINDOWS_X64_REQUIRED" ||
           findingCode == "CPU_ISA_UNSUPPORTED" ||
           findingCode == "PHYSICAL_MEMORY_INSUFFICIENT" ||
           findingCode == "D3D11_DEVICE_UNAVAILABLE" ||
           findingCode == "D3D11_ONLY_SOFTWARE_ADAPTER" ||
           findingCode == "D3D_FEATURE_LEVEL_BELOW_MINIMUM";
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
        << "  --min-spec <path>            Minimum-spec file to measure against.\n"
        << "                               Default: <game-dir>/min-spec.json. A game\n"
        << "                               that ships none uses the built-in spec.\n"
        << "  --game-exe <name>            Game executable name. Default: WXQ.exe.\n"
        << "  --budget-seconds N           Hard probe budget. Default: 3, range 1-30.\n"
        << "  --quiet                      Do not print the summary.\n\n"
        << "Exit codes: 0=conclusion written (or another preflight already owns the\n"
        << "output file, in which case nothing is written), 30=tool error.\n"
        << "A non-zero verdict is reported in the file, not in the exit code: the\n"
        << "launcher reads the file, so `exe && next` in a .bat stays correct.\n";
}

} // namespace

std::uint32_t ParseFeatureLevelName(const std::string& name)
{
    // The runtime's own numbering, which is what a device hands back and what
    // comparisons are made against. Written out because these are the only
    // levels Direct3D has ever defined and anything else is a typo worth naming.
    if (name == "9_1") { return 0x9100; }
    if (name == "9_2") { return 0x9200; }
    if (name == "9_3") { return 0x9300; }
    if (name == "10_0") { return 0xa000; }
    if (name == "10_1") { return 0xa100; }
    if (name == "11_0") { return 0xb000; }
    if (name == "11_1") { return 0xb100; }
    if (name == "12_0") { return 0xc000; }
    if (name == "12_1") { return 0xc100; }
    if (name == "12_2") { return 0xc200; }
    return 0;
}

std::string FeatureLevelName(std::uint32_t value)
{
    switch (value)
    {
        case 0x9100: return "9_1";
        case 0x9200: return "9_2";
        case 0x9300: return "9_3";
        case 0xa000: return "10_0";
        case 0xa100: return "10_1";
        case 0xb000: return "11_0";
        case 0xb100: return "11_1";
        case 0xc000: return "12_0";
        case 0xc100: return "12_1";
        case 0xc200: return "12_2";
        default: return "none";
    }
}

std::string HexText(std::uint32_t value)
{
    std::ostringstream stream;
    stream << "0x" << std::hex << value;
    return stream.str();
}

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
    // Both memory floors below are advisory and deliberately below what the game
    // would like to have: they exist to catch the machine that is technically
    // fine and practically full, not to second-guess the minimum spec.
    requirements.minimumAvailableMemoryBytes = 2ull * 1024ull * 1024ull * 1024ull;
    requirements.minimumPageFileBytes = 2ull * 1024ull * 1024ull * 1024ull;
    requirements.pageFileRuleMaxMemoryBytes = 16ull * 1024ull * 1024ull * 1024ull;
    requirements.minimumPageFilePercentOfRam = 25;
    requirements.pageFileShareCapBytes = 8ull * 1024ull * 1024ull * 1024ull;
    requirements.maximumUptimeDays = 14;
    return requirements;
}

// A reader for the one shape min-spec.json is allowed to have: a flat object of
// booleans and integers. Deliberately not a JSON parser -- a file that nests,
// escapes, or turns into an array is rejected outright, and a game that writes
// something else gets the fallback rather than a half-understood spec. What is
// accepted is what a human can write by hand and still be sure of.
class SpecFileCursor
{
public:
    explicit SpecFileCursor(const std::string& text) : text_(text)
    {
        // A BOM is what a text editor leaves behind, and without this every file
        // saved on Windows would be unreadable.
        if (text_.size() >= 3 &&
            static_cast<unsigned char>(text_[0]) == 0xEF &&
            static_cast<unsigned char>(text_[1]) == 0xBB &&
            static_cast<unsigned char>(text_[2]) == 0xBF)
        {
            at_ = 3;
        }
    }

    void SkipSpace()
    {
        while (at_ < text_.size() && IsSpace(text_[at_]))
        {
            ++at_;
        }
    }

    bool Done() const { return at_ >= text_.size(); }
    char Peek() const { return at_ < text_.size() ? text_[at_] : '\0'; }

    bool Take(char expected)
    {
        if (Peek() != expected)
        {
            return false;
        }
        ++at_;
        return true;
    }

    bool ReadQuoted(std::string& out)
    {
        if (!Take('"'))
        {
            return false;
        }
        out.clear();
        while (at_ < text_.size() && text_[at_] != '"')
        {
            out.push_back(text_[at_++]);
        }
        return Take('"');
    }

    bool ReadUnsigned(std::uint64_t& out)
    {
        const std::size_t begin = at_;
        while (at_ < text_.size() && text_[at_] >= '0' && text_[at_] <= '9')
        {
            ++at_;
        }
        if (at_ == begin)
        {
            return false;
        }
        try
        {
            out = std::stoull(text_.substr(begin, at_ - begin));
        }
        catch (const std::exception&)
        {
            return false;
        }
        return true;
    }

    bool ReadBoolean(bool& out)
    {
        if (text_.compare(at_, 4, "true") == 0)
        {
            at_ += 4;
            out = true;
            return true;
        }
        if (text_.compare(at_, 5, "false") == 0)
        {
            at_ += 5;
            out = false;
            return true;
        }
        return false;
    }

    // Consumes a value of any supported shape, so an unrecognised key can be
    // skipped and the rest of the file still used.
    bool SkipValue()
    {
        std::uint64_t number = 0;
        if (ReadUnsigned(number))
        {
            return true;
        }
        bool flag = false;
        if (ReadBoolean(flag))
        {
            return true;
        }
        std::string text;
        return ReadQuoted(text);
    }

private:
    static bool IsSpace(char c)
    {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    }

    const std::string& text_;
    std::size_t at_ = 0;
};

bool ParseRequirementsJson(const std::string& text, Requirements& requirements,
                           std::vector<std::string>& problems)
{
    // Built on top of the built-in spec so a file that sets two keys overrides
    // two keys instead of redefining the whole minimum -- and published only on
    // success, because a caller that carries on after a failed parse must not be
    // holding half a minimum spec and believing it.
    Requirements parsed = DefaultPreflightRequirements();
    problems.clear();

    SpecFileCursor cursor(text);
    cursor.SkipSpace();
    if (!cursor.Take('{'))
    {
        problems.push_back("min-spec: expected '{' at the start of the file");
        return false;
    }

    bool closed = false;
    while (!closed)
    {
        cursor.SkipSpace();
        if (cursor.Take('}'))
        {
            closed = true;
            break;
        }
        std::string key;
        if (!cursor.ReadQuoted(key))
        {
            problems.push_back("min-spec: expected a quoted key");
            return false;
        }
        cursor.SkipSpace();
        if (!cursor.Take(':'))
        {
            problems.push_back("min-spec: expected ':' after \"" + key + "\"");
            return false;
        }
        cursor.SkipSpace();

        if (key == "minimumFeatureLevel")
        {
            // The one non-numeric threshold, and the only one spelled as text:
            // "11_0" is how the requirement is written down everywhere else,
            // and a bare number here would be a guess at the runtime's encoding.
            std::string name;
            if (!cursor.ReadQuoted(name))
            {
                problems.push_back("min-spec: \"minimumFeatureLevel\" must be a name like \"11_0\"");
                return false;
            }
            const std::uint32_t level = ParseFeatureLevelName(name);
            if (level == 0)
            {
                problems.push_back("min-spec: \"" + name + "\" is not a feature level");
                return false;
            }
            parsed.minimumFeatureLevel = level;
        }
        else if (key == "avx" || key == "avx2" || key == "fma" || key == "f16c")
        {
            bool value = false;
            if (!cursor.ReadBoolean(value))
            {
                problems.push_back("min-spec: \"" + key + "\" must be true or false");
                return false;
            }
            if (key == "avx") { parsed.avx = value; }
            else if (key == "avx2") { parsed.avx2 = value; }
            else if (key == "fma") { parsed.fma = value; }
            else { parsed.f16c = value; }
        }
        else if (key == "minimumMemoryBytes" || key == "minimumAvailableMemoryBytes" ||
                 key == "minimumPageFileBytes" || key == "pageFileRuleMaxMemoryBytes" ||
                 key == "minimumPageFilePercentOfRam" || key == "pageFileShareCapBytes" ||
                 key == "maximumUptimeDays" || key == "minimumFreeDiskSpaceBytes")
        {
            std::uint64_t value = 0;
            if (!cursor.ReadUnsigned(value))
            {
                problems.push_back("min-spec: \"" + key + "\" must be a whole number of bytes");
                return false;
            }
            if (key == "minimumMemoryBytes") { parsed.minimumMemoryBytes = value; }
            else if (key == "minimumAvailableMemoryBytes") { parsed.minimumAvailableMemoryBytes = value; }
            else if (key == "minimumPageFileBytes") { parsed.minimumPageFileBytes = value; }
            else if (key == "pageFileRuleMaxMemoryBytes") { parsed.pageFileRuleMaxMemoryBytes = value; }
            else if (key == "minimumPageFilePercentOfRam")
            {
                parsed.minimumPageFilePercentOfRam = static_cast<std::uint32_t>(value);
            }
            else if (key == "pageFileShareCapBytes") { parsed.pageFileShareCapBytes = value; }
            else if (key == "maximumUptimeDays") { parsed.maximumUptimeDays = static_cast<std::uint32_t>(value); }
            else { parsed.minimumFreeDiskSpaceBytes = value; }
        }
        else
        {
            // An unknown key is reported and skipped, not fatal: a file written
            // for a newer tool still carries keys this one understands, and
            // dropping all of them over one rename would apply a spec nobody
            // wrote. Zero is how a game turns a rule off, so this is also the
            // only way to find a threshold that was meant to be set and is not.
            if (!cursor.SkipValue())
            {
                problems.push_back("min-spec: cannot read the value of \"" + key + "\"");
                return false;
            }
            problems.push_back("min-spec: unrecognised key \"" + key + "\" was ignored");
        }

        cursor.SkipSpace();
        if (cursor.Take(','))
        {
            continue;
        }
        if (cursor.Take('}'))
        {
            closed = true;
            break;
        }
        problems.push_back("min-spec: expected ',' or '}' after \"" + key + "\"");
        return false;
    }

    cursor.SkipSpace();
    if (!cursor.Done())
    {
        problems.push_back("min-spec: trailing content after the closing '}'");
        return false;
    }
    requirements = parsed;
    return true;
}

std::filesystem::path DefaultMinSpecPath(const std::filesystem::path& gameDirectory)
{
    return gameDirectory / "min-spec.json";
}

LoadedRequirements LoadRequirements(const std::filesystem::path& gameDirectory,
                                    const std::filesystem::path& overridePath)
{
    LoadedRequirements loaded;
    loaded.requirements = DefaultPreflightRequirements();

    const std::filesystem::path path = overridePath.empty()
        ? DefaultMinSpecPath(gameDirectory)
        : overridePath;
    if (path.empty())
    {
        return loaded;
    }

    std::error_code ignored;
    if (!std::filesystem::exists(path, ignored))
    {
        // Shipping no file is a supported configuration, not a fault: it means
        // "measure against the spec this tool was built with". Staying quiet
        // here is what keeps a line out of every report for nothing.
        return loaded;
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open())
    {
        loaded.problems.push_back(
            "min-spec: " + diagnosis::WideToUtf8(path.wstring()) + " exists but could not be opened");
        return loaded;
    }
    const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());

    if (!ParseRequirementsJson(text, loaded.requirements, loaded.problems))
    {
        // Nothing to undo: the parser publishes only a complete spec, so what is
        // left here is the built-in one this function started from. A partial
        // minimum would be worse than a known default, because at least the
        // default is the same one every other report was measured against.
        return loaded;
    }
    loaded.source = "file:" + diagnosis::WideToUtf8(path.wstring());
    return loaded;
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
    AppendAdvisoryFindings(result, inputs, requirements);

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

    // What the verdict was measured against. A report that says Pass is
    // unanswerable without it: support cannot tell whether the machine was fine
    // or the spec handed to this tool was.
    stream << "  \"requirements\": {\"avx\": " << BoolText(context.requirements.avx)
           << ", \"avx2\": " << BoolText(context.requirements.avx2)
           << ", \"fma\": " << BoolText(context.requirements.fma)
           << ", \"f16c\": " << BoolText(context.requirements.f16c)
           << ", \"minimumMemoryBytes\": " << context.requirements.minimumMemoryBytes
           << ", \"minimumAvailableMemoryBytes\": " << context.requirements.minimumAvailableMemoryBytes
           << ", \"minimumPageFileBytes\": " << context.requirements.minimumPageFileBytes
           << ", \"pageFileRuleMaxMemoryBytes\": " << context.requirements.pageFileRuleMaxMemoryBytes
           << ", \"minimumFreeDiskSpaceBytes\": " << context.requirements.minimumFreeDiskSpaceBytes
           << ", \"minimumPageFilePercentOfRam\": " << context.requirements.minimumPageFilePercentOfRam
           << ", \"pageFileShareCapBytes\": " << context.requirements.pageFileShareCapBytes
           << ", \"maximumUptimeDays\": " << context.requirements.maximumUptimeDays
           << ", \"minimumFeatureLevel\": " << JsonString(FeatureLevelName(context.requirements.minimumFeatureLevel))
           << "},\n"
           << "  \"requirementsSource\": " << JsonString(context.requirementsSource) << ",\n";

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
           << ", \"uptimeMilliseconds\": " << inputs.system.uptimeMilliseconds
           << ", \"insiderPreview\": " << BoolText(inputs.insiderPreview) << "},\n"
           << "    \"pageFile\": {\"totalBytes\": " << inputs.system.totalPageFileBytes
           << ", \"availableBytes\": " << inputs.system.availablePageFileBytes
           << ", \"effectivePageFileBytes\": "
           << (inputs.system.totalPageFileBytes > inputs.system.physicalMemoryBytes
                   ? inputs.system.totalPageFileBytes - inputs.system.physicalMemoryBytes
                   : 0ull)
           << "},\n"
           << "    \"runtime\": {\"vcRuntimePresent\": " << BoolText(inputs.vcRuntimePresent)
           << ", \"vcRuntimeKnown\": " << BoolText(inputs.vcRuntimeKnown) << "},\n"
           << "    \"reboot\": {\"pending\": " << BoolText(inputs.rebootPending)
           << ", \"known\": " << BoolText(inputs.rebootPendingKnown) << "},\n"
           << "    \"gpuDriverResets\": {\"known\": " << BoolText(inputs.gpuTdrKnown)
           << ", \"count\": " << inputs.gpuTdrCount
           << ", \"latestAt\": " << JsonString(diagnosis::FormatUtcIso8601(inputs.gpuTdrLatestAt))
           << "},\n"
           << "    \"direct3d11\": {\"known\": " << BoolText(inputs.direct3d11.known)
           << ", \"deviceCreated\": " << BoolText(inputs.direct3d11.deviceCreated)
           << ", \"onPreferredAdapter\": " << BoolText(inputs.direct3d11.onPreferredAdapter)
           << ", \"softwareOnlyAdapter\": " << BoolText(inputs.direct3d11.softwareOnlyAdapter)
           << ", \"featureLevel\": " << JsonString(FeatureLevelName(inputs.direct3d11.featureLevel))
           << ", \"featureLevelValue\": " << inputs.direct3d11.featureLevel
           << ", \"adapterName\": " << JsonString(inputs.direct3d11.adapterName)
           << ", \"failureHresult\": " << JsonString(HexText(inputs.direct3d11.failureHresult))
           << "},\n";

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
        else if (argument == L"--min-spec")
        {
            options.minSpecPath = RequireValue(index, argumentCount, arguments, "--min-spec");
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
    // The minimum spec is the game's business, not this tool's, so it is read
    // from beside the game at every launch instead of being compiled in. Read
    // after the probes: it is one small local file, and it cannot change what
    // the probes measured.
    const LoadedRequirements loaded = LoadRequirements(gameDirectory, options.minSpecPath);
    PreflightResult result = EvaluatePreflight(inputs, loaded.requirements, withinBudget);
    for (const auto& problem : loaded.problems)
    {
        result.notes.push_back(problem);
    }

    PreflightReportContext context;
    context.generatedAt = generatedAt;
    context.gameExecutable = options.gameExecutable;
    context.budgetSeconds = options.budgetSeconds;
    context.requirements = loaded.requirements;
    context.requirementsSource = loaded.source;
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
