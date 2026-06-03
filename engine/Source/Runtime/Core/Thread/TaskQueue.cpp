#include "Runtime/Core/Thread/TaskQueue.h"

void TaskQueue::Enqueue(Task task)
{
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (m_Stopped.load())
        {
            return;
        }
        m_Queue.push(std::move(task));
    }
    m_Condition.notify_one();
}

bool TaskQueue::TryDequeue(Task& task)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (m_Queue.empty() || m_Stopped.load())
    {
        return false;
    }

    task = std::move(m_Queue.front());
    m_Queue.pop();
    return true;
}

bool TaskQueue::Dequeue(Task& task, uint32_t timeout_ms)
{
    std::unique_lock<std::mutex> lock(m_Mutex);

    if (timeout_ms == UINT32_MAX)
    {
        // 无限等待
        m_Condition.wait(lock, [this] { return !m_Queue.empty() || m_Stopped.load(); });
    }
    else
    {
        // 带超时的等待
        auto timeout = std::chrono::milliseconds(timeout_ms);
        if (!m_Condition.wait_for(lock, timeout, [this] { return !m_Queue.empty() || m_Stopped.load(); }))
        {
            return false;  // 超时
        }
    }

    if (m_Stopped.load() && m_Queue.empty())
    {
        return false;
    }

    if (!m_Queue.empty())
    {
        task = std::move(m_Queue.front());
        m_Queue.pop();
        return true;
    }

    return false;
}

void TaskQueue::clear()
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    while (!m_Queue.empty())
    {
        m_Queue.pop();
    }
}

size_t TaskQueue::size() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_Queue.size();
}

bool TaskQueue::IsEmpty() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_Queue.empty();
}

void TaskQueue::Stop()
{
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Stopped.store(true);
    }
    m_Condition.notify_all();
}

bool TaskQueue::ComparePriority(const Task& a, const Task& b)
{
    return static_cast<int>(a.priority) < static_cast<int>(b.priority);
}