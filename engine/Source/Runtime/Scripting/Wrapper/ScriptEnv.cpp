#include "ScriptEnv.h"

#include "Runtime/Scripting/Backend.h"
#include "Runtime/Scripting/Native/PuertsNative.h"
#include "Runtime/Scripting/Wrapper/ObjectPool.h"
#include "Runtime/Scripting/Wrapper/Puerts.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

std::vector<ScriptEnv*> ScriptEnv::scriptEnvs;
bool ScriptEnv::isInitialized = false;
pesapi_registry ScriptEnv::registry = nullptr;

static void ZLogCallback(const char* msg)
{
    LOG_INFO(ZEngine, msg);
}

static void ZLogWarningCallback(const char* msg)
{
    LOG_WARNING(ZEngine, msg);
}

static void ZLogErrorCallback(const char* msg)
{
    LOG_ERROR(ZEngine, msg);
}

// ---------------------------------------------------------------------------
// Console / Debug globals
// ---------------------------------------------------------------------------
//
// QuickJS doesn't ship a `console` object. ScriptingManager's bootstrap
// previously installed a JS-only shim that forwarded to `print`, but `print`
// is itself only present after we hook our LogCallback. We replace that
// shim with a pair of *native* sinks (`__zlog_native_*`) that talk directly
// to the engine logger. JS-side `console` and Unity-style `Debug` simply
// stringify their arguments with the standard `Array.prototype.join` / JSON
// shim and delegate.
//
// Why a native shim instead of leaning on `print` again: with native sinks
// we can route warnings and errors to LOG_WARNING / LOG_ERROR so the editor
// console can colour them appropriately, instead of all messages collapsing
// onto INFO.
//
// All three callbacks share the same body (only the LOG_* macro differs)
// so they're written as a single helper.

namespace
{
    enum class LogLevel
    {
        Info,
        Warning,
        Error
    };

    void zEmit(LogLevel level, const char* msg)
    {
        if (!msg)
            msg = "";
        switch (level)
        {
            case LogLevel::Info:
                LOG_INFO(ZScripting, msg);
                break;
            case LogLevel::Warning:
                LOG_WARNING(ZScripting, msg);
                break;
            case LogLevel::Error:
                LOG_ERROR(ZScripting, msg);
                break;
        }
    }

    // The native callback receives a single utf-8 string already joined on
    // the JS side. We extract it and forward to zEmit. The function takes
    // its level via the `data` pointer (cast from intptr_t).
    void zNativeLogCallback(struct pesapi_ffi* apis, pesapi_callback_info info)
    {
        const auto level = static_cast<LogLevel>(reinterpret_cast<intptr_t>(pesapi_get_userdata(apis, info)));

        const int argc = pesapi_get_args_len(apis, info);
        std::string joined;
        joined.reserve(64);
        auto env = pesapi_get_env(apis, info);
        for (int i = 0; i < argc; ++i)
        {
            pesapi_value v = pesapi_get_arg(apis, info, i);
            if (i > 0)
                joined.push_back(' ');
            // Use stack buffer first; fall back to heap if longer.
            char stack_buf[512];
            size_t len = sizeof(stack_buf) - 1;
            const char* s = nullptr;
            if (v && pesapi_is_string(apis, env, v))
            {
                s = pesapi_get_value_string_utf8(apis, env, v, stack_buf, &len);
                if (s)
                {
                    // pesapi may have written nul-terminated; rely on len.
                    joined.append(s, len);
                }
            }
            else
            {
                // Non-string arg: rely on JS-side stringification done in the
                // `console.log` shim. If it slipped through (e.g. user called
                // __zlog_native_info directly), best-effort: skip.
                joined.append("[non-string]");
            }
        }
        zEmit(level, joined.c_str());
    }
}  // namespace

ScriptEnv::ScriptEnv(Backend* backend, int debugPort)
{
    m_Backend = backend;
    if (!isInitialized)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (!isInitialized)
        {
            SetLogCallback(ZLogCallback, ZLogWarningCallback, ZLogErrorCallback);
            struct pesapi_registry_api* reg_api = static_cast<pesapi_registry_api*>(GetRegisterApi());
            registry = reg_api->create_registry();
            Puerts::InitialPuerts(reg_api, registry);
            isInitialized = true;
        }
    }

    const int libVersionExpect = 11;
    int libVersion = GetPapiVersion();
    if (libVersion != libVersionExpect)
    {
        LOG_FATAL(ZEngine, "expect lib version {}, but got {}", libVersionExpect, libVersion);
    }
    InitApi(libVersionExpect);

    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Index = scriptEnvs.size();
        scriptEnvs.push_back(this);
    }

    m_ObjectPool = new ObjectPool();

    auto&& scope = pesapi_open_scope(m_Papis, m_EnvRef);
    auto&& env = pesapi_get_env_from_ref(m_Papis, m_EnvRef);
    auto&& moduleExecutorFunc = m_Backend->GetModuleExecutor(env);
    (void)moduleExecutorFunc;
    pesapi_close_scope(m_Papis, scope);

    // OnEnter must run BEFORE OpenRemoteDebugger so the backend can capture
    // its live VM handle (e.g. v8::Isolate*) from ScriptEnv before the
    // inspector tries to use it. BackendQuickJS / BackendV8 stash whatever
    // they need inside OnEnter. The C#-side puerts BackendV8 doesn't need
    // this dance because its `isolate` field is populated at construction
    // time; our pesapi-only Backend types only learn about ScriptEnv here.
    m_Backend->OnEnter(this);

    if (debugPort != -1)
    {
        m_Backend->OpenRemoteDebugger(debugPort);
    }
    m_DebugPort = debugPort;

    // Inject console/Debug globals after the backend has finished setting
    // up the env. This must happen before any user script tries to call
    // console.log (which it will - Hello.ts does).
    InstallConsoleAndDebugGlobals();
}

ScriptEnv::~ScriptEnv()
{
    // Symmetric with the constructor: tear the inspector down BEFORE
    // releasing the env (the inspector holds raw pointers into the V8
    // isolate / context), then drop the module cache, then destroy the
    // env_ref. Quickjs CloseRemoteDebugger is a no-op so this is safe
    // for both backends.
    if (m_DebugPort != -1)
    {
        m_Backend->CloseRemoteDebugger();
    }
    ClearModuleCache();
    m_Backend->DestroyEnvRef(m_EnvRef);
}

void ScriptEnv::InitApi(int apiVersionExpect)
{
    m_EnvRef = m_Backend->CreateEnvRef();
    m_Papis = m_Backend->GetApi();
    if (m_Backend->GetApiVersion() != apiVersionExpect)
    {
        LOG_FATAL(ZEngine, "backend: version not match for {}, expect {}, but got {}", m_Backend->GetName(), apiVersionExpect, m_Backend->GetApiVersion());
    }
}

// ---------------------------------------------------------------------------
// console + Debug global injection
// ---------------------------------------------------------------------------
void ScriptEnv::InstallConsoleAndDebugGlobals()
{
    auto scope = pesapi_open_scope(m_Papis, m_EnvRef);
    auto env = pesapi_get_env_from_ref(m_Papis, m_EnvRef);
    auto global = pesapi_global(m_Papis, env);

    // 1) Native sinks (one per level). userdata encodes the level so all
    //    three share a single C function.
    auto installSink = [&](const char* name, LogLevel lvl) {
        pesapi_value f = pesapi_create_function(
            m_Papis,
            env,
            zNativeLogCallback,
            reinterpret_cast<void*>(static_cast<intptr_t>(lvl)),
            /*finalize=*/nullptr);
        pesapi_set_property(m_Papis, env, global, name, f);
    };
    installSink("__zlog_native_info", LogLevel::Info);
    installSink("__zlog_native_warning", LogLevel::Warning);
    installSink("__zlog_native_error", LogLevel::Error);

    // 2) JS-side shim. Stringify each argument (so callers can pass numbers,
    //    objects, etc.) and forward to the native sink as a single joined
    //    string. We deliberately avoid a real `JSON.stringify` for objects
    //    here because puerts' QuickJS may not have JSON wired in some bind
    //    configurations - String() always works.
    //
    //    `Debug.Log` is the Unity-style alias TS code is most likely to call.
    //    Both branches end up at the same native sink.
    static const char kShim[] =
        "(function(g) {"
        "  function stringifyArg(v) {"
        "    if (v === null) return 'null';"
        "    if (v === undefined) return 'undefined';"
        "    try { return String(v); } catch (e) { return '[unstringifiable]'; }"
        "  }"
        "  function makeFwd(sink) {"
        "    return function() {"
        "      var parts = new Array(arguments.length);"
        "      for (var i = 0; i < arguments.length; ++i) parts[i] = stringifyArg(arguments[i]);"
        "      sink(parts.join(' '));"
        "    };"
        "  }"
        "  g.console = {"
        "    log:   makeFwd(g.__zlog_native_info),"
        "    info:  makeFwd(g.__zlog_native_info),"
        "    debug: makeFwd(g.__zlog_native_info),"
        "    warn:  makeFwd(g.__zlog_native_warning),"
        "    error: makeFwd(g.__zlog_native_error)"
        "  };"
        "  g.Debug = {"
        "    Log:        makeFwd(g.__zlog_native_info),"
        "    LogWarning: makeFwd(g.__zlog_native_warning),"
        "    LogError:   makeFwd(g.__zlog_native_error)"
        "  };"
        "  g.print = makeFwd(g.__zlog_native_info);"
        // P5: pesapi has no 'new' operator, so we synthesize one. C++ side
        // calls __zNewInstance(ctor) to invoke `new ctor()`. Returning the
        // resulting object lets the caller wrap it in a strong ref.
        "  g.__zNewInstance = function(ctor) {"
        "    if (typeof ctor !== 'function') {"
        "      throw new Error('__zNewInstance: not a constructor');"
        "    }"
        "    return new ctor();"
        "  };"
        // P7: serialised-field bridge. C++ side never touches user JS
        // objects directly -- it hands a (instance, key, valueStr) triple
        // to __zApplyField, which inspects the existing slot's typeof
        // (set by the class's field-initialiser, which has already run by
        // the time this is called) and coerces the string accordingly.
        // Returns true on success, false on parse failure (caller logs).
        "  g.__zApplyField = function(instance, key, valueStr) {"
        "    if (instance === null || typeof instance !== 'object') return false;"
        "    var existing = instance[key];"
        "    var t = typeof existing;"
        "    try {"
        "      if (t === 'number') {"
        "        var n = parseFloat(valueStr);"
        "        if (Number.isNaN(n)) return false;"
        "        instance[key] = n;"
        "      } else if (t === 'boolean') {"
        "        instance[key] = (valueStr === 'true' || valueStr === '1');"
        "      } else if (t === 'string') {"
        "        instance[key] = String(valueStr);"
        "      } else if (t === 'undefined') {"
        // No initialiser -> default to string. This is also the path
        // taken when a field is added in TS but the component's stored
        // override predates the field; we just push the raw string and
        // let the user re-type if needed.
        "        instance[key] = String(valueStr);"
        "      } else {"
        // Object/array fields: out of scope for P7 overrides (would need
        // JSON parse + structural validation). Intentionally drop on the
        // floor, surfaced by the C++ caller's logger.
        "        return false;"
        "      }"
        "      return true;"
        "    } catch (e) { return false; }"
        "  };"
        // P7: introspection helper. Returns a packed string of the form
        // "name1\\u0001type1\\u0001default1\\u0002name2\\u0001..."  using
        // 0x01 / 0x02 as field/record separators (chosen so they survive
        // the pesapi string FFI without fancy array marshalling). The C++
        // side splits on these. We deliberately list ONLY own-enumerable
        // properties (so prototype methods like OnUpdate don't leak in)
        // and skip private-by-convention names starting with `_` or `$`
        // (they're conventionally "engine internals" -- matches Unity's
        // serialiser default).
        "  g.__zEnumerateFields = function(instance) {"
        "    if (instance === null || typeof instance !== 'object') return '';"
        "    var keys = Object.keys(instance);"
        "    var parts = [];"
        "    for (var i = 0; i < keys.length; ++i) {"
        "      var k = keys[i];"
        "      if (k.length === 0 || k.charAt(0) === '_' || k.charAt(0) === '$') continue;"
        "      var v = instance[k];"
        "      var t = typeof v;"
        "      if (t === 'function') continue;"
        "      var s;"
        "      try { s = (v === null || t === 'undefined') ? '' : String(v); }"
        "      catch (e) { s = ''; }"
        "      parts.push(k + '\\u0001' + t + '\\u0001' + s);"
        "    }"
        "    return parts.join('\\u0002');"
        "  };"
        // Minimal Unity-MonoBehaviour-compatible Behaviour base. Real
        // gameObject / transform plumbing comes later (P5 binding generator);
        // this just lets `class Foo extends Behaviour` parse and instantiate
        // without referencing an external module.
        "  g.Behaviour = function() {};"
        "  g.Behaviour.prototype.OnAwake = function() {};"
        "  g.Behaviour.prototype.OnStart = function() {};"
        "  g.Behaviour.prototype.OnUpdate = function(dt) {};"
        "  g.Behaviour.prototype.OnDestroy = function() {};"
        "})(globalThis);";
    pesapi_eval(m_Papis, env, reinterpret_cast<const uint8_t*>(kShim), sizeof(kShim) - 1, "<console_shim>");
    if (pesapi_has_caught(m_Papis, scope))
    {
        const char* msg = pesapi_get_exception_as_string(m_Papis, scope, 1);
        LOG_ERROR(ZScripting, "console shim install failed: {}", msg ? msg : "<no message>");
    }

    pesapi_close_scope(m_Papis, scope);
}

// ---------------------------------------------------------------------------
// Module loader
// ---------------------------------------------------------------------------
void ScriptEnv::SetJsRoot(const std::string& js_root_utf8)
{
    if (m_JsRoot == js_root_utf8)
        return;
    ClearModuleCache();
    m_JsRoot = js_root_utf8;
    LOG_INFO(ZScripting, "ScriptEnv: js root set to '{}'", m_JsRoot);
}

void ScriptEnv::ClearModuleCache()
{
    if (m_Modules.empty())
        return;
    for (auto& kv : m_Modules)
    {
        if (kv.second)
            pesapi_release_value_ref(m_Papis, kv.second);
    }
    m_Modules.clear();
}

bool ScriptEnv::IsModuleLoaded(const std::string& module_id) const
{
    return m_Modules.find(module_id) != m_Modules.end();
}

void ScriptEnv::UnloadModule(const std::string& module_id)
{
    auto it = m_Modules.find(module_id);
    if (it == m_Modules.end())
        return;
    if (it->second)
        pesapi_release_value_ref(m_Papis, it->second);
    m_Modules.erase(it);
    LOG_INFO(ZScripting, "module unloaded: {}", module_id);
}

bool ScriptEnv::ReloadModule(const std::string& module_id)
{
    UnloadModule(module_id);
    return LoadModule(module_id);
}

bool ScriptEnv::LoadModule(const std::string& module_id)
{
    if (m_JsRoot.empty())
    {
        LOG_ERROR(ZScripting, "LoadModule('{}'): js root not set; call SetJsRoot first", module_id);
        return false;
    }
    if (m_Modules.find(module_id) != m_Modules.end())
        return true;  // already cached

    auto scope = pesapi_open_scope(m_Papis, m_EnvRef);
    bool ok = LoadModuleInternal(module_id);
    if (pesapi_has_caught(m_Papis, scope))
    {
        const char* msg = pesapi_get_exception_as_string(m_Papis, scope, 1);
        LOG_ERROR(ZScripting, "LoadModule('{}') threw: {}", module_id, msg ? msg : "<no message>");
        ok = false;
    }
    pesapi_close_scope(m_Papis, scope);
    return ok;
}

bool ScriptEnv::LoadModuleInternal(const std::string& module_id)
{
    // 1. Resolve module_id -> on-disk .js path.
    std::string js_path = m_JsRoot;
    if (!js_path.empty() && js_path.back() != '/' && js_path.back() != '\\')
        js_path.push_back('/');
    js_path.append(module_id);
    js_path.append(".js");

    // 2. Read source. Use binary mode so we don't translate CRLF and
    //    introduce off-by-one offsets in error messages.
    std::ifstream ifs(js_path, std::ios::binary);
    if (!ifs)
    {
        LOG_ERROR(ZScripting, "LoadModule('{}'): cannot open '{}'", module_id, js_path);
        return false;
    }
    std::ostringstream oss;
    oss << ifs.rdbuf();
    std::string raw = oss.str();

    // 3. Wrap in IIFE-factory so we get a function value back. The QuickJS
    //    in puerts doesn't expose CommonJS `require`, so we synthesize one
    //    pair of `module` / `exports` per call.
    //
    //    NOTE: tsc emits CommonJS-style `Object.defineProperty(exports, ...)`
    //    and `exports.Foo = ...` calls into the body, which is exactly what
    //    this wrapper supports. ESM-style `export class Foo {}` would NOT
    //    work here, but tsconfig is pinned to "module": "commonjs".
    std::string wrapped;
    wrapped.reserve(raw.size() + 64);
    wrapped.append("(function(module, exports){\n");
    wrapped.append(raw);
    wrapped.append("\n;return module.exports;})");

    auto env = pesapi_get_env_from_ref(m_Papis, m_EnvRef);

    pesapi_value factory = pesapi_eval(
        m_Papis, env, reinterpret_cast<const uint8_t*>(wrapped.data()), wrapped.size(), js_path.c_str());

    if (!factory || !pesapi_is_function(m_Papis, env, factory))
    {
        LOG_ERROR(ZScripting, "LoadModule('{}'): factory eval did not yield a function", module_id);
        return false;
    }

    // 4. Build module = { exports: {} }
    pesapi_value module_obj = pesapi_create_object(m_Papis, env);
    pesapi_value exports_obj = pesapi_create_object(m_Papis, env);
    pesapi_set_property(m_Papis, env, module_obj, "exports", exports_obj);

    // 5. Call factory(module, exports). The factory's `return module.exports`
    //    gives us the live exports object even if user code reassigned
    //    `module.exports = ...`.
    const pesapi_value args[2] = {module_obj, exports_obj};
    pesapi_value resolved_exports = pesapi_call_function(
        m_Papis, env, factory, /*this=*/nullptr, 2, args);

    if (!resolved_exports)
    {
        // call_function returned null/undefined while no exception caught -
        // fall back to the original exports.
        resolved_exports = pesapi_get_property(m_Papis, env, module_obj, "exports");
    }
    if (!resolved_exports)
    {
        LOG_ERROR(ZScripting, "LoadModule('{}'): module.exports came back null", module_id);
        return false;
    }

    // 6. Store a strong reference. internal_field_count=0 because we don't
    //    need to attach native private data to the exports object itself.
    pesapi_value_ref ref = pesapi_create_value_ref(m_Papis, env, resolved_exports, 0);
    if (!ref)
    {
        LOG_ERROR(ZScripting, "LoadModule('{}'): create_value_ref failed", module_id);
        return false;
    }
    m_Modules[module_id] = ref;

    LOG_INFO(ZScripting, "module loaded: {} (from {})", module_id, js_path);
    return true;
}

bool ScriptEnv::InvokeExportedFunction(const std::string& module_id, const char* fn_name)
{
    auto it = m_Modules.find(module_id);
    if (it == m_Modules.end())
    {
        LOG_ERROR(ZScripting, "InvokeExportedFunction: module '{}' not loaded", module_id);
        return false;
    }
    if (!fn_name || !*fn_name)
        return false;

    auto scope = pesapi_open_scope(m_Papis, m_EnvRef);
    auto env = pesapi_get_env_from_ref(m_Papis, m_EnvRef);

    pesapi_value exports_val = pesapi_get_value_from_ref(m_Papis, env, it->second);
    if (!exports_val)
    {
        LOG_ERROR(ZScripting, "InvokeExportedFunction('{}','{}'): exports value is null", module_id, fn_name);
        pesapi_close_scope(m_Papis, scope);
        return false;
    }
    pesapi_value fn = pesapi_get_property(m_Papis, env, exports_val, fn_name);
    if (!fn || !pesapi_is_function(m_Papis, env, fn))
    {
        LOG_ERROR(ZScripting, "InvokeExportedFunction('{}','{}'): export is not a function", module_id, fn_name);
        pesapi_close_scope(m_Papis, scope);
        return false;
    }
    pesapi_call_function(m_Papis, env, fn, /*this=*/nullptr, 0, nullptr);
    bool ok = true;
    if (pesapi_has_caught(m_Papis, scope))
    {
        const char* msg = pesapi_get_exception_as_string(m_Papis, scope, 1);
        LOG_ERROR(ZScripting, "InvokeExportedFunction('{}','{}') threw: {}", module_id, fn_name, msg ? msg : "<no message>");
        ok = false;
    }
    pesapi_close_scope(m_Papis, scope);
    return ok;
}

// ---------------------------------------------------------------------------
// P5 instance API
// ---------------------------------------------------------------------------
//
// Why not bind a pesapi_class via pesapi_define_class? That path is for
// native-side classes that get *exposed* to JS. We're going the other way:
// instantiating user-authored JS classes from C++. With QuickJS-only-via-
// pesapi we can't issue a `new` directly, so we eval one tiny helper at
// shim install time:
//
//     globalThis.__zNewInstance = function(ctor) { return new ctor(); }
//
// CreateInstance grabs the class function from module exports, looks up
// __zNewInstance, calls it with the class as the only arg, and stores a
// strong ref to the resulting object.

pesapi_value_ref ScriptEnv::CreateInstance(const std::string& module_id, const std::string& class_name)
{
    auto it = m_Modules.find(module_id);
    if (it == m_Modules.end())
    {
        LOG_ERROR(ZScripting, "CreateInstance: module '{}' not loaded", module_id);
        return nullptr;
    }
    if (class_name.empty())
    {
        LOG_ERROR(ZScripting, "CreateInstance('{}'): empty class_name", module_id);
        return nullptr;
    }

    auto scope = pesapi_open_scope(m_Papis, m_EnvRef);
    auto env = pesapi_get_env_from_ref(m_Papis, m_EnvRef);

    pesapi_value_ref result_ref = nullptr;
    do
    {
        pesapi_value exports_val = pesapi_get_value_from_ref(m_Papis, env, it->second);
        if (!exports_val)
        {
            LOG_ERROR(ZScripting, "CreateInstance('{}','{}'): exports is null", module_id, class_name);
            break;
        }
        pesapi_value ctor = pesapi_get_property(m_Papis, env, exports_val, class_name.c_str());
        if (!ctor || !pesapi_is_function(m_Papis, env, ctor))
        {
            LOG_ERROR(ZScripting, "CreateInstance('{}','{}'): export is not a class/function", module_id, class_name);
            break;
        }
        pesapi_value global = pesapi_global(m_Papis, env);
        pesapi_value newer = pesapi_get_property(m_Papis, env, global, "__zNewInstance");
        if (!newer || !pesapi_is_function(m_Papis, env, newer))
        {
            LOG_ERROR(ZScripting, "CreateInstance: __zNewInstance helper missing - shim install failed?");
            break;
        }
        const pesapi_value args[1] = {ctor};
        pesapi_value instance = pesapi_call_function(m_Papis, env, newer,
                                                     /*this=*/nullptr,
                                                     1,
                                                     args);
        if (pesapi_has_caught(m_Papis, scope))
        {
            const char* msg = pesapi_get_exception_as_string(m_Papis, scope, 1);
            LOG_ERROR(ZScripting, "CreateInstance('{}','{}') threw: {}", module_id, class_name, msg ? msg : "<no message>");
            break;
        }
        if (!instance)
        {
            LOG_ERROR(ZScripting, "CreateInstance('{}','{}'): instantiation returned null", module_id, class_name);
            break;
        }
        result_ref = pesapi_create_value_ref(m_Papis, env, instance, 0);
        if (!result_ref)
        {
            LOG_ERROR(ZScripting, "CreateInstance('{}','{}'): create_value_ref failed", module_id, class_name);
            break;
        }
        LOG_INFO(ZScripting, "instance created: {}.{}", module_id, class_name);
    } while (false);

    pesapi_close_scope(m_Papis, scope);
    return result_ref;
}

void ScriptEnv::DestroyInstance(pesapi_value_ref instance)
{
    if (!instance)
        return;
    pesapi_release_value_ref(m_Papis, instance);
}

bool ScriptEnv::InvokeInstanceMethod(pesapi_value_ref instance, const char* method_name)
{
    if (!instance || !method_name || !*method_name)
        return false;

    auto scope = pesapi_open_scope(m_Papis, m_EnvRef);
    auto env = pesapi_get_env_from_ref(m_Papis, m_EnvRef);

    pesapi_value this_val = pesapi_get_value_from_ref(m_Papis, env, instance);
    if (!this_val)
    {
        pesapi_close_scope(m_Papis, scope);
        return false;
    }
    pesapi_value fn = pesapi_get_property(m_Papis, env, this_val, method_name);
    bool ok = true;
    // Missing method is a no-op success (matches Unity's MonoBehaviour),
    // not an error. Only an actual throw counts as failure.
    if (fn && pesapi_is_function(m_Papis, env, fn))
    {
        pesapi_call_function(m_Papis, env, fn, this_val, 0, nullptr);
        if (pesapi_has_caught(m_Papis, scope))
        {
            const char* msg = pesapi_get_exception_as_string(m_Papis, scope, 1);
            LOG_ERROR(ZScripting, "instance.{} threw: {}", method_name, msg ? msg : "<no message>");
            ok = false;
        }
    }
    pesapi_close_scope(m_Papis, scope);
    return ok;
}

bool ScriptEnv::InvokeInstanceMethodNumber(pesapi_value_ref instance, const char* method_name, double numeric_arg)
{
    if (!instance || !method_name || !*method_name)
        return false;

    auto scope = pesapi_open_scope(m_Papis, m_EnvRef);
    auto env = pesapi_get_env_from_ref(m_Papis, m_EnvRef);

    pesapi_value this_val = pesapi_get_value_from_ref(m_Papis, env, instance);
    if (!this_val)
    {
        pesapi_close_scope(m_Papis, scope);
        return false;
    }
    pesapi_value fn = pesapi_get_property(m_Papis, env, this_val, method_name);
    bool ok = true;
    if (fn && pesapi_is_function(m_Papis, env, fn))
    {
        pesapi_value arg_val = pesapi_create_double(m_Papis, env, numeric_arg);
        const pesapi_value args[1] = {arg_val};
        pesapi_call_function(m_Papis, env, fn, this_val, 1, args);
        if (pesapi_has_caught(m_Papis, scope))
        {
            const char* msg = pesapi_get_exception_as_string(m_Papis, scope, 1);
            LOG_ERROR(ZScripting, "instance.{}({}) threw: {}", method_name, numeric_arg, msg ? msg : "<no message>");
            ok = false;
        }
    }
    pesapi_close_scope(m_Papis, scope);
    return ok;
}

// ---------------------------------------------------------------------------
// P7 serialised-field bridge
// ---------------------------------------------------------------------------
//
// Both helpers below funnel through globalThis.__zApplyField /
// __zEnumerateFields, which are installed by InstallConsoleAndDebugGlobals
// at env construction. The actual coercion / introspection logic lives in
// JS so we don't have to special-case 'number'/'boolean'/'string' on the
// C++ side -- typeof is the natural way to ask "what's the user's
// initialiser default for this slot?".

bool ScriptEnv::ApplyInstanceField(pesapi_value_ref instance,
                                   const std::string& key,
                                   const std::string& value_str)
{
    if (!instance || key.empty())
        return false;

    auto scope = pesapi_open_scope(m_Papis, m_EnvRef);
    auto env = pesapi_get_env_from_ref(m_Papis, m_EnvRef);

    bool ok = false;
    do
    {
        pesapi_value this_val = pesapi_get_value_from_ref(m_Papis, env, instance);
        if (!this_val)
            break;

        pesapi_value global = pesapi_global(m_Papis, env);
        pesapi_value applier = pesapi_get_property(m_Papis, env, global, "__zApplyField");
        if (!applier || !pesapi_is_function(m_Papis, env, applier))
        {
            LOG_ERROR(ZScripting,
                      "ApplyInstanceField('{}'): __zApplyField helper missing",
                      key);
            break;
        }

        pesapi_value k_val = pesapi_create_string_utf8(m_Papis, env, key.data(), key.size());
        pesapi_value v_val = pesapi_create_string_utf8(m_Papis, env, value_str.data(), value_str.size());
        const pesapi_value args[3] = {this_val, k_val, v_val};
        pesapi_value rv = pesapi_call_function(m_Papis, env, applier,
                                               /*this=*/nullptr,
                                               3,
                                               args);
        if (pesapi_has_caught(m_Papis, scope))
        {
            const char* msg = pesapi_get_exception_as_string(m_Papis, scope, 1);
            LOG_ERROR(ZScripting, "ApplyInstanceField('{}'='{}') threw: {}", key, value_str, msg ? msg : "<no message>");
            break;
        }
        if (rv && pesapi_is_boolean(m_Papis, env, rv))
        {
            ok = pesapi_get_value_bool(m_Papis, env, rv) != 0;
        }
        else
        {
            // __zApplyField always returns a boolean; if it didn't,
            // something replaced the helper -- treat as failure.
            ok = false;
        }
        if (!ok)
        {
            LOG_WARNING(ZScripting,
                        "ApplyInstanceField: could not coerce '{}'='{}' "
                        "(field absent or non-scalar)",
                        key,
                        value_str);
        }
    } while (false);

    pesapi_close_scope(m_Papis, scope);
    return ok;
}

std::vector<std::tuple<std::string, std::string, std::string>>
ScriptEnv::EnumerateInstanceFields(pesapi_value_ref instance)
{
    std::vector<std::tuple<std::string, std::string, std::string>> out;
    if (!instance)
        return out;

    auto scope = pesapi_open_scope(m_Papis, m_EnvRef);
    auto env = pesapi_get_env_from_ref(m_Papis, m_EnvRef);

    do
    {
        pesapi_value this_val = pesapi_get_value_from_ref(m_Papis, env, instance);
        if (!this_val)
            break;

        pesapi_value global = pesapi_global(m_Papis, env);
        pesapi_value enumer = pesapi_get_property(m_Papis, env, global, "__zEnumerateFields");
        if (!enumer || !pesapi_is_function(m_Papis, env, enumer))
        {
            LOG_ERROR(ZScripting, "EnumerateInstanceFields: helper missing");
            break;
        }

        const pesapi_value args[1] = {this_val};
        pesapi_value rv = pesapi_call_function(m_Papis, env, enumer,
                                               /*this=*/nullptr,
                                               1,
                                               args);
        if (pesapi_has_caught(m_Papis, scope))
        {
            const char* msg = pesapi_get_exception_as_string(m_Papis, scope, 1);
            LOG_ERROR(ZScripting, "EnumerateInstanceFields threw: {}", msg ? msg : "<no message>");
            break;
        }
        if (!rv || !pesapi_is_string(m_Papis, env, rv))
            break;

        // First pass: query the byte length, then allocate. pesapi's
        // get_value_string_utf8 takes len IN as buffer-size and writes
        // bytes-needed back; passing nullptr buffer + len=0 is the
        // canonical way to size up.
        size_t needed = 0;
        pesapi_get_value_string_utf8(m_Papis, env, rv, nullptr, &needed);
        if (needed == 0)
            break;

        std::string packed(needed, '\0');
        pesapi_get_value_string_utf8(m_Papis, env, rv, packed.data(), &needed);
        // Defensive: needed may have been written as the number of bytes
        // including/excluding the NUL depending on backend; trim to the
        // actual content.
        while (!packed.empty() && packed.back() == '\0')
            packed.pop_back();

        // Split on 0x02 (records), each split further on 0x01 (fields).
        // See __zEnumerateFields in InstallConsoleAndDebugGlobals.
        const char REC = '\x02';
        const char FLD = '\x01';
        size_t i = 0;
        while (i < packed.size())
        {
            size_t j = packed.find(REC, i);
            if (j == std::string::npos)
                j = packed.size();
            std::string rec = packed.substr(i, j - i);

            size_t a = rec.find(FLD);
            size_t b = (a == std::string::npos) ? std::string::npos
                                                : rec.find(FLD, a + 1);
            if (a != std::string::npos && b != std::string::npos)
            {
                out.emplace_back(rec.substr(0, a),
                                 rec.substr(a + 1, b - a - 1),
                                 rec.substr(b + 1));
            }
            i = j + 1;
        }
    } while (false);

    pesapi_close_scope(m_Papis, scope);
    return out;
}

void ScriptEnv::Eval(std::string& chunk, const char* chunkName)
{
    // Open a value scope so any pesapi_value we allocate during eval is
    // properly cleaned up. This mirrors what Puerts' Unity/Unreal hosts do.
    std::printf("[ScriptEnv::Eval] entering, chunk='%s', size=%zu\n", chunkName, chunk.size());
    auto&& scope = pesapi_open_scope(m_Papis, m_EnvRef);
    auto&& env = pesapi_get_env_from_ref(m_Papis, m_EnvRef);
    pesapi_eval(m_Papis,
                env,
                reinterpret_cast<const uint8_t*>(chunk.data()),
                chunk.size(),
                chunkName);
    if (pesapi_has_caught(m_Papis, scope))
    {
        const char* msg = pesapi_get_exception_as_string(m_Papis, scope, 1);
        std::printf("[ScriptEnv::Eval] EXCEPTION in '%s': %s\n",
                    chunkName,
                    msg ? msg : "<no message>");
        LOG_ERROR(ZEngine, "ScriptEnv::Eval('{}') threw: {}", chunkName, msg ? msg : "<no message>");
    }
    else
    {
        // Read back globalThis.__zHello (if the chunk set it) to *prove*
        // execution actually happened, not just that we streamed bytes
        // somewhere. This is paranoia for the bootstrap smoke-test only;
        // production callers should not depend on this side-effect.
        auto&& global = pesapi_global(m_Papis, env);
        auto&& helloVal = pesapi_get_property(m_Papis, env, global, "__zHello");
        if (helloVal && pesapi_is_string(m_Papis, env, helloVal))
        {
            char buf[256] = {0};
            size_t len = sizeof(buf) - 1;
            const char* s = pesapi_get_value_string_utf8(m_Papis, env, helloVal, buf, &len);
            std::printf("[ScriptEnv::Eval] readback __zHello = %s\n", s ? s : "<null>");
        }
        else
        {
            std::printf("[ScriptEnv::Eval] '%s' completed (no __zHello).\n", chunkName);
        }
    }
    pesapi_close_scope(m_Papis, scope);
}

void ScriptEnv::Tick()
{
    m_Backend->OnTick();
}

void ScriptEnv::WaitDebugger()
{
    if (m_DebugPort == -1)
        return;
    while (!m_Backend->DebuggerTick())
    {
    }
}
