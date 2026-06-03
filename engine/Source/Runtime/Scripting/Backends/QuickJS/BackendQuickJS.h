#pragma once

#include "Runtime/Scripting/Backend.h"

#if PAPI_QUICKJS
class BackendQuickJS : public Backend
{
public:
    explicit BackendQuickJS(void* loader) { m_Loader = loader; }

    BackendQuickJS() {}

    virtual pesapi_env_ref CreateEnvRef() override { return CreateQjsPapiEnvRef(); }

    virtual int GetApiVersion() override { return GetQjsPapiVersion(); }

    virtual struct pesapi_ffi* GetApi() override { return GetQjsFFIApi(); }

    virtual void DestroyEnvRef(pesapi_env_ref envRef) override
    {
        DestroyQjsPapiEnvRef(envRef);
    }

    virtual pesapi_value GetModuleExecutor(pesapi_env env) override;

    virtual void* GetLoader() override { return nullptr; }

    virtual void OnEnter(ScriptEnv* script_env) override;

    virtual void OnTick() override {}

    virtual void OnExit(void* script_env) override {}

    virtual void OpenRemoteDebugger(int port) override {}

    virtual bool DebuggerTick() override { return false; }

    virtual void CloseRemoteDebugger() override {}

    virtual void LowMemoryNotification() override {}

private:
    void* m_Loader = nullptr;
};
#endif
