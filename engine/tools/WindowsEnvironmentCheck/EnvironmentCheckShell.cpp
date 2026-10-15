// ============================================================================
// ZEnvironmentCollector — one shell for everything under
// engine/tools/WindowsEnvironmentCheck.
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
// The executable is a CONSOLE-subsystem binary so `-batchmode` behaves like a
// real CLI under cmd.exe and PowerShell: stdout/stderr work, $LASTEXITCODE is
// set, and `exe && next` inside a .bat actually waits for it. The alternative
// (WINDOWS subsystem + AttachConsole) silently breaks `&&`, which is exactly how
// CI drives this tool. GUI mode therefore calls FreeConsole() first thing, which
// drops the console the loader created without leaving a window behind.
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
#include "EnvironmentCheckCommand.h"
#include "WerCapture.h"

#include <shellapi.h>
#include <iostream>
#include <memory>
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
        if (HasGpuSelectionVerb(count, args))
            return zengine::envcheck::RunGpuSelectionCommand(count, args);
        return zengine::envcheck::RunEnvironmentCheckCommand(count, args);
    }
    catch (const std::exception& exception)
    {
        std::cerr << "Environment check failed: " << exception.what() << '\n';
        return 30;
    }
}
} // namespace

// wmain (not wWinMain): the executable is a CONSOLE-subsystem binary so that
// -batchmode behaves like a real CLI, and the CRT entry point for that is wmain.
int wmain(int argc, wchar_t** argv)
{
    bool batchMode = false;
    bool helpRequested = false;
    for (int index = 1; index < argc; ++index)
    {
        const std::wstring argument = argv[index];
        if (IsBatchModeToken(argument)) batchMode = true;
        if (IsHelpToken(argument)) helpRequested = true;
    }
    if (batchMode || helpRequested || IsInternalWorkerVerb(argc, argv))
    {
        return RunBatchMode(argc, argv);
    }
    return RunEnvironmentCheckGui(SW_SHOW, argc, argv);
}
