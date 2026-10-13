#include "CrashCapture.h"
#include "DiagnosisUtil.h"
#include <Windows.h>
#include <sddl.h>
#include <fstream>
#include <iterator>
#include <future>
#include <iostream>
#include <stdexcept>
using namespace zengine::envcheck::collector;
int wmain(int argc, wchar_t** argv)
{
    if (argc != 5 || std::wstring(argv[4]) != L"--consent-test-memory-and-sysinternals-license") return 2;
    const std::wstring mode = argv[2];
    if (mode != L"crash" && mode != L"normal" && mode != L"cancel" && mode != L"wait-cancel" && mode != L"callback-error" && mode != L"access-denied") return 2;
    HANDLE trigger = nullptr;
    PROCESS_INFORMATION child{};
    try
    {
        const auto executable = ExecutableDirectory() / L"EnvironmentCaptureTestTarget.exe";
        const auto eventName = L"Local\\EnvironmentCaptureTest-" + std::to_wstring(GetCurrentProcessId());
        trigger = CreateEventW(nullptr, TRUE, FALSE, eventName.c_str());
        if (!trigger) throw std::runtime_error("create event failed");
        std::atomic_bool stop{false};
        CaptureOptions options; options.procDump = argv[1]; options.gameExecutable = executable;
        options.memoryConsent = true; options.licenseAccepted = true; options.waitSeconds = 40;
        options.environment.profile.id = "capture-test"; options.environment.profile.gameExecutable = "EnvironmentCaptureTestTarget.exe";
        options.environment.outputDirectory = argv[3]; options.environment.profile.lookbackHours = 1;
        if (mode == L"access-denied")
        {
            HANDLE token = nullptr;
            if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
                throw std::runtime_error("cannot restrict fixture runner token");
            TOKEN_PRIVILEGES privileges{}; privileges.PrivilegeCount = 1;
            const BOOL foundPrivilege = LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &privileges.Privileges[0].Luid);
            const BOOL restricted = foundPrivilege && AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr, nullptr);
            CloseHandle(token);
            if (!restricted) throw std::runtime_error("cannot disable inherited debug privilege in fixture runner");
            PSECURITY_DESCRIPTOR descriptor = nullptr;
            if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(D;;0x0010;;;WD)(A;;0x101000;;;WD)", SDDL_REVISION_1, &descriptor, nullptr))
                throw std::runtime_error("fixture security descriptor failed");
            SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
            std::wstring command = L"\"" + executable.wstring() + L"\" " + eventName + L" normal";
            STARTUPINFOW startup{}; startup.cb = sizeof(startup);
            const BOOL launched = CreateProcessW(executable.c_str(), command.data(), &attributes, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child);
            LocalFree(descriptor);
            if (!launched) throw std::runtime_error("restricted fixture launch failed");
            HANDLE accessProbe = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | SYNCHRONIZE, FALSE, child.dwProcessId);
            const DWORD probeError = GetLastError();
            if (accessProbe) CloseHandle(accessProbe);
            if (accessProbe || probeError != ERROR_ACCESS_DENIED) throw std::runtime_error("fixture did not establish real access denial; refusing to start capture");
            auto result = CaptureCrash(options, stop, [](const std::string& text, bool) { std::cout << text << std::endl; });
            if (result.dumpCaptured || !result.procDumpVerified || !result.report.partial || result.report.archive.filename() != L"capture-failure.zip")
                throw std::runtime_error("process access failure did not produce a diagnostic-only package");
            std::ifstream input(result.report.directory / "capture.json", std::ios::binary);
            const std::string json((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
            if (json.find("\"failedOperation\":\"OpenProcess(capture)\"") == std::string::npos || json.find("\"win32Error\":5") == std::string::npos ||
                json.find("\"failedStage\":\"OPENING_TARGET\"") == std::string::npos || json.find("\"procDumpStarted\":false") == std::string::npos)
                throw std::runtime_error("Windows access-denied API/phase/code evidence missing");
            if (WaitForSingleObject(child.hProcess, 0) != WAIT_TIMEOUT) throw std::runtime_error("restricted fixture unexpectedly exited");
            SetEvent(trigger); WaitForSingleObject(child.hProcess, 5000);
            CloseHandle(child.hProcess); CloseHandle(child.hThread); CloseHandle(trigger);
            std::cout << "PASS real Windows access denial: signature passed, ProcDump not started, diagnostic ZIP saved\n";
            return 0;
        }
        auto task = std::async(std::launch::async, [&] { return CaptureCrash(options, stop, [&](const std::string& text, bool) {
            std::cout << text << std::endl;
            if (mode == L"callback-error" && text.find("正在监控游戏") != std::string::npos) throw std::runtime_error("injected callback failure");
        }); });
        if (mode == L"wait-cancel")
        {
            Sleep(1000); stop = true;
            auto result = task.get();
            if (result.dumpCaptured || !result.report.archive.empty()) throw std::runtime_error("wait cancellation produced false success");
            CloseHandle(trigger); trigger = nullptr;
            std::cout << "PASS wait cancellation\n"; return 0;
        }
        Sleep(800);
        std::wstring command = L"\"" + executable.wstring() + L"\" " + eventName + L" " + (mode == L"crash" ? L"crash" : L"normal");
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child))
            throw std::runtime_error("fixture launch failed");
        const auto deadline = GetTickCount64() + 30000;
        bool attached = false;
        while (GetTickCount64() < deadline)
        {
            BOOL debug = FALSE;
            if (CheckRemoteDebuggerPresent(child.hProcess, &debug) && debug) { attached = true; break; }
            if (task.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready) break;
        }
        if (mode == L"callback-error")
        {
            auto result = task.get();
            BOOL debug = TRUE;
            if (!CheckRemoteDebuggerPresent(child.hProcess, &debug) || debug || WaitForSingleObject(child.hProcess, 0) != WAIT_TIMEOUT ||
                result.message.find("injected callback failure") == std::string::npos)
                throw std::runtime_error("callback failure left debugger attached or lost cause");
            SetEvent(trigger); WaitForSingleObject(child.hProcess, 5000);
            CloseHandle(child.hProcess); CloseHandle(child.hThread); CloseHandle(trigger);
            std::cout << "PASS callback failure detaches safely\n"; return 0;
        }
        if (!attached)
        {
            stop = true; SetEvent(trigger);
            auto result = task.get();
            throw std::runtime_error("not attached: " + result.message);
        }
        Sleep(500);
        if (mode == L"cancel") stop = true;
        else SetEvent(trigger);
        auto result = task.get();
        if (mode == L"cancel")
        {
            BOOL debug = TRUE;
            if (!CheckRemoteDebuggerPresent(child.hProcess, &debug) || debug || WaitForSingleObject(child.hProcess, 0) != WAIT_TIMEOUT)
                throw std::runtime_error("cancel did not detach safely from live fixture");
            if (result.dumpCaptured || !result.report.cancelled) throw std::runtime_error("invalid cancel result");
            SetEvent(trigger);
        }
        else if (mode == L"crash")
        {
            if (!result.dumpCaptured || result.report.archive.empty()) throw std::runtime_error("capture failed: " + result.message);
            auto metadata = ValidateFullDump(result.report.directory / "crash.dmp", child.dwProcessId);
            if (metadata.exceptionCode != 0xe0424242) throw std::runtime_error("wrong exception captured");
            bool rejected = false;
            try { ValidateFullDump(result.report.directory / "crash.dmp", child.dwProcessId + 1); } catch (...) { rejected = true; }
            if (!rejected) throw std::runtime_error("wrong PID was accepted");
            const auto truncated = result.report.directory / "test-truncated.dmp";
            std::filesystem::copy_file(result.report.directory / "crash.dmp", truncated);
            std::filesystem::resize_file(truncated, metadata.fileBytes - 1);
            rejected = false;
            try { ValidateFullDump(truncated, child.dwProcessId); } catch (...) { rejected = true; }
            std::filesystem::remove(truncated);
            if (!rejected) throw std::runtime_error("truncated full dump accepted");
            std::cout << "ARCHIVE " << zengine::envcheck::diagnosis::WideToUtf8(result.report.archive.wstring()) << '\n';
        }
        else if (result.dumpCaptured || !result.report.archive.empty()) throw std::runtime_error("normal exit falsely reported as capture");
        WaitForSingleObject(child.hProcess, 5000);
        CloseHandle(child.hProcess); CloseHandle(child.hThread); CloseHandle(trigger);
        std::cout << "PASS capture integration\n"; return 0;
    }
    catch (const std::exception& exception)
    {
        if (trigger) { SetEvent(trigger); CloseHandle(trigger); }
        if (child.hProcess) { WaitForSingleObject(child.hProcess, 5000); CloseHandle(child.hProcess); CloseHandle(child.hThread); }
        std::cerr << exception.what() << '\n'; return 1;
    }
}
