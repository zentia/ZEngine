#include "Runtime/Resource/Addressables/AsyncOperationBase.h"

#include <utility>

namespace zengine::addressables
{

bool AsyncOperationBase::IsDone() const
{
    const OperationStatus status = GetStatus();
    return status == OperationStatus::Succeeded || status == OperationStatus::Failed;
}

void AsyncOperationBase::AddCompletionCallback(std::function<void()> callback)
{
    if (callback == nullptr)
    {
        return;
    }

    bool fire_now = false;
    {
        std::lock_guard<std::mutex> lock(m_callbackMutex);
        if (m_callbacksFired)
        {
            // Already complete — run it inline so the caller never has to
            // poll "did I miss the completion?" before registering.
            fire_now = true;
        }
        else
        {
            m_callbacks.push_back(std::move(callback));
        }
    }

    if (fire_now)
    {
        callback();
    }
}

void AsyncOperationBase::Cancel()
{
    // Only meaningful while in flight; a finished operation keeps its result so
    // that handles already handed out to game code stay valid.
    OperationStatus expected = OperationStatus::Loading;
    if (!m_status.compare_exchange_strong(expected, OperationStatus::Failed))
    {
        return;
    }
    m_error.store(LoadError::Cancelled, std::memory_order_relaxed);
    FireCompletionCallbacks();
}

void AsyncOperationBase::Complete(OperationStatus status, LoadError error)
{
    m_error.store(error, std::memory_order_relaxed);
    m_progress.store(1.0f, std::memory_order_relaxed);
    m_status.store(status, std::memory_order_release);
    FireCompletionCallbacks();
}

void AsyncOperationBase::FireCompletionCallbacks()
{
    std::vector<std::function<void()>> to_run;
    {
        std::lock_guard<std::mutex> lock(m_callbackMutex);
        if (m_callbacksFired)
        {
            return;
        }
        m_callbacksFired = true;
        to_run.swap(m_callbacks);
    }

    // Invoked outside the lock: a callback may register further callbacks or
    // inspect the operation, and holding m_callbackMutex during that would
    // deadlock.
    for (const std::function<void()>& callback : to_run)
    {
        callback();
    }
}

} // namespace zengine::addressables
