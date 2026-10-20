#pragma once

#include "Runtime/Core/Base/Macro.h"

#include <EASTL/string.h>
#include <EASTL/vector.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

// Forward declaration — the payload type lives in Object.h, which pulls in far
// more than a handle needs.
class Object;

namespace zengine::addressables
{

// Status of an async load operation. Mirrors Unity's
// AsyncOperationStatus so ported game code reads the same.
enum class OperationStatus : uint8_t
{
    None = 0,      // handle default-constructed, never issued
    Loading,       // in flight (IO on the worker pool, or awaiting main-thread completion)
    Succeeded,     // Result is valid
    Failed         // load failed; Result is null
};

// Why a load failed. Kept deliberately coarse — callers almost always only
// branch on "retry or give up", and Addressables in Unity exposes the same
// information throughOperationException rather than a rich error taxonomy.
enum class LoadError : uint8_t
{
    None = 0,
    AddressNotMapped,  // no entry in the catalog for this address
    FileMissing,       // catalog mapped it, but the file is gone
    DeserializeFailed, // file present, object graph could not be hydrated
    Cancelled,         // Release()/Cancel() before completion
    InvalidHandle      // operation on a handle that was never issued
};

// Strong id for an in-flight operation. Zero is never a valid handle id, which
// lets AssetHandle store 0 to mean "empty" without a separate flag.
using OperationId = uint64_t;

// -----------------------------------------------------------------------------
// AsyncOperationBase — the untyped half of a load operation.
//
// AsyncLoadManager owns these and hands out shared_ptr so a handle can outlive
// the manager's internal bookkeeping entry (the manager drops its reference as
// soon as the operation completes, but late callers may still hold one).
// -----------------------------------------------------------------------------
class AsyncOperationBase
{
public:
    AsyncOperationBase() = default;
    virtual ~AsyncOperationBase() = default;

    AsyncOperationBase(const AsyncOperationBase&) = delete;
    AsyncOperationBase& operator=(const AsyncOperationBase&) = delete;

    OperationId GetId() const { return m_id; }
    OperationStatus GetStatus() const { return m_status.load(std::memory_order_acquire); }
    LoadError GetError() const { return m_error.load(std::memory_order_relaxed); }
    float GetProgress() const { return m_progress.load(std::memory_order_relaxed); }

    bool IsDone() const;
    bool HasSucceeded() const { return GetStatus() == OperationStatus::Succeeded; }
    bool HasFailed() const { return GetStatus() == OperationStatus::Failed; }

    // Address the operation was issued for — used by the completion log and by
    // AssetHandle::GetErrorString for a readable diagnostic.
    const eastl::string& GetAddress() const { return m_address; }

    // Invoked at most once, on the game thread, after m_status leaves Loading.
    // Callbacks registered after completion fire immediately on registration —
    // otherwise a "did I miss it?" race would force every caller to poll.
    void AddCompletionCallback(std::function<void()> callback);

    // Request cancellation. Only honoured while still Loading; a completed
    // operation keeps its result so that handles already handed to game code
    // stay valid.
    void Cancel();

protected:
    // Completion is driven exclusively by AsyncLoadManager, hence protected.
    void SetProgress(float progress) { m_progress.store(progress, std::memory_order_relaxed); }
    void Complete(OperationStatus status, LoadError error);

    OperationId m_id {0};
    eastl::string m_address;

private:
    void FireCompletionCallbacks();

    std::atomic<OperationStatus> m_status {OperationStatus::None};
    std::atomic<LoadError> m_error {LoadError::None};
    std::atomic<float> m_progress {0.0f};

    mutable std::mutex m_callbackMutex;
    std::vector<std::function<void()>> m_callbacks;
    bool m_callbacksFired {false};

    friend class AsyncLoadManager;
};

} // namespace zengine::addressables
