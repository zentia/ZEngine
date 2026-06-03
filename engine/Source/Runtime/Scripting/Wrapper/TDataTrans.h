#pragma once

#include "pesapi.h"

namespace Puerts
{
    using FieldWrapFuncPtr = void (*)(struct pesapi_ffi* apis, pesapi_callback_info info, void* field);
    using WrapFuncPtr = bool (*)(struct pesapi_ffi* apis, void* method, pesapi_callback_info info, pesapi_env env, void* self, bool checkArgument);

    struct FieldWrapData
    {
        FieldWrapFuncPtr getter;
        FieldWrapFuncPtr setter;
        void* fieldInfo;
    };

    struct WrapData
    {
        WrapFuncPtr wrap;
        void* method;
        bool isStatic;
    };

    struct PObjectRefInfo
    {
        struct pesapi_ffi* apis;
        pesapi_value_ref valueRef;
        void* envPrivate;
    };

    class AutoValueScope
    {
    public:
        AutoValueScope(struct pesapi_ffi* apis, pesapi_env_ref envRef)
        {
            m_Apis = apis;
            m_Scope = apis->open_scope_placement(envRef, &m_Mem);
        }

        ~AutoValueScope()
        {
            if (m_Scope)
            {
                m_Apis->close_scope_placement(m_Scope);
            }
        }

        inline pesapi_scope scope()
        {
            return m_Scope;
        }

        struct pesapi_ffi* m_Apis;
        pesapi_scope_memory m_Mem;
        pesapi_scope m_Scope;
    };

    struct DataTransfer
    {
    };

    struct WrapFuncInfo
    {
    };

    struct BridgeFuncInfo
    {
    };

    struct FieldWrapFuncInfo
    {
    };

    namespace Converter
    {
        template<typename T, typename Enable = void>
        struct Converter;
    }
}  // namespace Puerts