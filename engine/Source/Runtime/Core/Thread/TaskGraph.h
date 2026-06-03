#pragma once

#include "Runtime/Core/Base/EngineSystem.h"
#include "Runtime/Core/Base/Platform.h"
#include "Runtime/Core/Thread/TaskQueue.h"
#include "ThreadManager.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// 任务节点句柄（用于引用任务节点）
using TaskHandle = uint32_t;
static constexpr TaskHandle InvalidTaskHandle = UINT32_MAX;

// 任务节点状态
enum class TaskState : uint8_t
{
    Pending,    // 等待执行
    Ready,      // 依赖已满足，可以执行
    Running,    // 正在执行
    Completed,  // 已完成
    Failed      // 执行失败
};

// 前向声明
class TaskGraph;

// 任务节点
// 表示一个可执行的任务，包含依赖关系信息
class TaskNode
{
public:
    TaskNode(TaskHandle handle,
             ThreadType thread_type,
             std::function<void()> task,
             TaskPriority priority = TaskPriority::normal,
             const std::string& name = "");
    ~TaskNode() = default;

    // 禁止拷贝，允许移动
    TaskNode(const TaskNode&) = delete;
    TaskNode& operator=(const TaskNode&) = delete;
    TaskNode(TaskNode&&) = default;
    TaskNode& operator=(TaskNode&&) = default;

    // 添加依赖：当前任务依赖于 other 任务
    void AddDependency(TaskHandle dependency);

    // 添加前置任务：other 任务依赖于当前任务
    void AddPrerequisite(TaskHandle prerequisite);

    // 检查依赖是否已满足
    bool AreDependenciesSatisfied() const;

    // 标记依赖完成
    void OnDependencyCompleted(TaskHandle dependency);

    // 执行任务
    void Execute();

    // 重置状态（用于任务图重置）
    void ResetState();

    // Getters
    TaskHandle getHandle() const { return m_Handle; }
    ThreadType getThreadType() const { return m_ThreadType; }
    TaskPriority getPriority() const { return m_Priority; }
    TaskState getState() const { return m_State.load(); }
    const std::string& GetName() const { return m_Name; }

    // 获取依赖数量
    size_t GetDependencyCount() const;
    size_t GetPrerequisiteCount() const;

    // 获取依赖集合（用于拓扑排序）
    const std::unordered_set<TaskHandle>& GetDependencies() const { return m_Dependencies; }
    const std::unordered_set<TaskHandle>& getPrerequisites() const { return m_Prerequisites; }

private:
    friend class TaskGraph;

    TaskHandle m_Handle;
    ThreadType m_ThreadType;
    std::function<void()> m_Task;
    TaskPriority m_Priority;
    std::string m_Name;

    mutable std::atomic<TaskState> m_State {TaskState::Pending};

    // 依赖关系：当前任务依赖的任务
    std::unordered_set<TaskHandle> m_Dependencies;

    // 前置任务：依赖当前任务的任务
    std::unordered_set<TaskHandle> m_Prerequisites;

    // 已完成的依赖计数
    mutable std::mutex m_DependencyMutex;
    std::atomic<uint32_t> m_CompletedDependencies {0};
};

// 任务图
// 管理任务节点和依赖关系，支持跨线程任务调度
class TaskGraph : public IEngineSystem
{
public:
    std::string GetName() const override { return GET_CLASS_NAME(TaskGraph); }
    SystemInitPhase GetInitPhase() const override { return SystemInitPhase::PreInit; }
    bool Initialize() override { return true; }
    void Shutdown() override {}
    TaskGraph();
    ~TaskGraph();

    // 添加任务节点
    // 返回任务句柄，可用于添加依赖关系
    TaskHandle AddTask(ThreadType thread_type,
                       std::function<void()> task,
                       TaskPriority priority = TaskPriority::normal,
                       const std::string& name = "");

    // 添加依赖关系：task 依赖于 prerequisite
    void AddDependency(TaskHandle task, TaskHandle prerequisite);

    // 添加依赖关系：task 依赖于多个 prerequisite
    void AddDependencies(TaskHandle task, const std::vector<TaskHandle>& prerequisites);

    // 编译任务图：检查依赖关系，准备执行
    // 返回是否成功（如果有循环依赖则返回 false）
    bool Compile();

    // 执行任务图（异步）
    // 任务会在对应的线程上按依赖顺序执行
    void Execute();

    // 等待任务图执行完成
    // 阻塞直到所有任务完成
    void wait();

    // 等待任务图执行完成（带超时）
    // 返回是否在超时前完成
    bool WaitFor(uint32_t timeout_ms);

    // 检查任务图是否已完成
    bool IsCompleted() const;

    // 重置任务图（用于重复使用）
    void reset();

    // 清空所有任务
    void clear();

    // 获取任务状态
    TaskState GetTaskState(TaskHandle handle) const;

    // 获取任务数量
    size_t getTaskCount() const { return m_Tasks.size(); }

    // 获取已完成的任务数量
    size_t GetCompletedTaskCount() const;

    // 获取任务名称（用于调试）
    std::string GetTaskName(TaskHandle handle) const;

private:
    // 检查是否有循环依赖（使用 DFS）
    bool HasCycle() const;

    // 拓扑排序：获取任务执行顺序
    std::vector<TaskHandle> TopologicalSort() const;

    // 任务完成回调
    void OnTaskCompleted(TaskHandle handle);

    // 检查并调度就绪的任务
    void ScheduleReadyTasks();

    // 任务节点列表
    std::vector<std::unique_ptr<TaskNode>> m_Tasks;

    // 任务句柄到索引的映射
    std::unordered_map<TaskHandle, size_t> m_HandleToIndex;

    // 下一个任务句柄
    TaskHandle m_NextHandle {0};

    // 任务图状态
    std::atomic<bool> m_Compiled {false};
    std::atomic<bool> m_Executing {false};
    std::atomic<uint32_t> m_CompletedCount {0};

    // 同步原语
    mutable std::mutex m_Mutex;
    std::condition_variable m_CompletionCondition;

    // 任务执行顺序（拓扑排序结果）
    std::vector<TaskHandle> m_ExecutionOrder;
};