#include "ScriptEnv.h"

#include "Runtime/AssetBundle/AssetBundleManager.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/Scripting/Backend.h"
#include "Runtime/Scripting/Binding/ScriptBinding.h"
#include "Runtime/Scripting/Native/PuertsNative.h"
#include "Runtime/Scripting/ScriptDebug/ScriptDebug.h"
#include "Runtime/Scripting/Wrapper/ObjectPool.h"
#include "Runtime/Scripting/Wrapper/Puerts.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <unordered_set>
#include <vector>

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

    // `Debug.IsDebugBuild()`。无参，返回编译期常量 —— 这个值必须来自 C++，
    // JS 侧没有"当前二进制是不是 Release"的任何线索。
    void zNativeIsDebugBuildCallback(struct pesapi_ffi* apis, pesapi_callback_info info)
    {
        auto env = pesapi_get_env(apis, info);
        pesapi_value result = pesapi_create_boolean(apis, env, ScriptDebug::IsDebugBuild());
        pesapi_add_return(apis, info, result);
    }

    // `Debug.Break(msg)`。参数已经在 JS 侧拼成单个字符串。
    void zNativeBreakCallback(struct pesapi_ffi* apis, pesapi_callback_info info)
    {
        auto env = pesapi_get_env(apis, info);
        const int argc = pesapi_get_args_len(apis, info);
        std::string joined;
        for (int i = 0; i < argc; ++i)
        {
            pesapi_value v = pesapi_get_arg(apis, info, i);
            if (i > 0)
            {
                joined.push_back(' ');
            }
            char stack_buf[512];
            size_t len = sizeof(stack_buf) - 1;
            if (v && pesapi_is_string(apis, env, v))
            {
                if (const char* s = pesapi_get_value_string_utf8(apis, env, v, stack_buf, &len))
                {
                    joined.append(s, len);
                }
            }
        }
        ScriptDebug::Break(joined.c_str());
    }
}  // namespace

ScriptEnv::ScriptEnv(Backend* backend, int debugPort, bool editorDomain)
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
            // ZHeaderTool phase 3: every ZFUNCTION-annotated class registers a
            // ZClassScriptBinding at static-init time. This is the single point
            // where they are turned into real pesapi classes. Must happen
            // before any env is created, because the registry is shared.
            ScriptBindingRegistry::InstallAll(reg_api, registry);
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

    // Hand the shared class registry to this env. Without this the VM cannot
    // resolve any pesapi type id, so class objects and native-object wrapping
    // would silently fail (create_class returns undefined).
    m_Papis->set_registry(env, registry);
    // Publish `GameObject`, `Transform`, ... as globals, matching the
    // `declare class` block of the generated zengine.d.ts.
    ScriptBindingRegistry::InstallGlobals(m_Papis, env, editorDomain);

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

    // `Debug` 的两个"JS 做不了"的口子：编译期事实（isDebugBuild）与调试器
    // 附着状态（Break）。见 ScriptDebug.h 关于为什么 `Debug` 不是绑定类。
    {
        pesapi_value is_debug_build = pesapi_create_function(
            m_Papis, env, zNativeIsDebugBuildCallback, nullptr, /*finalize=*/nullptr);
        pesapi_set_property(m_Papis, env, global, "__zdebug_native_is_debug_build", is_debug_build);

        pesapi_value script_break = pesapi_create_function(
            m_Papis, env, zNativeBreakCallback, nullptr, /*finalize=*/nullptr);
        pesapi_set_property(m_Papis, env, global, "__zdebug_native_break", script_break);
    }

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
        "    LogError:   makeFwd(g.__zlog_native_error),"
        // Unity 的 Debug.LogException：优先打 stack，退化为 String(e)。
        // stack 才是"异常从哪儿抛出来"的答案，只打 message 等于没打。
        "    LogException: function(e) {"
        "      var s = (e && e.stack) ? e.stack : String(e);"
        "      g.__zlog_native_error('[Exception] ' + s);"
        "    },"
        // Unity 的 Debug.Assert(condition, message)。条件为假时记一条 error
        // —— 与 C++ 的 assert 不同，**不中断**：脚本的一条断言不该把整个
        // 进程带走（Unity 也是只打日志，除非 BuildOptions 开了 halt）。
        "    Assert: function(cond, msg) {"
        "      if (!cond) {"
        "        g.__zlog_native_error('[Assert] ' + (msg === undefined ? '(no message)' : String(msg)));"
        "      }"
        "    },"
        "    IsDebugBuild: g.__zdebug_native_is_debug_build,"
        "    Break:        g.__zdebug_native_break"
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
        // Minimal Unity-MonoBehaviour-compatible Behaviour base. It is a plain
        // JS class on purpose: the instance is produced by __zNewInstance, so
        // the engine cannot own `this` -- TypeScriptComponent instead publishes
        // `gameObject` / `transform` / `component` onto every instance right
        // after construction (see ZBehaviourScript in ScriptBinding.h, which is
        // also where zengine.d.ts gets the matching declaration). The lifecycle
        // hooks below are the no-op defaults, so a script may implement any
        // subset -- exactly like Unity's MonoBehaviour.
        "  g.Behaviour = function() {};"
        "  g.Behaviour.prototype.OnAwake = function() {};"
        "  g.Behaviour.prototype.OnStart = function() {};"
        "  g.Behaviour.prototype.OnUpdate = function(dt) {};"
        "  g.Behaviour.prototype.OnDestroy = function() {};"
        // Unity 的 `Invoke` 家族：TS 侧写 `this.Invoke(...)`，这里转发到
        // `component`（TypeScriptComponent 上绑定的原生方法）。
        //
        // 为什么不直接把 ZFUNCTION 打在 C++ 的 Behaviour 上：那个名字已经属于
        // 合成脚本类 g_BehaviourClass（提供 gameObject / transform / component），
        // 再发布一次会生成第二个 `declare class Behaviour`，tsc 报重复标识符
        // （ReflectionCodegenSmokeTest 的 BEHAVIOUR 门就是抓这个的）。
        //
        // `component` 由 TypeScriptComponent::BindAndAwake 在 OnAwake 之前挂上，
        // 所以用户代码跑到这里时它一定在；仍做一次判空，免得构造期误用变成
        // "undefined is not an object" 这种没有任何上下文的报错。
        "  g.Behaviour.prototype.Invoke = function(name, t) {"
        "    if (this.component) this.component.Invoke(name, t);"
        "  };"
        "  g.Behaviour.prototype.InvokeRepeating = function(name, t, r) {"
        "    if (this.component) this.component.InvokeRepeating(name, t, r);"
        "  };"
        "  g.Behaviour.prototype.CancelInvoke = function(name) {"
        "    if (this.component) this.component.CancelInvoke(name === undefined ? '' : name);"
        "  };"
        "  g.Behaviour.prototype.IsInvoking = function(name) {"
        "    return this.component ? this.component.IsInvoking(name) : false;"
        "  };"
        // （P8 的 __zhmr_native_* / require 入口不在这里 —— 它们是真正的 native
        //   函数，见下面 InstallConsoleAndDebugGlobals 尾部。早先这里写成了纯 JS
        //   存根，只往 JS 侧的普通对象里塞回调，从不回调 C++ ⇒ m_HmrStates 恒空，
        //   HMR 全程"接上了但什么都没发生"。）
        "})(globalThis);";
    pesapi_eval(m_Papis, env, reinterpret_cast<const uint8_t*>(kShim), sizeof(kShim) - 1, "<console_shim>");
    if (pesapi_has_caught(m_Papis, scope))
    {
        const char* msg = pesapi_get_exception_as_string(m_Papis, scope, 1);
        LOG_ERROR(ZScripting, "console shim install failed: {}", msg ? msg : "<no message>");
    }

    // 3) P8 HMR: real native sinks on globalThis. Must be native (not JS stubs)
    //    because the whole point is to move the callback / dependency edge into
    //    C++ state (m_HmrStates / m_ModuleDeps).
    auto installHmrSink = [&](const char* name, void (*cb)(struct pesapi_ffi*, pesapi_callback_info)) {
        pesapi_value f = pesapi_create_function(m_Papis, env, cb, this, /*finalize=*/nullptr);
        pesapi_set_property(m_Papis, env, global, name, f);
    };
    installHmrSink("__zhmr_native_accept", &ScriptEnv::ZHmrAcceptSink);
    installHmrSink("__zhmr_native_dispose", &ScriptEnv::ZHmrDisposeSink);
    installHmrSink("__zhmr_native_decline", &ScriptEnv::ZHmrDeclineSink);
    installHmrSink("__zhmr_require", &ScriptEnv::ZHmrRequireSink);

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
    for (auto& kv : m_Modules)
    {
        if (kv.second)
            pesapi_release_value_ref(m_Papis, kv.second);
    }
    m_Modules.clear();

    // P8：模块缓存没了，热替换回调与依赖图也一并作废 —— 留着的话，下一次
    // ReloadModule 会拿一张指向已释放模块的图去算 cascade。
    for (auto& kv : m_HmrStates)
    {
        for (pesapi_value_ref r : kv.second.dispose_callbacks)
        {
            if (r)
                pesapi_release_value_ref(m_Papis, r);
        }
        if (kv.second.accept_callback)
            pesapi_release_value_ref(m_Papis, kv.second.accept_callback);
    }
    m_HmrStates.clear();
    m_ModuleDeps.clear();
    m_ModuleReverseDeps.clear();
    m_ReverseDepsDirty = false;
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
    ClearHmrState(module_id);
    LOG_INFO(ZScripting, "module unloaded: {}", module_id);
}

// ---------------------------------------------------------------------------
// P8 HMR
// ---------------------------------------------------------------------------

namespace
{
    // 读一个 utf-8 字符串实参。与 zNativeLogCallback 同一套写法（先栈缓冲）。
    bool ReadUtf8Arg(struct pesapi_ffi* apis, pesapi_env env, pesapi_value v, std::string& out)
    {
        if (!v || !pesapi_is_string(apis, env, v))
            return false;
        char         stack_buf[512];
        size_t       len = sizeof(stack_buf) - 1;
        const char*  s = pesapi_get_value_string_utf8(apis, env, v, stack_buf, &len);
        if (!s)
            return false;
        out.assign(s, len);
        return true;
    }

    // 把 require 的相对说明符按当前模块目录解析成 module id。
    // module id 是"相对 js root、无扩展名、正斜杠"的形式（与 LoadModuleInternal
    // 拼路径的方式一致），所以这里只需要处理 ./ ../ 与目录归一。
    std::string ResolveModuleSpecifier(const std::string& parent_module_id, const std::string& spec)
    {
        std::string base_dir;
        const size_t slash = parent_module_id.find_last_of('/');
        if (slash != std::string::npos)
            base_dir = parent_module_id.substr(0, slash + 1);

        const bool is_relative = spec == "." || spec == ".." ||
                                 spec.rfind("./", 0) == 0 || spec.rfind("../", 0) == 0;
        const std::string& combined_src = is_relative ? (base_dir + spec) : spec;

        std::vector<std::string> parts;
        size_t                   i = 0;
        while (i < combined_src.size())
        {
            size_t j = combined_src.find('/', i);
            if (j == std::string::npos)
                j = combined_src.size();
            const std::string seg = combined_src.substr(i, j - i);
            i = j + 1;
            if (seg.empty() || seg == ".")
                continue;
            if (seg == "..")
            {
                if (!parts.empty())
                    parts.pop_back();
                continue;
            }
            parts.push_back(seg);
        }
        std::string out;
        for (const std::string& p : parts)
        {
            if (!out.empty())
                out.push_back('/');
            out += p;
        }
        return out;
    }
}  // namespace

void ScriptEnv::ZHmrAcceptSink(struct pesapi_ffi* apis, pesapi_callback_info info)
{
    auto* self = static_cast<ScriptEnv*>(pesapi_get_userdata(apis, info));
    if (!self || pesapi_get_args_len(apis, info) < 2)
        return;
    auto        env = pesapi_get_env(apis, info);
    std::string module_id;
    if (!ReadUtf8Arg(apis, env, pesapi_get_arg(apis, info, 0), module_id))
        return;

    pesapi_value cb = pesapi_get_arg(apis, info, 1);
    auto&        st = self->m_HmrStates[module_id];
    st.accepted = true;
    if (st.accept_callback)
    {
        pesapi_release_value_ref(self->m_Papis, st.accept_callback);
        st.accept_callback = nullptr;
    }
    if (cb && pesapi_is_function(apis, env, cb))
        st.accept_callback = pesapi_create_value_ref(self->m_Papis, env, cb, 0);

    LOG_INFO(ZScripting, "hmr: {} accepted (callback={})", module_id, st.accept_callback != nullptr);
}

void ScriptEnv::ZHmrDisposeSink(struct pesapi_ffi* apis, pesapi_callback_info info)
{
    auto* self = static_cast<ScriptEnv*>(pesapi_get_userdata(apis, info));
    if (!self || pesapi_get_args_len(apis, info) < 2)
        return;
    auto        env = pesapi_get_env(apis, info);
    std::string module_id;
    if (!ReadUtf8Arg(apis, env, pesapi_get_arg(apis, info, 0), module_id))
        return;

    pesapi_value cb = pesapi_get_arg(apis, info, 1);
    if (cb && pesapi_is_function(apis, env, cb))
        self->m_HmrStates[module_id].dispose_callbacks.push_back(
            pesapi_create_value_ref(self->m_Papis, env, cb, 0));
}

void ScriptEnv::ZHmrDeclineSink(struct pesapi_ffi* apis, pesapi_callback_info info)
{
    auto* self = static_cast<ScriptEnv*>(pesapi_get_userdata(apis, info));
    if (!self || pesapi_get_args_len(apis, info) < 1)
        return;
    auto        env = pesapi_get_env(apis, info);
    std::string module_id;
    if (!ReadUtf8Arg(apis, env, pesapi_get_arg(apis, info, 0), module_id))
        return;

    self->m_HmrStates[module_id].declined = true;
    LOG_INFO(ZScripting, "hmr: {} declined (will always full-rebuild)", module_id);
}

void ScriptEnv::ZHmrRequireSink(struct pesapi_ffi* apis, pesapi_callback_info info)
{
    auto* self = static_cast<ScriptEnv*>(pesapi_get_userdata(apis, info));
    if (!self || pesapi_get_args_len(apis, info) < 2)
        return;
    auto        env = pesapi_get_env(apis, info);
    std::string spec;
    std::string parent;
    if (!ReadUtf8Arg(apis, env, pesapi_get_arg(apis, info, 0), spec))
        return;
    if (!ReadUtf8Arg(apis, env, pesapi_get_arg(apis, info, 1), parent))
        return;

    const std::string dep = ResolveModuleSpecifier(parent, spec);
    if (dep.empty())
    {
        LOG_WARNING(ZScripting, "hmr: require('{}') from '{}' resolved to nothing", spec, parent);
        return;
    }

    self->AddModuleDependency(parent, dep);
    self->LoadModule(dep);

    if (pesapi_value_ref exports = self->GetModuleExports(dep))
    {
        if (pesapi_value v = pesapi_get_value_from_ref(self->m_Papis, env, exports))
            pesapi_add_return(apis, info, v);
    }
}

bool ScriptEnv::HasHmrAcceptCallback(const std::string& module_id) const
{
    const auto it = m_HmrStates.find(module_id);
    return it != m_HmrStates.end() && it->second.accepted;
}

bool ScriptEnv::HasHmrDeclined(const std::string& module_id) const
{
    const auto it = m_HmrStates.find(module_id);
    return it != m_HmrStates.end() && it->second.declined;
}

ScriptEnv::HmrCallbacks ScriptEnv::TakeHmrCallbacks(const std::string& module_id)
{
    HmrCallbacks taken;
    const auto   it = m_HmrStates.find(module_id);
    if (it == m_HmrStates.end())
        return taken;
    taken.accept = it->second.accept_callback;
    taken.dispose.swap(it->second.dispose_callbacks);
    it->second.accept_callback = nullptr;
    return taken;
}

void ScriptEnv::ReleaseHmrCallbacks(HmrCallbacks& cb) const
{
    if (cb.accept)
    {
        pesapi_release_value_ref(m_Papis, cb.accept);
        cb.accept = nullptr;
    }
    for (pesapi_value_ref r : cb.dispose)
    {
        if (r)
            pesapi_release_value_ref(m_Papis, r);
    }
    cb.dispose.clear();
}

void ScriptEnv::InvokeAcceptRef(pesapi_value_ref cb, pesapi_value_ref new_exports)
{
    if (!cb)
        return;
    auto        scope = pesapi_open_scope(m_Papis, m_EnvRef);
    auto        env = pesapi_get_env_from_ref(m_Papis, m_EnvRef);
    pesapi_value fn = pesapi_get_value_from_ref(m_Papis, env, cb);
    if (fn)
    {
        pesapi_value arg = new_exports ? pesapi_get_value_from_ref(m_Papis, env, new_exports) : nullptr;
        if (arg)
            pesapi_call_function(m_Papis, env, fn, /*this=*/nullptr, 1, &arg);
        else
            pesapi_call_function(m_Papis, env, fn, /*this=*/nullptr, 0, nullptr);
    }
    pesapi_close_scope(m_Papis, scope);
}

void ScriptEnv::InvokeDisposeRefs(const std::vector<pesapi_value_ref>& refs)
{
    if (refs.empty())
        return;
    auto scope = pesapi_open_scope(m_Papis, m_EnvRef);
    auto env = pesapi_get_env_from_ref(m_Papis, m_EnvRef);
    for (pesapi_value_ref r : refs)
    {
        if (!r)
            continue;
        if (pesapi_value fn = pesapi_get_value_from_ref(m_Papis, env, r))
            pesapi_call_function(m_Papis, env, fn, /*this=*/nullptr, 0, nullptr);
    }
    pesapi_close_scope(m_Papis, scope);
}

void ScriptEnv::InvokeHmrDisposeCallback(const std::string& module_id)
{
    HmrCallbacks cb = TakeHmrCallbacks(module_id);
    InvokeDisposeRefs(cb.dispose);
    ReleaseHmrCallbacks(cb);
}

void ScriptEnv::InvokeHmrAcceptCallback(const std::string& module_id, pesapi_value_ref new_exports)
{
    const auto it = m_HmrStates.find(module_id);
    if (it == m_HmrStates.end())
        return;
    InvokeAcceptRef(it->second.accept_callback, new_exports);
}

void ScriptEnv::ClearHmrState(const std::string& module_id)
{
    const auto it = m_HmrStates.find(module_id);
    if (it == m_HmrStates.end())
        return;
    for (pesapi_value_ref r : it->second.dispose_callbacks)
    {
        if (r)
            pesapi_release_value_ref(m_Papis, r);
    }
    if (it->second.accept_callback)
        pesapi_release_value_ref(m_Papis, it->second.accept_callback);
    m_HmrStates.erase(it);
}

bool ScriptEnv::SwapModuleExports(const std::string& module_id, pesapi_value_ref new_exports)
{
    if (!new_exports)
        return false;
    const auto it = m_Modules.find(module_id);
    if (it != m_Modules.end() && it->second)
        pesapi_release_value_ref(m_Papis, it->second);
    m_Modules[module_id] = new_exports;
    return true;
}

pesapi_value_ref ScriptEnv::GetModuleExports(const std::string& module_id) const
{
    const auto it = m_Modules.find(module_id);
    return it == m_Modules.end() ? nullptr : it->second;
}

void ScriptEnv::AddModuleDependency(const std::string& parent_module_id, const std::string& child_module_id)
{
    if (parent_module_id.empty() || child_module_id.empty() || parent_module_id == child_module_id)
        return;
    auto& list = m_ModuleDeps[parent_module_id];
    if (std::find(list.begin(), list.end(), child_module_id) == list.end())
    {
        list.push_back(child_module_id);
        m_ReverseDepsDirty = true;
    }
}

void ScriptEnv::RebuildReverseDeps()
{
    m_ModuleReverseDeps.clear();
    for (const auto& kv : m_ModuleDeps)
    {
        for (const std::string& child : kv.second)
            m_ModuleReverseDeps[child].push_back(kv.first);
    }
    m_ReverseDepsDirty = false;
}

std::vector<std::string> ScriptEnv::GetCascadeReloadOrder(const std::string& changed_module)
{
    if (m_ReverseDepsDirty)
        RebuildReverseDeps();

    // 被改者在前，依赖它的依次在后 —— BFS 天然满足"一个模块出现在依赖它的人之前"。
    std::vector<std::string>              order;
    std::unordered_set<std::string>       visited;
    std::deque<std::string>               queue;
    visited.insert(changed_module);
    queue.push_back(changed_module);
    while (!queue.empty())
    {
        const std::string cur = queue.front();
        queue.pop_front();
        order.push_back(cur);

        const auto it = m_ModuleReverseDeps.find(cur);
        if (it == m_ModuleReverseDeps.end())
            continue;
        for (const std::string& parent : it->second)
        {
            if (visited.insert(parent).second)
                queue.push_back(parent);
        }
    }
    return order;
}

bool ScriptEnv::EvaluateModuleFresh(const std::string& module_id, pesapi_value_ref& out_ref)
{
    out_ref = nullptr;
    if (m_JsRoot.empty())
    {
        LOG_ERROR(ZScripting, "EvaluateModuleFresh('{}'): js root not set", module_id);
        return false;
    }
    auto scope = pesapi_open_scope(m_Papis, m_EnvRef);
    bool ok = LoadModuleInternal(module_id, /*cache=*/false, &out_ref);
    if (pesapi_has_caught(m_Papis, scope))
    {
        const char* msg = pesapi_get_exception_as_string(m_Papis, scope, 1);
        LOG_ERROR(ZScripting, "EvaluateModuleFresh('{}') threw: {}", module_id, msg ? msg : "<no message>");
        if (out_ref)
        {
            pesapi_release_value_ref(m_Papis, out_ref);
            out_ref = nullptr;
        }
        ok = false;
    }
    pesapi_close_scope(m_Papis, scope);
    return ok;
}

ScriptEnv::HmrReloadResult ScriptEnv::ReloadModule(const std::string& module_id)
{
    HmrReloadResult result;

    const std::vector<std::string> cascade = GetCascadeReloadOrder(module_id);

    // 两趟。第一趟"摘旧回调 + 重新求值"，第二趟才决定热不热 —— 因为新代码
    // **有没有 accept / decline 只有求值之后才知道**。按内存里的旧状态判定的
    // 话：一个上一版 accept 过、这一版改成 decline 的模块会被判成可热替换，
    // 于是 decline 静默失效（G3 就是这么红的）。
    struct Pending
    {
        std::string       id;
        HmrCallbacks      old_cb;
        pesapi_value_ref  fresh = nullptr;
        bool              evaluated = false;
        bool              accepted = false;
        bool              declined = false;
    };
    std::vector<Pending> pendings;
    pendings.reserve(cascade.size());

    for (const std::string& m : cascade)
    {
        Pending p;
        p.id = m;

        // 先摘走旧回调：重新求值会用新代码重新登记 accept/dispose，槽位必须是
        // 空的，否则 Register 会释放一份我们还持着的 ref（双释放）。
        p.old_cb = TakeHmrCallbacks(m);
        // 把"这一版有没有 opt-in"的标记清掉，让新代码的声明成为唯一依据。
        HmrState& st = m_HmrStates[m];
        st.accepted = false;
        st.declined = false;

        p.evaluated = EvaluateModuleFresh(m, p.fresh);
        p.accepted = HasHmrAcceptCallback(m);
        p.declined = HasHmrDeclined(m);

        // **必须边求值边换缓存**，不能等判定完再一起换：cascade 是按"被依赖者
        // 在前"排序的，parent 重新求值时 `require('./child')` 走的还是 m_Modules
        // 缓存 —— 不先换掉 child，parent 抓到的就是**旧** child，于是换完之后
        // 整条链处于"parent 是新的、它闭包里的 child 是旧的"这种半新半旧状态
        // （E12 就是这么红的）。换成"先换再判"之后，最坏情况只是多求值一次。
        if (p.evaluated && SwapModuleExports(m, p.fresh))
        {
            p.fresh = nullptr;   // 所有权已交给 m_Modules
        }

        pendings.push_back(p);
    }

    // 全 accept、无 decline、且都求值成功 ⇒ 热路径。不做"部分热替换"：一半热
    // 一半重建的中间状态最难推理，也最难复现。
    bool hot = !pendings.empty();
    for (const Pending& p : pendings)
    {
        if (!p.evaluated || !p.accepted || p.declined)
        {
            hot = false;
            break;
        }
    }

    if (hot)
    {
        for (Pending& p : pendings)
        {
            // 缓存已经在第一趟换过了（为了让依赖方看到新的）；accept 拿到的是
            // 显式传进去的新 exports，所以不需要旧缓存还活着。
            InvokeAcceptRef(p.old_cb.accept, GetModuleExports(p.id));
            InvokeDisposeRefs(p.old_cb.dispose);
            ReleaseHmrCallbacks(p.old_cb);
            result.reloaded.push_back(p.id);
        }
        result.ok = true;
        result.hot_replaced = true;
        LOG_INFO(ZScripting, "hmr: hot-replaced {} module(s) for '{}' (instances kept)",
                 result.reloaded.size(), module_id);
        return result;
    }

    // 回落：P6 全量重建。cascade 里每个都重来 —— 依赖方抓的是旧 exports，只重载
    // 被改的那个会留下不一致（这也是 P6 老行为里漏掉的一环）。
    for (Pending& p : pendings)
    {
        InvokeDisposeRefs(p.old_cb.dispose);
        ReleaseHmrCallbacks(p.old_cb);
        if (p.fresh)   // 求值成功但换缓存失败的漏网（正常路径下已经是 nullptr）
        {
            pesapi_release_value_ref(m_Papis, p.fresh);
            p.fresh = nullptr;
        }
    }
    result.reloaded.clear();
    for (const std::string& m : cascade)
    {
        UnloadModule(m);
        if (LoadModule(m))
            result.reloaded.push_back(m);
    }
    result.ok = IsModuleLoaded(module_id);
    result.hot_replaced = false;
    return result;
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
        // 模块执行到一半抛了（比如 `class X extends NotAConstructor` 在模块求值
        // 阶段就炸），但 LoadModuleInternal 的 exports 兜底分支仍然拿得到一个
        // 非空的 module.exports —— 于是它照样把引用塞进 m_Modules。不把这个半
        // 成品丢掉的话：IsModuleLoaded() 说已加载，之后每次 LoadModule 直接命中
        // 缓存返回 true，这段模块**永远不会再被执行一次**。改好源码再热重载，
        // 拿到的还是那次抛异常留下的空壳。
        UnloadModule(module_id);
        ok = false;
    }
    pesapi_close_scope(m_Papis, scope);
    return ok;
}

bool ScriptEnv::LoadModuleInternal(const std::string& module_id, bool cache, pesapi_value_ref* out_ref)
{
    // 1. Resolve module_id -> on-disk .js path.
    std::string js_path = m_JsRoot;
    if (!js_path.empty() && js_path.back() != '/' && js_path.back() != '\\')
        js_path.push_back('/');
    js_path.append(module_id);
    js_path.append(".js");

    // 2. Read source.
    //
    //    A packaged build carries the compiled modules inside the `scripts`
    //    bundle, and a player is allowed to ship no Intermediate/Scripts/ tree
    //    at all, so the bundle is asked FIRST. Not finding it there is the
    //    normal editing case (the editor mounts no bundle because a mounted
    //    bundle would shadow real files), so it falls through to the
    //    filesystem rather than failing.
    std::string raw;
    bool have_source = false;
    if (auto* bundles = GET_SYSTEM(AssetBundleManager); bundles != nullptr)
    {
        have_source = bundles->TryReadScriptModule(module_id, raw);
    }

    if (!have_source)
    {
        // Binary mode so we don't translate CRLF and introduce off-by-one
        // offsets in error messages.
        FILE* file = fopen(js_path.c_str(), "rb");
        if (!file)
        {
            LOG_ERROR(ZScripting, "LoadModule('{}'): cannot open '{}'", module_id, js_path);
            return false;
        }
        fseek(file, 0, SEEK_END);
        const long fsz = ftell(file);
        if (fsz > 0)
        {
            raw.resize(static_cast<size_t>(fsz));
            fseek(file, 0, SEEK_SET);
            fread(raw.data(), 1, static_cast<size_t>(fsz), file);
        }
        fclose(file);
    }

    // 3. Wrap in IIFE-factory so we get a function value back. The QuickJS
    //    in puerts doesn't expose CommonJS `require`, so we synthesize one
    //    pair of `module` / `exports` per call.
    //
    //    NOTE: tsc emits CommonJS-style `Object.defineProperty(exports, ...)`
    //    and `exports.Foo = ...` calls into the body, which is exactly what
    //    this wrapper supports. ESM-style `export class Foo {}` would NOT
    //    work here, but tsconfig is pinned to "module": "commonjs".
    //
    //    P8 HMR: the IIFE injects (a) a `require` binding -- the name user code
    //    actually writes, which forwards to the native sink together with this
    //    module's id so the dependency edge can be recorded -- and (b)
    //    `module.hot` with accept/dispose/decline that forward to native sinks.
    //    Both MUST be native: an all-JS version cannot move anything into C++.
    std::string wrapped;
    wrapped.reserve(raw.size() + 512);
    wrapped.append("(function(module, exports, __zhmr_module_id){\n");
    wrapped.append(
        "var require = function(spec) {\n"
        "  return globalThis.__zhmr_require(spec, __zhmr_module_id);\n"
        "};\n");
    // Inject module.hot BEFORE user code runs, so top-level accept() calls work.
    wrapped.append(
        "var __zhmr_hot = {\n"
        "  _moduleId: __zhmr_module_id,\n"
        "  accept: function(cb) {\n"
        "    globalThis.__zhmr_native_accept(__zhmr_module_id,\n"
        "      (typeof cb === 'function') ? cb : null);\n"
        "  },\n"
        "  dispose: function(cb) {\n"
        "    if (typeof cb === 'function') {\n"
        "      globalThis.__zhmr_native_dispose(__zhmr_module_id, cb);\n"
        "    }\n"
        "  },\n"
        "  decline: function() {\n"
        "    globalThis.__zhmr_native_decline(__zhmr_module_id);\n"
        "  }\n"
        "};\n"
        "module.hot = __zhmr_hot;\n"
    );
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

    // 4. Build module = { exports: {} } and inject module.hot
    pesapi_value module_obj = pesapi_create_object(m_Papis, env);
    pesapi_value exports_obj = pesapi_create_object(m_Papis, env);
    pesapi_set_property(m_Papis, env, module_obj, "exports", exports_obj);

    pesapi_value module_id_val = pesapi_create_string_utf8(m_Papis, env, module_id.data(), module_id.size());

    // 5. Call factory(module, exports, module_id). `require` is not a parameter:
    //    the wrapper body declares it as a local so the name user code writes
    //    resolves without depending on a global.
    const pesapi_value args[3] = {module_obj, exports_obj, module_id_val};
    pesapi_value resolved_exports = pesapi_call_function(
        m_Papis, env, factory, /*this=*/nullptr, 3, args);

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
    if (cache)
    {
        m_Modules[module_id] = ref;
    }
    else
    {
        // 热替换：调用方要在旧 exports 还活着的时候拿到新的，事毕自己释放。
        if (out_ref)
        {
            *out_ref = ref;
        }
        else
        {
            pesapi_release_value_ref(m_Papis, ref);
        }
    }

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

bool ScriptEnv::InvokeInstanceMethodGameObject(pesapi_value_ref instance, const char* method_name,
                                               GameObject* other)
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
        pesapi_value arg_val = ZScript::Converter<GameObject*>::ToJs(m_Papis, env, other);
        if (!arg_val)
        {
            // 拿不到脚本侧的包装就退化成 null，而不是整个调用失败 —— 收不到
            // `other` 的 OnCollisionEnter 至少还跑得到，能让作者看到日志。
            arg_val = pesapi_create_null(m_Papis, env);
        }
        const pesapi_value args[1] = {arg_val};
        pesapi_call_function(m_Papis, env, fn, this_val, 1, args);
        if (pesapi_has_caught(m_Papis, scope))
        {
            const char* msg = pesapi_get_exception_as_string(m_Papis, scope, 1);
            LOG_ERROR(ZScripting, "instance.{}(<GameObject>) threw: {}", method_name, msg ? msg : "<no message>");
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

bool ScriptEnv::SetInstanceNativeObject(pesapi_value_ref instance,
                                        const char* name,
                                        const void* type_id,
                                        void* object)
{
    if (instance == nullptr || name == nullptr || name[0] == '\0')
        return false;

    auto scope = pesapi_open_scope(m_Papis, m_EnvRef);
    auto env = pesapi_get_env_from_ref(m_Papis, m_EnvRef);

    bool ok = false;
    do
    {
        pesapi_value this_val = pesapi_get_value_from_ref(m_Papis, env, instance);
        if (this_val == nullptr)
            break;

        pesapi_value wrapped;
        if (object == nullptr || type_id == nullptr)
        {
            wrapped = pesapi_create_null(m_Papis, env);
        }
        else
        {
            // call_finalize = 0: JS never owns an engine object. The pointer
            // belongs to the scene / ObjectManager, so a JS-side GC finalizer
            // deleting it would be a use-after-free.
            wrapped = pesapi_native_object_to_value(m_Papis, env, type_id, object, 0);
            if (wrapped == nullptr)
            {
                LOG_ERROR(ZScripting,
                          "SetInstanceNativeObject('{}'): pesapi could not wrap the native "
                          "object -- is its type id registered (does the class own a "
                          "ZFUNCTION)?. Publishing null instead.",
                          name);
                wrapped = pesapi_create_null(m_Papis, env);
            }
        }

        ok = pesapi_set_property(m_Papis, env, this_val, name, wrapped) != 0;
        if (!ok)
            LOG_ERROR(ZScripting, "SetInstanceNativeObject('{}'): set_property failed", name);
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
