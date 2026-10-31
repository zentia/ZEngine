#pragma once

#include "Runtime/Core/Math/Quaternion.h"
#include "Runtime/Core/Math/Vector2.h"
#include "Runtime/Core/Math/Vector3.h"

#include <cstdint>

// =============================================================================
// Random -- Unity's global, seedable random number source.
//
// Why not the existing RandomNumberGenerator / std::mt19937
// ---------------------------------------------------------
// Two properties Unity's Random has that `std::` distributions do not:
//
//  1. **Cross-machine determinism.** `std::uniform_int_distribution` and
//     `std::uniform_real_distribution` are *implementation-defined*: the same
//     seed and the same call sequence produce different numbers on MSVC and
//     libstdc++. A replay, a lockstep network game, or a bug report that says
//     "seed 12345, it exploded" is worthless without this. Everything here is
//     built from one PCG32 step, which is fully specified by ~6 lines of
//     arithmetic.
//
//  2. **A state you can save.** `mt19937` carries 2.5 KB of state and no
//     portable way to serialise it; `Random::State` is one uint64. Saving a
//     seed mid-run (checkpoint / replay start) has to be cheap and exact.
//
// The API quirk that matters most
// -------------------------------
//   Random.Range(int,   int)   -> max is EXCLUSIVE
//   Random.Range(float, float) -> max is INCLUSIVE
//
// This asymmetry is real Unity behaviour, not a typo, and it is a famous
// footgun: `Random.Range(0, 10)` as ints never returns 10, as floats it can.
// It is reproduced here deliberately and pinned by tests, because "fixing" it
// would silently change what every ported script rolls.
// =============================================================================

class UnityRandom
{
public:
    /// One uint64. Enough to save/restore, small enough to put in a replay
    /// header or a save game.
    using State = uint64_t;

    /// Seeds the global stream. Pass the same seed, get the same sequence --
    /// on every platform, on every run.
    static void InitState(uint32_t seed);

    /// Seeds from a non-deterministic source (time + address entropy). Use for
    /// "I don't care"; use InitState for anything reproducible.
    static void InitUnseeded();

    static State GetState();
    static void SetState(State state);

    /// [0, 1). Never returns 1.0 -- code that indexes an array with
    /// `floor(value * n)` depends on that.
    static float value();

    /// Float range, **max inclusive**.
    static float Range(float min, float max);

    /// Int range, **max EXCLUSIVE** (Unity's own quirk). Returns `min` when
    /// `min >= max` rather than doing something clever: a caller that asks for
    /// an empty range has a bug, and `min` is the answer least likely to be
    /// mistaken for a valid roll.
    static int Range(int min, int max);

    /// Uniform **by volume**, not by radius. Uniform-by-radius clumps at the
    /// centre, which looks wrong the moment it is used for spawn scatter.
    static Vector3 insideUnitSphere();

    static Vector2 insideUnitCircle();

    /// Exactly on the sphere surface (length 1).
    static Vector3 onUnitSphere();

    static Quaternion rotation();

    /// Two-dimensional direction, angle in [0, 2pi).
    static Vector2 onUnitCircle();

    /// True with probability `p`. `p <= 0` -> never, `p >= 1` -> always: a
    /// probability that comes from a float calculation must not be allowed to
    /// produce "always" when the caller meant "almost never".
    static bool Chance(float probability);

private:
    /// PCG32 (xsh-rr 64/32). One step, fully specified, no libraries.
    static uint32_t NextUInt32();

    static State s_State;
};
