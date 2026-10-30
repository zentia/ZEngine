#pragma once

#include "Runtime/Core/Base/EngineSystem.h"

#include <EASTL/vector.h>

#include <coroutine>
#include <cstdint>
#include <functional>
#include <variant>

// =============================================================================
// Coroutines -- Unity's `StartCoroutine` / `yield return`, in C++.
//
//   Coroutine MyRoutine()
//   {
//       co_yield WaitForSeconds{0.5f};
//       DoSomething();
//       co_yield WaitForEndOfFrame{};
//       DoSomethingElse();
//   }
//
//   const uint64_t id = scheduler->Start(MyRoutine(), owner_id);
//
// Why C++20 coroutines and not a hand-rolled state machine
// --------------------------------------------------------
// A hand-rolled stepper (`std::function<Step()>`) forces the body to be written
// as an explicit switch over a state enum, which is exactly the code Unity's
// `IEnumerator` sugar exists to hide. C++20 gives us that sugar: the compiler
// generates the state machine, the body reads top-to-bottom, and local
// variables survive across a yield -- which is the whole point.
//
// The four resume phases
// ----------------------
// Unity does not have one tick; it has several, and *which* phase a coroutine
// resumes in is part of its meaning:
//
//   yield WaitForFixedUpdate   resumed from CoroutineScheduler::TickFixedUpdate
//   yield null / WaitForSeconds/Until/While
//                              resumed from TickUpdate
//   yield WaitForLateUpdate    resumed from TickLateUpdate
//   yield WaitForEndOfFrame    resumed from TickEndOfFrame
//
// Getting this wrong is silent: a coroutine that waits for end-of-frame but
// resumes before Update still produces *a* value, just one frame stale. So the
// phase is stored on the entry (`Entry::phase`) rather than inferred, and each
// Tick only looks at its own phase.
//
// Failure handling
// ----------------
// `unhandled_exception()` swallows. A coroutine is user gameplay code running
// inside the middle of the engine's frame; letting an exception escape through
// `resume()` would unwind past Level::Tick and take the frame down with it.
// Swallowing turns it into "this one coroutine stopped", which is the least
// destructive thing that is still honest.
// =============================================================================

// ---- yield instructions -----------------------------------------------------

/// `yield return null` in Unity: run again next frame, in the Update phase.
struct WaitForNextFrame
{
};

/// Scaled time. `WaitForSeconds(1)` at timeScale 0.5 takes two real seconds.
/// This is the one most scripts want; see WaitForSecondsRealtime for the other.
struct WaitForSeconds
{
    float seconds = 0.0f;
};

/// Wall-clock seconds. Unaffected by timeScale and by pause, so a pause menu
/// that animates with this still animates while the world is frozen.
struct WaitForSecondsRealtime
{
    float seconds = 0.0f;
};

struct WaitForFixedUpdate
{
};

struct WaitForLateUpdate
{
};

/// After every Update, right before the frame is presented. Camera follow and
/// "look at where the thing ended up" logic belongs here -- Update is too
/// early, because later Updates may still move the target.
struct WaitForEndOfFrame
{
};

/// Resumes the first frame `predicate()` returns true. Predicate is polled once
/// per Update phase.
struct WaitUntil
{
    std::function<bool()> predicate;
};

/// The inverse: resumes the first frame `predicate()` returns false.
struct WaitWhile
{
    std::function<bool()> predicate;
};

/// Waits for another coroutine to finish. `yield return StartCoroutine(other)`
/// in Unity. The id is the one `CoroutineScheduler::Start()` handed back.
struct WaitForCoroutine
{
    uint64_t id = 0;
};

using YieldInstruction = std::variant<WaitForNextFrame,
                                     WaitForSeconds,
                                     WaitForSecondsRealtime,
                                     WaitForFixedUpdate,
                                     WaitForLateUpdate,
                                     WaitForEndOfFrame,
                                     WaitUntil,
                                     WaitWhile,
                                     WaitForCoroutine>;

// ---- the coroutine itself ---------------------------------------------------

/// A suspended coroutine plus its current wait state.
///
/// Move-only: the coroutine frame is a unique allocation owned by the handle,
/// and copying it would give two owners of one frame.
class Coroutine
{
public:
    struct promise_type;

    using Handle = std::coroutine_handle<promise_type>;

    struct promise_type
    {
        /// What the body last yielded. Read by the scheduler after each resume.
        YieldInstruction pending {WaitForNextFrame{}};

        Coroutine get_return_object();

        /// Starts suspended: `Start()` performs the first resume itself, so the
        /// body runs up to its first yield at StartCoroutine time (Unity does
        /// the same -- the first segment is synchronous).
        std::suspend_always initial_suspend() noexcept { return {}; }

        std::suspend_always final_suspend() noexcept { return {}; }

        void return_void() {}

        std::suspend_always yield_value(YieldInstruction instruction)
        {
            pending = std::move(instruction);
            return {};
        }

        /// Swallowed on purpose -- see the header comment. The coroutine is
        /// marked finished so the scheduler drops it instead of resuming a
        /// handle that already unwound.
        void unhandled_exception() {}
    };

    Coroutine() = default;
    explicit Coroutine(Handle handle)
        : m_Handle(handle)
    {
    }

    Coroutine(const Coroutine&) = delete;
    Coroutine& operator=(const Coroutine&) = delete;

    Coroutine(Coroutine&& other) noexcept
        : m_Handle(other.m_Handle)
    {
        other.m_Handle = nullptr;
    }

    Coroutine& operator=(Coroutine&& other) noexcept
    {
        if (this != &other)
        {
            Destroy();
            m_Handle = other.m_Handle;
            other.m_Handle = nullptr;
        }
        return *this;
    }

    ~Coroutine() { Destroy(); }

    bool IsValid() const { return m_Handle != nullptr; }

    /// True when the body has run to completion (`done()` is the coroutine
    /// library's own flag; it is set once final_suspend is reached).
    bool IsDone() const { return (m_Handle == nullptr) || m_Handle.done(); }

    /// Runs the body up to its next yield (or to completion). Returns the
    /// instruction it stopped at; WaitForNextFrame when it finished, which the
    /// scheduler reads as "remove me".
    YieldInstruction Resume();

    const YieldInstruction& GetPending() const
    {
        static const YieldInstruction kDefault {WaitForNextFrame{}};
        return (m_Handle != nullptr) ? m_Handle.promise().pending : kDefault;
    }

private:
    void Destroy()
    {
        if (m_Handle != nullptr)
        {
            m_Handle.destroy();
            m_Handle = nullptr;
        }
    }

    Handle m_Handle {nullptr};
};

inline Coroutine Coroutine::promise_type::get_return_object()
{
    return Coroutine {Handle::from_promise(*this)};
}

// ---- the scheduler ----------------------------------------------------------

enum class CoroutinePhase : uint8_t
{
    kUpdate = 0,
    kFixedUpdate,
    kLateUpdate,
    kEndOfFrame,
};

/// Owns every running coroutine and drives them from the frame loop.
///
/// One global instance (registered as a system) rather than one per component:
/// a coroutine's owner can be destroyed mid-flight, and an owner-held list
/// would leave the frame dangling. Here, `StopAllForOwner()` is a single pass
/// over one list, and an owner id that is never stopped just leaks a slot in
/// that list until the coroutine finishes by itself -- which is the same
/// behaviour Unity has.
class CoroutineScheduler : public IEngineSystem
{
public:
    static constexpr uint64_t kInvalidId = 0;

    SystemInitPhase GetInitPhase() const override { return SystemInitPhase::Gameplay; }
    bool Initialize() override;
    void Shutdown() override;
    std::string GetName() const override { return AUTO_GET_CLASS_NAME(); }

    /// Takes ownership of `routine` and runs it up to its first yield right
    /// now -- the first segment is synchronous, matching Unity.
    ///
    /// `owner` is an opaque key for `StopAllForOwner()`; pass the component's
    /// instance id. It is NOT dereferenced, so a destroyed component's id is
    /// safe to pass.
    ///
    /// Returns kInvalidId only when the routine was already finished on entry.
    uint64_t Start(Coroutine&& routine, uint64_t owner = 0);

    void Stop(uint64_t id);
    void StopAll();
    void StopAllForOwner(uint64_t owner);

    bool IsRunning(uint64_t id) const;

    /// Scheduled coroutines, pending splice included. Counting only m_Entries
    /// would report 0 for a coroutine that was just started and has not been
    /// ticked yet -- and "I started it and it is not there" is exactly the
    /// shape of the bug this accessor is used to catch.
    int GetRunningCount() const
    {
        return static_cast<int>(m_Entries.size() + m_PendingAdds.size());
    }

    /// How many times `Resume()` has been called. The wiring gate uses it to
    /// prove a coroutine actually advanced rather than merely existing.
    uint64_t GetResumeCount() const { return m_ResumeCount; }

    // ---- frame hooks ----------------------------------------------------
    // Called from the frame loop, in this order. Each one only resumes the
    // entries parked in its own phase -- see the header comment on why the
    // phase is stored and not inferred.
    void TickUpdate(float scaled_delta_time, float unscaled_delta_time);
    void TickFixedUpdate();
    void TickLateUpdate();
    void TickEndOfFrame();

private:
    struct Entry
    {
        uint64_t id = 0;
        uint64_t owner = 0;
        Coroutine routine;
        CoroutinePhase phase = CoroutinePhase::kUpdate;

        /// Not ready to resume yet. Recomputed every Update for the conditional
        /// waits (Until / While / nested coroutine), and drained by the timer
        /// for the two time-based ones. `WaitForNextFrame` simply leaves it
        /// false, which is why it resumes on the very next Update.
        bool blocked = false;

        /// True while a WaitForSeconds / WaitForSecondsRealtime is running.
        bool has_timer = false;

        /// Accumulated, not counted down.
        ///
        /// Counting down (`remaining -= dt`) drifts: five subtractions of
        /// `0.1f` from `0.5f` leave **+1.49e-8**, which is still `> 0`, so
        /// `WaitForSeconds(0.5)` at a 0.1s step takes six frames instead of
        /// five. Accumulating the elapsed time and comparing with a tolerance
        /// rounds the other way -- five `0.1f` additions land on exactly 0.5f.
        ///
        /// The tolerance is 10 microseconds: three orders of magnitude below
        /// any frame (so it can never make a wait fire early in practice) and
        /// five orders above the drift at these magnitudes.
        static constexpr float kTimeEpsilon = 1.0e-5f;
        float elapsed_scaled = 0.0f;
        float elapsed_unscaled = 0.0f;
        float duration_scaled = 0.0f;
        float duration_unscaled = 0.0f;
    };

    /// Applies a freshly yielded instruction to an entry (sets phase, timers).
    void ApplyYield(Entry& entry, const YieldInstruction& instruction);

    /// Moves m_PendingAdds into m_Entries. Called at the TOP of every phase,
    /// not inside ResumePhase -- see the .cpp for why the difference costs a
    /// frame.
    void SplicePending();

    /// Resumes every entry parked in `phase`. Collects ids to drop in a
    /// separate pass: a resumed body may start or stop coroutines, which would
    /// invalidate any iterator held across the resume.
    void ResumePhase(CoroutinePhase phase);

    bool IsPending(uint64_t id) const;

    eastl::vector<Entry> m_Entries;
    eastl::vector<Entry> m_PendingAdds;
    uint64_t m_NextId = 1;
    uint64_t m_ResumeCount = 0;
};
