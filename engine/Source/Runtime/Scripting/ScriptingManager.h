#pragma once

#include "Runtime/BaseClasses/Type.h"
#include "Runtime/Core/Base/EngineSystem.h"
#include "Runtime/Scripting/Wrapper/ScriptEnv.h"
#include "Runtime/Utility/Hash128.h"

#include <filesystem>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <vector>

class ProjectInfo;

/**
 * @brief Top-level scripting subsystem (Phase 4).
 *
 * Holds the engine's single ScriptEnv and exposes module-level operations
 * that callers (TypeScriptCompiler hot-reload, Phase-5 TypeScriptComponent)
 * can use without touching pesapi directly.
 *
 * Init order
 * ----------
 *  PreInit:    ProjectInfo
 *  Core:       this -> creates QuickJS env, installs console / Debug shim
 *  Resource+:  TypeScriptComponent (Phase 5) issues LoadModule via this
 *
 * The class deliberately does NOT depend on the .ts source layer; it works
 * purely off the compiled .js files in <Project>/Intermediate/Scripts/.
 * That way Editor and standalone Player share one code path.
 */
class ScriptingManager : public IEngineSystem
{
public:
    ScriptingManager() = default;
    ~ScriptingManager() override = default;

    SystemInitPhase GetInitPhase() const override { return SystemInitPhase::Core; }
    std::vector<std::type_index> GetDependencies() const override;
    bool Initialize() override;
    void Shutdown() override;

    // -----------------------------------------------------------------------
    // Module loader API. These are thin forwarders to ScriptEnv so callers
    // outside Runtime/Scripting/ don't need to include ScriptEnv.h.
    // -----------------------------------------------------------------------

    /// (Re)point the loader at <project>/Intermediate/Scripts/. Called
    /// internally by Initialize(); exposed so the Editor can re-bind it
    /// when the user opens a different project mid-session.
    bool BindToProject(const ProjectInfo& project_info);

    /// Load and cache a module. `module_id` is the path under js_root
    /// without the .js extension (forward slashes; e.g. "Hello", "ai/Boss").
    bool LoadModule(const std::string& module_id);

    /// Re-read the .js from disk and re-evaluate. Drops the prior cache
    /// entry. Idempotent if the module was never loaded.
    bool ReloadModule(const std::string& module_id);

    void UnloadModule(const std::string& module_id);

    bool IsModuleLoaded(const std::string& module_id) const;

    /// Diagnostic helper: invoke `module.exports.<fn_name>()` (no args, no
    /// return). Phase 5 will replace this with proper component lifecycle
    /// invocations that pass `this` and arguments.
    bool InvokeExportedFunction(const std::string& module_id, const char* fn_name);

    // -----------------------------------------------------------------------
    // Phase-5 instance API. These are thin forwarders to ScriptEnv. The
    // returned pesapi_value_ref is owned by the caller (typically a
    // TypeScriptComponent member) and must be released exactly once via
    // DestroyInstance. Returns nullptr on failure (module/class missing,
    // ctor threw, ...). All errors are logged via LOG_ERROR(ZScripting).
    // -----------------------------------------------------------------------

    pesapi_value_ref CreateInstance(const std::string& module_id, const std::string& class_name);
    void DestroyInstance(pesapi_value_ref instance);
    bool InvokeInstanceMethod(pesapi_value_ref instance, const char* method_name);
    bool InvokeInstanceMethodNumber(pesapi_value_ref instance, const char* method_name, double numeric_arg);

    // -----------------------------------------------------------------------
    // Phase-7 serialised-field bridge (forwarders to ScriptEnv).
    //
    // ApplyInstanceField pushes a single (key, value-as-string) override
    // onto the live JS instance using the env's __zApplyField helper. The
    // helper coerces the string based on the existing slot's `typeof`.
    // Returns true iff the field existed (or was newly created from a
    // raw string) and the JS coercion succeeded.
    //
    // EnumerateInstanceFields returns the live instance's editable
    // properties as (name, type_string, current_value_string) triples,
    // for the Inspector's per-field UI. See ScriptEnv for the filtering
    // rules (skips functions, _-prefixed, $-prefixed names).
    // -----------------------------------------------------------------------
    bool ApplyInstanceField(pesapi_value_ref instance,
                            const std::string& key,
                            const std::string& value_str);
    std::vector<std::tuple<std::string, std::string, std::string>>
    EnumerateInstanceFields(pesapi_value_ref instance);

    /// Translate an absolute .js path back to a module id under the current
    /// js_root, or "" if the path is outside the root. Used by the
    /// TypeScriptCompiler watcher hook to map disk events to ReloadModule
    /// calls.
    std::string PathToModuleId(const std::filesystem::path& abs_js_path) const;

    // -----------------------------------------------------------------------
    // Phase-6 hot reload notifications. Observers are invoked on the thread
    // that called ReloadModule (currently the editor main thread, since the
    // TypeScriptCompiler watcher dispatches via the per-frame Tick() drain).
    // The observer should be cheap and non-throwing; for heavy work, queue
    // a follow-up. Re-entrant subscribe/unsubscribe from inside an observer
    // is safe (we copy the list before dispatch).
    //
    // Use case: TypeScriptComponent subscribes once per live instance; when
    // its module is reloaded it tears down the stale pesapi_value_ref and
    // rebuilds the JS instance. State is intentionally NOT preserved
    // (matches Unity's MonoBehaviour reload contract).
    // -----------------------------------------------------------------------
    using ModuleReloadObserver = std::function<void(const std::string& module_id)>;

    /// Register an observer. Returns a non-zero token; pass it to
    /// RemoveModuleReloadObserver to detach. Caller-owned lifetime of any
    /// captured pointers is the caller's responsibility.
    size_t AddModuleReloadObserver(ModuleReloadObserver observer);

    /// Detach a previously-added observer. No-op for unknown tokens.
    void RemoveModuleReloadObserver(size_t token);

    /// Fire all subscribed observers synchronously with the given module id.
    /// Public so the editor's hot-reload bridge can announce a *first-time*
    /// LoadModule (which by itself doesn't touch the observer chain, since
    /// the cache transition is "absent -> present" rather than "present ->
    /// fresh"). Components that subscribed while waiting for tsc to emit
    /// their .js use this notification to retry their bind.
    /// Note: ReloadModule already calls this internally on success; do not
    /// double-notify after a successful ReloadModule.
    void NotifyModuleReloaded(const std::string& module_id);

    /// Per-frame backend Tick (microtasks etc.). Cheap; safe to call when
    /// no script is loaded.
    void Tick();

    /// Direct access to the underlying env. Exposed primarily for Phase-5
    /// component code that needs to manage its own pesapi_value_refs.
    /// Returns nullptr if Initialize() failed (e.g. PAPI_QUICKJS unset).
    ScriptEnv* GetEnv() { return m_ScriptEnv; }

    /// Absolute path of <project>/Intermediate/Scripts/, or empty if no
    /// project is bound. Forward-slash form. Editor uses this to enumerate
    /// already-compiled .js files at startup.
    const std::filesystem::path& GetJsRoot() const { return m_JsRoot; }

protected:
    using ClassHashContainer = std::set<const Type*, Hash128>;
    ClassHashContainer m_RuntimeClassHashes;

private:
    ScriptEnv* m_ScriptEnv = nullptr;
    std::filesystem::path m_JsRoot;

    // Phase-6 reload-observer registry.
    struct ReloadObserverEntry
    {
        size_t token = 0;
        ModuleReloadObserver fn;
    };
    mutable std::mutex m_ReloadObserversMutex;
    std::vector<ReloadObserverEntry> m_ReloadObservers;
    size_t m_NextReloadObserverToken = 1;  // 0 reserved as "invalid"
};
