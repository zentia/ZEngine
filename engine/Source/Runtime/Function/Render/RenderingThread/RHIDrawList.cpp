#include "RHIDrawList.h"

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Function/Render/Interface/RHI.h"
#include "Runtime/Function/Render/RenderSystem.h"
#include "Runtime/Function/Render/RenderingThread/RenderThreadChecks.h"
#include "Runtime/Profiler/InsightsTrace.h"
#include "Runtime/Profiler/Profiler.h"

void RHIDrawList::Clear()
{
    CHECK_RENDER_THREAD();
    m_Entries.clear();
}

void RHIDrawList::Add(const char* debug_name, SubmitFn submit_fn)
{
    CHECK_RENDER_THREAD();
    if (!submit_fn)
    {
        return;
    }

    Entry entry;
    entry.debug_name = debug_name != nullptr ? debug_name : "RHIDrawList";
    entry.submit_fn = std::move(submit_fn);
    m_Entries.push_back(std::move(entry));
}

void RHIDrawList::ExecuteAll() const
{
    CHECK_RHI_THREAD();

    // ZEngine Insights: when capture is active, also wrap each pass in a GPU
    // timing scope (named by the same debug_name as the CPU scope), so the
    // Insights "GPU" track shows per-pass bars nested under "GPU Frame". The RHI
    // call is a no-op on backends without GPU timestamps (everything but DX12).
    RHI* rhi = nullptr;
    if (ZEngine::Insights::InsightsTrace::Get().IsCapturing())
    {
        if (std::shared_ptr<RenderSystem> render_system = GET_SYSTEM(RenderSystem))
        {
            // The RHI object is owned by RenderSystem for the whole frame, so the
            // raw pointer outlives this temporary shared_ptr.
            rhi = render_system->GetRHI().get();
        }
    }

    for (const Entry& entry : m_Entries)
    {
        Z_PROFILE_SCOPE(entry.debug_name.c_str());
        if (rhi != nullptr)
            rhi->BeginGpuTimingScope(entry.debug_name.c_str());
        entry.submit_fn();
        if (rhi != nullptr)
            rhi->EndGpuTimingScope();
    }
}
