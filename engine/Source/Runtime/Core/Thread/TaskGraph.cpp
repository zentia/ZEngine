#include "TaskGraph.h"

#include "Runtime/Core/Base/Macro.h"

#include <algorithm>
#include <queue>
#include <stack>

// ============================================================================
// TaskNode Implementation
// ============================================================================

TaskNode::TaskNode(TaskHandle handle,
                   ThreadType thread_type,
                   std::function<void()> task,
                   TaskPriority priority,
                   const std::string& name)
    : m_Handle(handle), m_ThreadType(thread_type), m_Task(std::move(task)), m_Priority(priority),
      m_Name(name.empty() ? "Task_" + std::to_string(handle) : name)
{
}

void TaskNode::AddDependency(TaskHandle dependency)
{
    std::lock_guard<std::mutex> lock(m_DependencyMutex);
    m_Dependencies.insert(dependency);
}

void TaskNode::AddPrerequisite(TaskHandle prerequisite)
{
    std::lock_guard<std::mutex> lock(m_DependencyMutex);
    m_Prerequisites.insert(prerequisite);
}

bool TaskNode::AreDependenciesSatisfied() const
{
    return m_CompletedDependencies.load() >= m_Dependencies.size();
}

void TaskNode::OnDependencyCompleted(TaskHandle dependency)
{
    std::lock_guard<std::mutex> lock(m_DependencyMutex);
    if (m_Dependencies.find(dependency) != m_Dependencies.end())
    {
        m_CompletedDependencies.fetch_add(1);
    }
}

void TaskNode::Execute()
{
    TaskState expected = TaskState::Ready;
    if (!m_State.compare_exchange_strong(expected, TaskState::Running))
    {
        // 状态不是 Ready，可能是并发问题
        return;
    }

    try
    {
        if (m_Task)
        {
            m_Task();
        }
        m_State.store(TaskState::Completed);
    }
    catch (...)
    {
        m_State.store(TaskState::Failed);
        LOG_ERROR(ZThread, "Task {} failed to execute", m_Name);
    }
}

void TaskNode::ResetState()
{
    std::lock_guard<std::mutex> lock(m_DependencyMutex);
    m_State.store(TaskState::Pending);
    m_CompletedDependencies.store(0);
}

size_t TaskNode::GetDependencyCount() const
{
    std::lock_guard<std::mutex> lock(m_DependencyMutex);
    return m_Dependencies.size();
}

size_t TaskNode::GetPrerequisiteCount() const
{
    std::lock_guard<std::mutex> lock(m_DependencyMutex);
    return m_Prerequisites.size();
}

// ============================================================================
// TaskGraph Implementation
// ============================================================================

TaskGraph::TaskGraph() {}

TaskGraph::~TaskGraph()
{
    wait();
    clear();
}

TaskHandle
TaskGraph::AddTask(ThreadType thread_type, std::function<void()> task, TaskPriority priority, const std::string& name)
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    if (m_Compiled.load())
    {
        LOG_WARNING(ZThread, "Cannot add task to compiled TaskGraph. Call reset() first.");
        return InvalidTaskHandle;
    }

    TaskHandle handle = m_NextHandle++;
    size_t index = m_Tasks.size();

    auto node = std::make_unique<TaskNode>(handle, thread_type, std::move(task), priority, name);
    m_Tasks.push_back(std::move(node));
    m_HandleToIndex[handle] = index;

    return handle;
}

void TaskGraph::AddDependency(TaskHandle task, TaskHandle prerequisite)
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    if (m_Compiled.load())
    {
        LOG_WARNING(ZThread, "Cannot add dependency to compiled TaskGraph. Call reset() first.");
        return;
    }

    auto task_it = m_HandleToIndex.find(task);
    auto prereq_it = m_HandleToIndex.find(prerequisite);

    if (task_it == m_HandleToIndex.end() || prereq_it == m_HandleToIndex.end())
    {
        LOG_ERROR(ZThread, "Invalid task handle in addDependency");
        return;
    }

    size_t task_index = task_it->second;
    size_t prereq_index = prereq_it->second;

    m_Tasks[task_index]->AddDependency(prerequisite);
    m_Tasks[prereq_index]->AddPrerequisite(task);
}

void TaskGraph::AddDependencies(TaskHandle task, const std::vector<TaskHandle>& prerequisites)
{
    for (TaskHandle prereq : prerequisites)
    {
        AddDependency(task, prereq);
    }
}

bool TaskGraph::Compile()
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    if (m_Compiled.load())
    {
        return true;  // 已经编译过
    }

    if (m_Tasks.empty())
    {
        m_Compiled.store(true);
        return true;
    }

    // 检查循环依赖
    if (HasCycle())
    {
        LOG_ERROR(ZThread, "TaskGraph has cycle dependencies");
        return false;
    }

    // 执行拓扑排序
    m_ExecutionOrder = TopologicalSort();

    if (m_ExecutionOrder.size() != m_Tasks.size())
    {
        LOG_ERROR(ZThread, "TaskGraph topological sort failed: not all tasks included");
        return false;
    }

    m_Compiled.store(true);
    return true;
}

void TaskGraph::Execute()
{
    if (!m_Compiled.load())
    {
        if (!Compile())
        {
            LOG_ERROR(ZThread, "TaskGraph compilation failed, cannot execute");
            return;
        }
    }

    if (m_Executing.load())
    {
        LOG_WARNING(ZThread, "TaskGraph is already executing");
        return;
    }

    if (m_Tasks.empty())
    {
        return;
    }

    m_Executing.store(true);
    m_CompletedCount.store(0);

    // 重置所有任务状态
    for (auto& task : m_Tasks)
    {
        task->ResetState();
    }

    // 调度就绪的任务（没有依赖的任务）
    ScheduleReadyTasks();
}

void TaskGraph::wait()
{
    if (!m_Executing.load())
    {
        return;
    }

    std::unique_lock<std::mutex> lock(m_Mutex);
    m_CompletionCondition.wait(lock,
                               [this] { return m_CompletedCount.load() >= m_Tasks.size() || !m_Executing.load(); });
}

bool TaskGraph::WaitFor(uint32_t timeout_ms)
{
    if (!m_Executing.load())
    {
        return true;
    }

    std::unique_lock<std::mutex> lock(m_Mutex);
    return m_CompletionCondition.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] {
        return m_CompletedCount.load() >= m_Tasks.size() || !m_Executing.load();
    });
}

bool TaskGraph::IsCompleted() const
{
    return m_Executing.load() && m_CompletedCount.load() >= m_Tasks.size();
}

void TaskGraph::reset()
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    wait();

    m_Compiled.store(false);
    m_Executing.store(false);
    m_CompletedCount.store(0);
    m_ExecutionOrder.clear();

    // 重置所有任务状态
    for (auto& task : m_Tasks)
    {
        task->ResetState();
    }
}

void TaskGraph::clear()
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    wait();

    m_Tasks.clear();
    m_HandleToIndex.clear();
    m_NextHandle = 0;
    m_Compiled.store(false);
    m_Executing.store(false);
    m_CompletedCount.store(0);
    m_ExecutionOrder.clear();
}

TaskState TaskGraph::GetTaskState(TaskHandle handle) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    auto it = m_HandleToIndex.find(handle);
    if (it == m_HandleToIndex.end())
    {
        return TaskState::Failed;
    }

    return m_Tasks[it->second]->getState();
}

size_t TaskGraph::GetCompletedTaskCount() const
{
    return m_CompletedCount.load();
}

std::string TaskGraph::GetTaskName(TaskHandle handle) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    auto it = m_HandleToIndex.find(handle);
    if (it == m_HandleToIndex.end())
    {
        return "InvalidTask";
    }

    return m_Tasks[it->second]->GetName();
}

bool TaskGraph::HasCycle() const
{
    if (m_Tasks.empty())
    {
        return false;
    }

    // 使用 DFS 检测循环
    std::vector<bool> visited(m_Tasks.size(), false);
    std::vector<bool> recursion_stack(m_Tasks.size(), false);

    std::function<bool(size_t)> dfs = [&](size_t index) -> bool {
        if (recursion_stack[index])
        {
            return true;  // 发现循环
        }

        if (visited[index])
        {
            return false;
        }

        visited[index] = true;
        recursion_stack[index] = true;

        // 检查所有依赖
        const auto& node = m_Tasks[index];
        const auto& deps = node->GetDependencies();
        for (TaskHandle dep : deps)
        {
            auto it = m_HandleToIndex.find(dep);
            if (it != m_HandleToIndex.end())
            {
                if (dfs(it->second))
                {
                    return true;
                }
            }
        }

        recursion_stack[index] = false;
        return false;
    };

    for (size_t i = 0; i < m_Tasks.size(); ++i)
    {
        if (!visited[i] && dfs(i))
        {
            return true;
        }
    }

    return false;
}

std::vector<TaskHandle> TaskGraph::TopologicalSort() const
{
    std::vector<TaskHandle> result;

    if (m_Tasks.empty())
    {
        return result;
    }

    // 计算每个节点的入度（依赖数量）
    std::vector<size_t> in_degree(m_Tasks.size(), 0);
    for (size_t i = 0; i < m_Tasks.size(); ++i)
    {
        in_degree[i] = m_Tasks[i]->GetDependencyCount();
    }

    // 使用 Kahn 算法进行拓扑排序
    std::queue<size_t> queue;
    for (size_t i = 0; i < m_Tasks.size(); ++i)
    {
        if (in_degree[i] == 0)
        {
            queue.push(i);
        }
    }

    while (!queue.empty())
    {
        size_t current = queue.front();
        queue.pop();

        TaskHandle handle = m_Tasks[current]->getHandle();
        result.push_back(handle);

        // 更新所有前置任务的入度
        for (size_t i = 0; i < m_Tasks.size(); ++i)
        {
            // 检查任务 i 是否依赖于 current
            if (i != current)
            {
                const auto& deps = m_Tasks[i]->GetDependencies();
                TaskHandle current_handle = m_Tasks[current]->getHandle();
                if (deps.find(current_handle) != deps.end())
                {
                    --in_degree[i];
                    if (in_degree[i] == 0)
                    {
                        queue.push(i);
                    }
                }
            }
        }
    }

    return result;
}

void TaskGraph::OnTaskCompleted(TaskHandle handle)
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    // 更新已完成计数
    uint32_t completed = m_CompletedCount.fetch_add(1) + 1;

    // 通知所有依赖此任务的任务
    auto it = m_HandleToIndex.find(handle);
    if (it != m_HandleToIndex.end())
    {
        size_t index = it->second;
        const auto& node = m_Tasks[index];

        // 通知所有前置任务（依赖此任务的任务）
        const auto& prerequisites = node->getPrerequisites();
        for (TaskHandle prereq : prerequisites)
        {
            auto prereq_it = m_HandleToIndex.find(prereq);
            if (prereq_it != m_HandleToIndex.end())
            {
                m_Tasks[prereq_it->second]->OnDependencyCompleted(handle);
            }
        }
    }

    // 检查是否所有任务都完成了
    if (completed >= m_Tasks.size())
    {
        m_Executing.store(false);
        m_CompletionCondition.notify_all();
    }
    else
    {
        // 调度新的就绪任务
        ScheduleReadyTasks();
    }
}

void TaskGraph::ScheduleReadyTasks()
{
    if (!GET_SYSTEM(ThreadManager))
    {
        // 如果线程管理器未初始化，直接执行任务（单线程模式）
        for (size_t i = 0; i < m_Tasks.size(); ++i)
        {
            auto& task = m_Tasks[i];
            if (task->getState() == TaskState::Pending && task->AreDependenciesSatisfied())
            {
                task->m_State.store(TaskState::Ready);
                task->Execute();
                OnTaskCompleted(task->getHandle());
            }
        }
        return;
    }

    // 遍历所有任务，找到就绪的任务
    for (size_t i = 0; i < m_Tasks.size(); ++i)
    {
        auto& task = m_Tasks[i];

        // 检查任务是否就绪
        if (task->getState() == TaskState::Pending && task->AreDependenciesSatisfied())
        {
            // 标记为就绪
            task->m_State.store(TaskState::Ready);

            // 获取任务信息
            ThreadType thread_type = task->getThreadType();
            TaskPriority priority = task->getPriority();
            TaskHandle handle = task->getHandle();

            // 创建包装函数，在任务完成后通知
            auto wrapped_task = [this, handle, task_ptr = task.get()]() {
                task_ptr->Execute();
                OnTaskCompleted(handle);
            };

            // 提交到对应的线程
            GET_SYSTEM(ThreadManager)->EnqueueTask(thread_type, wrapped_task, priority);
        }
    }
}