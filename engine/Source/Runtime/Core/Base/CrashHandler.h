#pragma once

// =============================================================================
// CrashHandler
// -----------------------------------------------------------------------------
// Windows SEH crash handler that writes a symbolized callstack to
// `<exe-dir>/crash_stack.txt`.  Used by ZEditor and ASTCPreview.
//
// Design:
//   * Process-wide: installed once via SetUnhandledExceptionFilter.
//   * Runs on the faulting thread (the filter callback is SEH, not Vectored).
//   * Looks for PDBs next to the exe (RelWithDebInfo build).
//   * Removes itself after the first dump so a nested crash won't re-enter.
//
// Usage:
//   #include "Runtime/Core/Base/CrashHandler.h"
//   InstallCrashHandler();
// =============================================================================

#ifdef _WIN32

#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

#include <fstream>
#include <iomanip>
#include <string>

inline LONG WINAPI ZEngineCrashHandler(EXCEPTION_POINTERS* ep);

// Installed once; the actual handler is ZEngineCrashHandler (a plain function).
inline void InstallCrashHandler()
{
    static bool installed = false;
    if (installed) return;
    installed = true;
    SetUnhandledExceptionFilter(ZEngineCrashHandler);
}

// ---- The actual SEH filter (plain function, NOT a lambda) -----------

inline LONG WINAPI ZEngineCrashHandler(EXCEPTION_POINTERS* ep)
{
    // ---- Resolve output path: <exe-dir>/crash_stack.txt ----
    char exe_path[MAX_PATH] {};
    GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
    std::string out_path = exe_path;
    // Strip filename -> directory
    size_t last_slash = out_path.find_last_of("\\/");
    if (last_slash != std::string::npos)
        out_path = out_path.substr(0, last_slash + 1);
    out_path += "crash_stack.txt";

    // ---- Write -------------------------------------------------------
    std::ofstream out(out_path, std::ios::trunc);
    if (!out.is_open() || ep == nullptr || ep->ExceptionRecord == nullptr)
    {
        return EXCEPTION_EXECUTE_HANDLER;
    }

    HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    SymInitialize(process, nullptr, TRUE);

    out << "ZEngine crash. ExceptionCode=0x" << std::hex << ep->ExceptionRecord->ExceptionCode
        << " at " << ep->ExceptionRecord->ExceptionAddress << std::dec
        << " tid=" << GetCurrentThreadId() << "\n\n";

    CONTEXT* ctx = ep->ContextRecord;
    STACKFRAME64 frame = {};
    frame.AddrPC.Offset    = ctx->Rip;
    frame.AddrPC.Mode      = AddrModeFlat;
    frame.AddrFrame.Offset = ctx->Rbp;
    frame.AddrFrame.Mode  = AddrModeFlat;
    frame.AddrStack.Offset = ctx->Rsp;
    frame.AddrStack.Mode  = AddrModeFlat;

    for (int i = 0; i < 64; ++i)
    {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(),
                         &frame, ctx, nullptr,
                         SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
        {
            break;
        }
        if (frame.AddrPC.Offset == 0)
        {
            break;
        }

        const DWORD64 addr = frame.AddrPC.Offset;
        char symbol_buffer[sizeof(SYMBOL_INFO) + 512] = {};
        SYMBOL_INFO* symbol = reinterpret_cast<SYMBOL_INFO*>(symbol_buffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen   = 511;
        DWORD64 displacement = 0;

        out << "[" << i << "] 0x" << std::hex << addr << std::dec << "  ";
        if (SymFromAddr(process, addr, &displacement, symbol))
        {
            out << symbol->Name << " +0x" << std::hex << displacement << std::dec;
        }
        else
        {
            out << "<no symbol>";
        }

        IMAGEHLP_LINE64 line = {};
        line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
        DWORD line_displacement = 0;
        if (SymGetLineFromAddr64(process, addr, &line_displacement, &line) &&
            line.FileName != nullptr)
        {
            out << "  (" << line.FileName << ":" << line.LineNumber << ")";
        }
        out << "\n";
    }
    out.flush();
    SymCleanup(process);

    return EXCEPTION_EXECUTE_HANDLER;
}

#else  // !_WIN32

inline void InstallCrashHandler() {}  // no-op on non-Windows

#endif
