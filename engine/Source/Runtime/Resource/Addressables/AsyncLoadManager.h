#pragma once

#include "Runtime/Core/Base/EngineSystem.h"
#include "Runtime/Core/Thread/TaskQueue.h"
#include "Runtime/Resource/Addressables/AssetHandle.h"

#include <EASTL/string.h>
#include <EASTL/unordered_map.h>
#include <EASTL/vector.h>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

// Required by the out-of-line template definitions at the bottom of this file
// (TypeContainer<T>::m_Type and the Object* result slot). Kept as forward
// declarations above so gameplay code including this header for a handle type
// does not drag the reflection machinery along.
#include "Runtime/BaseClasses/Object.h"
#include "Runtime/BaseClasses/Type.h"

class Object;
class Type;

namespace zengine::addressables
{

// Address -> on-disk path. Mirrors the Addressables group catalog: game code
// asks for "Characters/Hero", never for a path, so content can move between
// bundles without touching call sites.
struct CatalogEntry
{
    eastl::string address;      // logical key, e.g. "Characters/Hero"
    eastl::string asset_path;   // project-relative path to the .zasset/.json
    eastl::string guid;         // AssetRegistry GUID, empty if unregistered
    eastl::string asset_type;   // runtime type name, e.g. "MeshData"
    eastl::vector<eastl::string> dependencies;  // addresses loaded first
};

// -----------------------------------------------------------------------------
// AsyncLoadManager — ZEngine's Addressables runtime.
//
// Design constraint that shaped everything here: AssetManager is NOT thread
// safe. ReadObject() mutates m_Streams, m_ActiveStreams, the Remapper tables
// and the path-namespace table with no lock at all (the "ThreadSafe" in
// WriteObjectsToDiskThreadSafe is aspirational — that function also just
// forwards to WriteFile). SerializedFile itself has no locks either.
//
// So we use the same split Unity uses:
//
//   worker thread  : open the file, read the bytes, parse the SerializedFile
//                    header + TypeTree into a self-contained buffer. Touches
//                    nothing shared.
//   game thread    : take that buffer and hydrate the Object graph through
//                    AssetManager. All shared state mutation happens here,
//                    serialized by virtue of being on one thread.
//
// The parse step is where the wall-clock win comes from — file IO and
// TypeTree/header parsing dominate for large assets — while avoiding the
// alternative of putting a global lock around AssetManager, which would
// serialize every load and add contention to the synchronous paths that
// already work.
//
// ObjectManager is genuinely thread safe (IDToPointer and
// RegisterInstanceIDInternal take m_TableMutex; m_NextHeapInstanceID is
// atomic), and the PPtrResolver stack is thread_local, so nothing else on the
// hydration path needs guarding.
// -----------------------------------------------------------------------------
class AsyncLoadManager : public IEngineSystem
{
public:
    std::string GetName() const override { return "AsyncLoadManager"; }
    SystemInitPhase GetInitPhase() const override { return SystemInitPhase::Resource; }
    std::vector<std::type_index> GetDependencies() const override;

    bool Initialize() override;
    void Shutdown() override;

    // --- Catalog ---

    /// Register an address -> path mapping. Re-registering the same address
    /// overwrites. Typically populated from the Addressables group asset or,
    /// in the editor, straight from AssetRegistry.
    void RegisterAddress(const eastl::string& address,
                         const eastl::string& asset_path,
                         const eastl::string& guid = eastl::string(),
                         const eastl::string& asset_type = eastl::string());

    /// Register an address together with the addresses it depends on.
    /// Dependencies are guaranteed loaded before the dependent asset's
    /// completion fires, so PPtrs to them resolve during hydration.
    void RegisterAddressWithDependencies(const eastl::string& address,
                                         const eastl::string& asset_path,
                                         const eastl::vector<eastl::string>& dependencies,
                                         const eastl::string& guid = eastl::string(),
                                         const eastl::string& asset_type = eastl::string());

    bool TryGetCatalogEntry(const eastl::string& address, CatalogEntry& out_entry) const;
    void ClearCatalog();
    size_t GetCatalogSize() const;

    /// Build the catalog from AssetRegistry (editor/runtime-with-registry).
    /// Maps every indexed asset to an address derived from its project-relative
    /// path. No-op when GetAssetsByType yields nothing, which is the case for
    /// standalone runtime builds that ship without a registry.
    void RebuildCatalogFromRegistry();

    // --- Loading ---

    /// Issue an async load. Callable from any thread. The returned handle is
    /// immediately valid; IsDone() becomes true on the game thread once
    /// hydration finishes.
    template<typename T>
    AssetHandle<T> LoadAssetAsync(const eastl::string& address);

    /// Synchronous load through the same address layer. Blocks. Provided for
    /// call sites that cannot be made async yet, and for tests.
    template<typename T>
    AssetHandle<T> LoadAssetSync(const eastl::string& address);

    /// Per-frame pump. MUST be called on the game thread: it drains the
    /// completion queue and runs hydration. Without it no load ever completes.
    void Tick(float delta_time);

    /// Number of operations still in flight.
    size_t GetPendingCount() const;

    /// Blocks the calling thread until `handle` completes, pumping the
    /// completion queue while waiting when called on the game thread (so
    /// WaitForCompletion() from game code cannot deadlock against its own
    /// queued hydration step).
    void WaitForCompletion(OperationId id);

    /// Fetch the hydrated object for a completed operation. Returns nullptr
    /// while still loading or on failure. Used by AssetHandle<T>::GetResult().
    Object* GetResultForOperation(OperationId id);

    // --- Unloading ---

    /// Drop one reference. The asset is unloaded when the last handle is
    /// released. Refcount semantics like Unity Addressables.
    void ReleaseHandle(OperationId id);

    /// Force-unload regardless of outstanding handles. Escape hatch for level
    /// transitions; dangling handles will observe a null result.
    void UnloadAddress(const eastl::string& address);

    void UnloadAll();

    // --- Config ---

    /// Worker threads used for the IO/parse stage. Zero means "do the parse on
    /// the game thread during Tick" (single-threaded platforms / Web).
    void SetWorkerThreadCount(size_t count);
    size_t GetWorkerThreadCount() const { return m_workerThreads.size(); }

    /// Hard cap on bytes held in the staging area, to bound memory when many
    /// loads are issued at once. Loads beyond the cap simply wait.
    void SetMaxConcurrentLoads(size_t max_loads) { m_maxConcurrentLoads.store(max_loads); }

    struct Statistics
    {
        size_t issued = 0;
        size_t completed = 0;
        size_t failed = 0;
        size_t cancelled = 0;
        size_t in_flight = 0;
    };
    Statistics GetStatistics() const;

private:
    friend class AssetHandle<void>;

    // Payload produced by the worker thread: the raw bytes plus enough metadata
    // for the game thread to hydrate without touching the file again.
    struct StagedLoad
    {
        // Carried explicitly instead of looking the operation up by address:
        // two concurrent requests for the same address are perfectly legal, and
        // an address-keyed lookup would complete the wrong one and leave the
        // other waiting forever.
        OperationId operation_id {0};
        eastl::vector<uint8_t> bytes;
        eastl::string address;
        eastl::string asset_path;
        eastl::string asset_type;
        eastl::vector<eastl::string> dependencies;
        bool read_ok = false;
    };

    // Untyped issue path; templates above forward here.
    OperationId IssueLoad(const eastl::string& address, const Type* type, Object** out_result);

    void WorkerThreadMain();
    void PumpCompletions();
    void FinalizeLoad(const std::shared_ptr<StagedLoad>& staged);

    // Runs on the game thread. Turns staged bytes into a live Object.
    Object* HydrateOnGameThread(const StagedLoad& staged, const Type* type);

    OperationId AllocateOperationId();

    mutable std::mutex m_catalogMutex;
    eastl::unordered_map<eastl::string, CatalogEntry> m_catalog;

    // Live operations, keyed by id. Entries are erased on completion.
    mutable std::mutex m_operationsMutex;
    eastl::unordered_map<OperationId, std::shared_ptr<AsyncOperationBase>> m_operations;
    eastl::unordered_map<OperationId, const Type*> m_operationTypes;
    eastl::unordered_map<OperationId, Object**> m_operationResults;

    // Refcount per address, for Release semantics.
    eastl::unordered_map<eastl::string, size_t> m_addressRefCounts;
    eastl::unordered_map<OperationId, eastl::string> m_operationAddresses;

    // Stage 1 (worker) -> stage 2 (game thread) handoff.
    std::mutex m_stagedMutex;
    std::condition_variable m_stagedCv;
    eastl::vector<std::shared_ptr<StagedLoad>> m_stagedReady;

    // Work items waiting to enter stage 1.
    TaskQueue m_pendingQueue;

    std::vector<std::thread> m_workerThreads;
    std::atomic<bool> m_shutdown {false};
    std::atomic<size_t> m_maxConcurrentLoads {4};
    std::atomic<size_t> m_activeLoads {0};

    Statistics m_stats;
    mutable std::mutex m_statsMutex;

    std::atomic<OperationId> m_nextOperationId {1};
};

// ---------------------------------------------------------------------------
// Template definitions
//
// Kept out-of-line so AsyncLoadManager.h does not have to include Type.h or
// Object.h — those pull in the whole reflection machinery, and this header is
// included by gameplay code that only wants a handle.
// ---------------------------------------------------------------------------

template<typename T>
AssetHandle<T> AsyncLoadManager::LoadAssetAsync(const eastl::string& address)
{
    Object** slot = new Object*(nullptr);
    const Type* type = reinterpret_cast<Type*>(&TypeContainer<T>::m_Type);
    const OperationId id = IssueLoad(address, type, slot);

    std::shared_ptr<AsyncOperationBase> operation;
    {
        std::lock_guard<std::mutex> lock(m_operationsMutex);
        auto&& it = m_operations.find(id);
        if (it != m_operations.end())
        {
            operation = it->second;
        }
    }

    return AssetHandle<T>(operation);
}

template<typename T>
AssetHandle<T> AsyncLoadManager::LoadAssetSync(const eastl::string& address)
{
    AssetHandle<T> handle = LoadAssetAsync<T>(address);
    if (handle.IsValid())
    {
        WaitForCompletion(handle.GetId());
    }
    return handle;
}

// --- AssetHandle<T> ---

template<typename T>
T* AssetHandle<T>::WaitForCompletion() const
{
    if (m_operation == nullptr)
    {
        return nullptr;
    }

    AsyncLoadManager* manager = GET_SYSTEM(AsyncLoadManager);
    if (manager != nullptr)
    {
        manager->WaitForCompletion(m_operation->GetId());
    }
    return GetResult();
}

template<typename T>
T* AssetHandle<T>::GetResult() const
{
    AsyncLoadManager* manager = GET_SYSTEM(AsyncLoadManager);
    if (manager == nullptr || m_operation == nullptr)
    {
        return nullptr;
    }
    return static_cast<T*>(manager->GetResultForOperation(m_operation->GetId()));
}

template<typename T>
void AssetHandle<T>::Release()
{
    if (m_operation == nullptr)
    {
        return;
    }
    AsyncLoadManager* manager = GET_SYSTEM(AsyncLoadManager);
    if (manager != nullptr)
    {
        manager->ReleaseHandle(m_operation->GetId());
    }
    m_operation.reset();
}

} // namespace zengine::addressables
