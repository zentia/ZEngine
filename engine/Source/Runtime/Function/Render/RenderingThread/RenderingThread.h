#pragma once

#include "Runtime/Core/Thread/TaskGraph.h"
#include "Runtime/Core/Thread/ThreadManager.h"
#include "Runtime/Function/Render/RenderingThread/RenderCommand.h"

#include <functional>
#include <memory>
#include <vector>

class RHI;
class RenderResourceBase;
class RenderPipelineBase;
class RenderScene;
class RenderCamera;

#define ENQUEUE_RENDER_COMMAND(CmdType, ...) \
    ::RenderingThread::EnqueueRenderCommand(std::make_unique<CmdType>(__VA_ARGS__))

#define ENQUEUE_RHI_COMMAND(CmdType, ...) \
    ::RenderingThread::EnqueueRHICommand(std::make_unique<CmdType>(__VA_ARGS__))

// Debug thread-affinity checks: see RenderingThread/RenderThreadChecks.h
// (CHECK_GAME_THREAD / CHECK_RENDER_THREAD / CHECK_RHI_THREAD).

// 渲染线程封装类
// 提供类似 UE 的渲染线程和 RHI 线程接口
class RenderingThread
{
public:
    RenderingThread() = default;
    ~RenderingThread() = default;

    // UE-style typed command queues (see RenderCommand.h).
    static void EnqueueRenderCommand(RenderCommandPtr command);
    static void EnqueueRHICommand(RenderCommandPtr command);

    // Steal pending render commands and dispatch them on the render worker.
    static void SubmitRenderFrame();

    // Steal pending RHI commands and dispatch them on the RHI worker.
    static void SubmitRHICommandBatch();

    static void ClearPendingCommands();

    // 在渲染线程上执行任务
    static void EnqueueRenderThreadTask(std::function<void()> task, TaskPriority priority = TaskPriority::normal);

    // 在 RHI 线程上执行任务
    static void EnqueueRHITask(std::function<void()> task, TaskPriority priority = TaskPriority::normal);

    // 等待渲染线程完成所有任务
    static void WaitForRenderThread();

    // 等待 RHI 线程完成所有任务
    static void WaitForRHIThread();

    // 等待所有渲染相关线程完成
    static void WaitForAll();

    // UE-style Flush: drain worker queues, then wait for all pipelined frames to finish.
    static void FlushRenderingCommands();

    // 检查当前是否在游戏（主）线程
    static bool IsOnGameThread();

    // Desktop builds dispatch frames across game/render/RHI workers; Web runs inline.
    static bool IsParallelRenderingEnabled();

    // 检查当前是否在渲染线程
    static bool IsOnRenderThread();

    // 检查当前是否在 RHI 线程
    static bool IsOnRHIThread();

    // 在渲染线程上执行并等待完成（同步）
    static void ExecuteOnRenderThread(std::function<void()> task);

    // 在 RHI 线程上执行并等待完成（同步）
    static void ExecuteOnRHIThread(std::function<void()> task);

    // ========== 任务图接口 ==========

    // 创建任务图（用于管理任务依赖关系）
    static std::unique_ptr<TaskGraph> CreateTaskGraph();

    // 执行任务图（异步）
    static void ExecuteTaskGraph(TaskGraph* graph);

    // 执行任务图并等待完成（同步）
    static void ExecuteTaskGraphAndWait(TaskGraph* graph);

private:
    static RenderCommandQueue s_RenderCommandQueue;
    static RenderCommandQueue s_RHICommandQueue;

    static void ExecuteRenderCommands(std::vector<RenderCommandPtr> commands);
    static void ExecuteRHICommands(std::vector<RenderCommandPtr> commands);
};