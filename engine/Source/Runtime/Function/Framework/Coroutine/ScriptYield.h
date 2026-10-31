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
};

struct ScriptYield
{
    ScriptYieldKind kind = ScriptYieldKind::kNull;

    /// Only meaningful for kSeconds / kSecondsRealtime.
    float seconds = 0.0f;

    /// True when the generator body ran to its return. `kind` / `seconds` are
    /// meaningless then, and the caller must stop pumping.
    bool done = false;
};
