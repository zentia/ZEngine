#include "WerCapture.h"
#include "CapturePackage.h"
#include "DiagnosisUtil.h"
#include "ReportWriter.h"
#include "ToolVersion.h"
#include <TlHelp32.h>
#include <sddl.h>
#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <sstream>
#include <thread>

namespace zengine::envcheck::collector
{
using namespace diagnosis;
namespace
{
constexpr std::uint64_t GiB = 1ull << 30;
struct Handle
{
    HANDLE h = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE value) : h(value) {}
    ~Handle() { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); }
    Handle(const Handle&) = delete;
};
struct DirectoryGuard
{
    std::vector<std::unique_ptr<Handle>> handles;
    explicit DirectoryGuard(std::filesystem::path path)
    {
        for (; !path.empty(); path = path == path.root_path() ? std::filesystem::path{} : path.parent_path())
        {
            auto handle = std::make_unique<Handle>(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            FILE_ATTRIBUTE_TAG_INFO info{};
            if (handle->h == INVALID_HANDLE_VALUE || !GetFileInformationByHandleEx(handle->h, FileAttributeTagInfo, &info, sizeof(info)) ||
                !(info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                throw std::runtime_error("转储路径不可用或包含目录链接，请使用本地普通目录");
            handles.push_back(std::move(handle));
        }
    }
};
void PrivateDirectory(const std::filesystem::path& path)
{
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) throw std::runtime_error("无法读取当前用户");
    Handle token(raw); DWORD size = 0; GetTokenInformation(raw, TokenUser, nullptr, 0, &size);
    std::vector<BYTE> user(size);
    if (!GetTokenInformation(raw, TokenUser, user.data(), size, &size)) throw std::runtime_error("无法读取用户SID");
    wchar_t* sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, &sid)) throw std::runtime_error("用户SID无效");
    const std::wstring acl = L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;" + std::wstring(sid) + L")"; LocalFree(sid);
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(acl.c_str(), SDDL_REVISION_1, &sd, nullptr)) throw std::runtime_error("无法建立隐私目录权限");
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), sd, FALSE};
    const BOOL created = CreateDirectoryW(path.c_str(), &attributes); LocalFree(sd);
    if (!created) throw std::runtime_error("无法独占创建转储目录（要求本地支持ACL的磁盘）");
}
std::uint64_t FreeSpace(const std::filesystem::path& root)
{
    ULARGE_INTEGER available{};
    if (!GetDiskFreeSpaceExW(root.c_str(), &available, nullptr, nullptr)) throw std::runtime_error("无法读取转储目录剩余空间");
    return available.QuadPart;
}
std::uint32_t UnixSeconds(UtcTicks ticks) { return static_cast<std::uint32_t>(ticks / 10000000ull - 11644473600ull); }
bool ModuleMatches(const DumpMetadata& metadata, const std::filesystem::path& game)
{
    for (const auto& name : metadata.modulePaths)
        if (_wcsicmp(std::filesystem::path(name).lexically_normal().c_str(), game.c_str()) == 0) return true;
    return false;
}
void Observe(const std::filesystem::path& game, std::map<DWORD, std::uint32_t>& observed)
{
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (snapshot.h == INVALID_HANDLE_VALUE) return;
    DWORD session = 0; if (!ProcessIdToSessionId(GetCurrentProcessId(), &session)) return;
    PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
    for (BOOL more = Process32FirstW(snapshot.h, &entry); more; more = Process32NextW(snapshot.h, &entry))
    {
        if (_wcsicmp(entry.szExeFile, game.filename().c_str())) continue;
        DWORD other = 0; if (!ProcessIdToSessionId(entry.th32ProcessID, &other) || other != session) continue;
        Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID));
        if (!process.h) continue;
        std::array<wchar_t, 32768> path{}; DWORD length = static_cast<DWORD>(path.size());
        FILETIME creation{}, exit{}, kernel{}, user{};
        if (!QueryFullProcessImageNameW(process.h, 0, path.data(), &length) || _wcsicmp(path.data(), game.c_str()) ||
            !GetProcessTimes(process.h, &creation, &exit, &kernel, &user)) continue;
        const auto ticks = (static_cast<ULONGLONG>(creation.dwHighDateTime) << 32) | creation.dwLowDateTime;
        observed[entry.th32ProcessID] = UnixSeconds(ticks);
    }
}
void CopyLocked(const EvidenceFile& source, const std::filesystem::path& path, const std::atomic_bool& stop)
{
    EvidenceFile dest(path, true); std::array<BYTE, 65536> data{};
    const auto length = source.Size();
    for (std::uint64_t offset = 0; offset < length;)
    {
        if (stop.load()) throw std::runtime_error("复制已取消；原始dump保留在系统转储目录");
        const auto bytes = static_cast<size_t>(std::min<std::uint64_t>(length - offset, data.size()));
        if (source.Read(offset, data.data(), bytes) != bytes || dest.Write(offset, data.data(), bytes) != bytes) throw std::runtime_error("复制完整dump失败");
        offset += bytes;
    }
    if (!FlushFileBuffers(dest.Get())) throw std::runtime_error("dump写入刷新失败");
}
}
CaptureResult CaptureWer(const WerOptions& options, const std::atomic_bool& stop, const std::function<void(const std::string&, bool)>& progress)
{
    if (!options.memoryConsent || (!options.importDump.empty() && !options.importConfirmed)) throw std::runtime_error("必须先确认完整内存隐私及导入来源");
    if (!options.waitSeconds || options.waitSeconds > 86400) throw std::runtime_error("等待时间范围为1至86400秒");
    const auto name = ValidateWerExecutable(Utf8ToWide(options.environment.profile.gameExecutable));
    const auto game = std::filesystem::absolute(options.gameExecutable).lexically_normal();
    if (!std::filesystem::is_regular_file(game) || _wcsicmp(game.filename().c_str(), name.c_str())) throw std::runtime_error("请选择当前项目对应的游戏exe");
    const auto root = std::filesystem::absolute(options.environment.outputDirectory.empty() ? DefaultOutputDirectory() : options.environment.outputDirectory).lexically_normal();
    std::filesystem::create_directories(root); DirectoryGuard rootGuard(root);
    const auto token = NewWerToken(); const auto consent = CurrentUtcTime();
    CaptureResult result; result.report.directory = root / (L"crash-capture-wer-" + token);
    PrivateDirectory(result.report.directory); DirectoryGuard sessionGuard(result.report.directory);
    const auto incoming = result.report.directory / L"system-dumps";
    const bool imported = !options.importDump.empty();
    bool restoreNeeded = false, enabled = false, restoreAttempted = false, configurationAttempted = false;
    DWORD enableCode = ERROR_SUCCESS, restoreCode = ERROR_SUCCESS;
    DumpMetadata metadata; std::string state = "PREPARING", identity = "NOT_VERIFIED", fileIssue;
    std::string sourceName;
    // Continuous collection. The registry stays configured for the whole
    // session, so a second crash is not silently lost the way it used to be:
    // the old code restored the settings as soon as the first dump was copied,
    // which made every later crash invisible. Each verified dump gets its own
    // copy; the first keeps the historical name "crash.dmp" so existing readers
    // and tests are unaffected.
    struct CapturedDump { std::wstring name; DumpMetadata metadata; std::string sourceName; };
    std::vector<CapturedDump> dumps;
    constexpr size_t MaxDumps = 8;
    auto manifest = [&] {
        std::ostringstream json;
        json << "{\"schemaVersion\":2,\"toolVersion\":" << JsonString(ToolVersion) << ",\"captureBackend\":" << JsonString(imported ? "dump_import" : "wer")
            << ",\"status\":" << JsonString(state) << ",\"targetName\":" << JsonString(WideToUtf8(game.filename().wstring()))
            << ",\"consentUtc\":" << JsonString(FormatUtcIso8601(consent)) << ",\"fullMemoryConsent\":true,\"automaticUpload\":false,\"memoryRedacted\":false"
            << ",\"procDumpStarted\":false,\"configurationAttempted\":" << (configurationAttempted ? "true" : "false")
            << ",\"configurationEnabled\":" << (enabled ? "true" : "false") << ",\"configurationWin32\":" << enableCode
            << ",\"registryScope\":" << JsonString(imported ? "NOT_APPLICABLE" : "HKLM_64_PER_EXE_BASENAME")
            << ",\"dumpType\":" << (imported ? "null" : "2") << ",\"dumpCount\":" << (imported ? "null" : "2")
            << ",\"restoreAttempted\":" << (restoreAttempted ? "true" : "false") << ",\"restoreWin32\":" << restoreCode
            << ",\"restorePending\":" << (restoreNeeded ? "true" : "false") << ",\"identity\":" << JsonString(identity)
            // The single-dump fields below keep reporting the first dump so
            // existing consumers do not break; `dumps` lists every one.
            << ",\"capturedDumpCount\":" << dumps.size() << ",\"dumps\":[";
        for (size_t i = 0; i < dumps.size(); ++i)
        {
            if (i) json << ',';
            json << "{\"index\":" << (i + 1) << ",\"fileName\":" << JsonString(WideToUtf8(dumps[i].name))
                 << ",\"sourceFileName\":" << JsonString(dumps[i].sourceName)
                 << ",\"pid\":" << dumps[i].metadata.processId << ",\"processCreatedUnix\":" << dumps[i].metadata.processCreatedUnix
                 << ",\"dumpTimestampUnix\":" << dumps[i].metadata.timestamp << ",\"exceptionCode\":" << dumps[i].metadata.exceptionCode
                 << ",\"dumpBytes\":" << dumps[i].metadata.fileBytes << '}';
        }
        json << ']'
            << ",\"sourceFileName\":" << JsonString(sourceName) << ",\"pid\":" << metadata.processId << ",\"processCreatedUnix\":" << metadata.processCreatedUnix
            << ",\"dumpTimestampUnix\":" << metadata.timestamp << ",\"exceptionCode\":" << metadata.exceptionCode << ",\"dumpBytes\":" << metadata.fileBytes
            << ",\"dumpValidated\":" << (result.dumpCaptured ? "true" : "false") << ",\"lastFileIssue\":" << JsonString(fileIssue)
            << ",\"message\":" << JsonString(result.message) << '}';
        WriteReportFile(result.report.directory / "capture.json", json.str());
    };
    auto restore = [&] {
        if (!restoreNeeded || restoreAttempted) return;
        restoreAttempted = true;
        restoreCode = RequestWerRestoreSession(name, token, options.owner);
        restoreNeeded = restoreCode != ERROR_SUCCESS;
        if (restoreNeeded) { result.report.partial = true; result.message += "；系统捕获配置恢复未完成，请点击恢复按钮（Win32=" + std::to_string(restoreCode) + "）"; }
    };
    try
    {
        manifest();
        if (FreeSpace(root) < 4 * GiB) throw std::runtime_error("至少需要4GiB可用空间；完整dump可能需要更多空间");
        std::map<DWORD, std::uint32_t> observed;
        if (!imported)
        {
            if (HasWerBackup(WideToUtf8(name))) throw std::runtime_error("当前exe存在未恢复的WER备份，请先点击恢复原配置");
            PrivateDirectory(incoming);
            Observe(game, observed);
            if (stop.load()) throw std::runtime_error("已取消，未修改系统转储设置");
            progress("请确认UAC：仅启用当前exe的系统崩溃转储", false);
            configurationAttempted = true;
            enableCode = RequestWerEnable(name, incoming, token, options.owner);
            restoreNeeded = enableCode != ERROR_CANCELLED && enableCode != ERROR_ALREADY_EXISTS;
            if (enableCode) throw std::runtime_error("WER启用失败或UAC被取消（Win32=" + std::to_string(enableCode) + "）");
            enabled = true; state = "WAITING_FOR_SYSTEM_DUMP"; manifest();
            progress("系统捕获已启用，请正常启动游戏并复现崩溃；最多等待1小时", true);
        }
        const auto deadline = GetTickCount64() + options.waitSeconds * 1000ull;
        std::vector<std::wstring> handled;
        std::uint64_t committed = 0;   // bytes this session already copied in
        bool spaceOut = false;
        while (!stop.load() && GetTickCount64() < deadline && dumps.size() < MaxDumps)
        {
            if (!imported) Observe(game, observed);
            std::vector<std::filesystem::path> files;
            if (imported) files.push_back(std::filesystem::absolute(options.importDump).lexically_normal());
            else for (const auto& entry : std::filesystem::directory_iterator(incoming))
            {
                if (entry.path().extension() == L".dmp") files.push_back(entry.path());
                if (files.size() > 32) throw std::runtime_error("系统转储目录文件过多，请停止后人工确认");
            }
            for (const auto& file : files)
            {
                // Reject sources already copied *before* opening them. The wait
                // can last an hour at 300 ms per pass, and re-parsing a multi-GiB
                // dump each time just to skip it would burn a core the whole
                // time. WER names files "<exe>.<pid>.dmp", so the name is unique
                // per crash; a file still being written is not in this list yet.
                if (std::find(handled.begin(), handled.end(), file.filename().wstring()) != handled.end()) continue;
                std::unique_ptr<EvidenceFile> locked;
                std::unique_ptr<DirectoryGuard> parents;
                try
                {
                    parents = std::make_unique<DirectoryGuard>(file.parent_path());
                    locked = std::make_unique<EvidenceFile>(file);
                    if (locked->Size() > 64 * GiB) throw std::runtime_error("dump超过64GiB上限");
                    metadata = ValidateFullDump(file, 0);
                    if (!ModuleMatches(metadata, game)) throw std::runtime_error("dump模块列表与所选游戏完整路径不匹配");
                }
                catch (const std::exception& error)
                {
                    fileIssue = error.what();
                    if (imported) throw;
                    continue;
                }
                sourceName = WideToUtf8(file.filename().wstring());
                if (!imported)
                {
                    const auto match = observed.find(metadata.processId);
                    if (metadata.timestamp + 1 < UnixSeconds(consent) || metadata.timestamp > UnixSeconds(CurrentUtcTime()) + 5 ||
                        match == observed.end() || !metadata.processCreatedUnix || match->second != metadata.processCreatedUnix)
                        throw std::runtime_error("发现dump但无法可靠关联本次目标PID/启动时间，已保留system-dumps；请手动选择该文件确认后导入");
                    identity = "OBSERVED_PID_CREATION_TIME_AND_MODULE_PATH";
                }
                else identity = "USER_CONFIRMED_IMPORT_AND_MODULE_PATH";
                // First dump keeps "crash.dmp"; later ones are numbered. Space is
                // budgeted against what this session has already committed, not
                // just the current dump, or a second crash would be copied onto
                // a disk that is already full.
                const std::wstring copyName = dumps.empty() ? L"crash.dmp"
                    : L"crash-" + std::to_wstring(dumps.size() + 1) + L".dmp";
                if (FreeSpace(root) < metadata.fileBytes * 2 + GiB + committed) { spaceOut = true; break; }
                progress("已确认完整dump，复制到本次报告目录；不删除原文件", true);
                CopyLocked(*locked, result.report.directory / copyName, stop);
                metadata = ValidateFullDump(result.report.directory / copyName, metadata.processId);
                handled.push_back(file.filename().wstring());
                dumps.push_back({copyName, metadata, sourceName});
                committed += metadata.fileBytes;
                result.dumpCaptured = true;
                progress(dumps.size() == 1
                    ? "已收集第1份完整dump；配置仍生效，可继续复现下一次崩溃，点击“安全停止捕获”结束并打包"
                    : "已收集第" + std::to_string(dumps.size()) + "份完整dump；点击“安全停止捕获”结束并打包", true);
            }
            if (imported || spaceOut) break;
            if (FreeSpace(root) < GiB + committed) { spaceOut = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
        if (dumps.empty())
        {
            if (stop.load()) { result.report.cancelled = true; throw std::runtime_error("已停止等待/后处理；不会终止WER或游戏，晚到dump仍保留本地"); }
            if (spaceOut) throw std::runtime_error("剩余空间不足，未获得可验证dump；不终止系统写入");
            throw std::runtime_error("等待超时，未获得可验证dump；自定义崩溃处理或策略可能影响系统转储");
        }
        // Stopping or timing out with dumps in hand is a normal finish, not a
        // cancellation: package what was collected. Only an empty session fails.
        // The registry is restored here, once, after the last dump -- which is
        // exactly what made later crashes collectable.
        metadata = dumps.front().metadata; sourceName = dumps.front().sourceName;
        state = "DUMP_VALIDATED";
        result.message = dumps.size() > 1 ? "已收集 " + std::to_string(dumps.size()) + " 份完整dump" : "完整dump已验证";
        if (dumps.size() >= MaxDumps) result.message += "（已达单次会话收集上限，结束等待）";
        else if (stop.load()) result.message += "（已结束等待）";
        else if (spaceOut) result.message += "（剩余空间不足，提前结束等待）";
        if (restoreNeeded) progress("准备恢复WER原配置，请确认UAC；不会中断系统写入", false);
        restore(); manifest();
        // The caller's stop flag has done its job -- it ended the wait. Passing
        // it on would make post-processing cancel itself instantly and package
        // none of the dumps: whoever clicked "stop" still wants the evidence.
        static std::atomic_bool noStop{false};
        EvidenceFile stable(result.report.directory / dumps.front().name);
        CollectDumpEnvironment(options.environment, metadata, result.report, noStop, progress);
        state = "CAPTURED"; manifest();
        std::vector<std::string> packaged;
        for (const auto& dump : dumps) packaged.push_back(WideToUtf8(dump.name));
        packaged.push_back("environment.zip"); packaged.push_back("capture.json");
        PackageDumpEvidence(result.report, packaged, noStop, progress);
        result.message = restoreNeeded ? "ZIP已生成，但WER配置待恢复，请点击恢复按钮"
            : dumps.size() > 1 ? "多份完整dump与环境ZIP已生成，请确认隐私后手动发送"
            : "完整dump与环境ZIP已生成，请确认隐私后手动发送";
        return result;
    }
    catch (const std::exception& error)
    {
        result.report.partial = true; result.report.cancelled = stop.load();
        result.message = error.what(); state = result.dumpCaptured ? "POSTPROCESS_FAILED" : result.report.cancelled ? "CANCELLED" : "NO_VALID_DUMP";
        try { progress("结束系统捕获，恢复原配置（可能需要UAC）", false); } catch (...) {}
        restore();
        try
        {
            manifest();
            const auto archive = result.report.directory / "capture-failure.zip";
            WriteZip(result.report.directory, archive, {"capture.json"}); result.report.archive = archive;
            result.message += "；失败诊断ZIP已保存（包内无dump）";
        }
        catch (...) { result.message += "；报告写入/打包失败，请检查本地目录"; }
        return result;
    }
}
}
