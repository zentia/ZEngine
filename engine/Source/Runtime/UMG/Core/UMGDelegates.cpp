#include "Runtime/UMG/Core/UMGDelegates.h"

namespace ZUMG
{
// Function-local static: thread-safe init-on-first-use, no translation-unit
// init-order trap. Every TU that calls GetUMGEventRegistry() shares this one
// process-wide instance (assets reference event ids; the game binds them here).
UMGEventRegistry& GetUMGEventRegistry()
{
    static UMGEventRegistry s_Registry;
    return s_Registry;
}
}  // namespace ZUMG
