#pragma once

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Function/Render/RenderingThread/RenderingThread.h"

// Debug-only thread-affinity checks. No-op when parallel rendering is disabled
// (single-threaded / Web / Apple stub paths run render+RHI work on the game thread).
#define CHECK_GAME_THREAD() \
    ASSERT(!::RenderingThread::IsParallelRenderingEnabled() || ::RenderingThread::IsOnGameThread())

#define CHECK_RENDER_THREAD() \
    ASSERT(!::RenderingThread::IsParallelRenderingEnabled() || ::RenderingThread::IsOnRenderThread())

#define CHECK_RHI_THREAD() \
    ASSERT(!::RenderingThread::IsParallelRenderingEnabled() || ::RenderingThread::IsOnRHIThread())
