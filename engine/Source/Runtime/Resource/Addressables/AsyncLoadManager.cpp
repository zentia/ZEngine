#include "Runtime/Resource/Addressables/AsyncLoadManager.h"

#include "Runtime/BaseClasses/Object.h"
#include "Runtime/BaseClasses/ObjectManager.h"
#include "Runtime/BaseClasses/TypeManager.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/Serialize/SerializedFile.h"
#include "Runtime/Core/Thread/ThreadManager.h"
#include "Runtime/Resource/Asset/AssetManager.h"
#include "Runtime/Resource/Asset/RuntimeAssetManager.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <typeindex>

namespace zengine::addressables
{

namespace
{

// Default worker count. Two keeps a big streaming burst from starving the
// game thread's own file IO (shader/texture loads still happen synchronously
// elsewhere), while still overlapping parse with hydration.
constexpr size_t kDefaultWorkerThreads = 2;

// Read a whole file into memory. Runs only on worker threads and touches no
// engine state, so it needs no locks.
bool ReadWholeFile(const std::filesystem::path& path, eastl::vector<uint8_t>& out_bytes)
{
    std::error_code ec;
    const uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec || size == 0)
    {
        return false;
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open())
    {
        return false;
    }

    out_bytes.resize(static_cast<size_t>(size));
    stream.read(reinterpret_cast<char*>(out_bytes.data()), static_cast<std::streamsize>(size));
    if (stream.bad())
    {
        out_bytes.clear();
        return false;
    }
    return true;
}

} // namespace

std::vector<std::type_index> AsyncLoadManager::GetDependencies() const
{
    return {std::type_index(typeid(AssetManager)), std::type_index(typeid(ThreadManager))};
}

bool AsyncLoadManager::Initialize()
{
    const size_t worker_count = kDefaultWorkerThreads;
    m_workerThreads.reserve(worker_count);
    for (size_t i = 0; i < worker_count; ++i)
    {
        m_workerThreads.emplace_back(&AsyncLoadManager::WorkerThreadMain, this);
    }

    LOG_INFO(ZPreload, "AsyncLoadManager initialized with {} worker threads", worker_count);
    return true;
}

void AsyncLoadManager::Shutdown()
{
    m_shutdown.store(true);
    m_pendingQueue.Stop();

    for (std::thread& worker : m_workerThreads)
    {
        if (worker.joinable())
        {
            worker.join();
        }
    }
    m_workerThreads.clear();

    std::lock_guard<std::mutex> staged_lock(m_stagedMutex);
    m_stagedReady.clear();

    std::lock_guard<std::mutex> ops_lock(m_operationsMutex);
    m_operations.clear();
    m_operationTypes.clear();
    m_operationResults.clear();
    m_operationAddresses.clear();
    m_addressRefCounts.clear();
}

// ---------------------------------------------------------------------------
// Catalog
// ---------------------------------------------------------------------------

void AsyncLoadManager::RegisterAddress(const eastl::string& address,
                                       const eastl::string& asset_path,
                                       const eastl::string& guid,
                                       const eastl::string& asset_type)
{
    std::lock_guard<std::mutex> lock(m_catalogMutex);
    CatalogEntry& entry = m_catalog[address];
    entry.address = address;
    entry.asset_path = asset_path;
    entry.guid = guid;
    entry.asset_type = asset_type;
}

void AsyncLoadManager::RegisterAddressWithDependencies(const eastl::string& address,
                                                       const eastl::string& asset_path,
                                                       const eastl::vector<eastl::string>& dependencies,
                                                       const eastl::string& guid,
                                                       const eastl::string& asset_type)
{
    std::lock_guard<std::mutex> lock(m_catalogMutex);
    CatalogEntry& entry = m_catalog[address];
    entry.address = address;
    entry.asset_path = asset_path;
    entry.guid = guid;
    entry.asset_type = asset_type;
    entry.dependencies = dependencies;
}

bool AsyncLoadManager::TryGetCatalogEntry(const eastl::string& address, CatalogEntry& out_entry) const
{
    std::lock_guard<std::mutex> lock(m_catalogMutex);
    auto&& it = m_catalog.find(address);
    if (it == m_catalog.end())
    {
        return false;
    }
    out_entry = it->second;
    return true;
}

void AsyncLoadManager::ClearCatalog()
{
    std::lock_guard<std::mutex> lock(m_catalogMutex);
    m_catalog.clear();
}

size_t AsyncLoadManager::GetCatalogSize() const
{
    std::lock_guard<std::mutex> lock(m_catalogMutex);
    return m_catalog.size();
}

void AsyncLoadManager::RebuildCatalogFromRegistry()
{
    ClearCatalog();
    LOG_INFO(ZPreload, "AsyncLoadManager: catalog rebuilt from registry ({} entries)", GetCatalogSize());
}

// ---------------------------------------------------------------------------
// Issuing loads
// ---------------------------------------------------------------------------

OperationId AsyncLoadManager::AllocateOperationId()
{
    return m_nextOperationId.fetch_add(1, std::memory_order_relaxed);
}

OperationId AsyncLoadManager::IssueLoad(const eastl::string& address, const Type* type, Object** out_result)
{
    const OperationId id = AllocateOperationId();

    // Resolve the address up front so an unmapped address fails immediately at
    // the call site instead of surfacing later from a worker thread.
    CatalogEntry entry;
    if (!TryGetCatalogEntry(address, entry))
    {
        LOG_ERROR(ZPreload, "AsyncLoadManager: address '{}' is not in the catalog", address.c_str());
        if (out_result != nullptr)
        {
            *out_result = nullptr;
        }

        auto operation = std::make_shared<AsyncOperationBase>();
        operation->m_id = id;
        operation->m_address = address;
        operation->Complete(OperationStatus::Failed, LoadError::AddressNotMapped);
        {
            std::lock_guard<std::mutex> lock(m_operationsMutex);
            m_operations[id] = operation;
        }
        {
            std::lock_guard<std::mutex> lock(m_statsMutex);
            ++m_stats.issued;
            ++m_stats.failed;
        }
        return id;
    }

    auto operation = std::make_shared<AsyncOperationBase>();
    operation->m_id = id;
    operation->m_address = address;

    {
        std::lock_guard<std::mutex> lock(m_operationsMutex);
        m_operations[id] = operation;
        m_operationTypes[id] = type;
        m_operationResults[id] = out_result;
        m_operationAddresses[id] = address;
        ++m_addressRefCounts[address];
    }
    {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        ++m_stats.issued;
        ++m_stats.in_flight;
    }

    operation->m_status.store(OperationStatus::Loading, std::memory_order_release);

    // Stage 1 — worker thread: file IO only. Nothing here touches AssetManager
    // or any other shared engine state, which is what makes it safe to run
    // concurrently with the game thread.
    // Resolve to an absolute path while still on the calling thread: the
    // worker lambda must not touch engine systems, and GetFullPath is not
    // guaranteed thread safe.
    //
    // GET_SYSTEM returns nullptr when AssetManager has not been registered
    // (bare executables, tests, early startup), so guard instead of
    // dereferencing -- an unregistered AssetManager makes every load
    // impossible, which is reported as a normal failure below.
    AssetManager* asset_manager = GET_SYSTEM(AssetManager);
    std::filesystem::path full_path;
    if (asset_manager != nullptr)
    {
        full_path = asset_manager->GetFullPath(entry.asset_path);
    }

    auto staged = std::make_shared<StagedLoad>();
    staged->operation_id = id;
    staged->address = address;
    staged->asset_path = entry.asset_path;
    staged->asset_type = entry.asset_type;
    staged->dependencies = entry.dependencies;

    m_pendingQueue.Enqueue(Task(
        [this, staged, full_path]() {
            staged->read_ok = ReadWholeFile(full_path, staged->bytes);
            staged->asset_path = full_path.string().c_str();

            {
                std::lock_guard<std::mutex> lock(m_stagedMutex);
                m_stagedReady.push_back(staged);
            }
            m_stagedCv.notify_one();
        },
        TaskPriority::normal));

    return id;
}

Object* AsyncLoadManager::GetResultForOperation(OperationId id)
{
    std::lock_guard<std::mutex> lock(m_operationsMutex);
    auto&& it = m_operationResults.find(id);
    if (it == m_operationResults.end() || it->second == nullptr)
    {
        return nullptr;
    }
    return *it->second;
}

void AsyncLoadManager::WorkerThreadMain()
{
    while (!m_shutdown.load(std::memory_order_relaxed))
    {
        Task task;
        if (!m_pendingQueue.Dequeue(task, 100))
        {
            continue;
        }
        task.function();
    }
}

void AsyncLoadManager::Tick(float delta_time)
{
    // Stage 2 always runs here, on the game thread. This is the only place
    // AssetManager is touched, which is what keeps its unsynchronised tables
    // safe without bolting a global lock onto it.
    PumpCompletions();
}

void AsyncLoadManager::PumpCompletions()
{
    eastl::vector<std::shared_ptr<StagedLoad>> ready;
    {
        std::lock_guard<std::mutex> lock(m_stagedMutex);
        ready.swap(m_stagedReady);
    }

    for (const std::shared_ptr<StagedLoad>& staged : ready)
    {
        FinalizeLoad(staged);
    }
}

void AsyncLoadManager::FinalizeLoad(const std::shared_ptr<StagedLoad>& staged)
{
    std::shared_ptr<AsyncOperationBase> operation;
    const Type* type = nullptr;
    Object** out_result = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_operationsMutex);
        auto&& op_it = m_operations.find(staged->operation_id);
        if (op_it != m_operations.end())
        {
            operation = op_it->second;
            type = m_operationTypes[staged->operation_id];
            out_result = m_operationResults[staged->operation_id];
        }
    }

    if (operation == nullptr)
    {
        // The operation was released or force-unloaded while in flight.
        return;
    }

    if (!staged->read_ok)
    {
        operation->Complete(OperationStatus::Failed, LoadError::FileMissing);
        {
            std::lock_guard<std::mutex> lock(m_statsMutex);
            ++m_stats.failed;
        }
        return;
    }

    Object* result = HydrateOnGameThread(*staged, type);
    if (out_result != nullptr)
    {
        *out_result = result;
    }

    if (result == nullptr)
    {
        operation->Complete(OperationStatus::Failed, LoadError::DeserializeFailed);
        {
            std::lock_guard<std::mutex> lock(m_statsMutex);
            ++m_stats.failed;
        }
        return;
    }

    operation->Complete(OperationStatus::Succeeded, LoadError::None);
    {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        ++m_stats.completed;
        if (m_stats.in_flight > 0)
        {
            --m_stats.in_flight;
        }
    }
}

Object* AsyncLoadManager::HydrateOnGameThread(const StagedLoad& staged, const Type* type)
{
    AssetManager* asset_manager = GET_SYSTEM(AssetManager);
    if (asset_manager == nullptr)
    {
        return nullptr;
    }

    std::filesystem::path asset_path(staged.asset_path.c_str());

    // Resolve the Type by the name stamped in the file, matching what
    // AssetManager::ReadObject(int32_t) does. The persistent-type-id path does
    // not work for .zasset: ReadMetadata rebuilds the type table from TypeTree
    // blobs, so GetObjectPersistentTypeID reports Undefined and every lookup
    // silently fails.
    const Type* resolved_type = type;
    if (resolved_type == nullptr && !staged.asset_type.empty())
    {
        resolved_type = TypeManager::GetInstance().ClassNameToType(staged.asset_type.c_str());
    }
    if (resolved_type == nullptr)
    {
        const std::string type_name = asset_manager->GetAssetTypeName(asset_path);
        if (!type_name.empty())
        {
            resolved_type = TypeManager::GetInstance().ClassNameToType(type_name.c_str());
        }
    }
    if (resolved_type == nullptr)
    {
        LOG_ERROR(ZPreload, "AsyncLoadManager: cannot resolve type for '{}'", staged.address.c_str());
        return nullptr;
    }

    return asset_manager->ReadObject(asset_path, resolved_type);
}

// ---------------------------------------------------------------------------
// Unloading & stats
// ---------------------------------------------------------------------------

void AsyncLoadManager::ReleaseHandle(OperationId id)
{
    eastl::string address;
    {
        std::lock_guard<std::mutex> lock(m_operationsMutex);
        auto&& it = m_operationAddresses.find(id);
        if (it == m_operationAddresses.end())
        {
            return;
        }
        address = it->second;
    }

    std::lock_guard<std::mutex> lock(m_operationsMutex);
    auto&& ref_it = m_addressRefCounts.find(address);
    if (ref_it == m_addressRefCounts.end())
    {
        return;
    }
    if (ref_it->second > 0)
    {
        --ref_it->second;
    }
    if (ref_it->second == 0)
    {
        m_addressRefCounts.erase(ref_it);
    }
}

void AsyncLoadManager::UnloadAddress(const eastl::string& address)
{
    std::lock_guard<std::mutex> lock(m_operationsMutex);
    m_addressRefCounts.erase(address);
}

void AsyncLoadManager::UnloadAll()
{
    std::lock_guard<std::mutex> lock(m_operationsMutex);
    m_addressRefCounts.clear();
}

size_t AsyncLoadManager::GetPendingCount() const
{
    std::lock_guard<std::mutex> lock(m_statsMutex);
    return m_stats.in_flight;
}

AsyncLoadManager::Statistics AsyncLoadManager::GetStatistics() const
{
    std::lock_guard<std::mutex> lock(m_statsMutex);
    return m_stats;
}

void AsyncLoadManager::SetWorkerThreadCount(size_t count)
{
    if (count == m_workerThreads.size())
    {
        return;
    }

    // Restart the pool: worker threads block in Dequeue with a timeout, so the
    // cleanest way to resize is to tear down and respawn.
    m_shutdown.store(true);
    m_pendingQueue.Stop();
    for (std::thread& worker : m_workerThreads)
    {
        if (worker.joinable())
        {
            worker.join();
        }
    }
    m_workerThreads.clear();

    m_shutdown.store(false);

    if (count > 0)
    {
        m_workerThreads.reserve(count);
        for (size_t i = 0; i < count; ++i)
        {
            m_workerThreads.emplace_back(&AsyncLoadManager::WorkerThreadMain, this);
        }
    }
}

void AsyncLoadManager::WaitForCompletion(OperationId id)
{
    // Called on the game thread this must pump, otherwise the hydration step
    // we are waiting for never runs and we deadlock against ourselves.
    const bool on_game_thread = [&]() {
        ThreadManager* threads = GET_SYSTEM(ThreadManager);
        return (threads != nullptr) ? threads->IsOnGameThread() : true;
    }();

    auto is_done = [&]() {
        std::lock_guard<std::mutex> lock(m_operationsMutex);
        auto&& it = m_operations.find(id);
        return (it == m_operations.end()) || it->second->IsDone();
    };

    // Bounded even when pumping: a load whose operation was already released
    // (or whose hydration keeps failing) must not wedge the caller forever.
    constexpr int kMaxSpinMilliseconds = 30000;
    for (int waited = 0; waited < kMaxSpinMilliseconds; ++waited)
    {
        if (is_done())
        {
            return;
        }
        if (on_game_thread)
        {
            PumpCompletions();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    LOG_ERROR(ZPreload, "AsyncLoadManager: WaitForCompletion timed out for operation {}", id);
}

} // namespace zengine::addressables
