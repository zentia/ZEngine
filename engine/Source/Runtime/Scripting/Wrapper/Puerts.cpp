#include "Puerts.h"

#include "TDataTrans.h"

namespace Puerts
{
    static pesapi_registry_api* g_registry_api = nullptr;
    static pesapi_registry g_registry = nullptr;

    struct MethodInfo
    {
        const char* name;
        bool isStatic;
        bool isGetter;
    };

    struct FieldInfo
    {
        bool isStatic;
    };

    struct JsClassInfoHeader
    {
    };

    struct JsClassInfo : public JsClassInfoHeader
    {
        std::vector<WrapData*> ctors;
        std::vector<MethodInfo> methods;
        std::vector<FieldInfo> fields;
    };

    void* EvalInternal(struct pesapi_ffi* apis, pesapi_env_ref envRef, void* code, void* path)
    {
        AutoValueScope valueScope(apis, envRef);
        auto env = apis->get_env_from_ref(envRef);

        return nullptr;
    }

    void InitialPuerts(pesapi_registry_api* registry_api, pesapi_registry registry)
    {
        g_registry_api = registry_api;
        g_registry = registry;
    }

    struct JsEnvPrivate
    {
        struct pesapi_ffi* apis;
        pesapi_env_ref envRef;

        void AddPendingKillScriptObjects(pesapi_value_ref valueRef)
        {
            uint32_t fieldCount;
            void** store = apis->get_ref_internal_fields(valueRef, &fieldCount);
        }

        void CleanupPendingKillScriptObjects()
        {
            AutoValueScope valueScope(apis, envRef);
            auto env = apis->get_env_from_ref(envRef);
        }
    };

    void AddPendingKillScriptObjects(struct pesapi_ffi* apis, JsEnvPrivate* jsEnvPrivate, pesapi_value_ref valueRef)
    {
        pesapi_env_ref envRef = apis->get_ref_associated_env(valueRef);
        if (!apis->env_ref_is_valid(envRef))
        {
            apis->release_value_ref(valueRef);
            return;
        }
        jsEnvPrivate->AddPendingKillScriptObjects(valueRef);
    }

    void CleanupPendingKillScriptObjects(Puerts::JsEnvPrivate* jsEnvPrivate)
    {
        jsEnvPrivate->CleanupPendingKillScriptObjects();
    }
}  // namespace Puerts
