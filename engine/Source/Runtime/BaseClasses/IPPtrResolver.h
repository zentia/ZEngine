#pragma once

// IPPtrResolver — analogue of Unity's ILSOIResolver, used by PPtr<T>::Transfer
// to translate between (m_FileID, m_PathID) on disk and engine-runtime
// InstanceID. The active resolver is per-thread (TLS stack); callers push
// one with ScopedPPtrResolver around any Read/Write that may serialize
// PPtr fields, and pop on scope exit.
//
// Per-thread granularity is intentional and differs from Unity's single
// shared resolver: AssetRegistry scan and AssetManager bulk loads run
// concurrently and would otherwise contend on a global mutex. Each thread
// services its own SerializedFile, so a thread_local stack maps cleanly
// onto the actual ownership boundary.

#include "LocalSerializedObjectIdentifier.h"

#include <stdint.h>

class IPPtrResolver
{
public:
    virtual ~IPPtrResolver() = default;

    // Read path: convert a (FileID, PathID) pair found in a serialized
    // PPtr field into the runtime InstanceID of the target Object.
    // Returns 0 if the reference is null or unresolvable; the PPtr will
    // then deserialize as null and ObjectManager fallthrough kicks in
    // when the consumer dereferences it.
    virtual int32_t LSOIToInstanceID(const LocalSerializedObjectIdentifier& lsoi) = 0;

    // Write path: convert a runtime InstanceID into the (FileID, PathID)
    // pair to write. The resolver is responsible for adding any new
    // external file to its m_Externals table on first sight and
    // returning a stable, deduplicated FileID. Self-references must
    // resolve to FileID == 0.
    virtual void InstanceIDToLSOI(int32_t instanceID, LocalSerializedObjectIdentifier& out) = 0;
};

// RAII guard. Constructor pushes `r` onto the calling thread's resolver
// stack; destructor pops it. Nested scopes are supported (each scope
// shadows the outer resolver until it exits). Passing nullptr is legal
// and pushes a null marker — useful for unit tests that want to assert
// PPtr serialization fails loudly when no resolver is active.
class ScopedPPtrResolver
{
public:
    explicit ScopedPPtrResolver(IPPtrResolver* resolver) noexcept;
    ~ScopedPPtrResolver() noexcept;

    ScopedPPtrResolver(const ScopedPPtrResolver&) = delete;
    ScopedPPtrResolver& operator=(const ScopedPPtrResolver&) = delete;
    ScopedPPtrResolver(ScopedPPtrResolver&&) = delete;
    ScopedPPtrResolver& operator=(ScopedPPtrResolver&&) = delete;
};

// Returns the resolver on top of the current thread's stack, or nullptr
// if no scope is active. PPtr<T>::Transfer uses this to fall back to
// raw m_InstanceID round-trip when no resolver is registered (legacy
// in-memory serialization paths, smoke tests for primitive transfer,
// etc.) — keeping behaviour byte-identical with the pre-PR-SE3a-refine
// world for any code path that hasn't opted into PPtr remapping yet.
IPPtrResolver* GetCurrentPPtrResolver() noexcept;
