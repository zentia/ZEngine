// ============================================================================
// ZAppShell — what a ZEngine executable does before it does anything else.
// ============================================================================
// Two things had each been written more than once:
//
//   * Reading the command line. CommandLineToArgvW(GetCommandLineW()) appears in
//     ZEditor, ZInsights, CommandSystem and LogSystem; ZSlate carried a UTF-8
//     variant of its own; LuaMemPreview used the CRT's __argv, which is in the
//     ANSI codepage -- so a non-ASCII path arrived already mangled, and its
//     batch mode takes a path.
//
//   * Getting a console. A /SUBSYSTEM:WINDOWS executable has none, so printf
//     goes nowhere and a headless mode looks like it hung. Two implementations
//     existed and had drifted: the one in ZSlate::Win32 and a shorter one in
//     WindowsEnvironmentCheck that lacked both the AllocConsole() fallback and
//     the unbuffered stdout.
//
// Header-only, and it has to stay that way. ZEnvironmentCollector ships /MT
// while the engine tree is /MD, so anything compiled into a library can serve
// only one of them (LNK2038) -- which is exactly why the /MT collector could
// not call the /MD ZSlate version and grew its own. Inline code is compiled by
// whoever includes it, with that target's own runtime library. For the same
// reason this calls nothing but the Win32 API and the CRT: no ZSlate, no
// ZRuntime.
//
// Deliberately NOT here: deciding whether this run is a GUI run or a batch run.
// The three executables that have a batch mode do not agree on what that means
// -- WindowsEnvironmentCheck takes -batchmode as a switch anywhere on the line,
// LuaMemPreview takes it as a positional subcommand in argv[1] next to
// --stats/--dump-ui, and ZEditor reads -p/--rhi to pick a startup mode. Sharing
// the mechanism is worth it. Sharing that decision is not.
// ============================================================================

#ifndef ZENGINE_APPSHELL_APP_MAIN_H
#define ZENGINE_APPSHELL_APP_MAIN_H

#include <string>
#include <vector>

#ifdef _WIN32

// Windows.h defines min/max unless told not to, and they break any later use of
// std::min/std::max. Defined here and left defined: it only ever removes macros.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <shellapi.h>  // CommandLineToArgvW

#include <cstdio>

namespace zengine::appshell
{

// ---- Command line ---------------------------------------------------------

// The wide command line, split. Owns the array CommandLineToArgvW allocates
// (LocalFree, not delete[]). Empty -- valid() false -- if the split failed,
// which in practice means no command line at all.
class CommandLine
{
public:
    CommandLine() = default;
    CommandLine(const CommandLine&) = delete;
    CommandLine& operator=(const CommandLine&) = delete;

    CommandLine(CommandLine&& other) noexcept
        : values_(other.values_), count_(other.count_)
    {
        other.values_ = nullptr;
        other.count_ = 0;
    }
    CommandLine& operator=(CommandLine&& other) noexcept
    {
        if (this != &other)
        {
            Release();
            values_ = other.values_;
            count_ = other.count_;
            other.values_ = nullptr;
            other.count_ = 0;
        }
        return *this;
    }
    ~CommandLine() { Release(); }

    static CommandLine Acquire()
    {
        int count = 0;
        wchar_t** values = ::CommandLineToArgvW(::GetCommandLineW(), &count);
        CommandLine result;
        if (values)
        {
            result.values_ = values;
            result.count_ = count;
        }
        return result;
    }

    bool valid() const { return values_ != nullptr; }
    // argv-style: count() includes the executable at index 0, as with main().
    int count() const { return count_; }
    wchar_t** values() const { return values_; }
    const wchar_t* operator[](int index) const { return values_[index]; }

    std::vector<std::wstring> Wide() const
    {
        std::vector<std::wstring> out;
        out.reserve(static_cast<std::size_t>(count_));
        for (int index = 0; index < count_; ++index) out.emplace_back(values_[index]);
        return out;
    }

    // UTF-8, for code that hands paths to std::filesystem or prints them.
    std::vector<std::string> Utf8() const
    {
        std::vector<std::string> out;
        out.reserve(static_cast<std::size_t>(count_));
        for (int index = 0; index < count_; ++index) out.push_back(Utf8FromWide(values_[index]));
        return out;
    }

    // Loses information for anything outside the active codepage; only for
    // handing a value to an API that has no wide form.
    static std::string Utf8FromWide(const wchar_t* wide)
    {
        if (!wide) return {};
        const int needed = ::WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
        if (needed <= 1) return {};
        std::string out(static_cast<std::size_t>(needed - 1), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), needed, nullptr, nullptr);
        return out;
    }

private:
    void Release()
    {
        if (values_) ::LocalFree(values_);
        values_ = nullptr;
        count_ = 0;
    }

    wchar_t** values_ = nullptr;
    int count_ = 0;
};

// ---- Console --------------------------------------------------------------

// True when stdout is a file or a pipe, i.e. the caller is already capturing
// our output (`>`, `|`, a CI harness). Forcing a console on top of that would
// move the output off the pipe and the capture would come back empty.
inline bool StdOutIsRedirected()
{
    const HANDLE out = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (out == nullptr || out == INVALID_HANDLE_VALUE) return false;
    const DWORD type = ::GetFileType(out);
    return type == FILE_TYPE_DISK || type == FILE_TYPE_PIPE;
}

// Gives a /SUBSYSTEM:WINDOWS process somewhere to print. Attaches the caller's
// console if there is one, otherwise allocates one -- a headless run started
// from Explorer with no console would otherwise print into the void.
//
// Safe to call more than once; a no-op when the caller is already capturing
// output. Call it only on the path that is going to print: a GUI run must not,
// or a double-click would flash a console window.
inline void EnsureConsole()
{
    if (StdOutIsRedirected()) return;
    if (!::AttachConsole(ATTACH_PARENT_PROCESS) && !::AllocConsole()) return;

    // The C runtime captured handles before we had a console, so stdout/stderr
    // still point nowhere until they are reopened. Reopening is mandatory, not
    // cosmetic.
    FILE* stream = nullptr;
    freopen_s(&stream, "CONIN$", "r", stdin);
    freopen_s(&stream, "CONOUT$", "w", stdout);
    freopen_s(&stream, "CONOUT$", "w", stderr);
    // Unbuffered: without it a crash or an early return loses the last lines --
    // typically the ones explaining the crash.
    setvbuf(stdout, nullptr, _IONBF, 0);
}

}  // namespace zengine::appshell

#endif  // _WIN32

#endif  // ZENGINE_APPSHELL_APP_MAIN_H
