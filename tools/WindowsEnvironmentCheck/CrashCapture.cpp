#include "CrashCapture.h"
#include "CapturePackage.h"
#include "DiagnosisUtil.h"
#include "ReportWriter.h"
#include "ToolVersion.h"
#include <Windows.h>
#include <TlHelp32.h>
#include <wintrust.h>
#include <softpub.h>
#include <wincrypt.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <sstream>
#include <iomanip>
#include <stdexcept>
#include <thread>

namespace zengine::envcheck::collector
{
using namespace diagnosis;
namespace
{
constexpr std::uint64_t GiB = 1ull << 30;
struct Handle
{
    HANDLE value = nullptr;
    explicit Handle(HANDLE h = nullptr) : value(h) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
std::wstring Quote(const std::wstring& text)
{
    std::wstring out = L"\"";
    size_t slash = 0;
    for (wchar_t c : text)
    {
        if (c == L'\\') { ++slash; continue; }
        out.append(c == L'"' ? slash * 2 + 1 : slash, L'\\'); out += c; slash = 0;
    }
    out.append(slash * 2, L'\\'); return out + L'"';
}
std::string Version(const std::filesystem::path& file)
{
    DWORD ignored = 0;
    DWORD size = GetFileVersionInfoSizeW(file.c_str(), &ignored);
    if (!size || size > (4 << 20)) return "unknown";
    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(file.c_str(), 0, size, data.data())) return "unknown";
    VS_FIXEDFILEINFO* info = nullptr; UINT length = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &length) || length < sizeof(*info)) return "unknown";
    return std::to_string(HIWORD(info->dwFileVersionMS)) + "." + std::to_string(LOWORD(info->dwFileVersionMS)) + "." +
        std::to_string(HIWORD(info->dwFileVersionLS)) + "." + std::to_string(LOWORD(info->dwFileVersionLS));
}
std::wstring OriginalName(const std::filesystem::path& file)
{
    DWORD ignored = 0; DWORD size = GetFileVersionInfoSizeW(file.c_str(), &ignored);
    if (!size || size > (4 << 20)) return {};
    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(file.c_str(), 0, size, data.data())) return {};
    struct Translation { WORD language, codepage; };
    Translation* translations = nullptr; UINT length = 0;
    if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&translations), &length)) return {};
    for (UINT i = 0; i < length / sizeof(Translation); ++i)
    {
        wchar_t key[128]{};
        swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\OriginalFilename", translations[i].language, translations[i].codepage);
        wchar_t* value = nullptr; UINT count = 0;
        if (VerQueryValueW(data.data(), key, reinterpret_cast<void**>(&value), &count) && count > 0) return value;
    }
    return {};
}
std::string HexStatus(LONG status)
{
    std::ostringstream text;
    text << "0x" << std::uppercase << std::hex << std::setfill('0') << std::setw(8) << static_cast<std::uint32_t>(status);
    return text.str();
}
struct TrustFailureInfo { const char* code; const char* description; };
TrustFailureInfo TrustFailure(const ProcDumpVerification& verification)
{
    if (!verification.trustAttempted) return {"NOT_CHECKED", "尚未完成签名检查"};
    switch (verification.trustStatus)
    {
    case ERROR_SUCCESS:
        if (verification.signer.empty()) return {"SIGNER_UNAVAILABLE", "信任检查通过但无法取得签名者"};
        if (!verification.microsoftSigner) return {"SIGNER_NOT_MICROSOFT", "签名者不是Microsoft Corporation"};
        return {"TRUSTED_MICROSOFT", "微软签名信任检查通过"};
    case CRYPT_E_REVOCATION_OFFLINE: return {"CRYPT_E_REVOCATION_OFFLINE", "证书吊销查询离线或不可达"};
    case CERT_E_REVOCATION_FAILURE: return {"CERT_E_REVOCATION_FAILURE", "证书吊销状态无法验证"};
    case CRYPT_E_NO_REVOCATION_CHECK: return {"CRYPT_E_NO_REVOCATION_CHECK", "证书缺少可用吊销检查信息"};
    case CERT_E_UNTRUSTEDROOT: return {"CERT_E_UNTRUSTEDROOT", "证书根不受系统信任"};
    case CERT_E_CHAINING: return {"CERT_E_CHAINING", "无法建立完整证书链"};
    case CERT_E_EXPIRED: return {"CERT_E_EXPIRED", "证书有效期检查失败，请核对系统时间"};
    case CERT_E_REVOKED: return {"CERT_E_REVOKED", "证书已吊销"};
    case TRUST_E_BAD_DIGEST: return {"TRUST_E_BAD_DIGEST", "文件摘要与签名不符，请从微软重新下载"};
    case TRUST_E_NOSIGNATURE: return {"TRUST_E_NOSIGNATURE", "没有可验证的数字签名"};
    case TRUST_E_SUBJECT_FORM_UNKNOWN: return {"TRUST_E_SUBJECT_FORM_UNKNOWN", "所选文件不是支持的签名格式"};
    case TRUST_E_PROVIDER_UNKNOWN: return {"TRUST_E_PROVIDER_UNKNOWN", "系统信任提供程序不可用"};
    case TRUST_E_SUBJECT_NOT_TRUSTED: return {"TRUST_E_SUBJECT_NOT_TRUSTED", "文件未通过系统信任策略"};
    default: return {"UNKNOWN_TRUST_ERROR", "签名信任检查失败，需查看原始错误码"};
    }
}
void VerifyMicrosoft(const std::filesystem::path& file, HANDLE handle, ProcDumpVerification& verification)
{
    WINTRUST_FILE_INFO info{}; info.cbStruct = sizeof(info); info.pcwszFilePath = file.c_str(); info.hFile = handle;
    WINTRUST_DATA trust{}; trust.cbStruct = sizeof(trust); trust.dwUIChoice = WTD_UI_NONE;
    trust.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN; trust.dwUnionChoice = WTD_CHOICE_FILE; trust.pFile = &info;
    trust.dwStateAction = WTD_STATEACTION_VERIFY; trust.dwProvFlags = WTD_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT;
    GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    verification.trustAttempted = true;
    verification.trustStatus = WinVerifyTrust(nullptr, &policy, &trust);
    wchar_t name[512]{}, issuer[512]{};
    auto* provider = WTHelperProvDataFromStateData(trust.hWVTStateData);
    auto* signer = provider ? WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0) : nullptr;
    if (signer && signer->csCertChain && signer->pasCertChain[0].pCert)
    {
        const auto certificate = signer->pasCertChain[0].pCert;
        SetLastError(ERROR_SUCCESS);
        if (CertGetNameStringW(certificate, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, name, 512) <= 1)
            verification.signerNameError = GetLastError();
        CertGetNameStringW(certificate, CERT_NAME_SIMPLE_DISPLAY_TYPE, CERT_NAME_ISSUER_FLAG, nullptr, issuer, 512);
    }
    trust.dwStateAction = WTD_STATEACTION_CLOSE; WinVerifyTrust(nullptr, &policy, &trust);
    verification.signer = WideToUtf8(name);
    verification.issuer = WideToUtf8(issuer);
    verification.microsoftSigner = _wcsicmp(name, L"Microsoft Corporation") == 0;
    if (verification.trustStatus != ERROR_SUCCESS || !verification.microsoftSigner)
        throw std::runtime_error(DescribeProcDumpTrustFailure(verification));
}
std::uint64_t FreeBytes(const std::filesystem::path& path)
{
    ULARGE_INTEGER available{};
    if (!GetDiskFreeSpaceExW(path.c_str(), &available, nullptr, nullptr)) throw std::runtime_error("无法查询剩余磁盘空间");
    return available.QuadPart;
}
class ProcessAccessError : public std::runtime_error
{
public:
    std::string operation;
    DWORD code, pid, access;
    ProcessAccessError(const char* api, DWORD error, DWORD processId, DWORD requestedAccess = 0)
        : std::runtime_error(std::string("目标进程访问失败：") + api + "，PID=" + std::to_string(processId) +
            "，Win32=" + std::to_string(error) + (error == ERROR_ACCESS_DENIED ? "（访问被拒绝）" :
                error == ERROR_INVALID_PARAMETER ? "（参数无效或进程已退出）" : "") + "；未绕过系统限制"),
          operation(api), code(error), pid(processId), access(requestedAccess) {}
};
std::filesystem::path ProcessPath(HANDLE process, DWORD pid)
{
    std::vector<wchar_t> buffer(32768); DWORD length = static_cast<DWORD>(buffer.size());
    if (!QueryFullProcessImageNameW(process, 0, buffer.data(), &length))
        throw ProcessAccessError("QueryFullProcessImageNameW", GetLastError(), pid);
    return std::filesystem::path(std::wstring(buffer.data(), length));
}
DWORD FindTarget(const std::filesystem::path& expected)
{
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (snapshot.value == INVALID_HANDLE_VALUE) throw std::runtime_error("无法枚举目标程序");
    PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
    DWORD currentSession = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &currentSession)) throw std::runtime_error("无法读取会话");
    DWORD found = 0;
    for (BOOL more = Process32FirstW(snapshot.value, &entry); more; more = Process32NextW(snapshot.value, &entry))
    {
        if (_wcsicmp(entry.szExeFile, expected.filename().c_str()) != 0) continue;
        DWORD session = 0;
        if (!ProcessIdToSessionId(entry.th32ProcessID, &session) || session != currentSession) continue;
        Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, entry.th32ProcessID));
        if (!process.value) throw ProcessAccessError("OpenProcess(discovery)", GetLastError(), entry.th32ProcessID, PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE);
        if (WaitForSingleObject(process.value, 0) == WAIT_OBJECT_0) continue;
        auto path = ProcessPath(process.value, entry.th32ProcessID);
        if (_wcsicmp(path.c_str(), expected.c_str()) != 0) continue;
        if (found) throw std::runtime_error("存在多个相同游戏进程，请仅保留需要采集的实例");
        found = entry.th32ProcessID;
    }
    return found;
}
std::uint64_t CommittedBytes(HANDLE process)
{
    std::uint64_t total = 0;
    uintptr_t address = 0;
    MEMORY_BASIC_INFORMATION region{};
    while (VirtualQueryEx(process, reinterpret_cast<void*>(address), &region, sizeof(region)) == sizeof(region))
    {
        if (region.State == MEM_COMMIT) total += region.RegionSize;
        const auto next = reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize;
        if (next <= address) break;
        address = next;
    }
    if (!total || total > 60 * GiB) throw std::runtime_error("无法估计dump大小或目标内存超过60GiB上限");
    return total;
}
void DrainPipe(HANDLE pipe, std::string& log)
{
    DWORD available = 0;
    std::array<char, 4096> buffer{};
    while (PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) && available)
    {
        DWORD read = 0;
        if (!ReadFile(pipe, buffer.data(), std::min<DWORD>(available, static_cast<DWORD>(buffer.size())), &read, nullptr) || !read) break;
        if (log.size() < (1 << 20)) log.append(buffer.data(), std::min<size_t>(read, (1 << 20) - log.size()));
    }
}
std::string DecodeLog(const std::string& bytes)
{
    if (bytes.size() >= 2 && (bytes[1] == 0 || (static_cast<unsigned char>(bytes[0]) == 0xff && static_cast<unsigned char>(bytes[1]) == 0xfe)))
    {
        std::wstring text;
        const size_t begin = static_cast<unsigned char>(bytes[0]) == 0xff ? 2 : 0;
        for (size_t i = begin; i + 1 < bytes.size(); i += 2)
            text += static_cast<wchar_t>(static_cast<unsigned char>(bytes[i]) | (static_cast<unsigned char>(bytes[i + 1]) << 8));
        return WideToUtf8(text);
    }
    return bytes;
}
struct MonitorLifetime
{
    HANDLE process;
    HANDLE output;
    const wchar_t* eventName;
    ~MonitorLifetime() noexcept
    {
        std::array<char, 4096> discarded{};
        while (WaitForSingleObject(process, 100) != WAIT_OBJECT_0)
        {
            HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, eventName);
            if (event) { SetEvent(event); CloseHandle(event); }
            DWORD available = 0, read = 0;
            if (PeekNamedPipe(output, nullptr, 0, nullptr, &available, nullptr) && available)
                ReadFile(output, discarded.data(), std::min<DWORD>(available, static_cast<DWORD>(discarded.size())), &read, nullptr);
        }
    }
};
std::filesystem::path CreateSession(const std::filesystem::path& root)
{
    std::filesystem::create_directories(root);
    auto stamp = FormatUtcIso8601(CurrentUtcTime());
    stamp.erase(std::remove(stamp.begin(), stamp.end(), ':'), stamp.end());
    for (unsigned i = 0; i < 100; ++i)
    {
        auto path = root / ("crash-capture-" + stamp + "-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(i));
        if (std::filesystem::create_directory(path)) return path;
    }
    throw std::runtime_error("无法创建新的取证目录");
}
}
std::string DescribeProcDumpTrustFailure(const ProcDumpVerification& verification)
{
    const auto failure = TrustFailure(verification);
    return std::string("ProcDump：") + failure.description + " [" + HexStatus(verification.trustStatus) + "]，已拒绝运行";
}
std::string ValidateProcDump(const std::filesystem::path& file, ProcDumpVerification* verification)
{
    ProcDumpVerification local;
    auto& details = verification ? *verification : local;
    details = {};
    EvidenceFile locked(file);
    details.fileVersion = Version(file);
    const auto original = OriginalName(file);
    details.originalName = WideToUtf8(original);
    VerifyMicrosoft(file, locked.Get(), details);
    if (_wcsicmp(original.c_str(), L"procdump") != 0 && _wcsicmp(original.c_str(), L"procdump.exe") != 0 && _wcsicmp(original.c_str(), L"procdump64.exe") != 0)
        throw std::runtime_error("所选文件不是微软ProcDump");
    DWORD binary = 0;
    if (!GetBinaryTypeW(file.c_str(), &binary) || binary != SCS_64BIT_BINARY)
        throw std::runtime_error("请选择64位ProcDump程序");
    details.verified = true;
    return details.fileVersion;
}
std::filesystem::path LoadProcDumpPreference()
{
    wchar_t path[32768]{}; DWORD size = sizeof(path);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\ZEngine\\EnvironmentCollector", L"ProcDumpPath", RRF_RT_REG_SZ, nullptr, path, &size) != ERROR_SUCCESS) return {};
    return path;
}
void SaveProcDumpPreference(const std::filesystem::path& file)
{
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\ZEngine\\EnvironmentCollector", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        throw std::runtime_error("无法保存ProcDump位置");
    auto text = file.wstring();
    LONG result = RegSetValueExW(key, L"ProcDumpPath", 0, REG_SZ, reinterpret_cast<const BYTE*>(text.c_str()), static_cast<DWORD>((text.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    if (result != ERROR_SUCCESS) throw std::runtime_error("无法保存ProcDump位置");
}
CaptureResult CaptureCrash(const CaptureOptions& options, const std::atomic_bool& stop,
                           const std::function<void(const std::string&, bool)>& progress)
{
    if (!options.memoryConsent || !options.licenseAccepted) throw std::runtime_error("必须先同意完整内存采集和Sysinternals许可");
    if (options.waitSeconds == 0 || options.waitSeconds > 86400) throw std::runtime_error("监控超时范围为1至86400秒");
    const auto game = std::filesystem::absolute(options.gameExecutable).lexically_normal();
    if (!std::filesystem::is_regular_file(game) || !EqualsIgnoreCaseAscii(WideToUtf8(game.filename().wstring()), options.environment.profile.gameExecutable))
        throw std::runtime_error("目标程序必须与当前项目profile指定的exe一致");
    const auto root = std::filesystem::absolute(options.environment.outputDirectory.empty() ? DefaultOutputDirectory() : options.environment.outputDirectory);
    CaptureResult result; result.report.directory = CreateSession(root);
    std::string outcome = "VERIFYING_PROCDUMP", log, failedStage, failedOperation;
    DWORD pid = 0, exitCode = STILL_ACTIVE, processError = ERROR_SUCCESS, requestedAccess = 0;
    bool procDumpStarted = false;
    ProcDumpVerification verification;
    std::string exeVersion, exeHash, procHash;
    DumpMetadata metadata{};
    const auto consentTime = CurrentUtcTime();
    auto manifest = [&](bool includeLog = true) {
        std::ostringstream json;
        json << "{\"schemaVersion\":1,\"toolVersion\":" << JsonString(ToolVersion) << ",\"status\":" << JsonString(outcome)
            << ",\"consentUtc\":" << JsonString(FormatUtcIso8601(consentTime)) << ",\"fullMemoryConsent\":true,\"sysinternalsLicenseAccepted\":true"
            << ",\"targetName\":" << JsonString(WideToUtf8(game.filename().wstring())) << ",\"pid\":" << pid
            << ",\"exeFileVersion\":" << JsonString(exeVersion) << ",\"exeSha256\":" << JsonString(exeHash)
            << ",\"procDumpVersion\":" << JsonString(verification.fileVersion) << ",\"procDumpSha256\":" << JsonString(procHash)
            << ",\"procDumpStarted\":" << (procDumpStarted ? "true" : "false")
            << ",\"failedStage\":" << JsonString(failedStage) << ",\"failedOperation\":" << JsonString(failedOperation)
            << ",\"win32Error\":" << (failedOperation.empty() ? "null" : std::to_string(processError))
            << ",\"requestedProcessAccess\":" << requestedAccess
            << ",\"verification\":{\"trustAttempted\":" << (verification.trustAttempted ? "true" : "false")
            << ",\"trustStatus\":" << (verification.trustAttempted ? std::to_string(verification.trustStatus) : "null")
            << ",\"trustStatusHex\":" << (verification.trustAttempted ? JsonString(HexStatus(verification.trustStatus)) : "null")
            << ",\"trustStatusName\":" << JsonString(TrustFailure(verification).code)
            << ",\"signer\":" << JsonString(verification.signer) << ",\"issuer\":" << JsonString(verification.issuer)
            << ",\"signerNameError\":" << verification.signerNameError << ",\"originalName\":" << JsonString(verification.originalName)
            << ",\"microsoftSigner\":" << (verification.microsoftSigner ? "true" : "false")
            << ",\"verified\":" << (verification.verified ? "true" : "false")
            << ",\"policy\":\"WINTRUST_ACTION_GENERIC_VERIFY_V2\",\"revocationChecks\":\"WHOLECHAIN\",\"providerFlags\":\"REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT\"}"
            << ",\"procDumpExitCode\":" << (procDumpStarted ? std::to_string(exitCode) : "null") << ",\"exceptionCode\":" << metadata.exceptionCode
            << ",\"dumpTimestampUnix\":" << metadata.timestamp << ",\"memoryBytes\":" << metadata.memoryBytes
            << ",\"dumpBytes\":" << metadata.fileBytes << ",\"dumpValidation\":" << JsonString(result.dumpCaptured ? "FULL_MEMORY_EXCEPTION_PID_AND_FILE_RANGES" : "NO_VALID_DUMP")
            << ",\"automaticUpload\":false,\"memoryRedacted\":false,\"message\":" << JsonString(result.message) << '}';
        WriteReportFile(result.report.directory / "capture.json", json.str());
        if (includeLog && procDumpStarted) WriteReportFile(result.report.directory / "procdump.log", DecodeLog(log));
    };
    try
    {
        manifest();
        EvidenceFile procDumpLock(options.procDump);
        if (procDumpLock.Size() > (64ull << 20)) throw std::runtime_error("所选ProcDump文件异常大，已拒绝运行");
        progress("校验微软ProcDump签名", true);
        procHash = Sha256File(options.procDump, &stop);
        ValidateProcDump(options.procDump, &verification);
        result.procDumpVerified = verification.verified;
        if (stop.load())
        {
            outcome = "CANCELLED"; result.report.cancelled = true;
            result.message = "验证后已取消，ProcDump未启动"; manifest(); return result;
        }
        outcome = "WAITING";
        manifest();
        const auto deadline = GetTickCount64() + options.waitSeconds * 1000ull;
        progress("等待所选游戏启动（最多1小时）", true);
        for (;;)
        {
            pid = FindTarget(game);
            if (pid) break;
            if (stop.load() || GetTickCount64() >= deadline)
            {
                outcome = stop.load() ? "CANCELLED" : "WAIT_TIMEOUT"; result.report.cancelled = stop.load();
                result.message = "未附加游戏，等待已结束"; manifest(); return result;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
        outcome = "OPENING_TARGET";
        constexpr DWORD targetAccess = PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | SYNCHRONIZE;
        Handle process(OpenProcess(targetAccess, FALSE, pid));
        if (!process.value) throw ProcessAccessError("OpenProcess(capture)", GetLastError(), pid, targetAccess);
        outcome = "CHECKING_TARGET_IDENTITY";
        if (WaitForSingleObject(process.value, 0) == WAIT_OBJECT_0)
            throw std::runtime_error("目标进程已退出，请保持游戏运行后重试；未附加");
        if (_wcsicmp(ProcessPath(process.value, pid).c_str(), game.c_str()) != 0)
            throw std::runtime_error("目标进程路径已变化，与所选exe不符；未附加");
        BOOL debugged = FALSE, wow64 = FALSE;
        if (!CheckRemoteDebuggerPresent(process.value, &debugged)) throw ProcessAccessError("CheckRemoteDebuggerPresent", GetLastError(), pid);
        if (debugged) throw std::runtime_error("目标已有调试器，不会重复附加");
        if (!IsWow64Process(process.value, &wow64)) throw ProcessAccessError("IsWow64Process", GetLastError(), pid);
        if (wow64) throw std::runtime_error("此模式仅支持64位游戏进程");
        const auto eventName = L"ProcDump-" + std::to_wstring(pid);
        Handle existing(OpenEventW(SYNCHRONIZE, FALSE, eventName.c_str()));
        if (existing.value || GetLastError() != ERROR_FILE_NOT_FOUND) throw std::runtime_error("目标已有ProcDump监控或无法确认其独占状态");
        const auto mutexName = L"Local\\ZEnvironmentCollector-Capture-" + std::to_wstring(pid);
        Handle mutex(CreateMutexW(nullptr, TRUE, mutexName.c_str()));
        if (!mutex.value || GetLastError() == ERROR_ALREADY_EXISTS) throw std::runtime_error("另一个采集器正在监控该进程");
        const auto estimated = CommittedBytes(process.value);
        if (FreeBytes(root) < std::max<std::uint64_t>(4 * GiB, estimated * 2 + 2 * GiB))
            throw std::runtime_error("磁盘空间不足：需同时保留完整dump与压缩包，至少预计内存的两倍加2GiB");
        exeVersion = Version(game); exeHash = Sha256File(game, &stop);
        if (stop.load()) { outcome = "CANCELLED"; result.report.cancelled = true; result.message = "附加前已取消"; manifest(); return result; }
        SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
        HANDLE readPipe = nullptr, writePipe = nullptr;
        if (!CreatePipe(&readPipe, &writePipe, &security, 0)) throw std::runtime_error("无法创建监控输出管道");
        Handle reader(readPipe), writer(writePipe);
        if (!SetHandleInformation(reader.value, HANDLE_FLAG_INHERIT, 0)) throw std::runtime_error("无法隔离管道句柄");
        Handle nullInput(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr));
        SIZE_T listSize = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &listSize);
        std::vector<BYTE> storage(listSize);
        auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        if (!InitializeProcThreadAttributeList(attributes, 1, 0, &listSize)) throw std::runtime_error("无法初始化句柄白名单");
        HANDLE inherited[] = {writer.value, nullInput.value};
        if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr))
        { DeleteProcThreadAttributeList(attributes); throw std::runtime_error("无法设置句柄白名单"); }
        STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup); startup.lpAttributeList = attributes;
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES; startup.StartupInfo.hStdOutput = writer.value;
        startup.StartupInfo.hStdError = writer.value; startup.StartupInfo.hStdInput = nullInput.value;
        const auto dump = result.report.directory / "crash.dmp";
        std::wstring command = Quote(options.procDump.wstring()) + L" -accepteula -ma -e -n 1 " + std::to_wstring(pid) + L" " + Quote(dump.wstring());
        PROCESS_INFORMATION child{};
        const BOOL started = CreateProcessW(options.procDump.c_str(), command.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, result.report.directory.c_str(), &startup.StartupInfo, &child);
        DeleteProcThreadAttributeList(attributes);
        if (!started) throw std::runtime_error("ProcDump启动失败，请检查权限或安全软件");
        procDumpStarted = true;
        Handle monitor(child.hProcess), thread(child.hThread);
        MonitorLifetime monitorLifetime{monitor.value, reader.value, eventName.c_str()};
        CloseHandle(writer.value); writer.value = nullptr;
        outcome = "MONITOR_STARTING";
        progress("ProcDump启动中，等待确认附加", true);
        bool stopping = false, signalled = false, writing = false, attached = false;
        std::string stopReason;
        auto nextSpaceCheck = GetTickCount64();
        // Once attached, never throw or terminate the debugger; always wait for graceful exit.
        for (;;)
        {
            DrainPipe(reader.value, log);
            const auto wait = WaitForSingleObject(monitor.value, 100);
            if (wait == WAIT_OBJECT_0) break;
            if (!attached && DecodeLog(log).find("Press Ctrl-C to end monitoring") != std::string::npos)
            { attached = true; outcome = "MONITORING"; progress("正在监控游戏；默认捕获一次未处理异常", true); }
            std::error_code error;
            if (!writing && std::filesystem::exists(dump, error))
            { writing = true; outcome = "WRITING_DUMP"; progress("正在保存完整dump，请勿退出或关闭游戏", true); }
            if (!stopping && (stop.load() || GetTickCount64() >= deadline || wait == WAIT_FAILED))
            { stopping = true; stopReason = stop.load() ? "CANCELLED" : "MONITOR_TIMEOUT"; }
            if (!stopping && GetTickCount64() >= nextSpaceCheck)
            {
                nextSpaceCheck = GetTickCount64() + 1000;
                ULARGE_INTEGER available{};
                if (!GetDiskFreeSpaceExW(root.c_str(), &available, nullptr, nullptr) || available.QuadPart < GiB)
                { stopping = true; stopReason = "LOW_DISK_SPACE"; }
            }
            if (stopping && !signalled)
            {
                Handle event(OpenEventW(EVENT_MODIFY_STATE, FALSE, eventName.c_str()));
                if (event.value && SetEvent(event.value))
                { signalled = true; progress("已请求安全停止，等待ProcDump恢复目标并退出；请勿强制关闭", false); }
                else progress("等待ProcDump提供安全停止入口；不会强制结束调试器或游戏", false);
            }
        }
        DrainPipe(reader.value, log);
        if (!GetExitCodeProcess(monitor.value, &exitCode)) exitCode = MAXDWORD;
        if (stopping || stop.load())
        {
            outcome = stopReason.empty() ? "CANCELLED" : stopReason; result.report.cancelled = true;
            result.message = "监控已安全结束；可能未完成的dump仅保留本地，不标记成功"; manifest(); return result;
        }
        if (!std::filesystem::exists(dump))
        {
            outcome = "NO_DUMP"; result.message = "未得到dump：可能正常退出、异常被处理或监控被阻止，请查看procdump.log"; manifest(); return result;
        }
        progress("验证完整dump结构与目标PID", true);
        const auto decoded = DecodeLog(log);
        if ((exitCode != 0 && exitCode != 1) || decoded.find("Dump 1 complete:") == std::string::npos || decoded.find("Dump count reached.") == std::string::npos)
            throw std::runtime_error("未确认ProcDump完整写入一次dump；保留原始文件但不标记成功");
        metadata = ValidateFullDump(dump, pid);
        if (metadata.fileBytes > 64 * GiB) throw std::runtime_error("dump超过64GiB上限，保留原文件不打包");
        result.dumpCaptured = true; outcome = "DUMP_VALIDATED";
        result.message = "已获取完整异常dump，正在收集环境信息"; manifest();
        CollectDumpEnvironment(options.environment, metadata, result.report, stop, progress);
        outcome = "CAPTURED"; result.message = result.report.partial ? "dump已捕获；部分环境信息不可用" : "完整dump与环境信息已捕获";
        manifest();
        PackageDumpEvidence(result.report, {"crash.dmp", "environment.zip", "capture.json", "procdump.log"}, stop, progress);
        result.message = result.report.partial ? "完整dump报告包已生成（部分环境数据缺失）" : "完整dump报告包已生成，请确认隐私后手动发送";
        return result;
    }
    catch (const std::exception& exception)
    {
        failedStage = outcome;
        const bool preflightFailed = outcome == "VERIFYING_PROCDUMP" && !procDumpStarted;
        if (const auto* accessError = dynamic_cast<const ProcessAccessError*>(&exception))
        {
            failedOperation = accessError->operation; processError = accessError->code;
            requestedAccess = accessError->access; pid = accessError->pid;
        }
        outcome = preflightFailed ? "PROCDUMP_PREFLIGHT_FAILED" : result.dumpCaptured ? "POSTPROCESS_FAILED" : "CAPTURE_FAILED";
        result.report.partial = true; result.report.cancelled = stop.load(); result.message = exception.what();
        bool reportWritten = false;
        try { if (procDumpStarted) WriteReportFile(result.report.directory / "procdump.log", DecodeLog(log)); } catch (...) {}
        try { manifest(false); reportWritten = true; } catch (...) { result.message += "；报告写入失败，原始证据目录已保留"; }
        if (reportWritten && !stop.load())
        {
            try
            {
                const auto archive = result.report.directory / (preflightFailed ? "verification.zip" : "capture-failure.zip");
                WriteZip(result.report.directory, archive, {"capture.json"});
                result.report.archive = archive;
                result.message += "；失败诊断ZIP已保存（包内无dump）";
            }
            catch (...) { result.message += "；失败诊断打包失败，请发送capture.json"; }
        }
        return result;
    }
}
}
