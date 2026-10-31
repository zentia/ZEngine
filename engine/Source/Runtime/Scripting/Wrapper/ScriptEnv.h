#pragma once
#include "pesapi.h"

#include <functional>
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
    /// `editorDomain` additionally publishes the Editor-domain bindings
    /// (ZScriptDomain::Editor) into this VM's globals. Only the editor-scripting
    /// VM passes true; the game VM stays free of editor API.
    ScriptEnv(Backend* backend, int debugPort = -1, bool editorDomain = false);
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

    /// ReloadModule 的结果（P8 HMR）。`reloaded` 是实际重载过的模块（含
    /// cascade），拓扑序；`hot_replaced` 为 true 表示走了热路径 —— 实例未重建，
    /// 运行时状态保留。定义在类前部：ReloadModule 的返回类型要在它之后可见。
    struct HmrReloadResult
    {
        bool                     ok           = false;
        bool                     hot_replaced = false;
        std::vector<std::string> reloaded;
    };

    /// Load a module if it's not yet cached, or return the cached exports
    /// object. Returns true if the module's exports are now in the cache.
    /// Compile / runtime errors are logged via LOG_ERROR(ZScripting,...).
    bool LoadModule(const std::string& module_id);

    /// Re-read the .js file from disk, drop the previous cache entry, and
    /// re-evaluate. If the module wasn't loaded yet this just calls
    /// LoadModule. Returns true on success.
    HmrReloadResult ReloadModule(const std::string& module_id);

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

    /// 带一个 `GameObject*` 参数的版本：碰撞 / 触发回调要把"撞到谁"交给脚本
    /// （`OnCollisionEnter(other)`）。参数由 `ZScript::Converter<GameObject*>`
    /// 转成脚本侧的 GameObject —— 复用绑定表的同一条路径，脚本拿到的对象与
    /// `gameObject` 属性是同一种东西。
    bool InvokeInstanceMethodGameObject(pesapi_value_ref instance, const char* method_name, class GameObject* other);

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

    // -----------------------------------------------------------------------
    // Engine-context injection (Behaviour binding).
    //
    // `class Foo extends Behaviour` produces a plain JS object, so the only
    // way to give the script `this.gameObject` / `this.transform` is to set
    // those properties on the instance after construction. SetInstanceNative
    // Object does exactly that: it wraps an already-existing C++ pointer into
    // a pesapi object and assigns it to `instance[name]`.
    //
    // `type_id` is the STATIC type to publish (ZScriptTypeId<T>::Get()) -- it
    // must have been define_class'd, otherwise pesapi cannot resolve it. Pass
    // nullptr for `object` to publish null.
    // -----------------------------------------------------------------------
    bool SetInstanceNativeObject(pesapi_value_ref instance, const char* name, const void* type_id, void* object);

    // -----------------------------------------------------------------------
    // P8 HMR (Hot Module Replacement) API.
    //
    // 语义（与 webpack 的"冒泡"模型相比刻意简化，见 TYPESCRIPT_SCRIPTING_DESIGN.md
    // Phase 8）：每个模块在顶层通过 IIFE 注入的 `module.hot` 自选：
    //   module.hot.accept()        —— 声明"我可以热替换"（最常见）
    //   module.hot.accept(cb)      —— 同上，热替换后以新 exports 为实参调 cb
    //   module.hot.dispose(cb)     —— 旧 exports 被丢弃之前调 cb（清定时器 / 监听）
    //   module.hot.decline()       —— 永不热替换，一律走 P6 全量重建
    //
    // ReloadModule 的判定：
    //   1. cascade = {被改的模块} ∪ 所有传递依赖它的模块，按 BFS 序（被改者在前）。
    //      依赖边由模块作用域里的 `require()` 采集。
    //   2. cascade 里**每一个**都 accept 了、且没有一个 decline ⇒ 走热路径；
    //      否则整个 cascade 回落 P6 全量重建（unload + load + 通知观察者）。
    //      不做"部分热替换"：一半热一半重建的状态最难推理，也最难复现。
    //   3. 热路径每个模块依次：重新求值（旧 exports 仍活着）→ 调旧的 accept(newExports)
    //      → 调旧的 dispose → 换掉缓存里的 exports。
    //      **不通知 P6 观察者** ⇒ TypeScriptComponent 不重建 ⇒ 运行时状态保留。
    //      这是 HMR 与 P6 唯一的、也是全部的用户可见差别。
    // -----------------------------------------------------------------------

    /// Returns true if the module opted in to HMR (i.e. accept() was called,
    /// with or without a callback).
    bool HasHmrAcceptCallback(const std::string& module_id) const;

    /// Returns true if the module has explicitly declined HMR (i.e. even
    /// if it called accept(), it should be treated as non-accepting).
    bool HasHmrDeclined(const std::string& module_id) const;

    /// Invoke the dispose callbacks for the given module (if any), then
    /// release them. Called BEFORE the old module cache is dropped.
    void InvokeHmrDisposeCallback(const std::string& module_id);

    /// Invoke the accept callback for the given module, passing the module's
    /// (already swapped-in) exports as the argument.
    ///
    /// 注意：为了"依赖方重新求值时能看到新的被依赖方"，缓存是在 cascade 顺序里
    /// **边求值边换**的，所以 accept 被调用时旧 exports 可能已经被换掉了。回调
    /// 拿到的是显式传进去的新 exports，不需要旧缓存还活着 —— 别按"回调里还能
    /// require 到旧的"来写代码。
    void InvokeHmrAcceptCallback(const std::string& module_id, pesapi_value_ref new_exports);

    /// Clear all HMR state for a module (called on unload / full reload).
    void ClearHmrState(const std::string& module_id);

    /// After a hot-replace (accept path), swap the old module cache entry
    /// for the new one and release the old ref. Returns true on success.
    bool SwapModuleExports(const std::string& module_id, pesapi_value_ref new_exports);

    /// Get the exports object for a loaded module. Returns nullptr if not
    /// loaded. The returned ref is NOT owned by the caller; it is the
    /// cached ref in m_Modules.
    pesapi_value_ref GetModuleExports(const std::string& module_id) const;

    /// Record a dependency edge parent -> child (dedup). Called by the
    /// injected `require` shim.
    void AddModuleDependency(const std::string& parent_module_id, const std::string& child_module_id);

    /// Modules that must be reloaded when `changed_module` changes: itself
    /// plus every transitive dependent, in BFS order (changed module first).
    std::vector<std::string> GetCascadeReloadOrder(const std::string& changed_module);

protected:
    int32_t m_DebugPort;

private:
    void InitApi(int apiVersionExpect);
    void InstallConsoleAndDebugGlobals();
    void ClearModuleCache();
    // assumes scope already open. `cache=false` 时求值结果不进 m_Modules，改由
    // out_ref 交出所有权 —— 热替换需要"旧 exports 还活着的时候先拿到新的"。
    bool LoadModuleInternal(const std::string& module_id, bool cache = true, pesapi_value_ref* out_ref = nullptr);

    // P8：重新求值但不进缓存。out_ref 由调用方负责释放。
    bool EvaluateModuleFresh(const std::string& module_id, pesapi_value_ref& out_ref);

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

    // -------------------------------------------------------------------
    // P8 HMR state.
    // -------------------------------------------------------------------
    struct HmrState
    {
        bool                             accepted = false;   // accept() 调过（带不带 cb 都算）
        pesapi_value_ref                 accept_callback = nullptr;  // fn(newExports)，可为 null
        std::vector<pesapi_value_ref>    dispose_callbacks;          // fn()，可多个
        bool                             declined = false;
    };
    std::unordered_map<std::string, HmrState> m_HmrStates;

    // Module dependency graph: module_id -> modules it directly requires.
    // Populated by the injected `require` shim (which forwards to
    // ZHmrRequireSink). Used for cascade detection.
    std::unordered_map<std::string, std::vector<std::string>> m_ModuleDeps;

    // Reverse edges: module_id -> modules that directly require it. Rebuilt
    // lazily whenever m_ModuleDeps changes.
    std::unordered_map<std::string, std::vector<std::string>> m_ModuleReverseDeps;
    bool                                                     m_ReverseDepsDirty = false;

    void RebuildReverseDeps();

    // 把某个模块当前登记的回调**摘出来**（从 state 里移走，所有权交给返回的
    // 结构）。重新求值会以新代码重新登记 accept/dispose，那时槽位必须是空的，
    // 否则 Register 会去释放一份我们还持着的 ref ⇒ 双释放。
    struct HmrCallbacks
    {
        pesapi_value_ref              accept = nullptr;
        std::vector<pesapi_value_ref> dispose;
    };
    HmrCallbacks TakeHmrCallbacks(const std::string& module_id);
    void         ReleaseHmrCallbacks(HmrCallbacks& cb) const;
    void         InvokeAcceptRef(pesapi_value_ref cb, pesapi_value_ref new_exports);
    void         InvokeDisposeRefs(const std::vector<pesapi_value_ref>& refs);

    // `module.hot.*` 与 `require` 的 native sink。userdata 带 ScriptEnv*。
    static void ZHmrAcceptSink(struct pesapi_ffi* apis, pesapi_callback_info info);
    static void ZHmrDisposeSink(struct pesapi_ffi* apis, pesapi_callback_info info);
    static void ZHmrDeclineSink(struct pesapi_ffi* apis, pesapi_callback_info info);
    static void ZHmrRequireSink(struct pesapi_ffi* apis, pesapi_callback_info info);

    static std::vector<ScriptEnv*> scriptEnvs;
    static bool isInitialized;
    static pesapi_registry registry;
};
