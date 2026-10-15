#include "DllProbeProtocol.h"

#include <Windows.h>
#include <shellapi.h>

namespace
{
zengine::envcheck::DllProbeWireResult g_result = {
    zengine::envcheck::DllProbeMagic,
    zengine::envcheck::DllProbeProtocolVersion,
    0, 0, 0, 0, 0, 0, 0
};
char g_exportName[256] = {};

bool IsProbeFailureException(DWORD exceptionCode)
{
    switch (exceptionCode)
    {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case EXCEPTION_DATATYPE_MISALIGNMENT:
    case EXCEPTION_FLT_DENORMAL_OPERAND:
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
    case EXCEPTION_FLT_INVALID_OPERATION:
    case EXCEPTION_FLT_OVERFLOW:
    case EXCEPTION_FLT_STACK_CHECK:
    case EXCEPTION_FLT_UNDERFLOW:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_INT_OVERFLOW:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_STACK_OVERFLOW:
    case 0xC0000409u:
        return true;
    default:
        return false;
    }
}

LONG CALLBACK CaptureProbeException(EXCEPTION_POINTERS* exceptionPointers)
{
    if (exceptionPointers != nullptr && exceptionPointers->ExceptionRecord != nullptr &&
        IsProbeFailureException(exceptionPointers->ExceptionRecord->ExceptionCode))
    {
        g_result.exceptionCode = exceptionPointers->ExceptionRecord->ExceptionCode;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

bool CopyExportName(const wchar_t* source, char* destination, DWORD capacity)
{
    if (source == nullptr || destination == nullptr || capacity == 0)
    {
        return false;
    }
    DWORD index = 0;
    while (source[index] != L'\0')
    {
        if (index + 1 >= capacity || source[index] > 0x7f)
        {
            return false;
        }
        destination[index] = static_cast<char>(source[index]);
        ++index;
    }
    destination[index] = '\0';
    return true;
}

void WriteResult(const wchar_t* path)
{
    if (path == nullptr)
    {
        return;
    }
    HANDLE file = CreateFileW(
        path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return;
    }
    DWORD written = 0;
    WriteFile(file, &g_result, sizeof(g_result), &written, nullptr);
    FlushFileBuffers(file);
    CloseHandle(file);
}

void ExecuteProbe(const wchar_t* dllPath, const char* exportName)
{
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS);
    HMODULE module = LoadLibraryExW(
        dllPath,
        nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS |
            LOAD_LIBRARY_SEARCH_USER_DIRS);
    g_result.win32Error = module == nullptr ? GetLastError() : ERROR_SUCCESS;
    g_result.loadSucceeded = module == nullptr ? 0u : 1u;

    if (module != nullptr && exportName != nullptr && exportName[0] != '\0')
    {
        FARPROC address = GetProcAddress(module, exportName);
        g_result.exportFound = address == nullptr ? 0u : 1u;
        if (address != nullptr)
        {
            using ProbeFunction = int (*)();
            g_result.exportResult = reinterpret_cast<ProbeFunction>(address)();
            g_result.exportCalled = 1;
        }
    }
    if (module != nullptr)
    {
        FreeLibrary(module);
    }
}
} // namespace

extern "C" __declspec(noreturn) void WINAPI ProbeEntry()
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);

    int argumentCount = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    const wchar_t* dllPath = nullptr;
    const wchar_t* resultPath = nullptr;
    if (arguments != nullptr)
    {
        for (int index = 1; index < argumentCount; ++index)
        {
            if (lstrcmpW(arguments[index], L"--dll") == 0 && index + 1 < argumentCount)
            {
                dllPath = arguments[++index];
            }
            else if (lstrcmpW(arguments[index], L"--result-file") == 0 && index + 1 < argumentCount)
            {
                resultPath = arguments[++index];
            }
            else if (lstrcmpW(arguments[index], L"--export") == 0 && index + 1 < argumentCount)
            {
                if (!CopyExportName(arguments[++index], g_exportName, sizeof(g_exportName)))
                {
                    LocalFree(arguments);
                    ExitProcess(50);
                }
                g_result.exportRequested = 1;
            }
            else
            {
                LocalFree(arguments);
                ExitProcess(50);
            }
        }
    }

    if (dllPath == nullptr || resultPath == nullptr)
    {
        if (arguments != nullptr)
        {
            LocalFree(arguments);
        }
        ExitProcess(50);
    }

    PVOID exceptionHandler = AddVectoredExceptionHandler(1, CaptureProbeException);
    ExecuteProbe(dllPath, g_exportName);
    if (exceptionHandler != nullptr)
    {
        RemoveVectoredExceptionHandler(exceptionHandler);
    }
    WriteResult(resultPath);
    if (arguments != nullptr)
    {
        LocalFree(arguments);
    }

    if (g_result.exceptionCode != 0)
    {
        ExitProcess(41);
    }
    if (!g_result.loadSucceeded)
    {
        ExitProcess(40);
    }
    if (g_result.exportRequested && !g_result.exportFound)
    {
        ExitProcess(42);
    }
    ExitProcess(0);
}
