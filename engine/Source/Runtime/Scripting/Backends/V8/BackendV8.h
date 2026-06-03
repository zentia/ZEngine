#pragma once

#include "Runtime/Scripting/Backend.h"

#if PAPI_V8
// BackendV8: pesapi-based V8 backend, structurally identical to
// BackendQuickJS. The pesapi FFI table abstracts over QuickJS / V8 so the
// rest of ZEngine's scripting layer (ScriptEnv, ScriptingManager,
// TypeScriptComponent, ...) needs zero conditional code per backend.
//
// V8 is selected for desktop editor + PC/console runtime. JIT brings
// script logic close to native C++ perf and the V8 Inspector path (wired
// in OpenRemoteDebugger / DebuggerTick / CloseRemoteDebugger) lets
// VSCode and Chrome DevTools attach via ws://127.0.0.1:<port>.
//
// Web (emscripten/wasm) and iOS / Android do NOT use this backend --
// they go through BackendQuickJS, which is JIT-free and tiny. See
// AGENTS.md 2.7 for the full backend split.
class BackendV8 : public Backend
{
public:
    explicit BackendV8(void* loader) { m_Loader = loader; }

    BackendV8() {}

    virtual pesapi_env_ref CreateEnvRef() override { return CreateV8PapiEnvRef(); }

    virtual int GetApiVersion() override { return GetV8PapiVersion(); }

    virtual struct pesapi_ffi* GetApi() override { return GetV8FFIApi(); }

    virtual void DestroyEnvRef(pesapi_env_ref envRef) override
    {
        DestroyV8PapiEnvRef(envRef);
    }

    virtual pesapi_value GetModuleExecutor(pesapi_env env) override;

    virtual void* GetLoader() override { return nullptr; }

    virtual void OnEnter(ScriptEnv* script_env) override;

    // Per-frame pump. The Editor main loop calls
    // ScriptingManager::Tick -> ScriptEnv::Tick -> Backend::OnTick once
    // per frame. When the V8 Inspector is open we MUST forward this into
    // puerts' InspectorTick (which calls Server.poll() on the websocket
    // server) -- otherwise inbound HTTP / WS frames sit in the kernel
    // buffer forever and Chrome DevTools / VSCode never receive a
    // response. Equivalent to UE puerts' FJsEnvImpl::Tick path. Cheap
    // when the inspector is closed (early-return inside DebuggerTick).
    virtual void OnTick() override;

    virtual void OnExit(void* /*script_env*/) override {}

    // V8 Inspector hooks. Implemented in backend_v8.cpp by calling into
    // puerts' papi-v8 C exports CreateInspector / InspectorTick /
    // DestroyInspector (see PapiExport.cpp). Each takes a v8::Isolate*
    // which we recover from the live ScriptEnv captured in OnEnter.
    //
    // ScriptEnv calls these in this order:
    //   ctor:  OnEnter(this) -> OpenRemoteDebugger(port)
    //   tick:  while (!DebuggerTick()) {}    // only when debugPort != -1
    //   dtor:  CloseRemoteDebugger()
    //
    // OnEnter MUST run before OpenRemoteDebugger so we can stash the
    // ScriptEnv pointer; the destructor's CloseRemoteDebugger fires
    // before DestroyEnvRef so v8::Isolate is still valid when we tear
    // the inspector down.
    virtual void OpenRemoteDebugger(int port) override;

    virtual bool DebuggerTick() override;

    virtual void CloseRemoteDebugger() override;

    virtual void LowMemoryNotification() override {}

private:
    void* m_Loader = nullptr;
    ScriptEnv* m_ScriptEnv = nullptr;  // captured in OnEnter; non-owning.
    bool m_InspectorOpen = false;      // true between Open* and Close*.
};
#endif
