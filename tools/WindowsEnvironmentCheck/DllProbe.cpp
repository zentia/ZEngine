#include "DllProbe.h"
#include "DllProbeProtocol.h"

#include <Windows.h>

#include <chrono>
#include <fstream>
#include <iterator>
#include <sstream>

namespace zengine::envcheck
{
namespace
{
std::string WideToUtf8(const std::wstring& value)
{
    if (value.empty())
    {
        return {};
    }
    const int byteCount = WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (byteCount <= 0)
    {
        return {};
    }
    std::string result(static_cast<std::size_t>(byteCount), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), byteCount, nullptr, nullptr);
    return result;
}

std::string PathToUtf8(const std::filesystem::path& path)
{
    return WideToUtf8(path.wstring());
}

std::wstring QuoteArgument(const std::wstring& argument)
{
    if (argument.empty())
    {
        return L"\"\"";
    }
    if (argument.find_first_of(L" \t\n\v\"") == std::wstring::npos)
    {
        return argument;
    }

    std::wstring result = L"\"";
    std::size_t slashCount = 0;
    for (const wchar_t character : argument)
    {
        if (character == L'\\')
        {
            ++slashCount;
            continue;
        }
        if (character == L'\"')
        {
            result.append(slashCount * 2 + 1, L'\\');
            result.push_back(L'\"');
            slashCount = 0;
            continue;
        }
        result.append(slashCount, L'\\');
        slashCount = 0;
        result.push_back(character);
    }
    result.append(slashCount * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

void AppendArgument(std::wstring& commandLine, const std::wstring& argument)
{
    if (!commandLine.empty())
    {
        commandLine.push_back(L' ');
    }
    commandLine += QuoteArgument(argument);
}

std::filesystem::path TemporaryResultPath()
{
    wchar_t directory[MAX_PATH + 1]{};
    if (GetTempPathW(MAX_PATH, directory) == 0)
    {
        return {};
    }
    wchar_t fileName[MAX_PATH + 1]{};
    if (GetTempFileNameW(directory, L"ZDP", 0, fileName) == 0)
    {
        return {};
    }
    DeleteFileW(fileName);
    return fileName;
}

std::string Win32Message(DWORD error)
{
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    if (length == 0 || buffer == nullptr)
    {
        return "Win32 error " + std::to_string(error);
    }
    std::wstring message(buffer, length);
    LocalFree(buffer);
    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n' || message.back() == L' '))
    {
        message.pop_back();
    }
    return WideToUtf8(message);
}

std::string ReadOutputFile(const std::filesystem::path& path)
{
    constexpr std::size_t maximumBytes = 64 * 1024;
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        return {};
    }
    std::string output;
    output.resize(maximumBytes);
    input.read(output.data(), static_cast<std::streamsize>(output.size()));
    output.resize(static_cast<std::size_t>(input.gcount()));
    if (input.peek() != std::char_traits<char>::eof())
    {
        output += "\n[output truncated at 64 KiB]";
    }
    return output;
}
} // namespace

DllProbeResult RunDllProbe(const std::filesystem::path& dllPath, const DllProbeOptions& options)
{
    DllProbeResult result;
    result.dllPath = PathToUtf8(dllPath);
    result.probeExecutable = PathToUtf8(options.probeExecutable);
    result.runnerExecutable = PathToUtf8(options.runnerExecutable);
    result.exportName = options.exportName;
    result.attempted = true;
    for (const auto& argument : options.runnerArguments)
    {
        result.runnerArguments.push_back(WideToUtf8(argument));
    }

    if (!std::filesystem::is_regular_file(options.probeExecutable))
    {
        result.error = "DLL probe executable does not exist: " + result.probeExecutable;
        return result;
    }
    if (!options.runnerExecutable.empty() && !std::filesystem::is_regular_file(options.runnerExecutable))
    {
        result.error = "Probe runner does not exist: " + result.runnerExecutable;
        return result;
    }

    const auto resultPath = TemporaryResultPath();
    if (resultPath.empty())
    {
        result.error = "Failed to allocate a temporary probe result path.";
        return result;
    }
    std::filesystem::path outputPath = resultPath;
    outputPath += L".output.txt";
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    HANDLE outputHandle = CreateFileW(
        outputPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);

    std::wstring commandLine;
    const bool useRunner = !options.runnerExecutable.empty();
    AppendArgument(commandLine, useRunner ? options.runnerExecutable.wstring() : options.probeExecutable.wstring());
    if (useRunner)
    {
        for (const auto& argument : options.runnerArguments)
        {
            AppendArgument(commandLine, argument);
        }
        AppendArgument(commandLine, L"--");
        AppendArgument(commandLine, options.probeExecutable.wstring());
    }
    AppendArgument(commandLine, L"--dll");
    AppendArgument(commandLine, dllPath.wstring());
    AppendArgument(commandLine, L"--result-file");
    AppendArgument(commandLine, resultPath.wstring());
    if (!options.exportName.empty())
    {
        AppendArgument(commandLine, L"--export");
        AppendArgument(commandLine, std::wstring(options.exportName.begin(), options.exportName.end()));
    }

    std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
    mutableCommandLine.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    const bool captureOutput = outputHandle != INVALID_HANDLE_VALUE;
    if (captureOutput)
    {
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = nullptr;
        startup.hStdOutput = outputHandle;
        startup.hStdError = outputHandle;
    }
    PROCESS_INFORMATION process{};
    const auto startTime = std::chrono::steady_clock::now();
    const std::wstring workingDirectory = dllPath.parent_path().wstring();
    const BOOL started = CreateProcessW(
        nullptr,
        mutableCommandLine.data(),
        nullptr,
        nullptr,
        captureOutput ? TRUE : FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        workingDirectory.empty() ? nullptr : workingDirectory.c_str(),
        &startup,
        &process);
    if (captureOutput)
    {
        CloseHandle(outputHandle);
    }
    if (started == FALSE)
    {
        const DWORD error = GetLastError();
        result.win32Error = error;
        result.error = "Failed to start DLL probe: " + Win32Message(error);
        DeleteFileW(resultPath.c_str());
        DeleteFileW(outputPath.c_str());
        return result;
    }

    result.processStarted = true;
    const DWORD waitResult = WaitForSingleObject(process.hProcess, options.timeoutMilliseconds);
    if (waitResult == WAIT_TIMEOUT)
    {
        result.timedOut = true;
        TerminateProcess(process.hProcess, 0xDEADu);
        WaitForSingleObject(process.hProcess, 5000);
    }
    else if (waitResult == WAIT_FAILED)
    {
        const DWORD error = GetLastError();
        result.win32Error = error;
        result.error = "Waiting for DLL probe failed: " + Win32Message(error);
    }

    DWORD exitCode = 0;
    if (GetExitCodeProcess(process.hProcess, &exitCode) != FALSE)
    {
        result.processExitCode = exitCode;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    result.durationMilliseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startTime).count());
    result.runnerOutput = ReadOutputFile(outputPath);
    result.runnerReportedIsaViolation =
        result.runnerOutput.find("SDE-ERROR: Executed instruction not valid for specified chip") !=
        std::string::npos;
    result.runnerReportedInternalError =
        result.runnerOutput.find("assertion failed:") != std::string::npos ||
        result.runnerOutput.find("NO STACK TRACE AVAILABLE") != std::string::npos;

    std::ifstream input(resultPath, std::ios::binary);
    DllProbeWireResult wire{};
    input.read(reinterpret_cast<char*>(&wire), sizeof(wire));
    if (input.gcount() == sizeof(wire) && wire.magic == DllProbeMagic &&
        wire.version == DllProbeProtocolVersion)
    {
        result.resultAvailable = true;
        result.loadSucceeded = wire.loadSucceeded != 0;
        result.win32Error = wire.win32Error;
        result.exceptionCode = wire.exceptionCode;
        result.exportRequested = wire.exportRequested != 0;
        result.exportFound = wire.exportFound != 0;
        result.exportCalled = wire.exportCalled != 0;
        result.exportResult = wire.exportResult;
    }
    else if (!result.timedOut && result.error.empty())
    {
        std::ostringstream stream;
        stream << "Probe produced no valid result; process exit code is 0x"
               << std::hex << std::uppercase << result.processExitCode << '.';
        result.error = stream.str();
        if ((result.processExitCode & 0xC0000000u) == 0xC0000000u)
        {
            result.exceptionCode = result.processExitCode;
        }
    }

    DeleteFileW(resultPath.c_str());
    DeleteFileW(outputPath.c_str());
    return result;
}
} // namespace zengine::envcheck
