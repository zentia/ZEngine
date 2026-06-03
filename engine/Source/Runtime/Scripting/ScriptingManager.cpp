#include "ScriptingManager.h"

#include "Runtime/BaseClasses/TypeManager.h"
#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Project/ProjectInfo.h"
#if PAPI_QUICKJS
    #include "Runtime/Scripting/Backends/QuickJS/BackendQuickJS.h"
#elif PAPI_V8
    #include "Runtime/Scripting/Backends/V8/BackendV8.h"
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>

std::vector<std::type_index> ScriptingManager::GetDependencies() const
{
    // ProjectInfo gives us the Intermediate/Scripts/ root. CommandSystem is
    // a transitive dep through ProjectInfo so we don't list it.
    return {GET_SYSTEM_TYPE(ProjectInfo)};
}

bool ScriptingManager::Initialize()
{
#if PAPI_QUICKJS
    std::printf("[ScriptingManager] creating QuickJS backend...\n");
    auto&& backend = new BackendQuickJS();
    m_ScriptEnv = new ScriptEnv(backend, -1);
    std::printf("[ScriptingManager] QuickJS env ready, running bootstrap...\n");

    // Smoke test: prove the JS VM is alive end-to-end with the new
    // console shim active. The bootstrap script previously installed its
    // own console; now we rely on the C++-side shim done in
    // ScriptEnv::InstallConsoleAndDebugGlobals(), so this chunk just
    // exercises console.log + the readback marker.
    std::string chunk =
        "globalThis.__zHello = '[ZEngine] hello from QuickJS! 1 + 2 = ' + (1 + 2);"
        "console.log(globalThis.__zHello);";
    m_ScriptEnv->Eval(chunk, "<bootstrap>");
    std::printf("[ScriptingManager] bootstrap eval returned.\n");

    // Bind to the active project (if any). On a fresh editor with no
    // project loaded ProjectInfo->GetProjectRoot() returns "", which we
    // treat as "no js root" and just skip - LoadModule calls will return
    // false until a project is opened and BindToProject is called again.
    auto project_info = GET_SYSTEM(ProjectInfo);
    if (project_info)
        BindToProject(*project_info);
#elif PAPI_V8
    // Desktop / PC / console path. Same boot sequence as QuickJS -- the
    // pesapi FFI layer hides every backend difference, so the only thing
    // that changes is the Backend instance handed to ScriptEnv. See
    // AGENTS.md 2.7 for the two-VM strategy rationale.
    //
    // V8 Inspector opt-in: if the environment variable
    // ZENGINE_V8_DEBUG_PORT is set to a positive integer, ScriptEnv will
    // forward it into BackendV8::OpenRemoteDebugger, which calls puerts'
    // CreateInspector(isolate, port). Chrome DevTools / VSCode can then
    // attach via ws://127.0.0.1:<port>. Default (-1) leaves the inspector
    // disabled, matching the QuickJS path. This is intentionally an env
    // var rather than a CLI flag so it's available before any command-
    // line plumbing is wired up at this engine layer.
    int debugPort = -1;
    if (const char* envPort = std::getenv("ZENGINE_V8_DEBUG_PORT"))
    {
        char* end = nullptr;
        long v = std::strtol(envPort, &end, 10);
        if (end != envPort && v > 0 && v < 65536)
        {
            debugPort = static_cast<int>(v);
            std::printf("[ScriptingManager] ZENGINE_V8_DEBUG_PORT=%d -> V8 Inspector enabled\n", debugPort);
        }
        else
        {
            std::printf("[ScriptingManager] ZENGINE_V8_DEBUG_PORT='%s' invalid; inspector disabled\n", envPort);
        }
    }
    std::printf("[ScriptingManager] creating V8 backend...\n");
    auto&& backend = new BackendV8();
    m_ScriptEnv = new ScriptEnv(backend, debugPort);
    std::printf("[ScriptingManager] V8 env ready, running bootstrap...\n");

    std::string chunk =
        "globalThis.__zHello = '[ZEngine] hello from V8! 1 + 2 = ' + (1 + 2);"
        "console.log(globalThis.__zHello);";
    m_ScriptEnv->Eval(chunk, "<bootstrap>");
    std::printf("[ScriptingManager] bootstrap eval returned.\n");

    auto project_info = GET_SYSTEM(ProjectInfo);
    if (project_info)
        BindToProject(*project_info);
#else
    // ZEngine is JS-only; either PAPI_QUICKJS or PAPI_V8 must be defined
    // by the build system. Without a backend we run in degraded mode --
    // LoadModule / instance calls just no-op + log, which is what the
    // editor expects when no project is loaded anyway. Hitting this
    // branch typically means -DPAPI_TYPE=... was not propagated; check
    // CMakeCache.txt:PAPI_TYPE first.
    m_ScriptEnv = nullptr;
#endif
    return true;
}

void ScriptingManager::Shutdown()
{
    if (m_ScriptEnv)
    {
        delete m_ScriptEnv;
        m_ScriptEnv = nullptr;
    }
}

bool ScriptingManager::BindToProject(const ProjectInfo& project_info)
{
    if (!m_ScriptEnv)
        return false;

    auto root = project_info.GetIntermediateScriptsRoot();
    if (root.empty())
    {
        // No project loaded - bring it back to "unbound" state.
        m_JsRoot.clear();
        m_ScriptEnv->SetJsRoot(std::string());
        return false;
    }
    // Make sure the directory exists; tsc will populate it lazily.
    std::error_code ec;
    std::filesystem::create_directories(root, ec);

    m_JsRoot = root;
    // generic_string -> forward slashes, which keeps ScriptEnv path joining
    // simple regardless of OS.
    m_ScriptEnv->SetJsRoot(root.generic_string());
    LOG_INFO(ZScripting, "ScriptingManager bound to js root: {}", root.generic_string());
    return true;
}

bool ScriptingManager::LoadModule(const std::string& module_id)
{
    return m_ScriptEnv && m_ScriptEnv->LoadModule(module_id);
}

bool ScriptingManager::ReloadModule(const std::string& module_id)
{
    if (!m_ScriptEnv)
        return false;
    if (!m_ScriptEnv->ReloadModule(module_id))
        return false;

    // Phase-6: tell every subscribed TypeScriptComponent (and anyone else
    // who cares) that this module just got fresh code. Fire AFTER the
    // re-eval so observers can immediately CreateInstance against the new
    // module exports.
    NotifyModuleReloaded(module_id);
    return true;
}

void ScriptingManager::UnloadModule(const std::string& module_id)
{
    if (m_ScriptEnv)
        m_ScriptEnv->UnloadModule(module_id);
}

bool ScriptingManager::IsModuleLoaded(const std::string& module_id) const
{
    return m_ScriptEnv && m_ScriptEnv->IsModuleLoaded(module_id);
}

bool ScriptingManager::InvokeExportedFunction(const std::string& module_id, const char* fn_name)
{
    return m_ScriptEnv && m_ScriptEnv->InvokeExportedFunction(module_id, fn_name);
}

pesapi_value_ref ScriptingManager::CreateInstance(const std::string& module_id, const std::string& class_name)
{
    return m_ScriptEnv ? m_ScriptEnv->CreateInstance(module_id, class_name) : nullptr;
}

void ScriptingManager::DestroyInstance(pesapi_value_ref instance)
{
    if (m_ScriptEnv)
        m_ScriptEnv->DestroyInstance(instance);
}

bool ScriptingManager::InvokeInstanceMethod(pesapi_value_ref instance, const char* method_name)
{
    return m_ScriptEnv && m_ScriptEnv->InvokeInstanceMethod(instance, method_name);
}

bool ScriptingManager::InvokeInstanceMethodNumber(pesapi_value_ref instance, const char* method_name, double numeric_arg)
{
    return m_ScriptEnv && m_ScriptEnv->InvokeInstanceMethodNumber(instance, method_name, numeric_arg);
}

// -----------------------------------------------------------------------
// Phase-7 serialised-field forwarders. Both are pure thin shells -- the
// real work lives in ScriptEnv where the pesapi handle is in scope.
// -----------------------------------------------------------------------
bool ScriptingManager::ApplyInstanceField(pesapi_value_ref instance,
                                          const std::string& key,
                                          const std::string& value_str)
{
    return m_ScriptEnv && m_ScriptEnv->ApplyInstanceField(instance, key, value_str);
}

std::vector<std::tuple<std::string, std::string, std::string>>
ScriptingManager::EnumerateInstanceFields(pesapi_value_ref instance)
{
    if (m_ScriptEnv == nullptr)
        return {};
    return m_ScriptEnv->EnumerateInstanceFields(instance);
}

void ScriptingManager::Tick()
{
    if (m_ScriptEnv)
        m_ScriptEnv->Tick();
}

size_t ScriptingManager::AddModuleReloadObserver(ModuleReloadObserver observer)
{
    if (!observer)
        return 0;
    std::lock_guard<std::mutex> lk(m_ReloadObserversMutex);
    const size_t token = m_NextReloadObserverToken++;
    m_ReloadObservers.push_back({token, std::move(observer)});
    return token;
}

void ScriptingManager::RemoveModuleReloadObserver(size_t token)
{
    if (token == 0)
        return;
    std::lock_guard<std::mutex> lk(m_ReloadObserversMutex);
    for (auto it = m_ReloadObservers.begin(); it != m_ReloadObservers.end(); ++it)
    {
        if (it->token == token)
        {
            m_ReloadObservers.erase(it);
            return;
        }
    }
}

void ScriptingManager::NotifyModuleReloaded(const std::string& module_id)
{
    // Snapshot under the lock so re-entrant subscribe/unsubscribe from
    // inside an observer (e.g. a component recreating itself) doesn't
    // invalidate iterators. The fn is copy-constructed; the captured state
    // is the observer's problem.
    std::vector<ReloadObserverEntry> snapshot;
    {
        std::lock_guard<std::mutex> lk(m_ReloadObserversMutex);
        snapshot = m_ReloadObservers;
    }
    for (auto& entry : snapshot)
    {
        if (entry.fn)
            entry.fn(module_id);
    }
}

std::string ScriptingManager::PathToModuleId(const std::filesystem::path& abs_js_path) const
{
    if (m_JsRoot.empty())
        return {};

    std::error_code ec;
    auto rel = std::filesystem::relative(abs_js_path, m_JsRoot, ec);
    if (ec)
        return {};

    auto rel_str = rel.generic_string();
    if (rel_str.empty() || rel_str.front() == '.')
        return {};  // outside js root

    // Strip trailing ".js".
    static constexpr const char kExt[] = ".js";
    static constexpr size_t kExtLen = sizeof(kExt) - 1;
    if (rel_str.size() > kExtLen && std::equal(rel_str.end() - kExtLen, rel_str.end(), kExt, [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == b; }))
    {
        rel_str.resize(rel_str.size() - kExtLen);
    }
    else
    {
        // Not a .js file - caller should have filtered. Be defensive.
        return {};
    }
    return rel_str;
}
