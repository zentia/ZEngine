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
// =============================================================================

#include "Runtime/Function/Framework/Component/Component.h"

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

    struct PendingInvoke
    {
        eastl::string method;
        uint64_t id {0};
    };

    eastl::vector<PendingInvoke> m_Invokes;
};
