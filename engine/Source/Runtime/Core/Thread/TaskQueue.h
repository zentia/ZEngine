#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>

// 任务优先级
enum class TaskPriority : uint8_t
{
    low,
    normal,
    high
};

// 任务封装
struct Task
{
    std::function<void()> function;
    TaskPriority priority {TaskPriority::normal};

    Task() = default;
    Task(std::function<void()> func, TaskPriority prio = TaskPriority::normal)
        : function(std::move(func)), priority(prio)
    {
    }
};

// 线程安全的任务队列
class TaskQueue
{
public:
    TaskQueue() = default;
    ~TaskQueue() = default;

    // 禁止拷贝和移动
    TaskQueue(const TaskQueue&) = delete;
    TaskQueue& operator=(const TaskQueue&) = delete;
    TaskQueue(TaskQueue&&) = delete;
    TaskQueue& operator=(TaskQueue&&) = delete;

    // 添加任务到队列
    void Enqueue(Task task);

    // 尝试从队列中取出任务（非阻塞）
    bool TryDequeue(Task& task);

    // 从队列中取出任务（阻塞，直到有任务或超时）
    bool Dequeue(Task& task, uint32_t timeout_ms = UINT32_MAX);

    // 清空队列
    void clear();

    // 获取队列大小
    size_t size() const;

    // 检查队列是否为空
    bool IsEmpty() const;

    // 停止队列（让所有等待的线程退出）
    void Stop();

private:
    mutable std::mutex m_Mutex;
    std::condition_variable m_Condition;
    std::queue<Task> m_Queue;
    std::atomic<bool> m_Stopped {false};

    // 优先级比较函数
    static bool ComparePriority(const Task& a, const Task& b);
};