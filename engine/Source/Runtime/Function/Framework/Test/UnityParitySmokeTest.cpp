// =============================================================================
// Unity-parity smoke test.
//
//   cmake -B build -DZENGINE_BUILD_UNITY_PARITY_SMOKE_TEST=ON
//   cmake --build build --target UnityParitySmokeTest
//   ctest --test-dir build -C Debug -R UnityParitySmokeTest
//
// Why one suite instead of one per feature
// ----------------------------------------
// Every new executable costs a CMakeLists.txt, a `option()`, two
// `list(REMOVE_ITEM ...)` entries in Runtime/CMakeLists.txt (SOURCE_FILES *and*
// REGULAR_SOURCE_FILES -- only one and /WHOLEARCHIVE drags the test's main()
// into ZRuntime and every consumer exe fails with LNK2005), and a `-D...=ON`
// in a workflow or the CI coverage ratchet (CiSmokeTestCoverageSmokeTest B3)
// goes red. The features in here are all small, all Unity-API-shaped and all
// headless, so they share one target and the boilerplate is paid once.
//
// Two groups
// ----------
//   A. Behaviour -- the rules themselves, tested against a real instance.
//   B. Wiring gate -- source-level: the rule exists in the engine at all.
//      A perfect implementation that is never called is indistinguishable from
//      no implementation, and it compiles and runs silently. This is the group
//      that catches that.
// =============================================================================

#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Core/Math/UnityRandom.h"
#include "Runtime/Function/Framework/Coroutine/Coroutine.h"
#include "Runtime/Function/Framework/TagLayer/TagLayerManager.h"
#include "Runtime/Function/Physics/Jolt/Utils.h"
#include "Runtime/Function/Physics/Physics.h"
#include "Runtime/Function/Physics/PhysicsManager.h"
#include "Runtime/Function/Physics/PhysicsScene.h"

#include "Jolt/Jolt.h"
#include "Jolt/Physics/Body/Body.h"
#include "Jolt/Physics/Body/BodyCreationSettings.h"
#include "Jolt/Physics/Collision/Shape/BoxShape.h"
#include "Jolt/Physics/PhysicsSystem.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#if defined(_MSC_VER) && defined(_DEBUG)
    #include <crtdbg.h>
#endif

// The engine root is baked in by CMake so the wiring gate can read real source
// files without hard-coding a developer's drive letter.
#if !defined(ZENGINE_ENGINE_ROOT_DIR)
    #define ZENGINE_ENGINE_ROOT_DIR ""
#endif

namespace
{
    int g_Checks = 0;
    int g_Failures = 0;

    void Check(bool ok, const char* id, const char* what)
    {
        ++g_Checks;
        if (ok)
        {
            std::printf("[ ok ] %-4s %s\n", id, what);
        }
        else
        {
            ++g_Failures;
            std::printf("[FAIL] %-4s %s\n", id, what);
        }
    }

    std::string ReadFile(const std::string& rel_path)
    {
        const std::string full = std::string(ZENGINE_ENGINE_ROOT_DIR) + "/" + rel_path;
        std::ifstream in(full, std::ios::binary);
        if (!in)
        {
            return std::string();
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    /// Occurrences of `needle` in `haystack`, skipping nothing. Used by the
    /// wiring gate: counting beats `find() != npos` because a threshold can
    /// distinguish "declared" from "declared AND used".
    int CountOccurrences(const std::string& haystack, const char* needle)
    {
        int count = 0;
        std::string::size_type pos = 0;
        const std::string n(needle);
        while ((pos = haystack.find(n, pos)) != std::string::npos)
        {
            ++count;
            pos += n.size();
        }
        return count;
    }

    // ---------------------------------------------------------------- A ----
    void TestTags(TagLayerManager& m)
    {
        Check(m.HasTag("Untagged"), "A1", "the default tag is registered");
        Check(!m.HasTag(""), "A2", "the empty tag is never registered");
        Check(!m.HasTag("ThisTagWasNeverAdded"), "A3", "an unknown tag is not registered");

        Check(m.AddTag("Enemy"), "A4", "a new user tag can be added");
        Check(m.HasTag("Enemy"), "A5", "and is then registered");
        Check(!m.AddTag("Enemy"), "A6", "adding it twice is rejected (no duplicates)");
        Check(!m.AddTag(""), "A7", "adding the empty tag is rejected");

        Check(!m.RemoveTag("Untagged"), "A8", "a built-in tag cannot be removed");
        Check(m.HasTag("Untagged"), "A9", "and is still there afterwards");
        Check(m.RemoveTag("Enemy"), "A10", "a user tag can be removed");
        Check(!m.HasTag("Enemy"), "A11", "and is gone afterwards");
        Check(!m.RemoveTag("Enemy"), "A12", "removing it twice is rejected");

        const eastl::vector<eastl::string> tags = m.GetTags();
        Check(!tags.empty() && tags[0] == "Untagged", "A13", "Untagged is listed first");
        Check(m.GetTagCount() == static_cast<int>(tags.size()), "A14", "GetTagCount matches GetTags");
    }

    void TestLayers(TagLayerManager& m)
    {
        Check(m.LayerToName(0) == "Default", "A15", "layer 0 is Default");
        Check(m.LayerToName(5) == "UI", "A16", "layer 5 is UI");
        Check(m.NameToLayer("UI") == 5, "A17", "NameToLayer(\"UI\") is 5");
        Check(m.NameToLayer("Default") == 0, "A18", "NameToLayer(\"Default\") is 0");
        Check(m.NameToLayer("NoSuchLayer") == TagLayerManager::kInvalidLayer,
              "A19", "an unknown layer name is -1, not 0");

        Check(!m.SetLayerName(0, "Ground"), "A20", "a built-in layer cannot be renamed");
        Check(m.LayerToName(0) == "Default", "A21", "and keeps its name");
        Check(!m.SetLayerName(99, "Ground"), "A22", "an out-of-range layer cannot be named");
        Check(!m.SetLayerName(-1, "Ground"), "A23", "a negative layer cannot be named");

        Check(m.SetLayerName(8, "Ground"), "A24", "a user layer (8) can be named");
        Check(m.NameToLayer("Ground") == 8, "A25", "and resolves back to 8");
        Check(!m.SetLayerName(9, "Ground"), "A26", "a duplicate layer name is rejected");
        Check(m.NameToLayer("Ground") == 8, "A27", "so the name stays unambiguous");

        Check(!m.IsUserLayer(0) && !m.IsUserLayer(7), "A28", "0 and 7 are not user layers");
        Check(m.IsUserLayer(8) && m.IsUserLayer(31), "A29", "8 and 31 are user layers");
        Check(!m.IsUserLayer(32), "A30", "32 is out of range");
    }

    void TestMasks()
    {
        Check(TagLayerManager::LayerToMask(0) == 1u, "A31", "layer 0 is bit 0");
        Check(TagLayerManager::LayerToMask(5) == (1u << 5), "A32", "layer 5 is bit 5");
        Check(TagLayerManager::LayerToMask(31) == (1u << 31), "A33", "layer 31 is bit 31");
        Check(TagLayerManager::LayerToMask(32) == 0u, "A34", "layer 32 is 0, not UB from 1<<32");
        Check(TagLayerManager::LayerToMask(-1) == 0u, "A35", "a negative layer is 0");

        Check(TagLayerManager::MaskContainsLayer(1u << 5, 5), "A36", "a mask contains its own layer");
        Check(!TagLayerManager::MaskContainsLayer(1u << 5, 6), "A37", "and not the neighbour");
        Check(!TagLayerManager::MaskContainsLayer(0u, 0), "A38", "an empty mask contains nothing");
    }

    void TestCollisionMatrix(TagLayerManager& m)
    {
        // Fresh table: everything collides except Ignore Raycast.
        Check(!m.GetIgnoreLayerCollision(0, 1), "A39", "by default every pair collides");
        Check(m.GetIgnoreLayerCollision(TagLayerManager::kIgnoreRaycastLayer, 0),
              "A40", "Ignore Raycast is excluded from the default layer");
        Check(m.GetIgnoreLayerCollision(TagLayerManager::kIgnoreRaycastLayer,
                                        TagLayerManager::kIgnoreRaycastLayer),
              "A41", "and from itself");
        Check((m.GetCollisionMask(TagLayerManager::kIgnoreRaycastLayer) == 0u),
              "A42", "so its collision mask is empty");

        m.SetIgnoreLayerCollision(8, 9, true);
        Check(m.GetIgnoreLayerCollision(8, 9), "A43", "a pair can be excluded");
        Check(m.GetIgnoreLayerCollision(9, 8), "A44", "and the exclusion is symmetric");
        Check((m.GetCollisionMask(8) & (1u << 9)) == 0u, "A45", "layer 8's mask drops bit 9");
        Check((m.GetCollisionMask(9) & (1u << 8)) == 0u, "A46", "and layer 9's mask drops bit 8");
        Check((m.GetCollisionMask(8) & (1u << 10)) != 0u, "A47", "but keeps bit 10");

        m.SetIgnoreLayerCollision(8, 9, false);
        Check(!m.GetIgnoreLayerCollision(8, 9), "A48", "the exclusion can be lifted");
        Check(!m.GetIgnoreLayerCollision(9, 8), "A49", "and symmetrically");

        m.SetIgnoreLayerCollision(0, 1, true);
        m.ResetCollisionMatrix();
        Check(!m.GetIgnoreLayerCollision(0, 1), "A50", "ResetCollisionMatrix clears exclusions");
        Check(m.GetIgnoreLayerCollision(TagLayerManager::kIgnoreRaycastLayer, 0),
              "A51", "but Ignore Raycast stays excluded");
        Check(m.GetCollisionMask(99) == 0u, "A52", "an out-of-range layer collides with nothing");
    }

    // ---------------------------------------------------------------- C ----
    // Coroutines. Every case ticks the scheduler by hand with an explicit
    // delta, so the expected frame counts are exact integers rather than "after
    // a while".
    int g_Log = 0;

    /// Increments a counter before and after one yield. Used to prove the first
    /// segment ran synchronously inside Start().
    Coroutine CountTwoFrames()
    {
        g_Log = 1;
        co_yield WaitForNextFrame {};
        g_Log = 2;
    }

    /// Two yields -> needs two Update ticks after the initial segment.
    Coroutine CountThree()
    {
        g_Log = 10;
        co_yield WaitForNextFrame {};
        g_Log = 11;
        co_yield WaitForNextFrame {};
        g_Log = 12;
    }

    Coroutine WaitHalfSecond()
    {
        g_Log = 20;
        co_yield WaitForSeconds {0.5f};
        g_Log = 21;
    }

    Coroutine WaitRealtime()
    {
        g_Log = 30;
        co_yield WaitForSecondsRealtime {0.3f};
        g_Log = 31;
    }

    Coroutine WaitFixed()
    {
        g_Log = 40;
        co_yield WaitForFixedUpdate {};
        g_Log = 41;
    }

    Coroutine WaitLate()
    {
        g_Log = 50;
        co_yield WaitForLateUpdate {};
        g_Log = 51;
    }

    Coroutine WaitEndOfFrame()
    {
        g_Log = 60;
        co_yield WaitForEndOfFrame {};
        g_Log = 61;
    }

    bool g_Gate = false;

    Coroutine WaitForGate()
    {
        g_Log = 70;
        co_yield WaitUntil {[]() { return g_Gate; }};
        g_Log = 71;
    }

    Coroutine WaitWhileGate()
    {
        g_Log = 80;
        co_yield WaitWhile {[]() { return g_Gate; }};
        g_Log = 81;
    }

    Coroutine WaitForNested(CoroutineScheduler& scheduler, uint64_t nested_id)
    {
        g_Log = 90;
        co_yield WaitForCoroutine {nested_id};
        g_Log = 91;
    }

    /// No yield at all: runs straight through inside Start().
    Coroutine RunsToCompletion()
    {
        g_Log = 100;
        co_return;
    }

    /// Starts another coroutine from inside its own body -- the re-entrancy
    /// case that would invalidate any iterator held across a resume.
    Coroutine StartsAnother(CoroutineScheduler& scheduler)
    {
        g_Log = 110;
        co_yield WaitForNextFrame {};
        scheduler.Start(CountTwoFrames(), 999);
        g_Log = 111;
    }

    void TestCoroutines()
    {
        CoroutineScheduler scheduler;
        Check(scheduler.Initialize(), "C0", "CoroutineScheduler initializes");

        // -- first segment is synchronous (Unity semantics) -----------------
        scheduler.StopAll();   // isolate: a leftover coroutine writing g_Log looks like this case failing
        g_Log = 0;
        const uint64_t id1 = scheduler.Start(CountTwoFrames(), 1);
        Check(id1 != CoroutineScheduler::kInvalidId, "C1", "Start() returns a usable id");
        Check(g_Log == 1, "C2", "the first segment runs inside Start(), before any tick");
        Check(scheduler.GetRunningCount() == 1, "C3", "and the coroutine is parked on a yield");

        scheduler.TickUpdate(0.016f, 0.016f);
        Check(g_Log == 2, "C4", "yield null resumes on the next Update");
        Check(scheduler.GetRunningCount() == 0, "C5", "and a finished coroutine is dropped");
        Check(!scheduler.IsRunning(id1), "C6", "IsRunning() is false afterwards");

        // -- multi-step -----------------------------------------------------
        scheduler.StopAll();   // isolate: a leftover coroutine writing g_Log looks like this case failing
        g_Log = 0;
        scheduler.Start(CountThree(), 1);
        Check(g_Log == 10, "C7", "first segment of a two-yield body ran");
        scheduler.TickUpdate(0.016f, 0.016f);
        Check(g_Log == 11, "C8", "second segment");
        scheduler.TickUpdate(0.016f, 0.016f);
        Check(g_Log == 12, "C9", "third segment");
        Check(scheduler.GetRunningCount() == 0, "C10", "then it is gone");

        // -- WaitForSeconds is scaled, and lands on the exact frame ----------
        scheduler.StopAll();   // isolate: a leftover coroutine writing g_Log looks like this case failing
        g_Log = 0;
        scheduler.Start(WaitHalfSecond(), 1);
        for (int i = 0; i < 4; ++i)
        {
            scheduler.TickUpdate(0.1f, 0.1f);
        }
        Check(g_Log == 20, "C11", "0.5s is not done after 4 x 0.1s");
        scheduler.TickUpdate(0.1f, 0.1f);
        Check(g_Log == 21, "C12", "and is done on the 5th -- not one frame late");

        // A timeScale of 0 freezes it: scaled dt is 0, realtime dt is not.
        scheduler.StopAll();   // isolate: a leftover coroutine writing g_Log looks like this case failing
        g_Log = 0;
        scheduler.Start(WaitHalfSecond(), 1);
        for (int i = 0; i < 50; ++i)
        {
            scheduler.TickUpdate(0.0f, 0.1f);
        }
        Check(g_Log == 20, "C13", "WaitForSeconds honours timeScale = 0");

        // -- WaitForSecondsRealtime ignores timeScale -----------------------
        scheduler.StopAll();   // isolate: a leftover coroutine writing g_Log looks like this case failing
        g_Log = 0;
        scheduler.Start(WaitRealtime(), 1);
        for (int i = 0; i < 2; ++i)
        {
            scheduler.TickUpdate(0.0f, 0.1f);
        }
        Check(g_Log == 30, "C14", "0.3s realtime is not done after 2 x 0.1s");
        scheduler.TickUpdate(0.0f, 0.1f);
        Check(g_Log == 31, "C15", "and is done on the 3rd even at timeScale = 0");

        // -- phase separation: the whole reason the phase is stored ---------
        scheduler.StopAll();   // isolate: a leftover coroutine writing g_Log looks like this case failing
        g_Log = 0;
        scheduler.Start(WaitFixed(), 1);
        scheduler.TickUpdate(0.016f, 0.016f);
        Check(g_Log == 40, "C16", "WaitForFixedUpdate does NOT resume in the Update phase");
        scheduler.TickLateUpdate();
        Check(g_Log == 40, "C17", "nor in LateUpdate");
        scheduler.TickEndOfFrame();
        Check(g_Log == 40, "C18", "nor at EndOfFrame");
        scheduler.TickFixedUpdate();
        Check(g_Log == 41, "C19", "only in the FixedUpdate phase");

        scheduler.StopAll();   // isolate: a leftover coroutine writing g_Log looks like this case failing
        g_Log = 0;
        scheduler.Start(WaitLate(), 1);
        scheduler.TickUpdate(0.016f, 0.016f);
        Check(g_Log == 50, "C20", "WaitForLateUpdate does not resume in Update");
        scheduler.TickLateUpdate();
        Check(g_Log == 51, "C21", "it resumes in LateUpdate");

        scheduler.StopAll();   // isolate: a leftover coroutine writing g_Log looks like this case failing
        g_Log = 0;
        scheduler.Start(WaitEndOfFrame(), 1);
        scheduler.TickUpdate(0.016f, 0.016f);
        scheduler.TickLateUpdate();
        Check(g_Log == 60, "C22", "WaitForEndOfFrame does not resume in Update/LateUpdate");
        scheduler.TickEndOfFrame();
        Check(g_Log == 61, "C23", "it resumes at EndOfFrame");

        // -- predicates ------------------------------------------------------
        g_Gate = false;
        scheduler.StopAll();   // isolate: a leftover coroutine writing g_Log looks like this case failing
        g_Log = 0;
        scheduler.Start(WaitForGate(), 1);
        scheduler.TickUpdate(0.016f, 0.016f);
        Check(g_Log == 70, "C24", "WaitUntil stays parked while the predicate is false");
        g_Gate = true;
        scheduler.TickUpdate(0.016f, 0.016f);
        Check(g_Log == 71, "C25", "and resumes the first frame it is true");

        g_Gate = true;
        scheduler.StopAll();   // isolate: a leftover coroutine writing g_Log looks like this case failing
        g_Log = 0;
        scheduler.Start(WaitWhileGate(), 1);
        scheduler.TickUpdate(0.016f, 0.016f);
        Check(g_Log == 80, "C26", "WaitWhile stays parked while the predicate is true");
        g_Gate = false;
        scheduler.TickUpdate(0.016f, 0.016f);
        Check(g_Log == 81, "C27", "and resumes the first frame it is false");

        // -- nested ----------------------------------------------------------
        scheduler.StopAll();   // isolate: a leftover coroutine writing g_Log looks like this case failing
        g_Log = 0;
        const uint64_t inner = scheduler.Start(WaitHalfSecond(), 1);
        Check(inner != CoroutineScheduler::kInvalidId, "C28", "the inner coroutine is running");
        scheduler.Start(WaitForNested(scheduler, inner), 1);
        scheduler.TickUpdate(0.1f, 0.1f);
        Check(g_Log == 90, "C29", "the waiter is parked while the inner one runs");
        for (int i = 0; i < 4; ++i)
        {
            scheduler.TickUpdate(0.1f, 0.1f);
        }
        Check(g_Log == 21, "C30", "the inner coroutine finished on its 5th tick");
        // The waiter needs one MORE Update: readiness is evaluated at the top of
        // a tick, so it cannot notice the inner one disappearing in the same
        // tick that removed it.
        scheduler.TickUpdate(0.1f, 0.1f);
        Check(g_Log == 91, "C31", "and the waiter resumes on the next Update");
        Check(scheduler.GetRunningCount() == 0, "C32", "and both are cleaned up");

        // -- no yield at all -------------------------------------------------
        scheduler.StopAll();   // isolate: a leftover coroutine writing g_Log looks like this case failing
        g_Log = 0;
        const uint64_t instant = scheduler.Start(RunsToCompletion(), 1);
        Check(g_Log == 100, "C33", "a body with no yield runs to completion in Start()");
        Check(instant == CoroutineScheduler::kInvalidId,
              "C34", "and gets no id -- an id for a finished coroutine would be waited on forever");
        Check(scheduler.GetRunningCount() == 0, "C35", "nothing is scheduled");

        // -- Stop / StopAll / StopAllForOwner --------------------------------
        scheduler.StopAll();   // isolate: a leftover coroutine writing g_Log looks like this case failing
        g_Log = 0;
        const uint64_t stop_id = scheduler.Start(WaitHalfSecond(), 1);
        scheduler.Stop(stop_id);
        Check(!scheduler.IsRunning(stop_id), "C36", "Stop() removes it");
        scheduler.TickUpdate(0.5f, 0.5f);
        Check(g_Log == 20, "C37", "and a stopped coroutine never runs again");

        scheduler.Start(WaitHalfSecond(), 7);
        scheduler.Start(WaitHalfSecond(), 7);
        scheduler.Start(WaitHalfSecond(), 8);
        Check(scheduler.GetRunningCount() == 3, "C38", "three coroutines across two owners");
        scheduler.StopAllForOwner(7);
        Check(scheduler.GetRunningCount() == 1, "C39", "StopAllForOwner() keeps the other owner's");
        scheduler.StopAll();
        Check(scheduler.GetRunningCount() == 0, "C40", "StopAll() clears everything");

        // -- re-entrancy -----------------------------------------------------
        scheduler.StopAll();   // isolate: a leftover coroutine writing g_Log looks like this case failing
        g_Log = 0;
        scheduler.Start(StartsAnother(scheduler), 1);
        scheduler.TickUpdate(0.016f, 0.016f);
        Check(g_Log == 111, "C41", "a body can start another coroutine from inside itself");
        Check(scheduler.GetRunningCount() == 1, "C42", "the new one is scheduled, not lost");
        // The newly started one must NOT run in the same phase it was started in,
        // otherwise a body that spawns a waiter each resume would spin forever.
        scheduler.TickUpdate(0.016f, 0.016f);
        Check(scheduler.GetRunningCount() == 0, "C43", "and it runs in a later phase");
    }

    // ---------------------------------------------------------------- D ----
    void TestCoroutineWiring()
    {
        const std::string app = ReadFile("Source/Runtime/Application/Application.cpp");
        Check(!app.empty(), "D1", "Application.cpp is readable");
        // All four phases, or a whole class of wait silently never resumes.
        Check(CountOccurrences(app, "TickFixedUpdate") >= 1,
              "D2", "the frame loop drives the FixedUpdate coroutine phase");
        Check(CountOccurrences(app, "TickUpdate") >= 1,
              "D3", "the frame loop drives the Update coroutine phase");
        Check(CountOccurrences(app, "TickLateUpdate") >= 1,
              "D4", "the frame loop drives the LateUpdate coroutine phase");
        Check(CountOccurrences(app, "TickEndOfFrame") >= 1,
              "D5", "the frame loop drives the EndOfFrame coroutine phase");
        Check(CountOccurrences(app, "GetUnscaledDeltaTime") >= 1,
              "D6", "Update gets the unscaled delta too (WaitForSecondsRealtime needs it)");

        const std::string reg = ReadFile("Source/Runtime/RegisterRuntime.cpp");
        Check(CountOccurrences(reg, "REGISTER_SYSTEM(CoroutineScheduler)") >= 1,
              "D7", "CoroutineScheduler is registered as a system");
    }

    // ---------------------------------------------------------------- E ----
    // Random. The determinism and the int/float Range asymmetry are the two
    // things a reimplementation gets wrong, so they get the most checks.
    void TestRandom()
    {
        // -- determinism ------------------------------------------------------
        float first_run[8];
        float second_run[8];
        Random::InitState(12345);
        for (float& v : first_run)
        {
            v = Random::value();
        }
        Random::InitState(12345);
        for (float& v : second_run)
        {
            v = Random::value();
        }
        bool identical = true;
        for (int i = 0; i < 8; ++i)
        {
            if (first_run[i] != second_run[i])
            {
                identical = false;
            }
        }
        Check(identical, "E1", "the same seed reproduces the same sequence exactly");

        Random::InitState(12346);
        const float other_seed = Random::value();
        Check(other_seed != first_run[0], "E2", "a different seed produces a different sequence");

        // Adjacent seeds must not produce correlated openings -- the classic
        // "seed from a frame counter" trap.
        Random::InitState(0);
        const float seed0 = Random::value();
        Random::InitState(1);
        const float seed1 = Random::value();
        Check(std::fabs(seed0 - seed1) > 0.01f, "E3", "adjacent seeds are not correlated");

        // -- value() range ----------------------------------------------------
        Random::InitState(7);
        bool in_range = true;
        for (int i = 0; i < 10000; ++i)
        {
            const float v = Random::value();
            if (v < 0.0f || v >= 1.0f)
            {
                in_range = false;
            }
        }
        Check(in_range, "E4", "value() stays in [0, 1) over 10000 draws");

        // -- the Unity asymmetry: int excludes max, float includes it ---------
        Random::InitState(99);
        int int_min = 1000;
        int int_max = -1000;
        for (int i = 0; i < 20000; ++i)
        {
            const int v = Random::Range(0, 10);
            int_min = (v < int_min) ? v : int_min;
            int_max = (v > int_max) ? v : int_max;
        }
        Check(int_min == 0, "E5", "Range(0, 10) as ints reaches 0");
        Check(int_max == 9, "E6",
              "Range(0, 10) as ints NEVER reaches 10 -- Unity excludes the int max");
        Check(Random::Range(5, 5) == 5, "E7", "an empty int range returns min");
        Check(Random::Range(7, 2) == 7, "E8", "an inverted int range returns min");

        Random::InitState(100);
        bool float_in_range = true;
        float float_max = -1000.0f;
        for (int i = 0; i < 20000; ++i)
        {
            const float v = Random::Range(2.0f, 3.0f);
            float_max = (v > float_max) ? v : float_max;
            if (v < 2.0f || v > 3.0f)
            {
                float_in_range = false;
            }
        }
        Check(float_in_range, "E9", "Range(2.0f, 3.0f) stays inside both bounds");
        Check(float_max > 2.99f, "E10",
              "Range(2.0f, 3.0f) can get arbitrarily close to 3 -- Unity INCLUDES the float max");
        Check(std::fabs(Random::Range(4.0f, 2.0f) - 3.0f) < 10.0f, "E11",
              "an inverted float range is swapped, not nonsense");

        // -- save / restore ---------------------------------------------------
        Random::InitState(2024);
        for (int i = 0; i < 5; ++i)
        {
            Random::value();
        }
        const Random::State saved = Random::GetState();
        float after_save[4];
        for (float& v : after_save)
        {
            v = Random::value();
        }
        Random::SetState(saved);
        bool restored = true;
        for (float v : after_save)
        {
            if (Random::value() != v)
            {
                restored = false;
            }
        }
        Check(restored, "E12", "SetState() resumes the stream exactly (one uint64 of state)");

        // -- shapes -----------------------------------------------------------
        Random::InitState(555);
        bool sphere_inside = true;
        for (int i = 0; i < 2000; ++i)
        {
            if (Random::insideUnitSphere().squaredLength() > 1.0f)
            {
                sphere_inside = false;
            }
        }
        Check(sphere_inside, "E13", "insideUnitSphere() never leaves the unit ball");

        bool circle_inside = true;
        for (int i = 0; i < 2000; ++i)
        {
            if (Random::insideUnitCircle().squaredLength() > 1.0f)
            {
                circle_inside = false;
            }
        }
        Check(circle_inside, "E14", "insideUnitCircle() never leaves the unit disc");

        bool on_sphere = true;
        for (int i = 0; i < 2000; ++i)
        {
            const float length = Random::onUnitSphere().length();
            if (std::fabs(length - 1.0f) > 0.01f)
            {
                on_sphere = false;
            }
        }
        Check(on_sphere, "E15", "onUnitSphere() is on the surface (length 1)");

        bool on_circle = true;
        for (int i = 0; i < 2000; ++i)
        {
            const float length = Random::onUnitCircle().length();
            if (std::fabs(length - 1.0f) > 0.01f)
            {
                on_circle = false;
            }
        }
        Check(on_circle, "E16", "onUnitCircle() is on the circle (length 1)");

        bool unit_rotation = true;
        for (int i = 0; i < 2000; ++i)
        {
            const Quaternion q = Random::rotation();
            const float sq = q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z;
            if (std::fabs(sq - 1.0f) > 0.01f)
            {
                unit_rotation = false;
            }
        }
        Check(unit_rotation, "E17", "rotation() returns a unit quaternion");

        // -- Chance -----------------------------------------------------------
        Random::InitState(3);
        Check(!Random::Chance(0.0f), "E18", "Chance(0) is never true");
        Check(Random::Chance(1.0f), "E19", "Chance(1) is always true");
        Check(!Random::Chance(-0.5f), "E20", "a negative probability is never true");
        Check(Random::Chance(2.0f), "E21", "a probability above 1 is always true");
        int trues = 0;
        for (int i = 0; i < 20000; ++i)
        {
            if (Random::Chance(0.25f))
            {
                ++trues;
            }
        }
        Check(trues > 4400 && trues < 5600, "E22",
              "Chance(0.25) fires about a quarter of the time (measured)");

        // -- no modulo bias on the low end ------------------------------------
        // Range(0, 3) over a big sample: a `% span` implementation skews the
        // first buckets whenever span does not divide 2^32.
        Random::InitState(4242);
        int buckets[3] = {0, 0, 0};
        for (int i = 0; i < 30000; ++i)
        {
            ++buckets[Random::Range(0, 3)];
        }
        const int lowest = (buckets[0] < buckets[1]) ? buckets[0] : buckets[1];
        const int min_bucket = (lowest < buckets[2]) ? lowest : buckets[2];
        const int highest = (buckets[0] > buckets[1]) ? buckets[0] : buckets[1];
        const int max_bucket = (highest > buckets[2]) ? highest : buckets[2];
        Check(min_bucket > 9000 && max_bucket < 11000, "E23",
              "Range(0, 3) is flat across buckets (no modulo bias)");
    }

    // ---------------------------------------------------------------- F ----
    // Hierarchy queries + messaging.
    //
    // 这里只做"空对象"这一档运行时检查 + 源码接线门。深度层级的运行时行为
    // （多级 Find("A/B")、InChildren 的先序顺序）需要构造 GameObject + Transform
    // 树，那要 MemoryManager / TransformHierarchy 一整套起来，留作后续 —— 本组
    // 先把"这些方法存在且真的被实现调用"钉住，防止空实现蒙混。
    void TestHierarchyQueries()
    {
        GameObject solo(1);
        solo.SetName("Solo");

        Check(solo.SendMessageByName("") == 0, "F1", "SendMessage with an empty name delivers nothing");
        Check(solo.SendMessageByName("OnDamaged") == 0,
              "F2", "SendMessage on an object with no script component returns 0");
        Check(solo.BroadcastMessageByName("OnDamaged") == 0, "F3", "BroadcastMessage likewise");
        Check(solo.SendMessageUpwardsByName("OnDamaged") == 0, "F4", "SendMessageUpwards likewise");
        Check(solo.GetComponentInChildrenByTypeName("Transform") == nullptr,
              "F5", "GetComponentInChildren on a component-less object is nullptr");
        Check(solo.GetComponentsInChildrenByTypeName("Transform").empty(),
              "F6", "GetComponentsInChildren on it is empty");
        Check(solo.GetComponentInParentByTypeName("Transform") == nullptr,
              "F7", "GetComponentInParent on it is nullptr");

        // A GameObject with a tag/layer must survive a default construction:
        // that is the state old assets deserialize into.
        Check(solo.GetTag() == TagLayerManager::kUntagged, "F8", "a fresh object is Untagged");
        Check(solo.GetLayer() == 0, "F9", "and on layer 0 (Default)");
        Check(!solo.CompareTagName("Untagged") == false, "F10", "CompareTag(\"Untagged\") is true");
        solo.SetLayerIndex(99);
        Check(solo.GetLayer() == 0, "F11", "an out-of-range layer is clamped to 0, not stored");
        solo.SetLayerIndex(-5);
        Check(solo.GetLayer() == 0, "F12", "a negative layer is clamped to 0");
        solo.SetLayerIndex(9);
        Check(solo.GetLayer() == 9, "F13", "a valid layer sticks");
        Check(!solo.SetTagByName("ThisTagDoesNotExist"),
              "F14", "setting an unregistered tag is refused, not silently accepted");
        Check(solo.GetTag() == TagLayerManager::kUntagged, "F15", "and the old tag is kept");
    }

    void TestHierarchyWiring()
    {
        const std::string go_header = ReadFile("Source/Runtime/BaseClasses/GameObject.h");
        Check(CountOccurrences(go_header, "GetComponentInChildren") >= 1,
              "G1", "GetComponentInChildren is declared");
        Check(CountOccurrences(go_header, "GetComponentsInChildren") >= 1,
              "G2", "GetComponentsInChildren is declared");
        Check(CountOccurrences(go_header, "GetComponentInParent") >= 1,
              "G3", "GetComponentInParent is declared");
        Check(CountOccurrences(go_header, "BroadcastMessage") >= 1, "G4", "BroadcastMessage is declared");
        Check(CountOccurrences(go_header, "SendMessageUpwards") >= 1, "G5", "SendMessageUpwards is declared");

        const std::string go_source = ReadFile("Source/Runtime/BaseClasses/GameObject.cpp");
        // >= 2: declared *and* called. A declaration with no call site is exactly
        // the "implemented but never reached" failure this gate exists for.
        Check(CountOccurrences(go_source, "CollectComponentsInChildren") >= 2,
              "G6", "the child search is both declared and used");
        Check(CountOccurrences(go_source, "DeliverMessageToSelfComponents") >= 3,
              "G7", "all three message entry points go through one delivery helper");
        Check(CountOccurrences(go_source, "TypeScriptComponent") >= 1,
              "G8", "messaging reaches script components");

        const std::string ts_header =
            ReadFile("Source/Runtime/Function/Framework/Component/Script/TypeScriptComponent.h");
        Check(CountOccurrences(ts_header, "InvokeMethod") >= 1,
              "G9", "TypeScriptComponent can invoke a method by name");

        const std::string tr_header =
            ReadFile("Source/Runtime/Function/Framework/Component/Transform/Transform.h");
        Check(CountOccurrences(tr_header, "Find(const eastl::string& path)") >= 1,
              "G10", "Transform.Find(path) is declared");

        const std::string tr_source =
            ReadFile("Source/Runtime/Function/Framework/Component/Transform/Transform.cpp");
        Check(CountOccurrences(tr_source, "Transform::Find") >= 1, "G11", "Transform.Find is implemented");
        Check(CountOccurrences(tr_source, "path.find('/'") >= 1,
              "G12", "and splits on '/' (a plain name match would break 'A/B')");
    }

    // ---------------------------------------------------------------- H ----
    // Physics queries + the layer collision matrix.
    //
    // These run against a real PhysicsScene: the whole point of the slice is
    // that the matrix is CONSUMED, and a source-level gate cannot tell the
    // difference between "the filter reads the matrix" and "the filter reads
    // the matrix and Jolt actually calls it".

    /// A box straight through Jolt, tagged with an explicit Unity layer. Going
    /// through RigidBodyComponentRes would drag the object + reflection layer
    /// in; the layer plumbing that path adds is covered by group I.
    uint32_t SpawnBox(PhysicsScene& in_scene, const Vector3& in_position, int32_t in_layer, bool in_dynamic)
    {
        JPH::BodyInterface& body_interface = in_scene.GetJoltPhysicsSystem()->GetBodyInterface();

        JPH::BoxShapeSettings box_settings(JPH::Vec3(0.5f, 0.5f, 0.5f));
        // RefTarget starts at refcount 0 and deletes at 0; a stack object
        // without SetEmbedded() is "delete a stack pointer". See the note in
        // PhysicsCharacterJointSmokeTest.
        box_settings.SetEmbedded();

        JPH::BodyCreationSettings body_settings(&box_settings,
                                                JPH::RVec3(toVec3(in_position)),
                                                JPH::Quat::sIdentity(),
                                                in_dynamic ? JPH::EMotionType::Dynamic : JPH::EMotionType::Static,
                                                static_cast<JPH::ObjectLayer>(in_layer));

        JPH::Body* jph_body = body_interface.CreateBody(body_settings);
        if (jph_body == nullptr)
        {
            return kInvalidPhysicsBodyId;
        }

        body_interface.AddBody(jph_body->GetID(), JPH::EActivation::Activate);
        return jph_body->GetID().GetIndexAndSequenceNumber();
    }

    void StepScene(PhysicsScene& in_scene, int in_steps)
    {
        for (int i = 0; i < in_steps; ++i)
        {
            in_scene.Tick(1.0f / 60.0f);
        }
    }

    void TestPhysicsQueries()
    {
        PhysicsManager* physics_manager = GET_SYSTEM(PhysicsManager);
        Check(physics_manager != nullptr, "H0", "PhysicsManager is reachable");
        if (physics_manager == nullptr)
        {
            return;
        }

        std::shared_ptr<PhysicsScene> scene = physics_manager->CreatePhysicsScene(Vector3(0.f, -9.81f, 0.f)).lock();
        Check(scene != nullptr, "H0b", "a physics scene can be created");
        if (!scene)
        {
            return;
        }

        // Two static boxes on two different layers, one nearer the ray origin.
        const uint32_t box_default = SpawnBox(*scene, Vector3(0.f, 0.f, 0.f), 0, false);
        const uint32_t box_ground = SpawnBox(*scene, Vector3(0.f, 0.f, 3.f), 8, false);
        Check(box_default != kInvalidPhysicsBodyId && box_ground != kInvalidPhysicsBodyId,
              "H0c", "both boxes were created");

        // --- layering of the bodies themselves ------------------------------
        int32_t read_layer = -1;
        Check(scene->GetBodyLayer(box_default, read_layer) && read_layer == 0,
              "H1", "a body reports the Unity layer it was created on");
        Check(scene->GetBodyLayer(box_ground, read_layer) && read_layer == 8, "H2", "and so does the layer-8 box");
        Check(!scene->GetBodyLayer(kInvalidPhysicsBodyId, read_layer), "H3", "an invalid body id reports no layer");
        Check(!scene->SetBodyLayer(box_default, 99), "H4", "an out-of-range layer is refused");

        // --- ray -------------------------------------------------------------
        const Vector3 kRayOrigin(0.f, 0.f, 10.f);
        const Vector3 kRayDirection(0.f, 0.f, -1.f);

        const RaycastHit nearest = Physics::Raycast(kRayOrigin, kRayDirection, 100.f, -1);
        Check(nearest.valid, "H5", "Raycast hits something");
        Check(nearest.layer == 8, "H6", "and the nearest hit is the nearer box");
        Check(nearest.bodyId == box_ground, "H7", "the hit carries the body id");
        Check(nearest.distance > 6.f && nearest.distance < 7.f, "H8", "at the expected distance");

        const RaycastHit only_default = Physics::Raycast(kRayOrigin, kRayDirection, 100.f, 1 << 0);
        Check(only_default.valid && only_default.layer == 0 && only_default.bodyId == box_default,
              "H9", "a layerMask of just layer 0 skips the layer-8 box");

        const RaycastHit only_ground = Physics::Raycast(kRayOrigin, kRayDirection, 100.f, 1 << 8);
        Check(only_ground.valid && only_ground.bodyId == box_ground, "H10", "and vice versa");

        Check(!Physics::Raycast(kRayOrigin, kRayDirection, 100.f, 1 << 9).valid,
              "H11", "a mask naming an empty layer hits nothing");
        // Unity: layerMask 0 means "nothing", not "everything".
        Check(!Physics::Raycast(kRayOrigin, kRayDirection, 100.f, 0).valid, "H12", "a mask of 0 hits nothing");
        Check(!Physics::Raycast(kRayOrigin, kRayDirection, 2.f, -1).valid,
              "H13", "maxDistance shorter than the gap hits nothing");

        const std::vector<RaycastHit> all = Physics::RaycastAll(kRayOrigin, kRayDirection, 100.f, -1);
        Check(all.size() == 2, "H14", "RaycastAll returns every hit");
        Check(all.size() == 2 && all[0].distance < all[1].distance, "H15", "sorted nearest first");
        Check(all.size() == 2 && all[0].bodyId == box_ground && all[1].bodyId == box_default,
              "H16", "in the order the ray meets them");
        Check(Physics::RaycastAll(kRayOrigin, kRayDirection, 100.f, 1 << 0).size() == 1,
              "H17", "RaycastAll honours the mask");

        Check(Physics::Linecast(Vector3(0.f, 0.f, 10.f), Vector3(0.f, 0.f, -10.f), -1),
              "H18", "Linecast between two points through a box is true");
        Check(!Physics::Linecast(Vector3(0.f, 5.f, 10.f), Vector3(0.f, 5.f, -10.f), -1),
              "H19", "and false on a parallel line that misses it");

        // --- sweeps ----------------------------------------------------------
        const RaycastHit sphere_hit = Physics::SphereCast(kRayOrigin, 0.5f, kRayDirection, 100.f, -1);
        Check(sphere_hit.valid && sphere_hit.bodyId == box_ground, "H20", "SphereCast hits the nearest box");
        Check(!Physics::SphereCast(kRayOrigin, 0.5f, kRayDirection, 100.f, 1 << 0).valid ||
                  Physics::SphereCast(kRayOrigin, 0.5f, kRayDirection, 100.f, 1 << 0).bodyId == box_default,
              "H21", "SphereCast honours the mask");
        Check(Physics::SphereCastAll(kRayOrigin, 0.5f, kRayDirection, 100.f, -1).size() == 2,
              "H22", "SphereCastAll returns both");
        Check(Physics::BoxCast(Vector3(0.f, 0.f, 10.f), Vector3(0.5f, 0.5f, 0.5f), kRayDirection, 100.f, -1).valid,
              "H23", "BoxCast hits");
        Check(Physics::CapsuleCast(Vector3(0.f, -0.5f, 10.f), Vector3(0.f, 0.5f, 10.f), 0.3f, kRayDirection, 100.f, -1)
                  .valid,
              "H24", "CapsuleCast hits");

        // --- overlap ---------------------------------------------------------
        Check(Physics::OverlapSphere(Vector3(0.f, 0.f, 0.f), 1.f, -1).size() == 1,
              "H25", "OverlapSphere finds the box it is centred on");
        Check(Physics::OverlapSphere(Vector3(0.f, 0.f, 1.5f), 5.f, -1).size() == 2,
              "H26", "and both when it is big enough");
        Check(Physics::OverlapSphere(Vector3(0.f, 0.f, 0.f), 1.f, 1 << 8).empty(),
              "H27", "OverlapSphere honours the mask");
        Check(Physics::OverlapSphere(Vector3(50.f, 50.f, 50.f), 1.f, -1).empty(), "H28", "and empty space is empty");

        Check(Physics::CheckSphere(Vector3(0.f, 0.f, 0.f), 1.f, -1), "H29", "CheckSphere is true on a box");
        Check(!Physics::CheckSphere(Vector3(50.f, 50.f, 50.f), 1.f, -1), "H30", "and false in empty space");
        Check(Physics::CheckBox(Vector3(0.f, 0.f, 0.f), Vector3(0.5f, 0.5f, 0.5f), -1), "H31", "CheckBox is true");
        Check(Physics::CheckCapsule(Vector3(0.f, -0.5f, 0.f), Vector3(0.f, 0.5f, 0.f), 0.3f, -1),
              "H32", "CheckCapsule is true");
        Check(Physics::OverlapBox(Vector3(0.f, 0.f, 0.f), Vector3(0.5f, 0.5f, 0.5f), -1).size() >= 1,
              "H33", "OverlapBox finds it");
        Check(Physics::OverlapCapsule(Vector3(0.f, -0.5f, 0.f), Vector3(0.f, 0.5f, 0.f), 0.3f, -1).size() >= 1,
              "H34", "OverlapCapsule finds it");

        // --- layer names / masks through the facade --------------------------
        Check(Physics::LayerToName(0) == "Default", "H35", "Physics.LayerToName(0) is Default");
        Check(Physics::NameToLayer("UI") == 5, "H36", "Physics.NameToLayer(\"UI\") is 5");
        Check(Physics::NameToLayer("NoSuchLayer") == -1, "H37", "an unknown name is -1");

        // --- the matrix actually gates the simulation ------------------------
        //
        // This is the check the whole slice exists for. Two bodies, one resting
        // on the other; with the pair excluded they must pass straight through.
        const uint32_t obstacle = SpawnBox(*scene, Vector3(20.f, 0.f, 0.f), 8, false);
        const uint32_t faller = SpawnBox(*scene, Vector3(20.f, 3.f, 0.f), 9, true);
        (void)obstacle;

        auto reset_faller = [&]() {
            JPH::BodyInterface& body_interface = scene->GetJoltPhysicsSystem()->GetBodyInterface();
            body_interface.SetPosition(JPH::BodyID(faller), JPH::RVec3(20.f, 3.f, 0.f),
                                       JPH::EActivation::Activate);
            body_interface.SetLinearVelocity(JPH::BodyID(faller), JPH::Vec3::sZero());
        };

        Physics::IgnoreLayerCollision(8, 9, true);
        Check(Physics::GetIgnoreLayerCollision(8, 9), "H38", "IgnoreLayerCollision(8, 9) takes effect");
        Check(Physics::GetIgnoreLayerCollision(9, 8), "H39", "and is symmetric");
        Check((static_cast<uint32_t>(Physics::GetCollisionLayerMask(8)) & (1u << 9)) == 0u,
              "H40", "GetCollisionLayerMask(8) drops bit 9");

        reset_faller();
        StepScene(*scene, 90);
        Vector3 fallen;
        scene->GetRigidBodyPosition(faller, fallen);
        Check(fallen.y < 0.f, "H41", "with the pair excluded the body falls straight through");

        Physics::IgnoreLayerCollision(8, 9, false);
        Check(!Physics::GetIgnoreLayerCollision(8, 9), "H42", "the exclusion can be lifted");

        reset_faller();
        StepScene(*scene, 90);
        Vector3 rested;
        scene->GetRigidBodyPosition(faller, rested);
        Check(rested.y > 0.9f, "H43", "and once it collides again the body rests on top");

        // --- gravity ---------------------------------------------------------
        Check(std::fabs(Physics::GetGravity().y + 9.81f) < 0.001f, "H44", "Physics.GetGravity reads the scene");
        Physics::SetGravity(Vector3(0.f, -20.f, 0.f));
        Check(std::fabs(Physics::GetGravity().y + 20.f) < 0.001f, "H45", "Physics.SetGravity writes it");
        Physics::SetGravity(Vector3(0.f, -9.81f, 0.f));

        // A body retagged at runtime changes which queries see it.
        Check(scene->SetBodyLayer(box_default, 8), "H46", "a body can be retagged");
        Check(scene->GetBodyLayer(box_default, read_layer) && read_layer == 8, "H47", "and reads back the new layer");
        Check(Physics::Raycast(kRayOrigin, kRayDirection, 100.f, 1 << 0).valid == false,
              "H48", "so a mask of layer 0 no longer sees it");
        Check(Physics::RaycastAll(kRayOrigin, kRayDirection, 100.f, 1 << 8).size() == 2,
              "H49", "and a mask of layer 8 now sees both");
    }

    void TestPhysicsWiring()
    {
        const std::string scene_header = ReadFile("Source/Runtime/Function/Physics/PhysicsScene.h");
        Check(!scene_header.empty(), "I0", "PhysicsScene.h is readable");
        Check(CountOccurrences(scene_header, "layer_mask") >= 5,
              "I1", "every query on PhysicsScene takes a layer mask");
        Check(CountOccurrences(scene_header, "SetBodyLayer") >= 1, "I2", "bodies can be retagged");
        Check(CountOccurrences(scene_header, "FilterHitsByLayer") >= 1, "I3", "there is one shared mask filter");

        const std::string scene_source = ReadFile("Source/Runtime/Function/Physics/PhysicsScene.cpp");
        Check(!scene_source.empty(), "I4", "PhysicsScene.cpp is readable");
        // >= 4: declared once, called by raycast / sweep / overlap-all / the two
        // primitive queries. "Declared but never called" is the exact failure
        // the group-B gate was written for and it compiles silently.
        Check(CountOccurrences(scene_source, "FilterHitsByLayer") >= 5,
              "I5", "the mask filter is actually called by every query");
        Check(CountOccurrences(scene_source, "kPhysicsLayerCount") >= 1,
              "I6", "CreateRigidBody range-checks the Unity layer");

        // The matrix has to be read where collisions are decided, not only where
        // queries are filtered -- otherwise IgnoreLayerCollision moves no bodies.
        const std::string utils_source = ReadFile("Source/Runtime/Function/Physics/Jolt/Utils.cpp");
        Check(!utils_source.empty(), "I7", "Jolt/Utils.cpp is readable");
        Check(CountOccurrences(utils_source, "TagLayerManager") >= 1,
              "I8", "the Jolt object-layer filter includes the tag/layer manager");
        Check(CountOccurrences(utils_source, "GetIgnoreLayerCollision") >= 1,
              "I9", "and resolves pairs through the collision matrix");

        const std::string rb_source =
            ReadFile("Source/Runtime/Function/Framework/Component/Rigidbody/RigidbodyComponent.cpp");
        Check(!rb_source.empty(), "I10", "RigidbodyComponent.cpp is readable");
        Check(CountOccurrences(rb_source, "CurrentGameObjectLayer") >= 3,
              "I11", "every body creation path passes the GameObject's layer");
        Check(CountOccurrences(rb_source, "RefreshBodyLayer") >= 1, "I12", "and there is a retag path");

        const std::string go_source = ReadFile("Source/Runtime/BaseClasses/GameObject.cpp");
        Check(CountOccurrences(go_source, "RefreshBodyLayer") >= 1,
              "I13", "GameObject.SetLayer pushes the new layer into the body");

        const std::string binding_header = ReadFile("Source/Runtime/Scripting/Binding/ScriptBinding.h");
        Check(!binding_header.empty(), "I14", "ScriptBinding.h is readable");
        Check(CountOccurrences(binding_header, "Converter<RaycastHit>") >= 1,
              "I15", "RaycastHit has a script converter");
        Check(CountOccurrences(binding_header, "PhysicsQuery.h") >= 1,
              "I16", "and it comes from the Jolt-free header, not PhysicsScene.h");

        const std::string binding_source = ReadFile("Source/Runtime/Scripting/Binding/ScriptBinding.cpp");
        Check(CountOccurrences(binding_source, "declare interface RaycastHit") >= 1,
              "I17", "the .d.ts declares RaycastHit");
    }

    // ---------------------------------------------------------------- B ----
    void TestWiringGate()
    {
        const std::string go_header = ReadFile("Source/Runtime/BaseClasses/GameObject.h");
        Check(!go_header.empty(), "B1", "GameObject.h is readable");
        Check(CountOccurrences(go_header, "m_Tag") >= 2, "B2", "GameObject declares and uses m_Tag");
        Check(CountOccurrences(go_header, "m_Layer") >= 2, "B3", "GameObject declares and uses m_Layer");
        Check(CountOccurrences(go_header, "CompareTag") >= 1, "B4", "CompareTag is declared");
        Check(CountOccurrences(go_header, "FindGameObjectsWithTag") >= 1,
              "B5", "FindGameObjectsWithTag is declared");
        Check(CountOccurrences(go_header, "TagLayerManager.h") >= 1,
              "B6", "GameObject.h includes the tag/layer manager");

        const std::string go_source = ReadFile("Source/Runtime/BaseClasses/GameObject.cpp");
        Check(!go_source.empty(), "B7", "GameObject.cpp is readable");
        // >= 2: declared in the header *and* written in Transfer(). A field that
        // is only declared is never persisted, which looks exactly like "the
        // editor forgot to save" and costs an afternoon.
        Check(CountOccurrences(go_source, "m_Tag") >= 3, "B8", "m_Tag is transferred (not just declared)");
        Check(CountOccurrences(go_source, "m_Layer") >= 3, "B9", "m_Layer is transferred (not just declared)");
        Check(CountOccurrences(go_source, "TagLayerManager") >= 2,
              "B10", "GameObject.cpp consults the tag registry");
        Check(CountOccurrences(go_source, "IsActiveInHierarchy") >= 1,
              "B11", "tag search filters on activeInHierarchy like Unity");

        const std::string reg = ReadFile("Source/Runtime/RegisterRuntime.cpp");
        Check(!reg.empty(), "B12", "RegisterRuntime.cpp is readable");
        Check(CountOccurrences(reg, "TagLayerManager") >= 2,
              "B13", "TagLayerManager is included AND registered");
        Check(CountOccurrences(reg, "REGISTER_SYSTEM(TagLayerManager)") >= 1,
              "B14", "TagLayerManager is registered as a system");
    }
}  // namespace

int main()
{
    // ctest redirects stdout, which makes it fully buffered: a crash or a hang
    // would then swallow every line printed before it. Unbuffered keeps the log
    // honest.
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    std::printf("=== Unity parity smoke test ===\n\n");

    TagLayerManager manager;
    Check(manager.Initialize(), "A0", "TagLayerManager initializes");

    // Also publish it to the registry: GameObject::SetTagByName() looks the tag
    // table up through GET_SYSTEM, so without this the tag-validation checks
    // would exercise the "no manager" path instead of the real one -- and that
    // path is a deliberate fallback, not the behaviour under test.
    SystemRegistry::GetInstance().RegisterSystem(new TagLayerManager());
    Check(GET_SYSTEM(TagLayerManager) != nullptr, "A0b", "the tag registry is reachable via GET_SYSTEM");

    // Initialize the REGISTERED one, not just the local `manager`: the physics
    // group reaches the tag table through GET_SYSTEM, and an uninitialized
    // manager has no built-in layers -- so `Physics.LayerToName(0)` would come
    // back empty and look like a facade bug.
    Check(GET_SYSTEM(TagLayerManager)->Initialize(), "A0c", "the registered tag registry is initialized");

    // Physics needs a manager for `Physics.CurrentScene()` to find a scene, and
    // ObjectLayerPairFilterImpl needs the tag registry to resolve the collision
    // matrix. Neither is initialized: PhysicsManager::Initialize only sets up
    // the debug renderer, and TagLayerManager::Initialize just seeds built-ins.
    SystemRegistry::GetInstance().RegisterSystem(new PhysicsManager());

#if defined(_MSC_VER) && defined(_DEBUG)
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
    // Jolt's default AssertFailed breakpoints, which on a machine with no
    // debugger is an instant 0x80000003 with no output at all.
#if defined(JPH_ENABLE_ASSERTS)
    JPH::AssertFailed = [](const char* in_expression, const char* in_message, const char* in_file,
                           JPH::uint in_line) -> bool {
        std::fprintf(stderr, "JPH ASSERT FAILED: %s\n  at %s:%u\n  %s\n", in_expression, in_file, in_line,
                     in_message != nullptr ? in_message : "");
        ++g_Failures;
        return false;
    };
#endif

    TestTags(manager);
    TestLayers(manager);
    TestMasks();
    TestCollisionMatrix(manager);
    TestCoroutines();
    TestCoroutineWiring();
    TestRandom();
    TestHierarchyQueries();
    TestHierarchyWiring();
    TestPhysicsQueries();
    TestPhysicsWiring();
    TestWiringGate();

    std::printf("\n=== %d checks, %d failures ===\n", g_Checks, g_Failures);
    return (g_Failures == 0) ? 0 : 1;
}
