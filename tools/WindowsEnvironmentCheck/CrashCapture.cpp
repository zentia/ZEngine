#include "CrashCapture.h"
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
void VerifyMicrosoft(const std::filesystem::path& file, HANDLE handle)
{
    WINTRUST_FILE_INFO info{}; info.cbStruct = sizeof(info); info.pcwszFilePath = file.c_str(); info.hFile = handle;
    WINTRUST_DATA trust{}; trust.cbStruct = sizeof(trust); trust.dwUIChoice = WTD_UI_NONE;
    trust.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN; trust.dwUnionChoice = WTD_CHOICE_FILE; trust.pFile = &info;
    trust.dwStateAction = WTD_STATEACTION_VERIFY; trust.dwProvFlags = WTD_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT;
    GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    LONG status = WinVerifyTrust(nullptr, &policy, &trust);
    bool microsoft = false;
    if (status == ERROR_SUCCESS)
    {
        auto* provider = WTHelperProvDataFromStateData(trust.hWVTStateData);
        auto* signer = provider ? WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0) : nullptr;
        if (signer && signer->csCertChain)
        {
            wchar_t name[256]{};
            CertGetNameStringW(signer->pasCertChain[0].pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, name, 256);
            microsoft = _wcsicmp(name, L"Microsoft Corporation") == 0;
        }
    }
    trust.dwStateAction = WTD_STATEACTION_CLOSE; WinVerifyTrust(nullptr, &policy, &trust);
    if (status != ERROR_SUCCESS || !microsoft) throw std::runtime_error("ProcDump微软签名验证失败（也可能证书联网校验不可用），已拒绝运行");
}
std::uint64_t FreeBytes(const std::filesystem::path& path)
{
    ULARGE_INTEGER available{};
    if (!GetDiskFreeSpaceExW(path.c_str(), &available, nullptr, nullptr)) throw std::runtime_error("无法查询剩余磁盘空间");
    return available.QuadPart;
}
std::filesystem::path ProcessPath(HANDLE process)
{
    std::vector<wchar_t> buffer(32768); DWORD length = static_cast<DWORD>(buffer.size());
    if (!QueryFullProcessImageNameW(process, 0, buffer.data(), &length)) throw std::runtime_error("无法核验目标程序路径，拒绝附加");
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
        if (!process.value) throw std::runtime_error("目标程序访问被拒绝，请检查权限或反作弊兼容性；不会绕过");
        if (WaitForSingleObject(process.value, 0) == WAIT_OBJECT_0) continue;
        auto path = ProcessPath(process.value);
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
std::string ValidateProcDump(const std::filesystem::path& file)
{
    EvidenceFile locked(file);
    VerifyMicrosoft(file, locked.Get());
    const auto original = OriginalName(file);
    if (_wcsicmp(original.c_str(), L"procdump") != 0 && _wcsicmp(original.c_str(), L"procdump.exe") != 0 && _wcsicmp(original.c_str(), L"procdump64.exe") != 0)
        throw std::runtime_error("所选文件不是微软ProcDump");
    DWORD binary = 0;
    if (!GetBinaryTypeW(file.c_str(), &binary) || binary != SCS_64BIT_BINARY)
        throw std::runtime_error("请选择64位ProcDump程序");
    return Version(file);
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
    EvidenceFile procDumpLock(options.procDump);
    progress("校验微软ProcDump签名", true);
    const auto procVersion = ValidateProcDump(options.procDump);
    if (stop.load()) return {{}, false, "捕获已取消"};
    const auto root = std::filesystem::absolute(options.environment.outputDirectory.empty() ? DefaultOutputDirectory() : options.environment.outputDirectory);
    CaptureResult result; result.report.directory = CreateSession(root);
    std::string outcome = "WAITING", log;
    DWORD pid = 0, exitCode = STILL_ACTIVE;
    std::string exeVersion, exeHash, procHash;
    DumpMetadata metadata{};
    const auto consentTime = CurrentUtcTime();
    auto manifest = [&] {
        std::ostringstream json;
        json << "{\"schemaVersion\":1,\"toolVersion\":" << JsonString(ToolVersion) << ",\"status\":" << JsonString(outcome)
            << ",\"consentUtc\":" << JsonString(FormatUtcIso8601(consentTime)) << ",\"fullMemoryConsent\":true,\"sysinternalsLicenseAccepted\":true"
            << ",\"targetName\":" << JsonString(WideToUtf8(game.filename().wstring())) << ",\"pid\":" << pid
            << ",\"exeFileVersion\":" << JsonString(exeVersion) << ",\"exeSha256\":" << JsonString(exeHash)
            << ",\"procDumpVersion\":" << JsonString(procVersion) << ",\"procDumpSha256\":" << JsonString(procHash)
            << ",\"procDumpExitCode\":" << exitCode << ",\"exceptionCode\":" << metadata.exceptionCode
            << ",\"dumpTimestampUnix\":" << metadata.timestamp << ",\"memoryBytes\":" << metadata.memoryBytes
            << ",\"dumpBytes\":" << metadata.fileBytes << ",\"dumpValidation\":" << JsonString(result.dumpCaptured ? "FULL_MEMORY_EXCEPTION_PID_AND_FILE_RANGES" : "NO_VALID_DUMP")
            << ",\"automaticUpload\":false,\"memoryRedacted\":false,\"message\":" << JsonString(result.message) << '}';
        WriteReportFile(result.report.directory / "capture.json", json.str());
        WriteReportFile(result.report.directory / "procdump.log", DecodeLog(log));
    };
    try
    {
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
        Handle process(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | SYNCHRONIZE, FALSE, pid));
        if (!process.value || _wcsicmp(ProcessPath(process.value).c_str(), game.c_str()) != 0)
            throw std::runtime_error("目标进程已变化或访问被拒绝，不会附加");
        BOOL debugged = FALSE, wow64 = FALSE;
        if (!CheckRemoteDebuggerPresent(process.value, &debugged) || debugged) throw std::runtime_error("目标已有调试器或无法确认调试状态，不会重复附加");
        if (!IsWow64Process(process.value, &wow64) || wow64) throw std::runtime_error("此模式仅支持64位游戏进程");
        const auto eventName = L"ProcDump-" + std::to_wstring(pid);
        Handle existing(OpenEventW(SYNCHRONIZE, FALSE, eventName.c_str()));
        if (existing.value || GetLastError() != ERROR_FILE_NOT_FOUND) throw std::runtime_error("目标已有ProcDump监控或无法确认其独占状态");
        const auto mutexName = L"Local\\ZEnvironmentCollector-Capture-" + std::to_wstring(pid);
        Handle mutex(CreateMutexW(nullptr, TRUE, mutexName.c_str()));
        if (!mutex.value || GetLastError() == ERROR_ALREADY_EXISTS) throw std::runtime_error("另一个采集器正在监控该进程");
        const auto estimated = CommittedBytes(process.value);
        if (FreeBytes(root) < std::max<std::uint64_t>(4 * GiB, estimated * 2 + 2 * GiB))
            throw std::runtime_error("磁盘空间不足：需同时保留完整dump与压缩包，至少预计内存的两倍加2GiB");
        exeVersion = Version(game); exeHash = Sha256File(game, &stop); procHash = Sha256File(options.procDump, &stop);
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
        Options environment = options.environment; environment.outputDirectory = result.report.directory;
        environment.incidentKind = IncidentKind::Crash;
        environment.incidentTime = (static_cast<UtcTicks>(metadata.timestamp) + 11644473600ull) * 10000000ull;
        auto collected = Collect(environment, stop, progress);
        if (stop.load()) throw std::runtime_error("后处理已取消，完整dump保留本地");
        std::filesystem::rename(collected.archive, result.report.directory / "environment.zip");
        result.report.partial = collected.partial;
        outcome = "CAPTURED"; result.message = collected.partial ? "dump已捕获；部分环境信息不可用" : "完整dump与环境信息已捕获";
        manifest();
        std::vector<std::string> files{"crash.dmp", "environment.zip", "capture.json", "procdump.log"};
        progress("计算证据SHA256（大dump可能耗时）", true);
        std::ostringstream hashes; hashes << "{\"schemaVersion\":1,\"files\":[";
        for (size_t i = 0; i < files.size(); ++i)
        {
            if (i) hashes << ',';
            hashes << "{\"name\":" << JsonString(files[i]) << ",\"bytes\":" << std::filesystem::file_size(result.report.directory / files[i])
                << ",\"sha256\":" << JsonString(Sha256File(result.report.directory / files[i], &stop)) << '}';
        }
        hashes << "]}"; WriteReportFile(result.report.directory / "checksums.json", hashes.str()); files.push_back("checksums.json");
        if (FreeBytes(root) < metadata.fileBytes + GiB) throw std::runtime_error("压缩空间不足，原始dump及环境报告已保留");
        progress("流式压缩并回读校验ZIP64，不自动上传", true);
        const auto archive = std::filesystem::path(result.report.directory.wstring() + L".zip");
        WriteZip64(result.report.directory, archive, files, stop);
        result.report.archive = archive;
        result.message = result.report.partial ? "完整dump报告包已生成（部分环境数据缺失）" : "完整dump报告包已生成，请确认隐私后手动发送";
        return result;
    }
    catch (const std::exception& exception)
    {
        outcome = result.dumpCaptured ? "POSTPROCESS_FAILED" : "CAPTURE_FAILED";
        result.report.partial = true; result.report.cancelled = stop.load(); result.message = exception.what();
        try { WriteReportFile(result.report.directory / "procdump.log", DecodeLog(log)); } catch (...) {}
        try { manifest(); } catch (...) { result.message += "；报告写入失败，原始证据目录已保留"; }
        return result;
    }
}
}
