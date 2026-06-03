#include "TypeScriptCompiler.h"

#include "Editor/FileSystemWatcher/FileSystemWatcher.h"
#include "Runtime/Core/Base/EngineSystem.h"
#include "Runtime/Project/ProjectInfo.h"
#include "core/Log/LogSystem.h"

#include <chrono>
#include <cstring>
#include <thread>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
#else
    #include <fcntl.h>
    #include <signal.h>
    #include <sys/types.h>
    #include <sys/wait.h>
    #include <unistd.h>
#endif

namespace
{
    // Trim trailing whitespace / CR. tsc's --watch banner uses ANSI clear-screen
    // sequences (ESC [ <n> J etc.) on some terminals; we let them through as
    // printable characters because the engine console doesn't interpret them
    // anyway, and stripping would make diagnosing tsc behaviour harder.
    void rstrip(std::string& s)
    {
        while (!s.empty() && (s.back() == '\r' || s.back() == ' ' || s.back() == '\t'))
            s.pop_back();
    }

    // Look up an executable on PATH, returning its absolute path or an empty
    // path if not found. We rely on Windows' search semantics (PATHEXT) by
    // invoking SearchPathW; on POSIX we walk PATH manually because there is no
    // libc helper.
    std::filesystem::path findOnPath(const std::string& exe_name)
    {
#ifdef _WIN32
        // Convert UTF-8 -> wide once.
        int wlen = MultiByteToWideChar(CP_UTF8, 0, exe_name.c_str(), -1, nullptr, 0);
        if (wlen <= 0)
            return {};
        std::wstring wname(static_cast<size_t>(wlen - 1), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, exe_name.c_str(), -1, wname.data(), wlen);

        // Try each common extension in turn. PATHEXT defaults include .EXE / .CMD
        // / .BAT, so we mirror that explicitly because SearchPathW only honours
        // the lpExtension arg for one extension at a time.
        static const wchar_t* kExts[] = {L".cmd", L".exe", L".bat", L""};
        wchar_t buf[MAX_PATH];
        for (const wchar_t* ext : kExts)
        {
            DWORD ok = SearchPathW(nullptr, wname.c_str(), ext, MAX_PATH, buf, nullptr);
            if (ok > 0 && ok < MAX_PATH)
            {
                return std::filesystem::path(buf);
            }
        }
        return {};
#else
        const char* path_env = std::getenv("PATH");
        if (path_env == nullptr)
            return {};
        std::string path = path_env;
        size_t start = 0;
        while (start <= path.size())
        {
            size_t end = path.find(':', start);
            if (end == std::string::npos)
                end = path.size();
            std::filesystem::path candidate =
                std::filesystem::path(path.substr(start, end - start)) / exe_name;
            if (std::filesystem::exists(candidate))
                return candidate;
            if (end == path.size())
                break;
            start = end + 1;
        }
        return {};
#endif
    }
}  // namespace

// ============================================================================
// Construction / dependencies
// ============================================================================

TypeScriptCompiler::TypeScriptCompiler() = default;

TypeScriptCompiler::~TypeScriptCompiler()
{
    // Best-effort safety net in case Shutdown() wasn't called explicitly.
    StopTscWatch();
}

std::vector<std::type_index> TypeScriptCompiler::GetDependencies() const
{
    // ProjectInfo gives us the paths; we don't depend on ScriptRegistry
    // structurally (we never touch the registry) but ordering after it is
    // nice for log readability so the registry messages fire first.
    return {GET_SYSTEM_TYPE(ProjectInfo)};
}

// ============================================================================
// Initialize / Shutdown
// ============================================================================

bool TypeScriptCompiler::Initialize()
{
    auto project_info = GET_SYSTEM(ProjectInfo);
    if (project_info == nullptr || project_info->GetProjectRoot().empty())
    {
        // No project loaded yet; we'll be re-Initialised when one is opened.
        // (For now, the engine boots TypeScriptCompiler once at editor start;
        // a future "open another project" path would need to call Shutdown +
        // Initialize. Out of scope for P3.)
        LOG_INFO(ZTSC, "No project loaded; TypeScriptCompiler idle.");
        return true;
    }

    m_ProjectRoot = project_info->GetProjectRoot();
    m_IntermediateScriptsRoot = project_info->GetIntermediateScriptsRoot();

    ResolveToolchain(*project_info);

    if (m_Degraded)
    {
        LOG_WARNING(ZTSC,
                    "node/tsc not found - TypeScript compilation disabled. "
                    "Install Node.js, then run 'npm install' inside the project, "
                    "or 'npm i -g typescript'.");
        return true;  // Boot continues; user keeps editing pure-data assets.
    }

    LOG_INFO(ZTSC,
             "Resolved toolchain: tsc={}, node={}",
             m_TscPath.empty() ? std::string {"<via node>"} : m_TscPath.generic_string(),
             m_NodePath.empty() ? std::string {"<not needed>"} : m_NodePath.generic_string());

    // Make sure the output directory exists before tsc tries to write into
    // it - tsc happily creates it itself, but FileSystemWatcher requires
    // the directory to exist at watch-time.
    std::error_code ec;
    std::filesystem::create_directories(m_IntermediateScriptsRoot, ec);

    if (!StartTscWatch())
    {
        LOG_WARNING(ZTSC, "Failed to spawn tsc --watch; entering degraded mode.");
        m_Degraded = true;
        return true;
    }

    StartFileWatcher();
    return true;
}

void TypeScriptCompiler::Shutdown()
{
    StopTscWatch();

    if (m_JsWatcher)
    {
        m_JsWatcher->StopWatching();
        m_JsWatcher.reset();
    }
}

// ============================================================================
// Tick - main-thread per-frame drain
// ============================================================================

void TypeScriptCompiler::Tick()
{
    DrainLogQueue();
    if (m_JsWatcher)
        m_JsWatcher->Update();  // dispatches queued create/change/delete callbacks
}

void TypeScriptCompiler::SetOnJsModuleChanged(JsChangeHandler handler)
{
    m_OnJsChanged = std::move(handler);
}

// ============================================================================
// Toolchain discovery
// ============================================================================

void TypeScriptCompiler::ResolveToolchain(const ProjectInfo& project_info)
{
    // 1) Project-local install: <project>/node_modules/.bin/tsc(.cmd)
    {
#ifdef _WIN32
        const char* kBin = "tsc.cmd";
#else
        const char* kBin = "tsc";
#endif
        std::filesystem::path local_bin =
            project_info.GetProjectRoot() / "node_modules" / ".bin" / kBin;
        if (std::filesystem::exists(local_bin))
        {
            m_TscPath = local_bin;
            // tsc.cmd / tsc shell wrapper find their own node; no explicit
            // m_NodePath needed.
            m_Degraded = false;
            return;
        }
    }

    // 2) System-installed tsc on PATH (npm i -g typescript).
    {
#ifdef _WIN32
        std::filesystem::path system_tsc = findOnPath("tsc.cmd");
        if (system_tsc.empty())
            system_tsc = findOnPath("tsc.exe");
#else
        std::filesystem::path system_tsc = findOnPath("tsc");
#endif
        if (!system_tsc.empty())
        {
            m_TscPath = system_tsc;
            m_Degraded = false;
            return;
        }
    }

    // 3) node + bundled tsc via npx? Could be future enhancement; for now
    //    we just fall through to degraded mode and let users `npm install`.
    m_Degraded = true;
}

// ============================================================================
// Subprocess - Windows
// ============================================================================
#ifdef _WIN32

bool TypeScriptCompiler::StartTscWatch()
{
    if (m_ChildRunning.load())
        return true;

    // Build command line. We always quote tsc itself because user project
    // paths often contain spaces (e.g. "C:\Users\Foo Bar\..."), and we use
    // forward slashes for project paths because tsc accepts them on Windows
    // and CreateProcessW handles them transparently.
    std::wstring cmdline;
    {
        // tsc.cmd ships from npm, so we must invoke it through cmd /c which
        // also gives us "errorlevel" semantics. Direct CreateProcess on
        // .cmd files works on most Windows but not all (depends on
        // PATHEXT-aware loaders); cmd /c is the documented robust path.
        std::wstring tsc_w = m_TscPath.wstring();
        std::wstring proj_w = m_ProjectRoot.wstring();

        cmdline = L"cmd.exe /c \"\"" + tsc_w + L"\" --watch --pretty false -p \"" + proj_w + L"\"\"";
    }

    // Create stdout/stderr pipes. Inherit handles must be set on the WRITE
    // end only; the READ end stays in the parent.
    SECURITY_ATTRIBUTES sa {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE stdout_r = nullptr;
    HANDLE stdout_w = nullptr;
    HANDLE stderr_r = nullptr;
    HANDLE stderr_w = nullptr;

    if (!CreatePipe(&stdout_r, &stdout_w, &sa, 0))
    {
        LOG_ERROR(ZTSC, "CreatePipe(stdout) failed: error={}", static_cast<uint32_t>(GetLastError()));
        return false;
    }
    SetHandleInformation(stdout_r, HANDLE_FLAG_INHERIT, 0);  // parent end not inherited

    if (!CreatePipe(&stderr_r, &stderr_w, &sa, 0))
    {
        CloseHandle(stdout_r);
        CloseHandle(stdout_w);
        LOG_ERROR(ZTSC, "CreatePipe(stderr) failed: error={}", static_cast<uint32_t>(GetLastError()));
        return false;
    }
    SetHandleInformation(stderr_r, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;  // never flash a console window
    si.hStdOutput = stdout_w;
    si.hStdError = stderr_w;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi {};

    // CreateProcessW requires a writable cmdline buffer.
    std::wstring cmd_buf = cmdline;
    BOOL ok = CreateProcessW(nullptr,
                             cmd_buf.data(),
                             nullptr,
                             nullptr,
                             TRUE,  // inherit handles (the pipe write ends)
                             CREATE_NO_WINDOW,
                             nullptr,
                             m_ProjectRoot.wstring().c_str(),
                             &si,
                             &pi);

    // We must close the write ends in our process so that ReadFile sees EOF
    // when the child exits.
    CloseHandle(stdout_w);
    CloseHandle(stderr_w);

    if (!ok)
    {
        DWORD err = GetLastError();
        CloseHandle(stdout_r);
        CloseHandle(stderr_r);
        LOG_ERROR(ZTSC, "CreateProcessW failed: error={}", static_cast<uint32_t>(err));
        return false;
    }

    m_ProcHandle = pi.hProcess;
    m_ProcThread = pi.hThread;
    m_ProcStdoutRead = stdout_r;
    m_ProcStderrRead = stderr_r;
    m_ChildRunning.store(true);
    m_ShouldStop.store(false);

    // Spawn the reader thread that pumps both pipes into m_LogQueue.
    m_ReaderThread = std::thread([this]() { ReaderThreadMain(); });

    LOG_INFO(ZTSC, "tsc --watch spawned (pid={})", static_cast<uint32_t>(pi.dwProcessId));
    return true;
}

void TypeScriptCompiler::StopTscWatch()
{
    if (!m_ChildRunning.load() && !m_ReaderThread.joinable())
        return;

    m_ShouldStop.store(true);

    // Politely terminate. We don't try to send Ctrl+C - that requires a
    // shared console which we deliberately don't have. TerminateProcess is
    // safe because tsc is stateless (no in-flight FS writes that need
    // graceful flushing - .js outputs are atomic per-file).
    if (m_ProcHandle)
    {
        TerminateProcess(static_cast<HANDLE>(m_ProcHandle), 0);
        WaitForSingleObject(static_cast<HANDLE>(m_ProcHandle), 2000);
        CloseHandle(static_cast<HANDLE>(m_ProcHandle));
        m_ProcHandle = nullptr;
    }
    if (m_ProcThread)
    {
        CloseHandle(static_cast<HANDLE>(m_ProcThread));
        m_ProcThread = nullptr;
    }
    if (m_ProcStdoutRead)
    {
        CloseHandle(static_cast<HANDLE>(m_ProcStdoutRead));
        m_ProcStdoutRead = nullptr;
    }
    if (m_ProcStderrRead)
    {
        CloseHandle(static_cast<HANDLE>(m_ProcStderrRead));
        m_ProcStderrRead = nullptr;
    }

    m_ChildRunning.store(false);

    if (m_ReaderThread.joinable())
        m_ReaderThread.join();
}

void TypeScriptCompiler::ReaderThreadMain()
{
    // Multiplex stdout + stderr by alternating non-blocking-ish polls.
    // Windows anonymous pipes don't support overlapped I/O without extra
    // ceremony, so we use PeekNamedPipe to check availability before
    // ReadFile to avoid blocking forever on one stream while the other
    // produces output.

    HANDLE handles[2] = {static_cast<HANDLE>(m_ProcStdoutRead),
                         static_cast<HANDLE>(m_ProcStderrRead)};
    std::string partial[2];  // accumulator per-stream
    char buf[4096];

    while (!m_ShouldStop.load())
    {
        bool any_data = false;
        for (int i = 0; i < 2; ++i)
        {
            HANDLE h = handles[i];
            if (h == nullptr)
                continue;

            DWORD avail = 0;
            if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr))
            {
                // Broken pipe -> child exited.
                handles[i] = nullptr;
                continue;
            }
            if (avail == 0)
                continue;

            DWORD got = 0;
            if (!ReadFile(h, buf, sizeof(buf), &got, nullptr) || got == 0)
            {
                handles[i] = nullptr;
                continue;
            }

            partial[i].append(buf, got);
            any_data = true;

            // Split on '\n', queue each complete line.
            size_t start = 0;
            for (size_t pos = 0; pos < partial[i].size(); ++pos)
            {
                if (partial[i][pos] == '\n')
                {
                    std::string line = partial[i].substr(start, pos - start);
                    rstrip(line);
                    if (!line.empty())
                    {
                        std::lock_guard<std::mutex> lock(m_LogMutex);
                        m_LogQueue.push(std::move(line));
                    }
                    start = pos + 1;
                }
            }
            if (start > 0)
                partial[i].erase(0, start);
        }

        if (handles[0] == nullptr && handles[1] == nullptr)
            break;

        if (!any_data)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // Flush any final partial lines (without trailing \n).
    for (auto& p : partial)
    {
        rstrip(p);
        if (!p.empty())
        {
            std::lock_guard<std::mutex> lock(m_LogMutex);
            m_LogQueue.push(std::move(p));
        }
    }
}

#else  // !_WIN32

// ============================================================================
// Subprocess - POSIX (Linux / macOS)
// ============================================================================

bool TypeScriptCompiler::StartTscWatch()
{
    if (m_ChildRunning.load())
        return true;

    int stdout_pipe[2] = {-1, -1};
    int stderr_pipe[2] = {-1, -1};
    if (pipe(stdout_pipe) != 0 || pipe(stderr_pipe) != 0)
    {
        LOG_ERROR(ZTSC, "pipe() failed: errno={}", errno);
        return false;
    }

    pid_t pid = fork();
    if (pid < 0)
    {
        LOG_ERROR(ZTSC, "fork() failed: errno={}", errno);
        ::close(stdout_pipe[0]);
        ::close(stdout_pipe[1]);
        ::close(stderr_pipe[0]);
        ::close(stderr_pipe[1]);
        return false;
    }

    if (pid == 0)
    {
        // Child
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stderr_pipe[1], STDERR_FILENO);
        ::close(stdout_pipe[0]);
        ::close(stdout_pipe[1]);
        ::close(stderr_pipe[0]);
        ::close(stderr_pipe[1]);

        std::string tsc_str = m_TscPath.string();
        std::string proj_str = m_ProjectRoot.string();
        execlp(tsc_str.c_str(),
               tsc_str.c_str(),
               "--watch",
               "--pretty",
               "false",
               "-p",
               proj_str.c_str(),
               nullptr);
        _exit(127);
    }

    // Parent
    ::close(stdout_pipe[1]);
    ::close(stderr_pipe[1]);
    fcntl(stdout_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(stderr_pipe[0], F_SETFL, O_NONBLOCK);

    m_ProcHandle = reinterpret_cast<void*>(static_cast<intptr_t>(pid));
    m_ProcStdoutRead = reinterpret_cast<void*>(static_cast<intptr_t>(stdout_pipe[0]));
    m_ProcStderrRead = reinterpret_cast<void*>(static_cast<intptr_t>(stderr_pipe[0]));
    m_ChildRunning.store(true);
    m_ShouldStop.store(false);

    m_ReaderThread = std::thread([this]() { ReaderThreadMain(); });

    LOG_INFO(ZTSC, "tsc --watch spawned (pid={})", static_cast<int>(pid));
    return true;
}

void TypeScriptCompiler::StopTscWatch()
{
    if (!m_ChildRunning.load() && !m_ReaderThread.joinable())
        return;

    m_ShouldStop.store(true);

    pid_t pid = static_cast<pid_t>(reinterpret_cast<intptr_t>(m_ProcHandle));
    if (pid > 0)
    {
        kill(pid, SIGTERM);
        // Give it 2 seconds, then SIGKILL.
        for (int i = 0; i < 20; ++i)
        {
            int status = 0;
            pid_t r = waitpid(pid, &status, WNOHANG);
            if (r == pid || r < 0)
            {
                pid = -1;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (pid > 0)
        {
            kill(pid, SIGKILL);
            int status = 0;
            waitpid(pid, &status, 0);
        }
        m_ProcHandle = nullptr;
    }

    if (m_ProcStdoutRead)
    {
        ::close(static_cast<int>(reinterpret_cast<intptr_t>(m_ProcStdoutRead)));
        m_ProcStdoutRead = nullptr;
    }
    if (m_ProcStderrRead)
    {
        ::close(static_cast<int>(reinterpret_cast<intptr_t>(m_ProcStderrRead)));
        m_ProcStderrRead = nullptr;
    }

    m_ChildRunning.store(false);

    if (m_ReaderThread.joinable())
        m_ReaderThread.join();
}

void TypeScriptCompiler::ReaderThreadMain()
{
    int fds[2] = {static_cast<int>(reinterpret_cast<intptr_t>(m_ProcStdoutRead)),
                  static_cast<int>(reinterpret_cast<intptr_t>(m_ProcStderrRead))};
    std::string partial[2];
    char buf[4096];

    while (!m_ShouldStop.load())
    {
        bool any_data = false;
        for (int i = 0; i < 2; ++i)
        {
            if (fds[i] < 0)
                continue;
            ssize_t got = ::read(fds[i], buf, sizeof(buf));
            if (got > 0)
            {
                partial[i].append(buf, static_cast<size_t>(got));
                any_data = true;

                size_t start = 0;
                for (size_t pos = 0; pos < partial[i].size(); ++pos)
                {
                    if (partial[i][pos] == '\n')
                    {
                        std::string line = partial[i].substr(start, pos - start);
                        rstrip(line);
                        if (!line.empty())
                        {
                            std::lock_guard<std::mutex> lock(m_LogMutex);
                            m_LogQueue.push(std::move(line));
                        }
                        start = pos + 1;
                    }
                }
                if (start > 0)
                    partial[i].erase(0, start);
            }
            else if (got == 0)
            {
                fds[i] = -1;  // EOF
            }
            // got < 0 with EAGAIN/EWOULDBLOCK = no data right now; just skip.
        }

        if (fds[0] < 0 && fds[1] < 0)
            break;
        if (!any_data)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    for (auto& p : partial)
    {
        rstrip(p);
        if (!p.empty())
        {
            std::lock_guard<std::mutex> lock(m_LogMutex);
            m_LogQueue.push(std::move(p));
        }
    }
}

#endif

// ============================================================================
// Log drain (cross-platform)
// ============================================================================

void TypeScriptCompiler::DrainLogQueue()
{
    std::queue<std::string> local;
    {
        std::lock_guard<std::mutex> lock(m_LogMutex);
        local.swap(m_LogQueue);
    }
    while (!local.empty())
    {
        // Forward verbatim. tsc tags errors with "error TS2304" etc., so
        // categorising by severity would require parsing the line; we keep
        // it INFO so the user sees stream order intact.
        LOG_INFO(ZTSC, "[tsc] {}", local.front());
        local.pop();
    }
}

// ============================================================================
// File watcher (cross-platform; thin wrapper)
// ============================================================================

void TypeScriptCompiler::StartFileWatcher()
{
    if (m_JsWatcher)
        return;
    if (m_IntermediateScriptsRoot.empty())
        return;

    m_JsWatcher = std::make_unique<FileSystemWatcher>();

    // Opt out of the default .zasset-only filter and allow the extensions
    // tsc actually emits. We deliberately ignore .map (sourcemap) and .d.ts
    // since those don't carry executable code.
    m_JsWatcher->SetExtensionFilter({".js"});

    // The callbacks fire from FileSystemWatcher::Update() which we drive
    // from the main-thread Tick(), so they're already main-thread safe.
    auto fire_change = [this](const std::filesystem::path& abs) {
        // Only fire for .js (ignore .js.map, .d.ts that tsc may also emit).
        if (abs.extension() != ".js")
            return;
        LOG_INFO(ZTSC, "Compiled module updated: {}", abs.generic_string());
        if (m_OnJsChanged)
            m_OnJsChanged(abs);
    };
    m_JsWatcher->SetOnFileCreated(fire_change);
    m_JsWatcher->SetOnFileChanged(fire_change);
    m_JsWatcher->SetOnFileDeleted([](const std::filesystem::path& abs) {
        if (abs.extension() == ".js")
            LOG_INFO(ZTSC, "Compiled module deleted: {}", abs.generic_string());
    });
    m_JsWatcher->WatchDirectory(m_IntermediateScriptsRoot);
    LOG_INFO(ZTSC, "Watching JS output directory: {}", m_IntermediateScriptsRoot.generic_string());
}
