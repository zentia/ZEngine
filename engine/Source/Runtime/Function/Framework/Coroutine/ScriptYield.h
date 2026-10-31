#pragma once

// =============================================================================
// ScriptYield -- what a *script* generator yielded, in a form the C++ layer can
// read without knowing which script backend is running.
//
// Why this is its own header
// --------------------------
// The value crosses two boundaries that point in opposite directions:
//
//   ScriptEnv.cpp (owns pesapi)  ->  Behaviour/TypeScriptComponent  ->  Coroutine.h
//
// `Behaviour` must be able to hold one of these, and `Behaviour.h` is included
// by every component in the engine -- pulling pesapi.h in there would drag
// QuickJS into every translation unit that so much as forward-declares a
// component. So the struct is a plain POD with no dependencies of its own, and
// each side converts to / from its own representation at the edge.
//
// Deliberately NOT the full YieldInstruction
// ------------------------------------------
// Coroutine.h's YieldInstruction also carries WaitUntil / WaitWhile / WaitFor-
// Coroutine, all of which hold a `std::function` or an engine-side id. A script
// can express `WaitUntil` (it is just a predicate), but the closure would
// outlive the frame that created it and there is no safe way to poll it after
// the env has been torn down. Supporting those in script is a separate piece of
// work; the six kinds below cover everything Unity's YieldInstruction ships.
//
// Unknown / malformed values degrade to kNull, never to "hang". A coroutine
// that silently never resumes is far harder to debug than one that resumes one
// frame too early.
// =============================================================================

#include <cstdint>

/// Which Unity `yield return` construct the script asked for.
enum class ScriptYieldKind : uint8_t
{
    /// `yield return null` (or `undefined`, or nothing): resume next Update.
    kNull = 0,
    /// `WaitForSeconds` -- scaled game time, so timeScale == 0 pauses it.
    kSeconds,
    /// `WaitForSecondsRealtime` -- wall clock, ignores timeScale and pause.
    kSecondsRealtime,
    /// `WaitForFixedUpdate`.
    kFixedUpdate,
    /// `WaitForLateUpdate`.
    kLateUpdate,
    /// `WaitForEndOfFrame`.
    kEndOfFrame,
    /// `WaitUntil(predicate)` -- resume the first frame the predicate is true.
    kUntil,
    /// `WaitWhile(predicate)` -- resume the first frame the predicate is false.
    kWhile,
};

struct ScriptYield
{
    ScriptYieldKind kind = ScriptYieldKind::kNull;

    /// Only meaningful for kSeconds / kSecondsRealtime.
    float seconds = 0.0f;

    /// True when the generator body ran to its return. `kind` / `seconds` are
    /// meaningless then, and the caller must stop pumping.
    bool done = false;

    /// Only meaningful for kUntil / kWhile: a **strong ref** to the script-side
    /// predicate, as an opaque pointer so this header stays free of pesapi.
    ///
    /// Ownership: `ScriptEnv::GeneratorNext` creates it and hands it over;
    /// whoever called GeneratorNext (TypeScriptComponent::StepScriptCoroutine)
    /// must either store it or release it -- there is no RAII here because the
    /// owner is a component whose destruction order relative to the VM is not
    /// guaranteed.
    ///
    /// Why it is a ref and not "call it once and cache the bool": the closure
    /// captures `this` of the script behaviour, and Unity's contract is that
    /// the predicate is *polled every frame* until it flips. A snapshot would
    /// make `WaitUntil(() => this.ready)` wait forever.
    void* predicate = nullptr;
};
