#pragma once

#include "Runtime/Core/Base/EngineSystem.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <typeindex>
#include <vector>

class FileSystemWatcher;
class ProjectInfo;

/**
 * @brief Editor-only IEngineSystem that drives the TypeScript -> JavaScript
 *        build pipeline for the currently-loaded project.
 *
 * Why this class exists
 * ---------------------
 * ZEngine intentionally does NOT bundle its own TypeScript front-end (the
 * official `tsc` toolchain is far better tested and is already required for
 * VS Code IntelliSense in user projects). Instead, the Editor spawns
 * `tsc --watch` as a long-lived child process whose stdout/stderr is piped
 * back into ZEngine's log system. This mirrors how UE wraps the .NET / VS
 * compilers for C# source plugins, and how Unity drives Roslyn from
 * AssetDatabase.
 *
 * Responsibilities (per doc/TYPESCRIPT_SCRIPTING_DESIGN.md Phase 3):
 *   1. Detect node + tsc availability on PATH (or, on Windows, the
 *      `node_modules/.bin/tsc.cmd` shipped with the project).
 *   2. Spawn `tsc --watch -p <projectRoot>` in a background thread, forward
 *      stdout / stderr lines into the ZTSC log category so users see compile
 *      errors directly in the engine console.
 *   3. Watch `<Project>/Intermediate/Scripts/` for `.js` updates via the
 *      shared FileSystemWatcher. P3 simply logs the events; P4 will route
 *      them to ScriptingEngine::ReloadModule() once that exists.
 *
 * Degraded mode
 * -------------
 * If neither `node`/`tsc` nor a project-local tsc is found, we DON'T fail
 * the engine boot. Instead we set m_Degraded = true, log a one-shot warning,
 * and let the user keep editing in pure-data mode. The UI banner is the
 * Phase 5 task; in P3 we just record the state.
 *
 * Lifecycle
 * ---------
 * - InitPhase: PostInit (must be after ProjectInfo, ScriptRegistry, and the
 *   editor windows that emit log messages).
 * - Initialize() is a no-op if no project is loaded.
 * - Tick() is called by Editor::Run() once per frame. It:
 *     a. drains stdout/stderr lines queued by the worker thread and emits
 *        them as LOG_INFO(ZTSC, ...).
 *     b. ticks the FileSystemWatcher (its callbacks run on the main thread
 *        through this drain).
 * - Shutdown() terminates the tsc child process and joins the worker.
 *
 * Threading model
 * ---------------
 * The worker thread does the blocking ReadFile() on the pipe and pushes
 * complete \n-delimited lines into m_LogQueue. The main-thread Tick()
 * drains that queue under m_LogMutex. The FileSystemWatcher uses its own
 * background thread internally and exposes a queue we drain in Update().
 */
class TypeScriptCompiler : public IEngineSystem
{
public:
    TypeScriptCompiler();
    ~TypeScriptCompiler() override;

    // ---- IEngineSystem -----------------------------------------------------

    std::string GetName() const override { return "TypeScriptCompiler"; }
    SystemInitPhase GetInitPhase() const override { return SystemInitPhase::PostInit; }
    std::vector<std::type_index> GetDependencies() const override;
    bool Initialize() override;
    void Shutdown() override;

    /// Called from Editor::Run() once per frame.
    /// Cheap when idle (a few queue checks).
    void Tick();

    // ---- Diagnostic accessors (mainly for the future Phase-5 UI banner) ----

    /// True if no node/tsc was found and we are running without compilation.
    bool IsDegraded() const { return m_Degraded; }

    /// True if the tsc --watch child is currently believed to be running.
    bool IsWatcherRunning() const { return m_ChildRunning; }

    /// Absolute path of the tsc executable we resolved (or empty in
    /// degraded mode). Useful for the Phase-5 UI to show what it picked.
    const std::filesystem::path& GetResolvedTscPath() const { return m_TscPath; }

    /// Absolute path of the node executable we resolved (or empty if we
    /// found a tsc.cmd / tsc shell wrapper that doesn't need an explicit
    /// node argument).
    const std::filesystem::path& GetResolvedNodePath() const { return m_NodePath; }

    // ---- P4 hook -----------------------------------------------------------
    //
    // Wired up in Phase 4 once ScriptingEngine exists. The compiler stays
    // P4-agnostic - it just publishes "this .js file changed" events.

    /// Set the callback fired every time a `.js` file under
    /// Intermediate/Scripts/ is created or modified. Path is absolute.
    /// May be called from the main thread (via Tick()'s watcher drain).
    using JsChangeHandler = std::function<void(const std::filesystem::path&)>;
    void SetOnJsModuleChanged(JsChangeHandler handler);

private:
    // ---- Discovery ---------------------------------------------------------

    /// Locate node/tsc once. Sets m_NodePath / m_TscPath / m_Degraded.
    /// Strategy:
    ///   1. <Project>/node_modules/.bin/tsc(.cmd) - preferred, version-pinned.
    ///   2. tsc on PATH (system-installed @ -g typescript).
    ///   3. node on PATH + tsc.js bundled with package.json install.
    ///   4. None of the above -> degraded mode.
    void ResolveToolchain(const ProjectInfo& project_info);

    // ---- Subprocess --------------------------------------------------------

    /// Spawn tsc --watch with stdout+stderr piped into m_LogQueue. Returns
    /// false on launch failure (we then enter degraded mode).
    bool StartTscWatch();

    /// Tear down the running tsc child and join the worker thread. Safe to
    /// call multiple times.
    void StopTscWatch();

    /// Worker-thread main: blocking-reads the stdout pipe and pushes lines.
    void ReaderThreadMain();

    // ---- File watcher ------------------------------------------------------

    /// Set up FileSystemWatcher on Intermediate/Scripts/. Idempotent.
    void StartFileWatcher();

    /// Drain queued log lines and emit them via LOG_INFO. Called from Tick().
    void DrainLogQueue();

private:
    // Cached project paths captured at Initialize().
    std::filesystem::path m_ProjectRoot;
    std::filesystem::path m_IntermediateScriptsRoot;

    // Resolved toolchain locations.
    std::filesystem::path m_NodePath;
    std::filesystem::path m_TscPath;
    bool m_Degraded = false;

    // Subprocess plumbing. The `m_Proc*` handles are platform-typed via
    // intptr_t/void* to avoid leaking <Windows.h> into this header.
    void* m_ProcHandle = nullptr;      // Win32 HANDLE / POSIX intptr_t pid
    void* m_ProcThread = nullptr;      // Win32 thread HANDLE; unused on POSIX
    void* m_ProcStdoutRead = nullptr;  // pipe read end
    void* m_ProcStderrRead = nullptr;  // pipe read end
    std::atomic<bool> m_ChildRunning {false};
    std::atomic<bool> m_ShouldStop {false};
    std::thread m_ReaderThread;

    // Producer/consumer queue of complete log lines. The worker pushes,
    // Tick() pops.
    std::mutex m_LogMutex;
    std::queue<std::string> m_LogQueue;

    // Editor-thread-only file watcher for compiled .js outputs.
    std::unique_ptr<FileSystemWatcher> m_JsWatcher;
    JsChangeHandler m_OnJsChanged;
};
