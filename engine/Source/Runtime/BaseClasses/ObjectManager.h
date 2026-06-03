#pragma once
#include "Runtime/Core/Base/EngineSystem.h"

#include <atomic>
#include <mutex>
#include <unordered_map>
class Object;
class Type;

// =====================================================================================
// ObjectManager — owns the global InstanceID ↔ Object* table that PPtr uses to
// resolve cross-object references.
// -------------------------------------------------------------------------------------
// History:
//   The original implementation defined `m_IDToPointer` but never wrote to it, so
//   `IDToPointer()` always returned nullptr. `AllocateAndAssignInstanceID()` was a
//   pass-through stub. Together this meant PPtr could never resolve back to a heap
//   object, even within a single process — only the constructor `PPtr(const T*)` and
//   the `m_InstanceID` field were ever actually used. Phase 2b lifts both stubs.
//
// Identity allocation policy (mirrors Unity's PersistentManager):
//   * Heap-allocated objects (via NewObject<T> / AllocateAndAssignInstanceID) get a
//     NEGATIVE instance ID, decreasing monotonically from -1. This keeps them
//     distinct from persisted-on-disk objects which get positive IDs from
//     SerializedFile/PersistentManager.
//   * Both spaces share the same IDToPointer table, so PPtr resolves uniformly
//     regardless of where the object came from.
//
// Thread safety:
//   The table is guarded by `m_TableMutex`. We don't (yet) have many threads
//   constructing Objects, but background asset loading + main-thread instantiation
//   already overlap, and this surface will only get hotter as the engine grows.
// =====================================================================================
class ObjectManager : public IEngineSystem
{
public:
    SystemInitPhase GetInitPhase() const override { return SystemInitPhase::PreInit; }
    std::vector<std::type_index> GetDependencies() const override;
    bool Initialize() override;
    void Shutdown() override {}

    /// Look up a registered Object by its instance ID. Returns nullptr if the id is
    /// unknown — callers usually then fall back to `ReadObjectFromPersistentManager`.
    Object* IDToPointer(int32_t instanceID);

    /// Construct a new object of `type` with a CALLER-PROVIDED instanceID, and
    /// register it. Used by the asset-loading path where the persistent file
    /// dictates the id. Pass instanceID=0 to skip registration (rare; mostly used
    /// during in-place re-read where the caller will assign and register later).
    Object* Produce(const Type* type, int32_t instanceID);

    /// Assign a fresh negative instance ID to a newly-heap-allocated Object and
    /// register it in the table. Idempotent: if `obj` already has a non-zero ID,
    /// only the registration is performed (the existing ID is kept).
    /// Returns the same `obj` for call-site chaining.
    Object* AllocateAndAssignInstanceID(Object* obj);

    /// Drop `instanceID` from the table. Called from `Object::~Object()`.
    /// Safe to call with id==0 (no-op).
    void UnregisterInstanceID(int32_t instanceID);

    template<typename T>
    T* NewObject()
    {
        return MemoryManager::CreateObject<T>();
    }

private:
    /// Insert/overwrite an `(id, obj)` pair into the table. Internal helper used by
    /// both AllocateAndAssignInstanceID and Produce to avoid duplicating the lock
    /// dance.
    void RegisterInstanceIDInternal(int32_t instanceID, Object* obj);

    std::unordered_map<int32_t, Object*> m_IDToPointer;
    std::mutex m_TableMutex;

    /// Decreasing counter: each call to AllocateAndAssignInstanceID gets the next
    /// value (so the FIRST allocated heap object is id=-1). atomic so we can drop
    /// the mutex around the bump itself; the table write still takes the lock.
    std::atomic<int32_t> m_NextHeapInstanceID {-1};
};
