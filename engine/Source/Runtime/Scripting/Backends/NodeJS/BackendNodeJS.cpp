#include "BackendNodeJS.h"
#if PAPI_NODEJS
namespace Runtime
{
    void* BackendJS::GetModuleExecutor(void* env)
    {
        auto papis = getApi();
        return nullptr;
    }
}  // namespace Runtime
#endif
