// ============================================================================
// ZEnvironmentCollector — one shell for everything under
// engine/tools/ZEnvironmentCollector.
// ============================================================================
// Three executables used to live here and all three shipped to players:
//   * ZEnvironmentCollector.exe     -- ZSlate GUI, safe collection + capture
//   * ZGpuSelectionInspector.exe    -- ZSlate GUI, dual-GPU select verification
//   * ZWindowsEnvironmentCheck.exe  -- console CLI (--collect / --diagnose /
//                                      --wer-config and the plain check)
// That split forced the same questions on everybody: which exe do I run, and why
// does the GUI not do what the CLI does. Now there is one exe:
//
//   ZEnvironmentCollector.exe              -> GUI, three tabs
//   ZEnvironmentCollector.exe -batchmode … -> the CLI, no window
//
// `-batchmode` is the Unity/UE spelling on purpose: it is the switch everyone
// already reads as "run this headless and give me an exit code". It may appear
// anywhere on the line; the dispatcher below strips it before the per-command
// parsers run, because those are strict about unknown tokens.
//
// The executable is a WINDOWS-subsystem binary, so the loader never creates a
// console and a double-clicked GUI shows no black window -- not even for a
// frame. (As a CONSOLE binary the loader's console always existed before any of
// our code ran, so FreeConsole() could only close it *after* it had flashed.)
//
// The CLI is not lost: `-batchmode`, `--help` and the two internal worker verbs
// call zengine::appshell::EnsureConsole(), which attaches the caller's console
// and re-points the CRT streams at it before anything is printed, so under
// cmd.exe and PowerShell stdout/stderr still work and %ERRORLEVEL% /
// $LASTEXITCODE is still set. A caller that already redirected stdout to a pipe
// or a file is left alone, so CI captures keep working.
//
// cmd.exe still waits for it, so `exe && next` in a .bat is still correct. That
// is easy to get wrong by reasoning alone -- cmd is documented not to wait for
// GUI-subsystem processes in batch scripts -- so it was checked instead: a .bat
// runs a slow `-batchmode` command and looks for the file it produces on the
// very next line. It waits, both with and without the tool's output redirected.
// Re-check if this ever has to run on an old cmd.exe.
//
// Command dispatch (first match wins, same order the old CLI used):
//   --wer-config        internal elevated registry helper (never opens a window)
//   --collect           safe collection + ZIP           (also --collect-stage)
//   --diagnose          event diagnosis / replay
//   --gpu-selection     dual-GPU auto-select verification
//   <anything else>     the plain environment check
// ============================================================================

#include "EnvironmentCheckGui.h"

#include "Collection.h"
#include "Diagnosis.h"
#include "EnvCheckLog.h"
#include "EnvironmentCheckCommand.h"
#include "Preflight.h"
#include "WerCapture.h"

#include <ZAppShell/AppMain.h>

#include <shellapi.h>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace
{
bool IsBatchModeToken(const std::wstring& argument)
{
    return argument == L"-batchmode" || argument == L"--batchmode" || argument == L"/batchmode";
}
bool IsHelpToken(const std::wstring& argument)
{
    return argument == L"--help" || argument == L"-h" || argument == L"/?";
}
// Internal worker verbs are launched by the tool itself: a Job-managed stage
// child (Collection.cpp) and an elevated ShellExecute for the WER registry
// (WerRegistry.cpp). They must never open a window even if -batchmode is absent.
bool IsInternalWorkerVerb(int argumentCount, wchar_t** arguments)
{
    for (int index = 1; index < argumentCount; ++index)
    {
        const std::wstring argument = arguments[index];
        if (argument == L"--collect-stage" || argument == L"--wer-config") return true;
    }
    return false;
}
// One instance at a time. Two instances cannot both own the day's log file
// (see EnvCheckLog.h): the loser gets ERROR_SHARING_VIOLATION from bqlog, keeps
// running, and quietly logs to a console nobody is reading. Refusing to start is
// better than pretending to work.
//
// Exempt: --help (it answers and exits without touching anything) and the
// internal worker children (IsInternalWorkerVerb), which are relaunches of this
// exe that necessarily run while their parent is still alive.
constexpr wchar_t kSingleInstanceMutexName[] = L"ZEnvironmentCollector_SingleInstance";
constexpr int kAlreadyRunningExitCode = 31;

struct Handle
{
    HANDLE h = nullptr;
    Handle() = default;
    ~Handle() { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

// False when another instance already holds it. True when this one owns the
// mutex -- including when the mutex could not be created at all: a tool that
// refuses to start on a locked-down machine is worse than one that starts
// without the guard.
bool TryAcquireSingleInstance(Handle& mutex)
{
    mutex.h = CreateMutexW(nullptr, FALSE, kSingleInstanceMutexName);
    if (!mutex.h) return true;
    return GetLastError() != ERROR_ALREADY_EXISTS;
}

bool HasGpuSelectionVerb(int argumentCount, wchar_t** arguments)
{
    for (int index = 1; index < argumentCount; ++index)
    {
        if (std::wstring(arguments[index]) == L"--gpu-selection") return true;
    }
    return false;
}
int RunBatchMode(int argumentCount, wchar_t** arguments)
{
    // The per-command parsers reject unknown tokens, so -batchmode is removed
    // before they see the line. argv[0] is kept: IsWerCommand() reads argv[1].
    std::vector<const wchar_t*> filtered;
    filtered.reserve(static_cast<std::size_t>(argumentCount));
    for (int index = 0; index < argumentCount; ++index)
    {
        if (index > 0 && IsBatchModeToken(arguments[index])) continue;
        filtered.push_back(arguments[index]);
    }
    const int count = static_cast<int>(filtered.size());
    wchar_t** args = const_cast<wchar_t**>(filtered.data());

    SetConsoleOutputCP(CP_UTF8);
    try
    {
        if (zengine::envcheck::collector::IsWerCommand(count, args))
            return zengine::envcheck::collector::RunWerCommand(count, args);
        if (zengine::envcheck::collector::IsCollectionCommand(count, args))
            return zengine::envcheck::collector::RunCollectionCommand(count, args);
        if (zengine::envcheck::diagnosis::IsDiagnosisCommandLine(count, args))
            return zengine::envcheck::diagnosis::RunDiagnosis(
                zengine::envcheck::diagnosis::ParseDiagnosisCommandLine(count, args));
        if (zengine::envcheck::preflight::IsPreflightCommandLine(count, args))
            return zengine::envcheck::preflight::RunPreflight(
                zengine::envcheck::preflight::ParsePreflightCommandLine(count, args));
        if (HasGpuSelectionVerb(count, args))
            return zengine::envcheck::RunGpuSelectionCommand(count, args);
        return zengine::envcheck::RunEnvironmentCheckCommand(count, args);
    }
    catch (const std::exception& exception)
    {
        // stderr is what the operator sees; the log file is what they send us.
        std::cerr << "Environment check failed: " << exception.what() << '\n';
        zengine::envcheck::LogError(std::string("Environment check failed: ") + exception.what());
        return 30;
    }
}
} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    // No argc/argv parameters with this entry point; the command line is the
    // source of truth and is already wide.
    int argumentCount = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (!arguments || argumentCount < 1) return 30;
    struct CommandLine
    {
        wchar_t** values;
        ~CommandLine() { LocalFree(values); }
    } commandLine{arguments};

    // 扁鹊 (bqlog) up before anything can fail, and drained on every exit path
    // below -- including the throwing ones, which is what the scope guard is for.
    //
    // Deliberately NOT started in the internal worker children
    // (--collect-stage / --wer-config, see IsInternalWorkerVerb): those are
    // short-lived relaunches of this same exe, so the process that spawned them
    // already holds the day's log file and every child loses the open race with
    // ERROR_SHARING_VIOLATION. bqlog then falls back to the console, and a child
    // created with CREATE_NO_WINDOW and no inherited handles has none -- so both
    // the complaint and the log went nowhere. Nothing is lost: the collection
    // code makes no log calls at all, and a stage's failure reaches its parent
    // as an exit code, which is what marks the stage INCOMPLETE.
    const bool worker = IsInternalWorkerVerb(argumentCount, arguments);
    bool batchMode = false;
    bool helpRequested = false;
    for (int index = 1; index < argumentCount; ++index)
    {
        const std::wstring argument = arguments[index];
        if (IsBatchModeToken(argument)) batchMode = true;
        if (IsHelpToken(argument)) helpRequested = true;
    }

    std::optional<zengine::envcheck::ScopedLog> logScope;
    // --help prints usage and exits: it has nothing worth logging, and skipping
    // the log keeps it from racing the running instance for the log file.
    if (!worker && !helpRequested)
    {
        logScope.emplace();
    }
    if (batchMode || helpRequested || worker)
    {
        // A WINDOWS-subsystem process has no console of its own; this gives the
        // CLI somewhere to print. A no-op when the caller already redirected our
        // output, so a pipe or a CI capture is left alone.
        zengine::appshell::EnsureConsole();
        Handle singleInstance;
        if (!helpRequested && !worker && !TryAcquireSingleInstance(singleInstance))
        {
            std::cerr << "Another ZEnvironmentCollector is already running (usually an open collection window).\n"
                      << "Close it and try again; this instance did nothing.\n";
            return kAlreadyRunningExitCode;
        }
        return RunBatchMode(argumentCount, arguments);
    }
    Handle singleInstance;
    if (!TryAcquireSingleInstance(singleInstance))
    {
        // Plain literals, not ZSlate::Loc: the shell is also built with
        // ENVIRONMENT_CHECK_BUILD_GUI=OFF, where ZSlate is not linked.
        MessageBoxW(nullptr,
            L"采集器已经在运行，本次启动没有做任何事。\n如果原来的窗口找不到了，请确认该进程已退出后重试。",
            L"ZEnvironmentCollector", MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
        return kAlreadyRunningExitCode;
    }
    // Deliberately SW_SHOW and not the show command wWinMain was handed: a
    // launcher passing something odd (SW_SHOWMINIMIZED, SW_HIDE) must not be
    // able to start the collection window invisible.
    return RunEnvironmentCheckGui(SW_SHOW, argumentCount, arguments);
}
