#pragma once

#include "Runtime/Core/Base/EngineSystem.h"
#include "Runtime/Core/Base/Platform.h"
#include "Runtime/Core/Thread/TaskQueue.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// 线程类型枚举
enum class ThreadType : uint8_t
{
    game,    // 游戏线程（主线程）
    render,  // 渲染线程
    rhi      // RHI 线程（硬件接口线程）
};

// 线程管理器
class ThreadManager : public IEngineSystem
{
public:
    std::string GetName() const override { return "ThreadManager"; }
    SystemInitPhase GetInitPhase() const override { return SystemInitPhase::Core; }
    ThreadManager();
    ~ThreadManager();

    // 禁止拷贝和移动
    ThreadManager(const ThreadManager&) = delete;
    ThreadManager& operator=(const ThreadManager&) = delete;
    ThreadManager(ThreadManager&&) = delete;
    ThreadManager& operator=(ThreadManager&&) = delete;

    // 初始化线程系统
    bool Initialize() override;

    // 关闭线程系统
    void Shutdown() override;

    // 在指定线程上执行任务
    void EnqueueTask(ThreadType thread_type, std::function<void()> task, TaskPriority priority = TaskPriority::normal);

    // 在渲染线程上执行任务
    void EnqueueRenderTask(std::function<void()> task, TaskPriority priority = TaskPriority::normal);

    // 在 RHI 线程上执行任务
    void EnqueueRHITask(std::function<void()> task, TaskPriority priority = TaskPriority::normal);

    // 等待指定线程的所有任务完成
    void WaitForThread(ThreadType thread_type);

    // 等待所有线程的所有任务完成
    void WaitForAllThreads();

    // 检查当前是否在指定线程上
    bool IsOnThread(ThreadType thread_type) const;

    // 检查当前是否在渲染线程上
    bool IsOnRenderThread() const;

    // 检查当前是否在 RHI 线程上
    bool IsOnRHIThread() const;

    // 检查当前是否在游戏（主）线程上
    bool IsOnGameThread() const;

    // 获取线程的任务队列
    TaskQueue* GetTaskQueue(ThreadType thread_type);

private:
    // 线程工作函数
    void ThreadWorker(ThreadType thread_type, const std::string& thread_name);

    // 设置线程名称（平台特定）
    void SetThreadName(const std::string& name);

    std::unique_ptr<TaskQueue> m_RenderQueue;
    std::unique_ptr<TaskQueue> m_RhiQueue;

    std::thread m_RenderThread;
    std::thread m_RhiThread;

    std::atomic<bool> m_Initialized {false};
    std::atomic<bool> m_Shutdown {false};

    // 线程 ID 存储
    std::thread::id m_RenderThreadId;
    std::thread::id m_RhiThreadId;
    std::thread::id m_GameThreadId;
};