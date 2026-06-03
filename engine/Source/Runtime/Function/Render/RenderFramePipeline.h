#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>

// UE-style frame pipelining between the game thread and render/RHI workers.
// Limits how many submitted frames may be in flight before the game thread blocks.
class RenderFramePipeline
{
public:
    static void ConfigureMaxFramesInFlight(uint32_t max_frames);

    // Game thread: block until a new frame may be submitted.
    static void WaitForFrameSlot();

    // Game thread: call once after enqueueing render+RHI work for a frame.
    static void OnFrameSubmitted();

    // RHI thread: call after GPU work for a frame finishes.
    static void OnFrameCompleted();

    // Game thread: wait until every submitted frame has completed (shutdown / readback).
    static void WaitUntilIdle();

    static uint32_t GetMaxFramesInFlight();
    static uint32_t GetFramesInFlight();

private:
    static std::mutex s_Mutex;
    static std::condition_variable s_Condition;
    static std::atomic<uint32_t> s_FramesInFlight;
    static std::atomic<uint32_t> s_MaxFramesInFlight;
};
