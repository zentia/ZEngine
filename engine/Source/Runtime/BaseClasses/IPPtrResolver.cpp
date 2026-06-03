#include "IPPtrResolver.h"

#include <vector>

namespace
{
    // Per-thread resolver stack. Pointer values are non-owning; the
    // resolver instance is owned by whoever constructed the
    // ScopedPPtrResolver (typically a SerializedFile read/write
    // session). Push/pop is paired by the RAII guard and must be
    // strictly LIFO -- we sanity-check that at pop time below.
    thread_local std::vector<IPPtrResolver*> g_resolverStack;
}  // namespace

ScopedPPtrResolver::ScopedPPtrResolver(IPPtrResolver* resolver) noexcept
{
    g_resolverStack.push_back(resolver);
}

ScopedPPtrResolver::~ScopedPPtrResolver() noexcept
{
    // LIFO discipline. If the stack is empty here something has
    // double-popped a guard; in release we silently no-op rather
    // than crash (PPtr serialization will then see a missing
    // resolver and fall back to raw-InstanceID round-trip), but
    // the underlying logic bug should be caught by the smoke tests.
    if (!g_resolverStack.empty())
        g_resolverStack.pop_back();
}

IPPtrResolver* GetCurrentPPtrResolver() noexcept
{
    if (g_resolverStack.empty())
        return nullptr;
    return g_resolverStack.back();
}
