#pragma once

// =============================================================================
// Behaviour -- the base class of anything a script can be attached to.
//
// Unity's `MonoBehaviour.Invoke` family lives here rather than on Component:
// Transform / Renderer / Collider have no notion of "a method by name".
//
// Why the timers are coroutines and not a list ticked by Behaviour::Tick
// --------------------------------------------------------------------
// Two of the three Behaviour subclasses (TypeScriptComponent,
// AIControllerComponent) override Tick. A pump inside Behaviour::Tick would
// therefore only run for the subclass that remembers to call Super::Tick --
// and forgetting is silent: it compiles, it runs, and Invoke just never fires.
// The CoroutineScheduler is ticked from Application::LogicalTick for every
// phase, so routing through it removes the cooperation requirement entirely.
//
// Cost: an Invoke is a coroutine, so it is driven by the same clock as
// WaitForSeconds (scaled game time), which is what Unity does too.
//
// Script coroutines
// -----------------
// `StartCoroutine("MyRoutine")` starts the same kind of pump, except the body
// lives on the script side as a *generator*. One C++ coroutine per script
// generator: on every resume it calls `generator.next()` once and co_yields
// whatever the script yielded, translated into the engine's YieldInstruction
// (see ScriptYield.h). The scheduler never learns that a script is involved, so
// the four resume phases, timeScale and StopAllForOwner all work unchanged.
//
// The generator object itself is owned by the subclass (TypeScriptComponent),
// because only the scripting layer can hold a JS value. `Behaviour` refers to
// it through an opaque `uint64_t`, exactly the way it refers to a coroutine
// through the scheduler's id.
// =============================================================================

#include "Runtime/Function/Framework/Component/Component.h"
#include "Runtime/Function/Framework/Coroutine/ScriptYield.h"

#include <EASTL/string.h>
#include <EASTL/vector.h>

#include <cstdint>

#include "Behaviour.generated.h"

ZCLASS(Abstract)
class Behaviour : public Component
{
    ZGENERATED_BODY(Behaviour)

public:
    ~Behaviour() override;

    // --- Unity's Invoke family ---------------------------------------------

    // NOTE: none of these carry ZFUNCTION, and that is deliberate. `Behaviour`
    // is also the name of the SYNTHETIC script class TS behaviours extend
    // (ScriptBinding.cpp's g_BehaviourClass, which supplies `gameObject` /
    // `transform` / `component`). Publishing the C++ class would emit a second
    // `declare class Behaviour` and tsc would fail on the duplicate identifier
    // -- ReflectionCodegenSmokeTest's BEHAVIOUR gate is what catches it.
    //
    // Scripts reach these through `TypeScriptComponent`, whose ZFUNCTIONs
    // forward here, and ScriptEnv's `Behaviour.prototype` shim turns that into
    // Unity's `this.Invoke(...)`.

    /// Call `methodName` once, `time` seconds from now.
    ///
    /// `time` must be > 0: Unity rejects a negative delay outright, and 0 would
    /// be "sometime during this frame" -- which is what a direct call is for.
    void Invoke(const eastl::string& methodName, float time);

    /// Call `methodName` after `time`, then every `repeatRate` seconds forever,
    /// until CancelInvoke.
    ///
    /// `repeatRate` must be > 0: 0 would spin the scheduler in a tight loop
    /// inside a single frame.
    void InvokeRepeating(const eastl::string& methodName, float time, float repeatRate);

    /// Cancel pending invocations of `methodName`. An empty / unknown name
    /// cancels every invoke on this behaviour -- Unity's
    /// `CancelInvoke(string methodName = "")`.
    void CancelInvoke(const eastl::string& methodName);

    /// Cancel everything this behaviour has scheduled.
    void CancelAllInvokes();

    /// Is there a pending invocation of `methodName`?
    bool IsInvoking(const eastl::string& methodName);

    /// Deliver one invocation. Public because the coroutine that carries the
    /// timer is a file-local function in Behaviour.cpp, not a member.
    void FireInvoke(const eastl::string& method_name);

    // --- Unity's coroutine family -----------------------------------------

    /// Start `methodName` as a coroutine.
    ///
    /// `methodName` must name a method that returns a generator (`*MyRoutine()`
    /// in TypeScript). Unity also has a `StartCoroutine(IEnumerator)` overload;
    /// the engine only offers the string one, because a JS generator cannot be
    /// carried across the binding boundary -- which is fine, since the string
    /// overload is public Unity API and not a stand-in.
    ///
    /// The first segment runs synchronously, exactly like Unity's: the routine
    /// has already executed up to its first `yield` by the time this returns.
    ///
    /// Returns `CoroutineScheduler::kInvalidId` (0) on failure: empty name, no
    /// scheduler, or the method did not hand back a generator.
    uint64_t StartCoroutine(const eastl::string& methodName);

    /// Stop every coroutine this behaviour started from `methodName`.
    ///
    /// Unknown / empty name is a no-op, matching Unity's `StopCoroutine` on a
    /// routine that already finished.
    void StopCoroutine(const eastl::string& methodName);

    /// Stop every coroutine this behaviour owns. Also called from the
    /// destructor: a coroutine whose owner is gone would otherwise keep
    /// calling into freed script state on its next resume.
    void StopAllCoroutines();

    /// Is a coroutine started from `methodName` still running?
    bool IsCoroutineRunning(const eastl::string& methodName);

    /// How many coroutines this behaviour currently owns (finished ones
    /// pruned). Test seam: "started one" and "it is listed" are different
    /// claims and only the second one can be asserted from outside.
    size_t GetCoroutineCount() const;

    // -- Script-coroutine bridge -------------------------------------------
    // Public for the same reason FireInvoke is: the pump is a file-local
    // coroutine in Behaviour.cpp, not a member function.

    /// Ask the subclass to call `method_name` and keep whatever it returned.
    /// On success `out_script_id` names that value for the later two calls.
    /// Returns false when this behaviour has no script side at all -- which is
    /// not an error, it is what a plain C++ Behaviour answers.
    virtual bool StartScriptCoroutine(const char* method_name, uint64_t& out_script_id);

    /// Run the script body up to its next `yield`. `out.done` reports that the
    /// generator ran out. Returns false when the body threw or the value is
    /// gone -- the caller must stop, because polling a broken generator every
    /// frame forever is worse than dropping the coroutine.
    virtual bool StepScriptCoroutine(uint64_t script_id, ScriptYield& out);

    /// Release whatever `StartScriptCoroutine` handed out. Must tolerate an
    /// id that was already released (Stop-then-Prune is a normal sequence).
    virtual void EndScriptCoroutine(uint64_t script_id);

    /// Called by the pump when the body ends on its own, so the generator does
    /// not wait for a prune that may never come.
    ///
    /// Why this exists: `CoroutineScheduler::Stop()` drops the coroutine frame
    /// without running the rest of the body, so a *stopped* coroutine never
    /// reaches its own exit code and `Behaviour` has to release for it -- while
    /// a *finished* one does reach it, and would otherwise hold the JS value
    /// until somebody happened to call Start / IsRunning again. Routing both
    /// exits through the same flag is what makes the release exactly-once.
    void ReleaseScriptCoroutine(uint64_t script_id);

protected:
    /// Try to deliver `method_name` to THIS behaviour.
    ///
    /// The default is "I have no methods": TypeScriptComponent overrides it to
    /// call into its own JS instance. Returning false falls back to
    /// GameObject.SendMessage, which is the broadcast semantics Unity's
    /// SendMessage has and is what a non-script Behaviour needs.
    virtual bool InvokeBehaviourMethod(const char* method_name);

    /// Forget entries whose coroutine has finished (they are not removed by the
    /// scheduler, which has no back-reference to whoever started them).
    void PruneFinishedInvokes();

    /// The same, for coroutines. Also releases the script-side generator: a
    /// JS value that is never released is a leak the GC cannot see through a
    /// strong ref.
    void PruneFinishedCoroutines();

    struct PendingInvoke
    {
        eastl::string method;
        uint64_t id {0};
    };

    struct PendingCoroutine
    {
        eastl::string method;

        /// Scheduler id. Needed for `Stop()`.
        uint64_t id {0};

        /// Opaque handle to the script-side generator. Needed for
        /// `EndScriptCoroutine`, which the scheduler knows nothing about.
        uint64_t script_id {0};

        /// False once `EndScriptCoroutine` has been called for `script_id`.
        ///
        /// Two independent paths can end the same coroutine -- the pump (body
        /// ran out) and Stop*/Prune (somebody cancelled it) -- and a JS value
        /// ref must be released exactly once. Relying on the idempotence of
        /// `EndScriptCoroutine` instead would hide the double call, and the
        /// double call is exactly the thing that later turns into
        /// "released someone else's generator" once slots get reused.
        bool generator_owned {true};
    };

    /// Release `entry`'s generator unless it is already gone. Exactly-once.
    void ReleasePendingGenerator(PendingCoroutine& entry);

    eastl::vector<PendingInvoke> m_Invokes;
    eastl::vector<PendingCoroutine> m_Coroutines;
};
