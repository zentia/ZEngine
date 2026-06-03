#include "RenderFramePipeline.h"

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Function/Render/RenderingThread/RenderThreadChecks.h"
#include "Runtime/Function/Render/RenderingThread/RenderingThread.h"

#include <algorithm>

std::mutex RenderFramePipeline::s_Mutex;
std::condition_variable RenderFramePipeline::s_Condition;
std::atomic<uint32_t> RenderFramePipeline::s_FramesInFlight {0};
std::atomic<uint32_t> RenderFramePipeline::s_MaxFramesInFlight {2};

void RenderFramePipeline::ConfigureMaxFramesInFlight(uint32_t max_frames)
{
    const uint32_t clamped = std::max<uint32_t>(1u, max_frames);
    s_MaxFramesInFlight.store(clamped, std::memory_order_release);
}

void RenderFramePipeline::WaitForFrameSlot()
{
    CHECK_GAME_THREAD();
    if (!RenderingThread::IsParallelRenderingEnabled())
    {
        return;
    }

    std::unique_lock<std::mutex> lock(s_Mutex);
    s_Condition.wait(lock, [] {
        return s_FramesInFlight.load(std::memory_order_acquire) < s_MaxFramesInFlight.load(std::memory_order_acquire);
    });
}

void RenderFramePipeline::OnFrameSubmitted()
{
    CHECK_GAME_THREAD();
    if (!RenderingThread::IsParallelRenderingEnabled())
    {
        return;
    }

    s_FramesInFlight.fetch_add(1, std::memory_order_acq_rel);
}

void RenderFramePipeline::OnFrameCompleted()
{
    CHECK_RHI_THREAD();
    if (!RenderingThread::IsParallelRenderingEnabled())
    {
        return;
    }

    const uint32_t previous = s_FramesInFlight.fetch_sub(1, std::memory_order_acq_rel);
    if (previous == 0)
    {
        LOG_WARNING(ZRender, "RenderFramePipeline: OnFrameCompleted underflow");
    }

    s_Condition.notify_all();
}

void RenderFramePipeline::WaitUntilIdle()
{
    CHECK_GAME_THREAD();
    if (!RenderingThread::IsParallelRenderingEnabled())
    {
        return;
    }

    std::unique_lock<std::mutex> lock(s_Mutex);
    s_Condition.wait(lock, [] { return s_FramesInFlight.load(std::memory_order_acquire) == 0; });
}

uint32_t RenderFramePipeline::GetMaxFramesInFlight()
{
    return s_MaxFramesInFlight.load(std::memory_order_acquire);
}

uint32_t RenderFramePipeline::GetFramesInFlight()
{
    return s_FramesInFlight.load(std::memory_order_acquire);
}
