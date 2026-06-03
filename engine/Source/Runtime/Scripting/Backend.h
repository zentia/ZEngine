#pragma once

#include "Runtime/Scripting/Native/PuertsNative.h"

class ScriptEnv;

class Backend
{
public:
    virtual int GetApiVersion() = 0;

    virtual pesapi_env_ref CreateEnvRef() = 0;

    virtual struct pesapi_ffi* GetApi() = 0;

    virtual void DestroyEnvRef(pesapi_env_ref envRef) = 0;

    virtual pesapi_value GetModuleExecutor(pesapi_env env) = 0;

    virtual void* GetLoader() = 0;

    virtual void OnEnter(ScriptEnv* script_env) = 0;

    virtual void OnTick() = 0;

    virtual void OnExit(void* script_env) = 0;

    virtual void OpenRemoteDebugger(int port) = 0;

    virtual bool DebuggerTick() = 0;

    virtual void CloseRemoteDebugger() = 0;

    virtual void LowMemoryNotification() = 0;

    virtual std::string GetName() { return AUTO_GET_CLASS_NAME(); }
};