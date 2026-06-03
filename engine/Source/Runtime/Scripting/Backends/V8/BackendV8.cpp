#include "BackendV8.h"

#if PAPI_V8

    #include "Runtime/Core/Base/Macro.h"
    #include "Runtime/Scripting/Wrapper/ScriptEnv.h"

// ----------------------------------------------------------------------------
// puerts V8 Inspector imports.
//
// Forward-declare the puerts papi-v8 exports we need for V8 Inspector.
// We deliberately do NOT pull in <v8.h> here -- v8::Isolate* is opaque to
// us; we just round-trip the pointer that GetV8Isolate() hands back into
// CreateInspector / InspectorTick / DestroyInspector.
//
// The signatures must match papi-v8/source/PapiExport.cpp exactly:
//
//     V8_EXPORT v8::Isolate *GetV8Isolate(pesapi_env_ref env_ref);
//     V8_EXPORT void  CreateInspector(v8::Isolate *Isolate, int32_t Port);
//     V8_EXPORT void  DestroyInspector(v8::Isolate *Isolate);
//     V8_EXPORT int   InspectorTick(v8::Isolate *Isolate);
//
// On a static link (wee8.lib + PapiV8.lib + ZRuntime), V8_EXPORT collapses
// to plain extern "C" so the linker matches by name only. v8::Isolate is
// declared empty here just so the type system has a name -- we never
// inspect it.
// ----------------------------------------------------------------------------
namespace v8
{
    class Isolate;
}

extern "C"
{
    v8::Isolate* GetV8Isolate(pesapi_env_ref env_ref);
    void CreateInspector(v8::Isolate* Isolate, int32_t Port);
    void DestroyInspector(v8::Isolate* Isolate);
    int InspectorTick(v8::Isolate* Isolate);
}

pesapi_value BackendV8::GetModuleExecutor(pesapi_env env)
{
    // Same shape as BackendQuickJS: probe globalThis.require and let
    // ScriptEnv ignore it if undefined. ZEngine's module loader is built
    // on top of an IIFE wrapper inside ScriptEnv (see ScriptEnv.cpp:
    // "Wrap in IIFE-factory so we get a function value back"), so we do
    // not need a real CommonJS resolver from the backend.
    auto&& papis = GetApi();
    auto&& globalVal = pesapi_global(papis, env);
    return pesapi_get_property(papis, env, globalVal, "require");
}

void BackendV8::OnEnter(ScriptEnv* scriptEnv)
{
    // V8 already provides ECMAScript built-ins (globalThis, etc.). The
    // host-side `console` and `Debug` shims are installed centrally by
    // ScriptEnv::InstallConsoleAndDebugGlobals after the env is created,
    // so there is nothing language-side to bootstrap here.
    //
    // What we DO use this hook for: capturing the live ScriptEnv pointer
    // so OpenRemoteDebugger / DebuggerTick / CloseRemoteDebugger can
    // recover the v8::Isolate* via puerts' GetV8Isolate(env_ref) export.
    // ScriptEnv calls OnEnter before OpenRemoteDebugger, so by the time
    // any inspector function fires, m_ScriptEnv is non-null.
    m_ScriptEnv = scriptEnv;
}

// ---------------------------------------------------------------------------
// V8 Inspector implementation
// ---------------------------------------------------------------------------
//
// Mirrors the C# Puerts implementation
// (engine/3rdparty/puerts/unity/upms/v8/Runtime/Src/Backends/BackendV8.cs):
//
//     OpenRemoteDebugger(port)   -> PapiV8Native.CreateInspector(isolate, port)
//     DebuggerTick()             -> PapiV8Native.InspectorTick(isolate)
//     CloseRemoteDebugger()      -> PapiV8Native.DestroyInspector(isolate)
//
// The C# layer hands isolate as a cached field; we recover it on demand
// from m_ScriptEnv via the puerts GetV8Isolate(env_ref) export. Doing it
// per-call rather than caching keeps us safe across env lifecycle without
// invalidation tracking, and Inspector wiring is only invoked at startup,
// once per tick, and at shutdown -- not a hot path.
// ---------------------------------------------------------------------------

void BackendV8::OpenRemoteDebugger(int port)
{
    if (m_ScriptEnv == nullptr)
    {
        // OnEnter hasn't run -- this can only happen if a future refactor
        // re-orders ScriptEnv's constructor. Fail loudly rather than
        // silently silently no-op.
        LOG_ERROR(ZScripting, "BackendV8::OpenRemoteDebugger called before OnEnter; inspector not started");
        return;
    }
    if (m_InspectorOpen)
    {
        LOG_WARNING(ZScripting, "BackendV8::OpenRemoteDebugger called twice (port={}); ignoring", port);
        return;
    }

    auto envRef = m_ScriptEnv->GetEnvRef();
    auto isolate = GetV8Isolate(envRef);
    if (isolate == nullptr)
    {
        LOG_ERROR(ZScripting, "BackendV8::OpenRemoteDebugger: GetV8Isolate returned null; inspector not started");
        return;
    }
    CreateInspector(isolate, static_cast<int32_t>(port));
    m_InspectorOpen = true;
    LOG_INFO(ZScripting, "BackendV8: V8 Inspector listening on ws://127.0.0.1:{}", port);
}

bool BackendV8::DebuggerTick()
{
    // Returning true here means "no message processed this tick, the host
    // can move on". ScriptEnv::Tick / WaitDebugger drive this in tight
    // loops (`while (!DebuggerTick()) {}`) so we must short-circuit when
    // the inspector isn't running, otherwise WaitDebugger would spin
    // forever waiting for a non-existent inspector.
    if (!m_InspectorOpen || m_ScriptEnv == nullptr)
    {
        return true;
    }
    auto envRef = m_ScriptEnv->GetEnvRef();
    auto isolate = GetV8Isolate(envRef);
    if (isolate == nullptr)
    {
        return true;
    }
    // puerts' InspectorTick returns 1 when a message was processed (or
    // when paused at a breakpoint and waiting for resume), 0 when the
    // queue is empty. ScriptEnv treats a `true` return as "done for now".
    return InspectorTick(isolate) != 0;
}

void BackendV8::OnTick()
{
    // Per-frame pump for the V8 Inspector websocket server.
    //
    // puerts' InspectorTick -> V8InspectorClientImpl::Tick -> Server.poll()
    // is non-blocking: it drains whatever frames the kernel has accepted
    // and returns. Without this, /json HTTP discovery hangs (the OS
    // accepted the TCP connection but no user-space code ever called
    // accept()/read()) and WS upgrades from Chrome DevTools time out.
    //
    // We deliberately do NOT loop on InspectorTick here. WaitDebugger /
    // breakpoint resume is the only place that should spin -- those go
    // through DebuggerTick and ScriptEnv::WaitDebugger, not OnTick.
    //
    // Cheap when the inspector is closed: DebuggerTick early-returns true
    // before touching v8.
    DebuggerTick();
}

void BackendV8::CloseRemoteDebugger()
{
    if (!m_InspectorOpen || m_ScriptEnv == nullptr)
    {
        return;
    }
    auto envRef = m_ScriptEnv->GetEnvRef();
    auto isolate = GetV8Isolate(envRef);
    if (isolate != nullptr)
    {
        DestroyInspector(isolate);
    }
    m_InspectorOpen = false;
    LOG_INFO(ZScripting, "BackendV8: V8 Inspector closed");
}

#endif  // PAPI_V8
