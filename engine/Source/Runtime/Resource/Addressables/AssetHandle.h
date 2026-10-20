#pragma once

#include "Runtime/Resource/Addressables/AsyncOperationBase.h"

#include <utility>

namespace zengine::addressables
{

// -----------------------------------------------------------------------------
// AssetHandle<T> — Unity Addressables' AsyncOperationHandle<T>, adapted.
//
// A handle is a refcounted view onto one load operation. It is cheap to copy
// (one shared_ptr increment) and safe to store in gameplay objects; the
// underlying operation object is owned by AsyncLoadManager's live table and
// dropped on completion, so holding a handle never pins the operation itself.
//
// Threading contract:
//   * Issuing (LoadAssetAsync) is callable from any thread.
//   * GetStatus/IsDone/GetProgress are safe from any thread.
//   * Dereferencing (operator->, GetResult) is only valid once IsDone() is
//     true, and completing happens on the game thread, so a gameplay caller
//     that polls IsDone() on the game thread always sees a fully-published
//     object. Touching the result from another thread before IsDone() is a
//     data race and is not supported — same rule as Unity.
//
// The template parameter is the asset type. Unlike Unity, our assets are plain
// Object subclasses with no separate "AssetReference" wrapper, so T is the
// runtime type directly (MeshData, MaterialRes, PrefabRes, ...).
// -----------------------------------------------------------------------------
template<typename T>
class AssetHandle
{
public:
    AssetHandle() = default;

    explicit AssetHandle(std::shared_ptr<AsyncOperationBase> operation)
        : m_operation(std::move(operation)) {}

    // --- Status ---

    bool IsValid() const { return m_operation != nullptr; }
    bool IsDone() const { return m_operation != nullptr && m_operation->IsDone(); }
    bool HasSucceeded() const { return m_operation != nullptr && m_operation->HasSucceeded(); }
    bool HasFailed() const { return m_operation != nullptr && m_operation->HasFailed(); }

    OperationStatus GetStatus() const
    {
        return (m_operation != nullptr) ? m_operation->GetStatus() : OperationStatus::None;
    }

    LoadError GetError() const
    {
        return (m_operation != nullptr) ? m_operation->GetError() : LoadError::InvalidHandle;
    }

    float GetProgress() const { return (m_operation != nullptr) ? m_operation->GetProgress() : 0.0f; }

    OperationId GetId() const { return (m_operation != nullptr) ? m_operation->GetId() : 0; }

    // --- Result access ---
    //
    // Both accessors assert on invalid use rather than silently returning null:
    // a handle that game code forgot to wait on is a bug, and returning null
    // would turn it into a much harder-to-find crash at the first dereference.

    /// Blocks until the operation finishes, then returns the loaded object.
    /// Returns nullptr on failure. Calling this from the game thread while the
    /// completion step is also queued on the game thread deadlocks — see
    /// AsyncLoadManager::WaitForCompletion for the escape hatch used by tests.
    T* WaitForCompletion() const;

    /// Non-blocking. Only meaningful once IsDone() is true.
    T* GetResult() const;

    // --- Convenience dereference ---
    //
    // Provided so `handle->field` reads naturally, but callers MUST have
    // observed IsDone() first — hence the assert inside GetResult().
    T* operator->() const { return GetResult(); }
    T& operator*() const { return *GetResult(); }

    explicit operator bool() const { return IsValid() && HasSucceeded(); }

    // --- Lifetime ---

    /// Release interest in the result. Refcount semantics like Unity: the
    /// underlying asset is unloaded once every handle to it is released.
    /// Safe to call multiple times, and safe on a default-constructed handle.
    void Release();

    /// Register a callback fired when the operation completes (immediately if
    /// it already has). Runs on the game thread.
    void AddCompletionCallback(std::function<void(AssetHandle<T>)> callback)
    {
        if (m_operation == nullptr)
        {
            return;
        }
        AssetHandle<T> self = *this;
        m_operation->AddCompletionCallback([self, callback]() { callback(self); });
    }

    const std::shared_ptr<AsyncOperationBase>& GetOperation() const { return m_operation; }

private:
    std::shared_ptr<AsyncOperationBase> m_operation;
};

} // namespace zengine::addressables
