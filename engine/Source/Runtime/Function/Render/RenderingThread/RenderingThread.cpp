#include "RenderingThread.h"

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/Thread/ThreadManager.h"
#include "Runtime/Function/Render/RenderFramePipeline.h"
#include "Runtime/Function/Render/RenderingThread/RenderThreadChecks.h"
#include "Runtime/Profiler/Profiler.h"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <utility>

RenderCommandQueue RenderingThread::s_RenderCommandQueue;
RenderCommandQueue RenderingThread::s_RHICommandQueue;

void RenderingThread::EnqueueRenderCommand(RenderCommandPtr command)
{
    CHECK_GAME_THREAD();
    s_RenderCommandQueue.Enqueue(std::move(command));
}

void RenderingThread::EnqueueRHICommand(RenderCommandPtr command)
{
    CHECK_RENDER_THREAD();
    s_RHICommandQueue.Enqueue(std::move(command));
}

void RenderingThread::ClearPendingCommands()
{
    s_RenderCommandQueue.Clear();
    s_RHICommandQueue.Clear();
}

void RenderingThread::ExecuteRenderCommands(std::vector<RenderCommandPtr> commands)
{
    CHECK_RENDER_THREAD();
    for (auto& command : commands)
    {
        if (!command)
        {
            continue;
        }

        Z_PROFILE_SCOPE(command->GetDebugName());
        command->Execute();
    }
}

void RenderingThread::ExecuteRHICommands(std::vector<RenderCommandPtr> commands)
{
    CHECK_RHI_THREAD();
    for (auto& command : commands)
    {
        if (!command)
        {
            continue;
        }

        Z_PROFILE_SCOPE(command->GetDebugName());
        command->Execute();
    }
}

void RenderingThread::SubmitRenderFrame()
{
    CHECK_GAME_THREAD();
    auto commands =
        std::make_shared<std::vector<RenderCommandPtr>>(s_RenderCommandQueue.StealCommands());
    if (commands->empty())
    {
        return;
    }

    EnqueueRenderThreadTask(
        [commands]() {
            ExecuteRenderCommands(std::move(*commands));
        },
        TaskPriority::high);
}

void RenderingThread::SubmitRHICommandBatch()
{
    CHECK_RENDER_THREAD();
    auto commands = std::make_shared<std::vector<RenderCommandPtr>>(s_RHICommandQueue.StealCommands());
    if (commands->empty())
    {
        return;
    }

    EnqueueRHITask(
        [commands]() {
            ExecuteRHICommands(std::move(*commands));
            RenderFramePipeline::OnFrameCompleted();
        },
        TaskPriority::high);
}

void RenderingThread::EnqueueRenderThreadTask(std::function<void()> task, TaskPriority priority)
{
    GET_SYSTEM(ThreadManager)->EnqueueRenderTask(std::move(task), priority);
}

void RenderingThread::EnqueueRHITask(std::function<void()> task, TaskPriority priority)
{
    GET_SYSTEM(ThreadManager)->EnqueueRHITask(std::move(task), priority);
}

void RenderingThread::WaitForRenderThread()
{
    GET_SYSTEM(ThreadManager)->WaitForThread(ThreadType::render);
}

void RenderingThread::WaitForRHIThread()
{
    GET_SYSTEM(ThreadManager)->WaitForThread(ThreadType::rhi);
}

void RenderingThread::WaitForAll()
{
    WaitForRenderThread();
    WaitForRHIThread();
}

void RenderingThread::FlushRenderingCommands()
{
    CHECK_GAME_THREAD();
    WaitForAll();
    RenderFramePipeline::WaitUntilIdle();
    ClearPendingCommands();
}

bool RenderingThread::IsOnGameThread()
{
    return GET_SYSTEM(ThreadManager)->IsOnGameThread();
}

bool RenderingThread::IsParallelRenderingEnabled()
{
#if defined(__EMSCRIPTEN__) || defined(__APPLE__)
    return false;
#else
    return true;
#endif
}

bool RenderingThread::IsOnRenderThread()
{
    return GET_SYSTEM(ThreadManager)->IsOnRenderThread();
}

bool RenderingThread::IsOnRHIThread()
{
    return GET_SYSTEM(ThreadManager)->IsOnRHIThread();
}

void RenderingThread::ExecuteOnRenderThread(std::function<void()> task)
{
    if (!task)
    {
        return;
    }

    // 如果已经在渲染线程上，直接执行
    if (IsOnRenderThread())
    {
        task();
        return;
    }

    // 否则，提交任务并等待完成
    std::mutex mutex;
    std::condition_variable condition;
    bool completed = false;

    EnqueueRenderThreadTask(
        [&task, &mutex, &condition, &completed]() {
            task();
            {
                std::lock_guard<std::mutex> lock(mutex);
                completed = true;
            }
            condition.notify_one();
        },
        TaskPriority::high);

    // 等待任务完成
    std::unique_lock<std::mutex> lock(mutex);
    condition.wait(lock, [&completed] { return completed; });
}

void RenderingThread::ExecuteOnRHIThread(std::function<void()> task)
{
    if (!task)
    {
        return;
    }

    // 如果已经在 RHI 线程上，直接执行
    if (IsOnRHIThread())
    {
        task();
        return;
    }

    // 否则，提交任务并等待完成
    std::mutex mutex;
    std::condition_variable condition;
    bool completed = false;

    EnqueueRHITask(
        [&task, &mutex, &condition, &completed]() {
            task();
            {
                std::lock_guard<std::mutex> lock(mutex);
                completed = true;
            }
            condition.notify_one();
        },
        TaskPriority::high);

    // 等待任务完成
    std::unique_lock<std::mutex> lock(mutex);
    condition.wait(lock, [&completed] { return completed; });
}

std::unique_ptr<TaskGraph> RenderingThread::CreateTaskGraph()
{
    return std::make_unique<TaskGraph>();
}

void RenderingThread::ExecuteTaskGraph(TaskGraph* graph)
{
    if (graph)
    {
        graph->Execute();
    }
}

void RenderingThread::ExecuteTaskGraphAndWait(TaskGraph* graph)
{
    if (graph)
    {
        graph->Execute();
        graph->wait();
    }
}