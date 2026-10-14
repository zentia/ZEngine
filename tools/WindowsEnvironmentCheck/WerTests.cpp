#include "WerCapture.h"
#include "DiagnosisUtil.h"
#include <DbgHelp.h>
#include <sddl.h>
#include <fstream>
#include <iostream>
#include <future>

using namespace zengine::envcheck::collector;
using namespace zengine::envcheck::diagnosis;
namespace
{
void Check(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }
constexpr wchar_t Dumps[] = L"SOFTWARE\\Microsoft\\Windows\\Windows Error Reporting\\LocalDumps\\";
constexpr wchar_t Backups[] = L"SOFTWARE\\ZEngine\\EnvironmentCollector\\WerBackupsV1\\";
void Set(HKEY key, const wchar_t* name, DWORD type, const void* value, DWORD bytes)
{ Check(RegSetValueExW(key, name, 0, type, static_cast<const BYTE*>(value), bytes) == 0, "write fixture"); }
DWORD ReadNumber(HKEY key, const wchar_t* name)
{
    DWORD value = 0, size = sizeof(value), type = 0;
    Check(RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(&value), &size) == 0 && type == REG_DWORD, "read registry number");
    return value;
}
bool Exists(const std::wstring& path)
{
    HKEY key = nullptr; const auto code = RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &key);
    if (key) RegCloseKey(key); return code == 0;
}
void Registry(const std::filesystem::path& root)
{
    const auto name = L"collector-test-" + NewWerToken() + L".exe";
    const auto path = std::wstring(Dumps) + name, backup = std::wstring(Backups) + name;
    auto token = NewWerToken();
    Check(!Exists(path) && !Exists(backup), "unique registry fixture");
    struct Cleanup
    {
        std::wstring path, backup;
        ~Cleanup() { RegDeleteKeyExW(HKEY_CURRENT_USER, path.c_str(), KEY_WOW64_64KEY, 0); RegDeleteKeyExW(HKEY_CURRENT_USER, backup.c_str(), KEY_WOW64_64KEY, 0); }
    } cleanup{path, backup};
    Check(ChangeWerRegistry(HKEY_CURRENT_USER, name, root.wstring(), token, false) == 0, "enable absent settings");
    HKEY key = nullptr;
    Check(RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_ALL_ACCESS | KEY_WOW64_64KEY, &key) == 0, "open test key");
    Check(ReadNumber(key, L"DumpType") == 2 && ReadNumber(key, L"DumpCount") == 2, "full dump type and bounded count");
    DWORD type = 0, size = 0;
    Check(RegQueryValueExW(key, L"DumpFolder", nullptr, &type, nullptr, &size) == 0 && type == REG_EXPAND_SZ, "folder type");
    RegCloseKey(key);
    Check(ChangeWerRegistry(HKEY_CURRENT_USER, name, root.wstring(), token, false) == ERROR_ALREADY_EXISTS, "pending backup prevents replacement");
    Check(ChangeWerRegistry(HKEY_CURRENT_USER, name, L"", NewWerToken(), true) == ERROR_REVISION_MISMATCH, "wrong session cannot restore");
    Check(ChangeWerRegistry(HKEY_CURRENT_USER, name, L"", token, true) == 0 && !Exists(path) && !Exists(backup), "restore absence precisely");
    Check(RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS | KEY_WOW64_64KEY, nullptr, &key, nullptr) == 0, "create existing fixture");
    const BYTE odd[]{0x31,0,0x42}; const DWORD oldType = 1, external = 77;
    Set(key, L"DumpFolder", REG_BINARY, odd, sizeof(odd)); Set(key, L"DumpType", REG_DWORD, &oldType, sizeof(oldType));
    Set(key, L"Other", REG_DWORD, &external, sizeof(external)); RegCloseKey(key);
    Check(ChangeWerRegistry(HKEY_CURRENT_USER, name, root.wstring(), token, false) == 0, "enable existing typed values");
    Check(RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_ALL_ACCESS | KEY_WOW64_64KEY, &key) == 0, "open changed fixture");
    Set(key, L"DumpCount", REG_DWORD, &external, sizeof(external));
    Check(ChangeWerRegistry(HKEY_CURRENT_USER, name, L"", token, true) == ERROR_REVISION_MISMATCH, "external change blocks all restoration");
    Check(ReadNumber(key, L"DumpType") == 2 && Exists(backup), "conflict retains settings and backup");
    DWORD expected = 2; Set(key, L"DumpCount", REG_DWORD, &expected, sizeof(expected));
    Check(ChangeWerRegistry(HKEY_CURRENT_USER, name, L"", token, true) == 0, "retry restoration");
    BYTE bytes[8]{}; size = sizeof(bytes);
    Check(RegQueryValueExW(key, L"DumpFolder", nullptr, &type, bytes, &size) == 0 && type == REG_BINARY && size == sizeof(odd) && !memcmp(bytes, odd, size), "byte-exact original type restoration");
    Check(ReadNumber(key, L"DumpType") == 1 && ReadNumber(key, L"Other") == external, "unrelated settings unchanged");
    Check(RegQueryValueExW(key, L"DumpCount", nullptr, nullptr, nullptr, &size) == ERROR_FILE_NOT_FOUND, "missing original value restored");
    RegCloseKey(key);
    Check(RegCreateKeyExW(HKEY_CURRENT_USER, backup.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS | KEY_WOW64_64KEY, nullptr, &key, nullptr) == 0, "create interrupted empty backup"); RegCloseKey(key);
    Check(ChangeWerRegistry(HKEY_CURRENT_USER, name, L"", NewWerToken(), true) == 0 && !Exists(backup) && Exists(path), "interruption before token is recoverable without target changes");
    bool invalid = false; try { ValidateWerExecutable(L"..\\other.exe"); } catch (...) { invalid = true; }
    Check(invalid, "reject path as registry target");
}
void Dump(const std::filesystem::path& path)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
    Check(file != INVALID_HANDLE_VALUE, "create fixture dump");
    CONTEXT context{}; RtlCaptureContext(&context);
    EXCEPTION_RECORD record{}; record.ExceptionCode = 0xe0424242; record.ExceptionAddress = reinterpret_cast<void*>(&Dump);
    EXCEPTION_POINTERS pointers{&record, &context};
    MINIDUMP_EXCEPTION_INFORMATION exception{GetCurrentThreadId(), &pointers, FALSE};
    const BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
        static_cast<MINIDUMP_TYPE>(MiniDumpWithFullMemory | MiniDumpWithProcessThreadData), &exception, nullptr, nullptr);
    CloseHandle(file); Check(ok, "write self-contained synthetic test dump");
    const auto metadata = ValidateFullDump(path, GetCurrentProcessId());
    Check(metadata.processId == GetCurrentProcessId() && !metadata.modulePaths.empty() && metadata.processCreatedUnix, "dump module paths and process creation metadata");
}
int Integration(const std::filesystem::path& root, bool cancel)
{
    const auto game = ExecutableDirectory() / L"EnvironmentCaptureTestTarget.exe";
    std::atomic_bool stop{false};
    WerOptions options; options.environment.profile.id = "wer-fixture";
    options.environment.profile.gameExecutable = "EnvironmentCaptureTestTarget.exe"; options.environment.profile.lookbackHours = 1;
    options.environment.outputDirectory = root; options.gameExecutable = game; options.memoryConsent = true; options.waitSeconds = 90;
    const auto eventName = L"Local\\WerFixture-" + NewWerToken();
    const auto eventName2 = L"Local\\WerFixture-" + NewWerToken();
    HANDLE trigger = CreateEventW(nullptr, TRUE, FALSE, eventName.c_str()); Check(trigger != nullptr, "fixture event");
    HANDLE trigger2 = CreateEventW(nullptr, TRUE, FALSE, eventName2.c_str()); Check(trigger2 != nullptr, "second fixture event");
    PROCESS_INFORMATION child{}, child2{}; std::future<void> signal, signal2; bool launched2 = false;
    auto launch = [&](const std::wstring& event, HANDLE handle, PROCESS_INFORMATION& process) {
        std::wstring cmd = L"\"" + game.wstring() + L"\" " + event + L" wer-crash";
        STARTUPINFOW startup{sizeof(startup)};
        Check(CreateProcessW(game.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != FALSE, "launch synthetic WER target");
        return std::async(std::launch::async, [handle] { Sleep(2500); SetEvent(handle); });
    };
    auto result = CaptureWer(options, stop, [&](const std::string& text, bool) {
        std::cout << text << std::endl;
        // Only the "enabled" message should trigger the first launch. An early
        // return on every other message would also swallow the "collected #1"
        // progress, and the second crash would never be started.
        if (text.find("系统捕获已启用") != std::string::npos)
        {
            if (cancel) { stop = true; return; }
            if (!signal.valid()) signal = launch(eventName, trigger, child);
            return;
        }
        if (cancel) return;
        // The whole point of continuous collection: a second crash, after the
        // first dump was already copied, must still be captured -- with no
        // re-enabling and no second UAC prompt.
        if (text.find("已收集第1份") != std::string::npos && !launched2)
        { launched2 = true; signal2 = launch(eventName2, trigger2, child2); }
        if (text.find("已收集第2份") != std::string::npos) stop = true;
    });
    if (signal.valid()) signal.get();
    if (signal2.valid()) signal2.get();
    if (child.hProcess) { WaitForSingleObject(child.hProcess, 45000); CloseHandle(child.hProcess); CloseHandle(child.hThread); }
    if (child2.hProcess) { WaitForSingleObject(child2.hProcess, 45000); CloseHandle(child2.hProcess); CloseHandle(child2.hThread); }
    CloseHandle(trigger); CloseHandle(trigger2);
    std::cout << result.message << '\n' << result.report.directory << '\n';
    Check(!HasWerBackup(options.environment.profile.gameExecutable), "real fixture config restored");
    if (cancel) Check(result.report.cancelled && !result.dumpCaptured, "cancel does not capture or kill processes");
    else
    {
        Check(result.dumpCaptured && result.report.archive.filename() == L"capture.zip", "actual Windows WER capture and package");
        Check(std::filesystem::exists(result.report.directory / "crash.dmp") && std::filesystem::exists(result.report.directory / "crash-2.dmp"),
            "second crash collected as its own dump");
        std::ifstream report(result.report.directory / "capture.json");
        const std::string json((std::istreambuf_iterator<char>(report)), std::istreambuf_iterator<char>()); report.close();
        Check(json.find("\"capturedDumpCount\":2") != std::string::npos, "manifest records both dumps");
    }
    return 0;
}
}
int wmain(int argc, wchar_t** argv)
{
    auto root = std::filesystem::temp_directory_path() / (L"wer-tests-" + NewWerToken());
    bool preserve = false;
    try
    {
        if (argc == 4 && std::wstring(argv[1]) == L"--consent-system-test")
        {
            preserve = true;
            return Integration(std::filesystem::absolute(argv[3]), std::wstring(argv[2]) == L"cancel");
        }
        Check(std::filesystem::create_directory(root), "unique fixture folder");
        Registry(root);
        auto dump = root / "test.dmp"; Dump(dump);
        if (argc == 3 && std::wstring(argv[1]) == L"--import-test")
        {
            preserve = true;
            WerOptions options; options.environment.profile.id = "import-test";
            options.environment.profile.gameExecutable = "EnvironmentWerTests.exe"; options.environment.profile.lookbackHours = 1;
            options.environment.outputDirectory = argv[2]; options.gameExecutable = ExecutableDirectory() / "EnvironmentWerTests.exe";
            options.importDump = dump; options.memoryConsent = true; options.importConfirmed = true;
            std::atomic_bool stop{false};
            auto result = CaptureWer(options, stop, [](const std::string& text, bool) { std::cout << text << std::endl; });
            Check(result.dumpCaptured && result.report.archive.filename() == "capture.zip", "existing dump import package");
            Check(std::filesystem::exists(dump), "original dump retained");
            std::cout << result.report.archive << std::endl;
            std::filesystem::remove_all(root); return 0;
        }
        WerOptions options; std::atomic_bool stop{false}; bool rejected = false;
        try { CaptureWer(options, stop, [](const std::string&, bool) {}); } catch (...) { rejected = true; }
        Check(rejected, "consent before system changes");
        options.memoryConsent = true; options.environment.profile.gameExecutable = "EnvironmentWerTests.exe";
        options.gameExecutable = ExecutableDirectory() / "EnvironmentWerTests.exe"; options.environment.outputDirectory = root;
        stop = true;
        auto cancelled = CaptureWer(options, stop, [](const std::string&, bool) {});
        Check(cancelled.report.cancelled && !cancelled.dumpCaptured, "cancelled before enable produces diagnostic report");
        std::ifstream report(cancelled.report.directory / "capture.json");
        const std::string json((std::istreambuf_iterator<char>(report)), std::istreambuf_iterator<char>()); report.close();
        Check(json.find("\"configurationAttempted\":false") != std::string::npos && json.find("\"restoreAttempted\":false") != std::string::npos, "pre-cancel never requests UAC or registry writes");
        std::filesystem::remove_all(root);
        std::cout << "PASS isolated HKCU backup/restore/conflict/interruption, synthetic dump identity and consent\n"; return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        if (!preserve) { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
        return 1;
    }
}
