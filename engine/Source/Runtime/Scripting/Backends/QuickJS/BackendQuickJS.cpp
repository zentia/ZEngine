#include "BackendQuickJS.h"

#if PAPI_QUICKJS

    #include "Runtime/Scripting/Wrapper/ScriptEnv.h"

pesapi_value BackendQuickJS::GetModuleExecutor(pesapi_env env)
{
    // QuickJS exposes commonjs-like loading via globalThis.require when the
    // backend env has been seeded with one. For a hello-world bring-up we do
    // not need a loader yet, so just return the global "require" probe and
    // let ScriptEnv ignore it if undefined.
    auto&& papis = GetApi();
    auto&& globalVal = pesapi_global(papis, env);
    return pesapi_get_property(papis, env, globalVal, "require");
}

void BackendQuickJS::OnEnter(ScriptEnv* /*scriptEnv*/)
{
    // QuickJS already provides ECMAScript built-ins (globalThis, console.log,
    // print on shell builds, etc.). Puerts' pesapi_eval call wires print
    // through to the host log callback, so there's nothing language-side to
    // bootstrap right now. The hook is kept so that future modifications
    // (e.g. injecting a `Z` global with engine bindings) have a single place
    // to land.
}

#endif  // PAPI_QUICKJS
