#pragma once
#include "pesapi.h"

#include <mutex>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

class Backend;
class ObjectPool;

/**
 * @brief Thin C++ wrapper around a single puerts JS env (one VM instance).
 *
 * Owns the env_ref, the FFI table, the QuickJS/V8 backend, and the per-env
 * module cache used by the ZEngine scripting layer.
 *
 * Module loading model
 * --------------------
 * QuickJS shipped with puerts does NOT provide CommonJS `require`. ZEngine
 * implements a minimal "module factory" loader: each .js file under
 * <Project>/Intermediate/Scripts/ is wrapped in a function literal of the
 * form
 *
 *     (function (module, exports) { <USER CODE> })
 *
 * which is `eval`-ed to produce a function value, then immediately invoked
 * with a freshly-allocated `module = { exports: {} }`. The returned
 * `module.exports` is stored as a strong pesapi_value_ref in m_Modules so
 * subsequent `LoadModule(same id)` returns the cached exports without
 * re-running the script.
 *
 * Re-running (hot reload) goes through ReloadModule(): the prior cache
 * entry is released, the file is re-read from disk, and the IIFE is
 * re-evaluated. Any TypeScriptComponent instances that captured the old
 * exports in their `OnAwake/OnStart` slots will be re-instantiated by
 * Phase 5; this layer just makes sure the fresh exports object is
 * authoritative.
 *
 * Threading
 * ---------
 * pesapi (and especially QuickJS) is NOT thread-safe. All Eval/LoadModule/
 * Invoke calls must come from the engine main thread.
 */
class ScriptEnv
{
public:
    ScriptEnv(Backend* backend, int debugPort = -1);
    ~ScriptEnv();

    void Eval(std::string& chunk, const char* chunkName = "chunk");
    void Tick();
    void WaitDebugger();

    // -----------------------------------------------------------------------
    // Backend-internal accessors. These are not part of the public scripting
    // surface; they exist so a Backend subclass (currently BackendV8 for
    // V8 Inspector) can introspect the live env handle that ScriptEnv owns.
    // Do not call from gameplay or editor code -- use the Eval / Invoke /
    // module APIs above instead.
    // -----------------------------------------------------------------------
    pesapi_env_ref GetEnvRef() const { return m_EnvRef; }
    struct pesapi_ffi* GetPapis() const { return m_Papis; }

    // -----------------------------------------------------------------------
    // P4 module loader API.
    //
    // `module_id` is the dotted/slashed path of the .js file relative to the
    // js root (Intermediate/Scripts/), without the .js suffix. For example
    // a file at Intermediate/Scripts/foo/Bar.js has module_id "foo/Bar".
    // -----------------------------------------------------------------------

    /// Set the directory under which LoadModule resolves module ids to
    /// .js files. Must be called before any LoadModule(). Calling again
    /// invalidates the entire module cache (all stored exports refs are
    /// released).
    void SetJsRoot(const std::string& js_root_utf8);

    const std::string& GetJsRoot() const { return m_JsRoot; }

    /// Load a module if it's not yet cached, or return the cached exports
    /// object. Returns true if the module's exports are now in the cache.
    /// Compile / runtime errors are logged via LOG_ERROR(ZScripting,...).
    bool LoadModule(const std::string& module_id);

    /// Re-read the .js file from disk, drop the previous cache entry, and
    /// re-evaluate. If the module wasn't loaded yet this just calls
    /// LoadModule. Returns true on success.
    bool ReloadModule(const std::string& module_id);

    /// Drop the cached exports for the module id (does not affect any
    /// JS-side state; once nobody else references the old exports it
    /// becomes garbage-collectable).
    void UnloadModule(const std::string& module_id);

    bool IsModuleLoaded(const std::string& module_id) const;

    /// Diagnostic: invoke `module.exports.<fn_name>()` on a previously
    /// loaded module. No arguments, no return value plumbing (Phase 5 will
    /// extend this for component lifecycle methods). Returns true if the
    /// function existed and was called without throwing.
    bool InvokeExportedFunction(const std::string& module_id, const char* fn_name);

    // -----------------------------------------------------------------------
    // P5 instance API.
    //
    // QuickJS via pesapi does not expose a `new` operator directly. We
    // synthesize one with a tiny JS helper (`__zNewInstance`) installed
    // alongside the console shim. CreateInstance pulls the class function
    // from the module exports, hands it to that helper, and stores a
    // strong ref to the resulting object.
    //
    // The returned handle is opaque; callers MUST pair every successful
    // CreateInstance with a single DestroyInstance to release the ref.
    // pesapi_value_ref is what we hand back so TypeScriptComponent can
    // store it directly (no extra heap indirection).
    // -----------------------------------------------------------------------

    /// Construct `new module.exports[class_name]()` and return a strong
    /// reference. Returns nullptr on failure (module not loaded, class
    /// missing, JS-side throw, ...). Logs the cause via LOG_ERROR.
    pesapi_value_ref CreateInstance(const std::string& module_id, const std::string& class_name);

    /// Release a handle returned by CreateInstance. Safe to call with
    /// nullptr.
    void DestroyInstance(pesapi_value_ref instance);

    /// Invoke `instance.<method_name>(...)` if such a property exists and
    /// is callable. `numeric_arg` is the only argument plumbed today (used
    /// by OnUpdate(dt: number)); pass NaN to mean "no args". Returns true
    /// if the call succeeded (which includes "method not present" - that's
    /// a no-op, not an error, mirroring Unity's MonoBehaviour where the
    /// engine just skips missing lifecycle hooks).
    bool InvokeInstanceMethod(pesapi_value_ref instance, const char* method_name);
    bool InvokeInstanceMethodNumber(pesapi_value_ref instance, const char* method_name, double numeric_arg);

    // -----------------------------------------------------------------------
    // P7 serialised-field bridge.
    //
    // The component layer stores Inspector-authored field overrides as
    // (key, value-string) pairs. ApplyInstanceField pushes one pair at
    // CreateInstance time (or on demand from the Inspector when the user
    // edits a value live). It defers to a JS-side __zApplyField helper
    // installed alongside __zNewInstance; the helper inspects the
    // existing field's typeof to pick the right coercion (number ->
    // parseFloat, boolean -> "true"/"1" predicate, string -> verbatim).
    //
    // EnumerateInstanceFields returns the live instance's editable
    // properties so the Inspector can build a UI for them on the fly.
    // Skips function-typed properties (i.e. methods on the prototype
    // chain) and names beginning with `_` / `$` (convention for "private,
    // don't surface in editor"). Order matches Object.keys(). The tuple
    // format is (name, typeof_string, current_value_string).
    // -----------------------------------------------------------------------
    bool ApplyInstanceField(pesapi_value_ref instance, const std::string& key, const std::string& value_str);
    std::vector<std::tuple<std::string, std::string, std::string>>
    EnumerateInstanceFields(pesapi_value_ref instance);

protected:
    int32_t m_DebugPort;

private:
    void InitApi(int apiVersionExpect);
    void InstallConsoleAndDebugGlobals();
    void ClearModuleCache();
    bool LoadModuleInternal(const std::string& module_id);  // assumes scope already open

    Backend* m_Backend;
    pesapi_env_ref m_EnvRef;
    struct pesapi_ffi* m_Papis;
    std::mutex m_Mutex;
    int m_Index;
    ObjectPool* m_ObjectPool;

    // Project-supplied root dir for module resolution (absolute, OS path).
    // Stored as std::string (UTF-8) so we don't drag <filesystem> into the
    // public header. ScriptingManager translates from std::filesystem::path.
    std::string m_JsRoot;

    // module_id -> strong reference to the exports object. Released in
    // ReloadModule / UnloadModule / dtor.
    std::unordered_map<std::string, pesapi_value_ref> m_Modules;

    static std::vector<ScriptEnv*> scriptEnvs;
    static bool isInitialized;
    static pesapi_registry registry;
};
