#pragma once

#include "Runtime/Scripting/Backend.h"
#if PAPI_NODEJS
namespace Runtime
{
    class BackendJS : public Backend
    {
    public:
        explicit BackendJS(void* loader) { m_Loader = loader; }

        virtual void* GetModuleExecutor(void* env) override;

    private:
        void* m_Loader;
    };
}  // namespace Runtime
#endif
