#include "Runtime/Function/Framework/Coroutine/Coroutine.h"

#include "Runtime/Core/Base/Macro.h"

#include <algorithm>

// =============================================================================
// CoroutineScheduler
//
// The one subtle part is ResumePhase(): resuming a body runs arbitrary gameplay
// code, and that code may call Start() or Stop() -- including on the coroutine
// that is currently running. So:
//
//   * new entries go to m_PendingAdds and are spliced in after the loop, and
//   * finished entries are collected by id and erased afterwards.
//
// Nothing is read through an index or iterator across a resume(), because a
// resume() can invalidate both.
// =============================================================================

YieldInstruction Coroutine::Resume()
{
    if (m_Handle == nullptr || m_Handle.done())
    {
        return YieldInstruction {WaitForNextFrame{}};
    }
    m_Handle.resume();
    if (m_Handle.done())
    {
        return YieldInstruction {WaitForNextFrame{}};
    }
    return m_Handle.promise().pending;
}

bool CoroutineScheduler::Initialize()
{
    StopAll();
    m_NextId = 1;
    m_ResumeCount = 0;
    return true;
}

void CoroutineScheduler::Shutdown()
{
    StopAll();
}

uint64_t CoroutineScheduler::Start(Coroutine&& routine, uint64_t owner)
{
    if (!routine.IsValid())
    {
        return kInvalidId;
    }

    Entry entry;
    entry.owner = owner;
    entry.routine = std::move(routine);

    // First segment runs now, synchronously -- Unity's StartCoroutine does the
    // same, and code that yields before doing anything (e.g. `co_yield
    // WaitForEndOfFrame{}` as the first statement) relies on it.
    const YieldInstruction first = entry.routine.Resume();
    ++m_ResumeCount;

    if (entry.routine.IsDone())
    {
        // Ran straight through with no yield. Nothing to schedule; handing back
        // an id for a coroutine that is already over would let a caller wait on
        // it forever.
        return kInvalidId;
    }

    ApplyYield(entry, first);
    entry.id = m_NextId++;

    m_PendingAdds.push_back(std::move(entry));
    return m_PendingAdds.back().id;
}

void CoroutineScheduler::Stop(uint64_t id)
{
    if (id == kInvalidId)
    {
        return;
    }

    for (auto it = m_Entries.begin(); it != m_Entries.end(); ++it)
    {
        if (it->id == id)
        {
            m_Entries.erase(it);
            return;
        }
    }

    // Stop() during a resume is legal, and the entry may still be in
    // m_PendingAdds (started this tick, not yet spliced in). Missing that would
    // make `Start(id); Stop(id);` in the same call a silent no-op.
    for (auto it = m_PendingAdds.begin(); it != m_PendingAdds.end(); ++it)
    {
        if (it->id == id)
        {
            m_PendingAdds.erase(it);
            return;
        }
    }
}

void CoroutineScheduler::StopAll()
{
    m_Entries.clear();
    m_PendingAdds.clear();
}

void CoroutineScheduler::StopAllForOwner(uint64_t owner)
{
    // erase-remove rather than a loop of erase(): the latter is O(n^2) for a
    // component that spawned a burst (a particle-like effect spawner can have
    // hundreds) and, more importantly, it is easy to write the loop so that it
    // skips an element after each erase.
    m_Entries.erase(std::remove_if(m_Entries.begin(),
                                   m_Entries.end(),
                                   [owner](const Entry& e) { return e.owner == owner; }),
                    m_Entries.end());
    m_PendingAdds.erase(std::remove_if(m_PendingAdds.begin(),
                                       m_PendingAdds.end(),
                                       [owner](const Entry& e) { return e.owner == owner; }),
                        m_PendingAdds.end());
}

bool CoroutineScheduler::IsRunning(uint64_t id) const
{
    if (id == kInvalidId)
    {
        return false;
    }
    for (const Entry& entry : m_Entries)
    {
        if (entry.id == id)
        {
            return true;
        }
    }
    return IsPending(id);
}

bool CoroutineScheduler::IsPending(uint64_t id) const
{
    for (const Entry& entry : m_PendingAdds)
    {
        if (entry.id == id)
        {
            return true;
        }
    }
    return false;
}

void CoroutineScheduler::ApplyYield(Entry& entry, const YieldInstruction& instruction)
{
    entry.blocked = false;
    entry.has_timer = false;
    entry.elapsed_scaled = 0.0f;
    entry.elapsed_unscaled = 0.0f;
    entry.duration_scaled = 0.0f;
    entry.duration_unscaled = 0.0f;
    entry.phase = CoroutinePhase::kUpdate;

    if (const auto* seconds = std::get_if<WaitForSeconds>(&instruction))
    {
        // A non-positive duration still costs one frame -- Start() never
        // resumes past the first yield, so the earliest possible resume is the
        // next TickUpdate. Resuming in the same tick would spin: resume ->
        // yield 0 -> resume, forever inside one frame, with the whole engine
        // frozen and no log line to explain it.
        entry.has_timer = true;
        entry.duration_scaled = (seconds->seconds > 0.0f) ? seconds->seconds : 0.0f;
        entry.blocked = true;
    }
    else if (const auto* realtime = std::get_if<WaitForSecondsRealtime>(&instruction))
    {
        entry.has_timer = true;
        entry.duration_unscaled = (realtime->seconds > 0.0f) ? realtime->seconds : 0.0f;
        entry.blocked = true;
    }
    else if (std::holds_alternative<WaitForFixedUpdate>(instruction))
    {
        entry.phase = CoroutinePhase::kFixedUpdate;
    }
    else if (std::holds_alternative<WaitForLateUpdate>(instruction))
    {
        entry.phase = CoroutinePhase::kLateUpdate;
    }
    else if (std::holds_alternative<WaitForEndOfFrame>(instruction))
    {
        entry.phase = CoroutinePhase::kEndOfFrame;
    }
    // WaitForNextFrame: phase kUpdate, not blocked -> resumes next Update.
    // WaitUntil / WaitWhile / WaitForCoroutine: phase kUpdate, readiness is
    // re-evaluated at the top of TickUpdate.
}

void CoroutineScheduler::SplicePending()
{
    for (Entry& entry : m_PendingAdds)
    {
        m_Entries.push_back(std::move(entry));
    }
    m_PendingAdds.clear();
}

void CoroutineScheduler::ResumePhase(CoroutinePhase phase)
{
    // NOTE: splicing deliberately happens in the public Tick* methods, before
    // anything else, and NOT here.
    //
    // Splicing here instead costs every coroutine exactly one frame: a coroutine
    // started by Start() would sit in m_PendingAdds through its first tick, so
    // that tick would neither drain its timer nor evaluate its predicate, and
    // the resume would land one frame late. Invisible in a hand-written test
    // that just loops "until it happens", fatal for `WaitForSeconds(0.016)` at
    // 60Hz, which would take two frames instead of one.
    //
    // Splicing at the top of the phase keeps the other guarantee intact: a
    // coroutine started *during* a resume still lands in m_PendingAdds and is
    // only eligible from the next phase on, so a body that spawns a coroutine
    // every resume cannot spin inside one tick.
    eastl::vector<uint64_t> finished;

    // Indexed loop: m_Entries can grow (a body starts a new coroutine) while we
    // walk it. New entries are appended and deliberately NOT visited this phase.
    const std::size_t count = m_Entries.size();
    for (std::size_t i = 0; i < count && i < m_Entries.size(); ++i)
    {
        Entry& entry = m_Entries[i];
        if (entry.phase != phase || entry.blocked)
        {
            continue;
        }

        const YieldInstruction next = entry.routine.Resume();
        ++m_ResumeCount;

        if (entry.routine.IsDone())
        {
            finished.push_back(entry.id);
            continue;
        }
        ApplyYield(entry, next);
    }

    for (uint64_t id : finished)
    {
        Stop(id);
    }
}

void CoroutineScheduler::TickUpdate(float scaled_delta_time, float unscaled_delta_time)
{
    SplicePending();

    for (Entry& entry : m_Entries)
    {
        if (entry.phase != CoroutinePhase::kUpdate)
        {
            continue;
        }

        if (entry.has_timer)
        {
            // Advanced before the resume below, so a wait that expires this
            // frame resumes *this* frame. Deferring by one would make
            // WaitForSeconds(0.016) take two frames at 60Hz -- every short wait
            // off by one, which is invisible in a test and maddening in a game.
            entry.elapsed_scaled += scaled_delta_time;
            entry.elapsed_unscaled += unscaled_delta_time;
            // Both channels must be satisfied. A coroutine sets only one of the
            // two durations; the other stays 0 and is satisfied immediately,
            // which is what stops WaitForSecondsRealtime from being gated on
            // scaled time (and vice versa).
            const bool scaled_ready = entry.elapsed_scaled + Entry::kTimeEpsilon >= entry.duration_scaled;
            const bool unscaled_ready = entry.elapsed_unscaled + Entry::kTimeEpsilon >= entry.duration_unscaled;
            entry.blocked = !(scaled_ready && unscaled_ready);
            continue;
        }

        const YieldInstruction& instruction = entry.routine.GetPending();
        if (const auto* until = std::get_if<WaitUntil>(&instruction))
        {
            // A null predicate is "never ready", not "throw": these run inside
            // the frame loop where there is no caller to catch.
            entry.blocked = (until->predicate == nullptr) ? true : !until->predicate();
        }
        else if (const auto* while_inst = std::get_if<WaitWhile>(&instruction))
        {
            entry.blocked = (while_inst->predicate != nullptr) && while_inst->predicate();
        }
        else if (const auto* nested = std::get_if<WaitForCoroutine>(&instruction))
        {
            // Already-finished nested coroutines must NOT read as running: the
            // id is recycled-free but the entry is gone, so a "still running"
            // answer here would hang the waiter forever on something that is
            // over. IsRunning() covers m_PendingAdds too.
            entry.blocked = IsRunning(nested->id);
        }
        else
        {
            entry.blocked = false;
        }
    }

    ResumePhase(CoroutinePhase::kUpdate);
}

void CoroutineScheduler::TickFixedUpdate()
{
    SplicePending();
    ResumePhase(CoroutinePhase::kFixedUpdate);
}

void CoroutineScheduler::TickLateUpdate()
{
    SplicePending();
    ResumePhase(CoroutinePhase::kLateUpdate);
}

void CoroutineScheduler::TickEndOfFrame()
{
    SplicePending();
    ResumePhase(CoroutinePhase::kEndOfFrame);
}
