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

#include "Runtime/Application/Application.h"
#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/BaseClasses/ObjectDefines.h"
#include "Runtime/BaseClasses/ObjectManager.h"
#include "Runtime/BaseClasses/Type.h"
#include "Runtime/BaseClasses/TypeManager.h"
#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Core/Math/UnityRandom.h"
#include "Runtime/Function/Command/CommandSystem.h"
#include "Runtime/Function/Framework/Component/Behaviour.h"
#include "Runtime/Function/Framework/Level/Level.h"
#include "Runtime/Function/Framework/Object/ObjectIdAllocator.h"
#include "Runtime/Function/Framework/World/WorldManager.h"
#include "Runtime/Function/Framework/Component/Transform/Transform.h"
#include "Runtime/Function/Framework/Coroutine/Coroutine.h"
#include "Runtime/Function/Framework/TagLayer/TagLayerManager.h"
#include "Runtime/Function/Physics/Jolt/Utils.h"
#include "Runtime/Function/Physics/Physics.h"
#include "Runtime/Function/Physics/PhysicsManager.h"
#include "Runtime/Function/Physics/PhysicsScene.h"
#include "Runtime/Function/PlayerSettings/PlayerSettings.h"
#include "Runtime/Function/Render/WindowSystem.h"
#include "Runtime/Project/ProjectInfo.h"
#include "Runtime/Resource/Config/ConfigManager.h"
#include "Runtime/Scripting/ScriptDebug/ScriptDebug.h"
#include "Runtime/Scripting/ScriptRandom/ScriptRandom.h"
#include "Runtime/Scripting/ScriptScene/ScriptScene.h"
#include "Runtime/Scripting/ScriptScreen/ScriptScreen.h"

#include "Jolt/Jolt.h"
#include "Jolt/Physics/Body/Body.h"
#include "Jolt/Physics/Body/BodyCreationSettings.h"
#include "Jolt/Physics/Collision/Shape/BoxShape.h"
#include "Jolt/Physics/PhysicsSystem.h"

// 本 exe 不编任何 `*.gen.cpp`（编了会拿到第二份 `TypeContainer<T>::m_Type`），
// 于是 Transform / GameObject 的 Type 一直是"未注册"的占位：
// `Cast<Transform>()` 恒为 nullptr，`GameObject::GetTransform()` 因此永远返回
// nullptr —— 哪怕 Transform 明明挂在对象上。
//
// 但注册函数本身在镜像里：ZHeaderTool 把 `IMPLEMENT_REGISTER_CLASS` 写进了
// `<Name>.gen.cpp`，而 /WHOLEARCHIVE 把那个 .obj 拖进了 exe。显式声明之后可以
// 直接调用，不必（也不能）去编译 gen.cpp。
template<> void RegisterKlass<Component>();
template<> void RegisterKlass<Transform>();
template<> void RegisterKlass<GameObject>();
template<> void RegisterKlass<Random>();

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
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
        UnityRandom::InitState(12345);
        for (float& v : first_run)
        {
            v = UnityRandom::value();
        }
        UnityRandom::InitState(12345);
        for (float& v : second_run)
        {
            v = UnityRandom::value();
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

        UnityRandom::InitState(12346);
        const float other_seed = UnityRandom::value();
        Check(other_seed != first_run[0], "E2", "a different seed produces a different sequence");

        // Adjacent seeds must not produce correlated openings -- the classic
        // "seed from a frame counter" trap.
        UnityRandom::InitState(0);
        const float seed0 = UnityRandom::value();
        UnityRandom::InitState(1);
        const float seed1 = UnityRandom::value();
        Check(std::fabs(seed0 - seed1) > 0.01f, "E3", "adjacent seeds are not correlated");

        // -- value() range ----------------------------------------------------
        UnityRandom::InitState(7);
        bool in_range = true;
        for (int i = 0; i < 10000; ++i)
        {
            const float v = UnityRandom::value();
            if (v < 0.0f || v >= 1.0f)
            {
                in_range = false;
            }
        }
        Check(in_range, "E4", "value() stays in [0, 1) over 10000 draws");

        // -- the Unity asymmetry: int excludes max, float includes it ---------
        UnityRandom::InitState(99);
        int int_min = 1000;
        int int_max = -1000;
        for (int i = 0; i < 20000; ++i)
        {
            const int v = UnityRandom::Range(0, 10);
            int_min = (v < int_min) ? v : int_min;
            int_max = (v > int_max) ? v : int_max;
        }
        Check(int_min == 0, "E5", "Range(0, 10) as ints reaches 0");
        Check(int_max == 9, "E6",
              "Range(0, 10) as ints NEVER reaches 10 -- Unity excludes the int max");
        Check(UnityRandom::Range(5, 5) == 5, "E7", "an empty int range returns min");
        Check(UnityRandom::Range(7, 2) == 7, "E8", "an inverted int range returns min");

        UnityRandom::InitState(100);
        bool float_in_range = true;
        float float_max = -1000.0f;
        for (int i = 0; i < 20000; ++i)
        {
            const float v = UnityRandom::Range(2.0f, 3.0f);
            float_max = (v > float_max) ? v : float_max;
            if (v < 2.0f || v > 3.0f)
            {
                float_in_range = false;
            }
        }
        Check(float_in_range, "E9", "Range(2.0f, 3.0f) stays inside both bounds");
        Check(float_max > 2.99f, "E10",
              "Range(2.0f, 3.0f) can get arbitrarily close to 3 -- Unity INCLUDES the float max");
        Check(std::fabs(UnityRandom::Range(4.0f, 2.0f) - 3.0f) < 10.0f, "E11",
              "an inverted float range is swapped, not nonsense");

        // -- save / restore ---------------------------------------------------
        UnityRandom::InitState(2024);
        for (int i = 0; i < 5; ++i)
        {
            UnityRandom::value();
        }
        const UnityRandom::State saved = UnityRandom::GetState();
        float after_save[4];
        for (float& v : after_save)
        {
            v = UnityRandom::value();
        }
        UnityRandom::SetState(saved);
        bool restored = true;
        for (float v : after_save)
        {
            if (UnityRandom::value() != v)
            {
                restored = false;
            }
        }
        Check(restored, "E12", "SetState() resumes the stream exactly (one uint64 of state)");

        // -- shapes -----------------------------------------------------------
        UnityRandom::InitState(555);
        bool sphere_inside = true;
        for (int i = 0; i < 2000; ++i)
        {
            if (UnityRandom::insideUnitSphere().squaredLength() > 1.0f)
            {
                sphere_inside = false;
            }
        }
        Check(sphere_inside, "E13", "insideUnitSphere() never leaves the unit ball");

        bool circle_inside = true;
        for (int i = 0; i < 2000; ++i)
        {
            if (UnityRandom::insideUnitCircle().squaredLength() > 1.0f)
            {
                circle_inside = false;
            }
        }
        Check(circle_inside, "E14", "insideUnitCircle() never leaves the unit disc");

        bool on_sphere = true;
        for (int i = 0; i < 2000; ++i)
        {
            const float length = UnityRandom::onUnitSphere().length();
            if (std::fabs(length - 1.0f) > 0.01f)
            {
                on_sphere = false;
            }
        }
        Check(on_sphere, "E15", "onUnitSphere() is on the surface (length 1)");

        bool on_circle = true;
        for (int i = 0; i < 2000; ++i)
        {
            const float length = UnityRandom::onUnitCircle().length();
            if (std::fabs(length - 1.0f) > 0.01f)
            {
                on_circle = false;
            }
        }
        Check(on_circle, "E16", "onUnitCircle() is on the circle (length 1)");

        bool unit_rotation = true;
        for (int i = 0; i < 2000; ++i)
        {
            const Quaternion q = UnityRandom::rotation();
            const float sq = q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z;
            if (std::fabs(sq - 1.0f) > 0.01f)
            {
                unit_rotation = false;
            }
        }
        Check(unit_rotation, "E17", "rotation() returns a unit quaternion");

        // -- Chance -----------------------------------------------------------
        UnityRandom::InitState(3);
        Check(!UnityRandom::Chance(0.0f), "E18", "Chance(0) is never true");
        Check(UnityRandom::Chance(1.0f), "E19", "Chance(1) is always true");
        Check(!UnityRandom::Chance(-0.5f), "E20", "a negative probability is never true");
        Check(UnityRandom::Chance(2.0f), "E21", "a probability above 1 is always true");
        int trues = 0;
        for (int i = 0; i < 20000; ++i)
        {
            if (UnityRandom::Chance(0.25f))
            {
                ++trues;
            }
        }
        Check(trues > 4400 && trues < 5600, "E22",
              "Chance(0.25) fires about a quarter of the time (measured)");

        // -- no modulo bias on the low end ------------------------------------
        // Range(0, 3) over a big sample: a `% span` implementation skews the
        // first buckets whenever span does not divide 2^32.
        UnityRandom::InitState(4242);
        int buckets[3] = {0, 0, 0};
        for (int i = 0; i < 30000; ++i)
        {
            ++buckets[UnityRandom::Range(0, 3)];
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

    // ---------------------------------------------------------------- J ----
    // Invoke / InvokeRepeating / CancelInvoke.

    /// A Behaviour that records what was invoked on it. Behaviours are
    /// ZCLASS-registered objects, so the probe needs REGISTER_CLASS too -- the
    /// same spelling ObjectLifetimeSmokeTest uses for its probes.
    class InvokeProbe : public Behaviour
    {
        REGISTER_CLASS(InvokeProbe)

    public:
        eastl::vector<eastl::string> calls;

    protected:
        bool InvokeBehaviourMethod(const char* method_name) override
        {
            calls.emplace_back(method_name);
            return true;
        }
    };

    /// Drive the scheduler for `seconds` in `step` chunks, the way a real frame
    /// would. Both arguments go in as the scaled delta so WaitForSeconds (which
    /// is scaled, like Unity's Invoke) is what is being measured.
    void Advance(float seconds, float step)
    {
        CoroutineScheduler* scheduler = GET_SYSTEM(CoroutineScheduler);
        const int steps = static_cast<int>(seconds / step + 0.5f);
        for (int i = 0; i < steps && scheduler != nullptr; ++i)
        {
            scheduler->TickUpdate(step, step);
        }
    }

    void TestInvokes()
    {
        CoroutineScheduler* scheduler = GET_SYSTEM(CoroutineScheduler);
        Check(scheduler != nullptr, "J0", "CoroutineScheduler is reachable");
        if (scheduler == nullptr)
        {
            return;
        }
        scheduler->StopAll();

        InvokeProbe probe;
        Check(!probe.IsInvoking("Later"), "J1", "a fresh Behaviour is invoking nothing");

        // Rejected input must not queue anything: a queued "invoke" with a
        // nonsense delay is worse than a refused one, because it fires later
        // from a call site the author has already forgotten.
        probe.Invoke("Later", 0.f);
        probe.Invoke("Later", -1.f);
        probe.Invoke("", 1.f);
        probe.InvokeRepeating("Later", 0.1f, 0.f);
        Check(scheduler->GetRunningCount() == 0, "J2", "a bad delay / rate queues nothing");
        Check(!probe.IsInvoking("Later"), "J3", "and IsInvoking stays false");

        probe.Invoke("Later", 0.25f);
        Check(probe.IsInvoking("Later"), "J4", "Invoke queues the call");
        Check(!probe.IsInvoking("Other"), "J5", "under its own name only");

        Advance(0.2f, 0.1f);
        Check(probe.calls.empty(), "J6", "nothing fires before the delay");
        Check(probe.IsInvoking("Later"), "J7", "and it is still pending");

        Advance(0.1f, 0.1f);
        Check(probe.calls.size() == 1 && probe.calls[0] == "Later", "J8", "it fires once, on time");
        Check(!probe.IsInvoking("Later"), "J9", "and is no longer pending afterwards");

        // --- repeating ------------------------------------------------------
        probe.calls.clear();
        scheduler->StopAll();
        probe.InvokeRepeating("Tick", 0.1f, 0.1f);
        Check(probe.IsInvoking("Tick"), "J10", "InvokeRepeating is pending");
        Advance(1.0f, 0.1f);
        Check(probe.calls.size() == 10, "J11", "1.0s at 0.1s intervals fires 10 times");

        const std::size_t after_second = probe.calls.size();
        probe.CancelInvoke("Tick");
        Check(!probe.IsInvoking("Tick"), "J12", "CancelInvoke clears it");
        Advance(1.0f, 0.1f);
        Check(probe.calls.size() == after_second, "J13", "and it stops firing");

        // --- cancel one of two ---------------------------------------------
        probe.calls.clear();
        scheduler->StopAll();
        probe.InvokeRepeating("A", 0.1f, 0.2f);
        probe.InvokeRepeating("B", 0.1f, 0.2f);
        probe.CancelInvoke("A");
        Check(!probe.IsInvoking("A"), "J14", "cancelling by name drops only that name");
        Check(probe.IsInvoking("B"), "J15", "and leaves the other running");
        Advance(1.0f, 0.1f);
        Check(probe.calls.size() >= 4, "J16", "the survivor keeps firing");
        for (const eastl::string& call : probe.calls)
        {
            Check(call == "B", "J17", "and only the survivor's method is delivered");
        }

        // --- cancel everything ---------------------------------------------
        probe.calls.clear();
        scheduler->StopAll();
        probe.InvokeRepeating("A", 0.1f, 0.2f);
        probe.InvokeRepeating("B", 0.1f, 0.2f);
        // Unity: CancelInvoke() with no argument cancels all. pesapi cannot
        // express "no argument", so the empty string is that spelling.
        probe.CancelInvoke("");
        Check(!probe.IsInvoking("A") && !probe.IsInvoking("B"), "J18", "CancelInvoke(\"\") cancels all");
        Check(scheduler->GetRunningCount() == 0, "J19", "and the scheduler is empty");
        Advance(1.0f, 0.1f);
        Check(probe.calls.empty(), "J20", "so nothing is delivered");

        // --- a destroyed Behaviour cancels ----------------------------------
        scheduler->StopAll();
        {
            InvokeProbe temp;
            temp.InvokeRepeating("Later", 0.1f, 0.1f);
            Check(scheduler->GetRunningCount() == 1, "J21", "a pending invoke is running");
        }
        // The destructor must cancel: a routine that outlives its Behaviour
        // would call into freed memory on the very next frame.
        Check(scheduler->GetRunningCount() == 0, "J22", "destroying the Behaviour cancels it");
    }

    void TestInvokeWiring()
    {
        const std::string behaviour_header =
            ReadFile("Source/Runtime/Function/Framework/Component/Behaviour.h");
        Check(!behaviour_header.empty(), "K0", "Behaviour.h is readable");
        Check(CountOccurrences(behaviour_header, "void Invoke(") >= 1, "K1", "Invoke is declared");
        Check(CountOccurrences(behaviour_header, "InvokeRepeating") >= 1, "K2", "InvokeRepeating is declared");
        Check(CountOccurrences(behaviour_header, "CancelInvoke") >= 1, "K3", "CancelInvoke is declared");
        Check(CountOccurrences(behaviour_header, "IsInvoking") >= 1, "K4", "IsInvoking is declared");
        // The trap: `Behaviour` is ALSO the name of the synthetic script class
        // TS behaviours extend. Publishing the C++ class emits a second
        // `declare class Behaviour` and tsc fails on the duplicate identifier,
        // which is a user-project compile error with nothing pointing at the
        // cause. The ZFUNCTIONs must live on TypeScriptComponent instead.
        // "ZFUNCTION()" with the parens: the word also appears in the comment
        // that explains why it must not, and a grep that cannot tell the two
        // apart reports a violation that does not exist.
        Check(CountOccurrences(behaviour_header, "ZFUNCTION()") == 0,
              "K4b", "Behaviour itself is NOT published as a script class");

        const std::string ts_header_check =
            ReadFile("Source/Runtime/Function/Framework/Component/Script/TypeScriptComponent.h");
        Check(CountOccurrences(ts_header_check, "ZFUNCTION") >= 5,
              "K4c", "the Invoke family is published on TypeScriptComponent");
        Check(CountOccurrences(ts_header_check, "void Invoke(const eastl::string& methodName, float time)") >= 1,
              "K4d", "with Unity's argument order");

        const std::string binding_source_check =
            ReadFile("Source/Runtime/Scripting/Binding/ScriptBinding.cpp");
        Check(CountOccurrences(binding_source_check, "Invoke(methodName: string, time: number)") >= 1,
              "K4e", "the .d.ts Behaviour block declares Invoke");
        Check(CountOccurrences(binding_source_check, "CancelInvoke(methodName: string)") >= 1,
              "K4f", "and CancelInvoke");

        const std::string env_source = ReadFile("Source/Runtime/Scripting/Wrapper/ScriptEnv.cpp");
        Check(!env_source.empty(), "K4g", "ScriptEnv.cpp is readable");
        // The declaration alone is a lie: without the prototype forwarding,
        // `this.Invoke(...)` type-checks and then throws at runtime.
        Check(CountOccurrences(env_source, "Behaviour.prototype.Invoke") >= 1,
              "K4h", "the JS Behaviour prototype forwards Invoke to the component");
        Check(CountOccurrences(env_source, "Behaviour.prototype.CancelInvoke") >= 1,
              "K4i", "and CancelInvoke");

        const std::string behaviour_source =
            ReadFile("Source/Runtime/Function/Framework/Component/Behaviour.cpp");
        Check(!behaviour_source.empty(), "K5", "Behaviour.cpp is readable");
        // The point of the design: the timer is a COROUTINE, so it is driven by
        // Application's scheduler and not by Behaviour::Tick -- which two of the
        // three Behaviour subclasses override and would silently break.
        Check(CountOccurrences(behaviour_source, "CoroutineScheduler") >= 4,
              "K6", "invokes are scheduled, not ticked by the component");
        Check(CountOccurrences(behaviour_source, "scheduler->Start(") >= 2,
              "K7", "both the one-shot and the repeating path start a routine");
        Check(CountOccurrences(behaviour_source, "FireInvoke") >= 3,
              "K8", "both routines actually deliver");
        Check(CountOccurrences(behaviour_source, "SendMessageByName") >= 1,
              "K9", "delivery falls back to SendMessage for non-script Behaviours");

        const std::string ts_header2 =
            ReadFile("Source/Runtime/Function/Framework/Component/Script/TypeScriptComponent.h");
        Check(CountOccurrences(ts_header2, "InvokeBehaviourMethod") >= 1,
              "K10", "a script component delivers to its own JS instance");

        const std::string reg = ReadFile("Source/Runtime/RegisterRuntime.cpp");
        Check(CountOccurrences(reg, "REGISTER_SYSTEM(CoroutineScheduler)") >= 1,
              "K11", "the scheduler is registered, so GET_SYSTEM finds it");
    }

    // ---------------------------------------------------------------- L ----
    void TestApplicationFacade()
    {
        // 搭一个"最小工程"：ConfigManager 读一份临时 ini，ProjectInfo 只填字段不
        // Initialize（Initialize 要真的 .zproject 与 CommandSystem 的命令行）。
        // 这样下面每个 getter 的期望值都是确定的，而不是"取决于谁在跑测试"。
        std::error_code ec;
        const std::filesystem::path root =
            std::filesystem::temp_directory_path() / "zengine_unity_parity_app";
        std::filesystem::create_directories(root, ec);
        {
            std::ofstream ini(root / "ZEditor.ini", std::ios::binary);
            ini << "BinaryRootFolder=.\nAssetFolder=Assets\n";
        }

        CommandSystem* commands = new CommandSystem();
        commands->getConfigFilePath() = root / "ZEditor.ini";
        SystemRegistry::GetInstance().RegisterSystem(commands);

        ConfigManager* config = new ConfigManager();
        SystemRegistry::GetInstance().RegisterSystem(config);
        Check(config->Initialize(), "L0", "ConfigManager reads the temp ini");

        ProjectInfo* project = new ProjectInfo();
        project->project_path = root;
        project->name = "ParityTest";
        project->version = "1.2.3";
        project->engine_version = "0.9.0";
        SystemRegistry::GetInstance().RegisterSystem(project);

        PlayerSettings* settings = new PlayerSettings();
        SystemRegistry::GetInstance().RegisterSystem(settings);
        Check(settings->Initialize(), "L1", "PlayerSettings picks up the project defaults");

        SystemRegistry::GetInstance().RegisterSystem(new Application());
        Check(GET_SYSTEM(Application) != nullptr, "L2", "the application is reachable via GET_SYSTEM");

        // --- 路径 ---
        const std::string data_path = Application::GetDataPath();
        Check(!data_path.empty(), "L3", "GetDataPath is not empty");
        Check(std::filesystem::path(data_path).filename() == "Assets",
              "L4", "GetDataPath points at the asset folder");
        const std::string streaming = Application::GetStreamingAssetsPath();
        Check(std::filesystem::path(streaming).filename() == "StreamingAssets",
              "L5", "GetStreamingAssetsPath is dataPath/StreamingAssets");
        Check(streaming.size() > data_path.size() && streaming.compare(0, data_path.size(), data_path) == 0,
              "L6", "and is actually nested under it");
        Check(std::filesystem::path(Application::GetProjectPath()) ==
                  std::filesystem::weakly_canonical(root),
              "L7", "GetProjectPath is the project root");

        // --- 身份 ---
        Check(Application::GetProductName() == "ParityTest", "L8", "GetProductName follows the project");
        Check(Application::GetVersion() == "1.2.3", "L9", "GetVersion follows the project");
        Check(Application::GetEngineVersion() == "0.9.0", "L10", "GetEngineVersion reads ProjectInfo");
        Check(!Application::GetCompanyName().empty(), "L11", "GetCompanyName has a default");
        Check(Application::GetPersistentDataPath().find("ParityTest") != std::string::npos,
              "L12", "the persistent folder is per-product (not a shared 'ZEngine' one)");

        // --- 状态位 ---
        Check(!Application::GetIsPlaying(), "L13", "GetIsPlaying is false by default");
        g_isPlaying = true;
        Check(Application::GetIsPlaying(), "L14", "and follows g_isPlaying");
        g_isPlaying = false;
        Check(!Application::GetIsEditor(), "L15", "GetIsEditor is false in a runtime-only process");

        // Editor/Player 的取值是相邻的（1/2, 3/4, 5/6），所以"编辑器态一定是奇数"
        // 这条在任何桌面平台上都成立 —— 比硬编码 WindowsEditor 可移植。
        const int player_platform = static_cast<int>(Application::GetPlatform());
        Check(player_platform % 2 == 0, "L16", "a player's platform value is even");
        g_isEditor = true;
        Check(static_cast<int>(Application::GetPlatform()) == player_platform - 1,
              "L17", "the editor variant is the preceding value");
        g_isEditor = false;
        Check(Application::GetPlatform() != RuntimePlatform::Unknown, "L18", "the platform resolves");
        Check(!Application::GetPlatformName().empty() && Application::GetPlatformName() != "Unknown",
              "L19", "and has a printable name");

        // --- 限帧 / 后台 ---
        Check(Application::GetTargetFrameRate() == -1, "L20", "the default is 'unlimited' (-1)");
        Application::SetTargetFrameRate(30);
        Check(Application::GetTargetFrameRate() == 30, "L21", "SetTargetFrameRate sticks");
        Application::SetTargetFrameRate(0);
        Check(Application::GetTargetFrameRate() == 0, "L22", "0 means no limit");

        Check(Application::GetRunInBackground(), "L23", "runInBackground defaults to true");
        Application::SetRunInBackground(false);
        Check(!Application::GetRunInBackground(), "L24", "and can be turned off");
        Application::SetRunInBackground(true);

        // --- 退出 ---
        Check(!GET_SYSTEM(Application)->IsQuit(), "L25", "not quitting before Quit()");
        Application::Quit();
        Check(GET_SYSTEM(Application)->IsQuit(), "L26", "Quit() sets the quit flag");
    }

    // ---------------------------------------------------------------- M ----
    // 必须**先于**任何注册 WindowSystem 的组跑：M0 断言的是"根本没注册"这一支。
    void TestScreenFacade()
    {
        Check(Screen::GetWidth() == 0, "M0", "no WindowSystem at all: documented 0, not the fallback");

        WindowSystem* window_system = new WindowSystem();
        SystemRegistry::GetInstance().RegisterSystem(window_system);

        // 有系统但没有窗口（无头 / 尚未创建）：1280x720 是 WindowSystem 的兜底。
        // 与 M0 的 0 必须分得开 —— 两个不同的失败模式。
        Check(Screen::GetWidth() == 1280, "M1", "registered but windowless: the 1280x720 fallback");
        Check(Screen::GetHeight() == 720, "M2", "and 720 high");
        Check(std::fabs(Screen::GetDpi() - 1.0f) < 1.0e-5f, "M3", "dpi is 1 with no DPI info");
        Check(Screen::GetMonitorCount() == 0, "M4", "no monitor info without a platform application");
        Check(Screen::GetPrimaryMonitorWidth() == 0, "M5", "and the work area is all zeroes");
        Check(Screen::GetPrimaryMonitorHeight() == 0, "M6", "...");
        Check(Screen::GetPrimaryMonitorX() == 0 && Screen::GetPrimaryMonitorY() == 0, "M7", "...");

        // 宿主表面那一支（移动端 / Web）：尺寸由宿主推入，Screen 要照着报。
        window_system->SetHostWindowSize(1920, 1080);
        Check(Screen::GetWidth() == 1920, "M8", "the host surface size is reported");
        Check(Screen::GetHeight() == 1080, "M9", "...");
        Check(Screen::GetFramebufferWidth() == 1920, "M10", "and so is the framebuffer size");
        Check(Screen::GetFramebufferHeight() == 1080, "M11", "...");
        Check(std::fabs(Screen::GetDpi() - 1.0f) < 1.0e-5f, "M12", "dpi derives from buffer/window = 1");

        // SetResolution 在没有主窗口时是空操作（宿主拥有尺寸，引擎改了也会被
        // 下一次 surfaceChanged 覆盖）。要的是"不崩、也不偷偷改状态"。
        Screen::SetResolution(800, 600);
        Check(Screen::GetWidth() == 1920, "M13", "SetResolution is a no-op without a real window");

        window_system->SetHostWindowSize(0, 0);
        Check(Screen::GetWidth() == 1280, "M14", "clearing the host size falls back again");
    }

    // ---------------------------------------------------------------- N ----
    void TestApplicationScreenWiring()
    {
        const std::string app_header = ReadFile("Source/Runtime/Application/Application.h");
        Check(!app_header.empty(), "N0", "Application.h is readable");
        Check(CountOccurrences(app_header, "ZENUM()") >= 1, "N1", "RuntimePlatform is published to scripts");
        Check(CountOccurrences(app_header, "enum class RuntimePlatform") >= 1,
              "N2", "as a real enum, not a bag of ints");
        Check(CountOccurrences(app_header, "ZFUNCTION()") >= 15,
              "N3", "the whole Application facade is published");

        const std::string app_source = ReadFile("Source/Runtime/Application/Application.cpp");
        Check(!app_source.empty(), "N4", "Application.cpp is readable");
        // Quit() 只置 m_Isquit 的话主循环永远不退出 —— 它看的是
        // WindowSystem::ShouldClose()。这是个"看起来生效了"的死门。
        Check(CountOccurrences(app_source, "RequestClose") >= 1,
              "N5", "Quit() also asks the window to close");
        Check(CountOccurrences(app_source, "IsWindowFocused") >= 1,
              "N6", "runInBackground is enforced against real OS focus");
        Check(CountOccurrences(app_source, "PollEvents") >= 2,
              "N7", "and the background branch still pumps messages (no 'not responding')");
        Check(CountOccurrences(app_source, "LimitFrameRate") >= 2,
              "N8", "the frame limiter is defined AND called");
        Check(CountOccurrences(app_source, "Z_PLATFORM_IS_") >= 5,
              "N9", "the platform switch uses the engine's own macros (iOS is not macOS)");

        const std::string window_header = ReadFile("Source/Runtime/Function/Render/WindowSystem.h");
        Check(CountOccurrences(window_header, "IsWindowFocused") >= 1,
              "N10", "WindowSystem exposes focus");
        const std::string window_source = ReadFile("Source/Runtime/Function/Render/WindowSystem.cpp");
        Check(CountOccurrences(window_source, "m_IsWindowFocused") >= 1,
              "N11", "focus is STORED, not just forwarded (the callback fires once)");

        const std::string editor_source = ReadFile("Source/Editor/EditorApplication/EditorApplication.cpp");
        Check(!editor_source.empty(), "N12", "EditorApplication.cpp is readable");
        Check(CountOccurrences(editor_source, "g_isEditor = true") >= 1,
              "N13", "the editor actually sets g_isEditor");
        // 顺序断言，不是计数：编辑模式下 TS 组件也 Tick（[ExecuteAlways]），
        // 所以这个量必须在 Initialize 读任何东西之前就写好，而不是"在某处写了"。
        const std::string::size_type set_pos = editor_source.find("g_isEditor = true");
        const std::string::size_type first_read = editor_source.find("GetProjectPath()");
        Check(set_pos != std::string::npos && first_read != std::string::npos && set_pos < first_read,
              "N14", "and before Initialize reads anything");

        const std::string screen_header = ReadFile("Source/Runtime/Scripting/ScriptScreen/ScriptScreen.h");
        Check(!screen_header.empty(), "N15", "ScriptScreen.h is readable");
        Check(CountOccurrences(screen_header, "ZCLASS()") >= 1, "N16", "Screen is a script class");
        Check(CountOccurrences(screen_header, "SetSize") >= 1,
              "N17", "SetResolution really resizes the window");
        Check(CountOccurrences(screen_header, "GetPrimaryMonitorWorkArea") >= 1,
              "N18", "monitor info comes from GenericApplication");
        // 屏幕坐标与像素是两件事（DPI 缩放）。无头测试里两者恰好同值，所以行为
        // 断言分不出对错 —— 只能从源码上钉住各取各的源。
        // 精确个数不是吹毛求疵：3 = width / height / dpi 各一处。把 GetWidth
        // 改成读 GetFramebufferSize 之后总数仍然是 2，>= 2 的阈值放它过去 ——
        // 而无头测试里两者恰好同值，行为断言也放它过去。
        Check(CountOccurrences(screen_header, "GetWindowSize()") == 3,
              "N19", "GetWidth/GetHeight read the WINDOW size, not the framebuffer");
        Check(CountOccurrences(screen_header, "GetFramebufferSize()") == 3,
              "N20", "and the framebuffer accessors read the framebuffer (dpi uses it too)");
        Check(CountOccurrences(screen_header, "GetDpiScale") >= 1,
              "N21", "dpi prefers the platform window's own scale");
    }

    // ---------------------------------------------------------------- P ----
    void TestDebugFacade()
    {
        // 期望值用**另一套**宏算：MSVC 的 Debug 定义 _DEBUG（不是 NDEBUG），
        // 所以用 _DEBUG 反推。两条独立的判据互相印证 —— 直接拿 `!defined(NDEBUG)`
        // 比就变成同义反复，把 IsDebugBuild 改成 `return false` 也抓不到。
#if defined(_MSC_VER)
        const bool expected_debug_build =
#if defined(_DEBUG)
            true;
#else
            false;
#endif
#else
        const bool expected_debug_build =
#if defined(NDEBUG)
            false;
#else
            true;
#endif
#endif
        Check(ScriptDebug::IsDebugBuild() == expected_debug_build,
              "P0", "IsDebugBuild agrees with the compiler's own build macros");

        // 没有调试器时 Break 必须只记日志然后返回 —— CI 上无条件 __debugbreak
        // 会让测试进程以 0x80000003 死掉，连日志都来不及刷。
        ScriptDebug::Break("UnityParitySmokeTest::TestDebugFacade");
        Check(true, "P1", "Break() with no debugger attached returns instead of killing the process");

        const std::string debug_source = ReadFile("Source/Runtime/Scripting/ScriptDebug/ScriptDebug.cpp");
        Check(!debug_source.empty(), "P2", "ScriptDebug.cpp is readable");
        Check(CountOccurrences(debug_source, "IsDebuggerAttached") >= 2,
              "P3", "Break() really checks for a debugger (declared AND used)");
        Check(CountOccurrences(debug_source, "ZEngineForceFlushLogs") >= 1,
              "P4", "and flushes the async log before it might break");

        const std::string env_source = ReadFile("Source/Runtime/Scripting/Wrapper/ScriptEnv.cpp");
        Check(CountOccurrences(env_source, "LogException:") >= 1,
              "P5", "the shim exposes Debug.LogException");
        Check(CountOccurrences(env_source, "Assert: function") >= 1,
              "P6", "and Debug.Assert");
        Check(CountOccurrences(env_source, "IsDebugBuild: g.__zdebug_native_is_debug_build") >= 1,
              "P7", "and Debug.IsDebugBuild (bound to the native sink, not a JS constant)");
        Check(CountOccurrences(env_source, "Break:        g.__zdebug_native_break") >= 1,
              "P8", "and Debug.Break");

        const std::string binding_source = ReadFile("Source/Runtime/Scripting/Binding/ScriptBinding.cpp");
        Check(CountOccurrences(binding_source, "function LogException(e: any): void;") >= 1,
              "P9", "the .d.ts declares LogException");
        Check(CountOccurrences(binding_source, "function Assert(condition: boolean") >= 1,
              "P10", "and Assert");
        Check(CountOccurrences(binding_source, "function IsDebugBuild(): boolean;") >= 1,
              "P11", "and IsDebugBuild");
        Check(CountOccurrences(binding_source, "function Break(message?: string): void;") >= 1,
              "P12", "and Break");
        // `Debug` 一旦变成 ZCLASS：绑定类的全局是构造器，会把 shim 装的那个
        // JS 对象整个覆盖掉，而且 `Log(...args)` 的多参也没了。
        Check(CountOccurrences(binding_source, "declare namespace Debug") >= 1,
              "P13", "Debug stays a namespace (a bound class would clobber the JS object)");
    }

    // ---------------------------------------------------------------- Q ----
    /// 碰撞 / 触发事件：phase 三态、body 归属表、以及"没人接"的派发。
    void TestContactEvents()
    {
        PhysicsManager* physics_manager = GET_SYSTEM(PhysicsManager);
        Check(physics_manager != nullptr, "Q0", "PhysicsManager is reachable");
        if (physics_manager == nullptr)
        {
            return;
        }

        // 零重力：否则两块方块一边撞一边掉，"还接触着吗"取决于落体时间，
        // 这条测试就会随机器/帧率变。
        std::shared_ptr<PhysicsScene> scene = physics_manager->CreatePhysicsScene(Vector3(0.f, 0.f, 0.f)).lock();
        Check(scene != nullptr, "Q1", "a zero-gravity scene can be created");
        if (!scene)
        {
            return;
        }

        const uint32_t mover = SpawnBox(*scene, Vector3(-2.f, 0.f, 0.f), 0, true);
        const uint32_t wall = SpawnBox(*scene, Vector3(0.f, 0.f, 0.f), 0, false);
        Check(mover != kInvalidPhysicsBodyId && wall != kInvalidPhysicsBodyId, "Q2", "both bodies exist");

        std::vector<PhysicsCollisionEvent> events;
        scene->PollCollisionEvents(events);
        Check(events.empty(), "Q3", "nothing has collided yet");

        Check(scene->SetLinearVelocity(mover, Vector3(2.f, 0.f, 0.f)), "Q4", "the mover is given a velocity");

        // --- enter ---
        PhysicsCollisionEvent enter_event;
        bool saw_enter = false;
        for (int step = 0; step < 200 && !saw_enter; ++step)
        {
            StepScene(*scene, 1);
            scene->PollCollisionEvents(events);
            for (const PhysicsCollisionEvent& event : events)
            {
                if (event.m_Phase == PhysicsContactPhase::enter)
                {
                    enter_event = event;
                    saw_enter = true;
                }
            }
        }
        Check(saw_enter, "Q5", "a contact produces an enter event");
        Check((enter_event.m_BodyA == mover && enter_event.m_BodyB == wall) ||
                  (enter_event.m_BodyA == wall && enter_event.m_BodyB == mover),
              "Q6", "and it names both bodies");
        Check(enter_event.m_RelativeSpeed > 0.f, "Q7", "with a non-zero approach speed");

        // --- stay ---
        bool saw_stay = false;
        bool saw_duplicate_enter = false;
        for (int step = 0; step < 10; ++step)
        {
            StepScene(*scene, 1);
            scene->PollCollisionEvents(events);
            for (const PhysicsCollisionEvent& event : events)
            {
                if (event.m_Phase == PhysicsContactPhase::stay)
                {
                    saw_stay = true;
                }
                if (event.m_Phase == PhysicsContactPhase::enter)
                {
                    // Unity 只在撞上那一帧发 Enter。持续接触期间每帧再发一条
                    // Enter 是最容易写出来的 bug —— Jolt 的 OnContactPersisted
                    // 每个子步都会来，直接把它排进队列就会这样。
                    saw_duplicate_enter = true;
                }
            }
        }
        Check(saw_stay, "Q8", "a persisting contact produces stay events");
        Check(!saw_duplicate_enter, "Q9", "and NOT a fresh enter every step");

        // --- exit ---
        Check(scene->SetLinearVelocity(mover, Vector3(-30.f, 0.f, 0.f)), "Q10", "the mover is pulled away");
        bool saw_exit = false;
        for (int step = 0; step < 200 && !saw_exit; ++step)
        {
            StepScene(*scene, 1);
            scene->PollCollisionEvents(events);
            for (const PhysicsCollisionEvent& event : events)
            {
                if (event.m_Phase == PhysicsContactPhase::exit)
                {
                    saw_exit = true;
                }
            }
        }
        Check(saw_exit, "Q11", "separating the bodies produces an exit event");

        // --- 触发器的三态 ---
        const uint32_t trigger = SpawnBox(*scene, Vector3(0.f, 20.f, 0.f), 0, false);
        const uint32_t walker = SpawnBox(*scene, Vector3(0.f, 24.f, 0.f), 0, true);
        Check(scene->SetRigidBodySensor(trigger, true), "Q12", "a body can be made a sensor");
        Check(scene->SetLinearVelocity(walker, Vector3(0.f, -2.f, 0.f)), "Q13", "the walker moves into it");

        bool trigger_enter = false;
        bool trigger_stay = false;
        bool trigger_exit = false;
        for (int step = 0; step < 400 && !trigger_exit; ++step)
        {
            StepScene(*scene, 1);
            std::vector<PhysicsTriggerEvent> trigger_events;
            scene->PollTriggerEvents(trigger_events);
            for (const PhysicsTriggerEvent& event : trigger_events)
            {
                if (event.m_TriggerBodyId != trigger || event.m_OtherBodyId != walker)
                {
                    continue;
                }
                if (event.m_Phase == PhysicsContactPhase::enter)
                {
                    trigger_enter = true;
                }
                else if (event.m_Phase == PhysicsContactPhase::stay)
                {
                    trigger_stay = true;
                }
                else if (event.m_Phase == PhysicsContactPhase::exit)
                {
                    trigger_exit = true;
                }
            }
        }
        Check(trigger_enter, "Q14", "a sensor reports OnTriggerEnter");
        Check(trigger_stay, "Q15", "and OnTriggerStay while the overlap persists");
        Check(trigger_exit, "Q16", "and OnTriggerExit when it leaves");

        // --- body -> GameObject 归属表 ---
        GameObject* owner = new GameObject();
        physics_manager->RegisterBodyOwner(mover, owner);
        Check(physics_manager->FindBodyOwner(mover) == owner, "Q17", "a registered body resolves to its owner");
        physics_manager->UnregisterBodyOwner(mover);
        Check(physics_manager->FindBodyOwner(mover) == nullptr, "Q18", "and stops resolving once removed");
        Check(physics_manager->FindBodyOwner(0x1234u) == nullptr, "Q19", "an unknown body id resolves to nothing");
        Check(owner->SendMessageWithOther("OnCollisionEnter", nullptr) == 0,
              "Q20", "delivery to an object with no script component is a silent no-op");
        delete owner;
    }

    // ---------------------------------------------------------------- R ----
    void TestContactEventWiring()
    {
        const std::string scene_source = ReadFile("Source/Runtime/Function/Physics/PhysicsScene.cpp");
        Check(!scene_source.empty(), "R0", "PhysicsScene.cpp is readable");
        // 核心接线门：OnContactAdded 以前对非 sensor 的接触直接 return —— 物理层
        // 把碰撞事件整个丢掉了，于是框架层无论写什么，OnCollisionEnter 都不可
        // 能存在，而且没有任何报错。
        Check(CountOccurrences(scene_source, "OnContactPersisted") >= 1,
              "R1", "persisting contacts are tracked (the source of OnCollisionStay)");
        Check(CountOccurrences(scene_source, "Collide(in_body1, in_body2, in_manifold") >= 2,
              "R2", "both the added and the persisted path feed the collision table");
        Check(CountOccurrences(scene_source, "m_CollisionEvents.push_back(event)") >= 1,
              "R3", "new pairs are queued as enter");

        const std::string scene_header = ReadFile("Source/Runtime/Function/Physics/PhysicsScene.h");
        Check(CountOccurrences(scene_header, "enum class PhysicsContactPhase") >= 1,
              "R4", "the phase is a 3-state enum, not a bool");
        Check(CountOccurrences(scene_header, "PollCollisionEvents") >= 1,
              "R5", "collision events have a poll entry point");

        const std::string manager_source = ReadFile("Source/Runtime/Function/Physics/PhysicsManager.cpp");
        Check(!manager_source.empty(), "R6", "PhysicsManager.cpp is readable");
        Check(CountOccurrences(manager_source, "PollTriggerEvents") >= 1, "R7", "triggers are drained");
        Check(CountOccurrences(manager_source, "PollCollisionEvents") >= 1, "R8", "and so are collisions");
        Check(CountOccurrences(manager_source, "OnTriggerStay") >= 1, "R9", "OnTriggerStay is delivered");
        Check(CountOccurrences(manager_source, "OnCollisionStay") >= 1, "R10", "OnCollisionStay is delivered");
        Check(CountOccurrences(manager_source, "OnCollisionExit") >= 1, "R11", "OnCollisionExit is delivered");

        // 主循环只许有一个派发点：队列是被排空的，派发两次的第二次什么都拿不到。
        const std::string app_source = ReadFile("Source/Runtime/Application/Application.cpp");
        Check(CountOccurrences(app_source, "DispatchContactEvents") == 1,
              "R12", "exactly one dispatch point per frame");

        const std::string rigid_source =
            ReadFile("Source/Runtime/Function/Framework/Component/Rigidbody/RigidbodyComponent.cpp");
        Check(CountOccurrences(rigid_source, "RegisterBodyOwner") >= 1,
              "R13", "creating a body registers its owner");
        // 3 = 定义 + 析构 + RemoveRigidBody 两处拆除路径。少一处就是一条
        // 指向已销毁 GameObject 的野记录（body id 是会被复用的）。
        Check(CountOccurrences(rigid_source, "UnregisterOwner()") >= 3,
              "R14", "and both teardown paths remove it");

        const std::string binding_source = ReadFile("Source/Runtime/Scripting/Binding/ScriptBinding.cpp");
        Check(CountOccurrences(binding_source, "OnCollisionEnter(other?: GameObject): void;") >= 1,
              "R15", "the .d.ts declares OnCollisionEnter");
        Check(CountOccurrences(binding_source, "OnTriggerExit(other?: GameObject): void;") >= 1,
              "R16", "and OnTriggerExit");

        const std::string go_source = ReadFile("Source/Runtime/BaseClasses/GameObject.cpp");
        Check(CountOccurrences(go_source, "InvokeMethodWithOther") >= 1,
              "R17", "delivery passes the other GameObject through to the script");
    }

    // ---------------------------------------------------------------- S ----
    /// 分量级比较。方向向量一律是单位向量，逐分量比 > 用长度比 —— 长度对了方向反了
    /// 是最容易漏的一类错（"朝向"错了但"有多远"全对）。
    bool Near(const Vector3& a, const Vector3& b, float tolerance = 1.0e-3f)
    {
        return std::fabs(a.x - b.x) < tolerance && std::fabs(a.y - b.y) < tolerance &&
               std::fabs(a.z - b.z) < tolerance;
    }

    bool Near(float a, float b, float tolerance = 1.0e-3f) { return std::fabs(a - b) < tolerance; }

    /// 造一个带 Transform 的 GameObject。
    ///
    /// 两处都不能想当然：
    ///
    /// 1. **不能用 `om.Produce(TypeOf<Transform>(), 0)`**。Transform 的工厂函数
    ///    是 `IMPLEMENT_REGISTER_CLASS(Transform)` 填进 Type 的，而那行在
    ///    ZHeaderTool 生成的 `Transform.gen.cpp` 里 —— 本 exe 按规矩不编任何
    ///    `.gen.cpp`（编了就会因为 /WHOLEARCHIVE 拿到第二份
    ///    `TypeContainer<T>::m_Type`），所以这里的 `TypeOf<Transform>()` 是个
    ///    `factory == nullptr` 的占位 Type，`Produce` 一律返回 nullptr。
    ///    退一步说，就算真注册了，`TypeManager::Initialize()` 也得先跑一遍。
    ///
    /// 2. **但也不能只 `new Transform()`**。InstanceID 为 0 的 Object 对 PPtr
    ///    等于"不存在"（`PPtr::operator T*()` 在 m_InstanceID==0 时短路返回
    ///    nullptr），而 `m_Children` 是 `PPtr<Transform>` —— SetParent 记下来
    ///    的儿子下一行就读不回来，IsChildOf / DetachChildren 全线静默失效。
    ///    `AllocateAndAssignInstanceID()` 正是补这一环的（负 ID + 登记表）。
    Transform* MakeTransform(ObjectManager& om, GameObject*& out_owner)
    {
        GameObject* go = new GameObject();
        Transform* transform = new Transform();
        om.AllocateAndAssignInstanceID(go);
        om.AllocateAndAssignInstanceID(transform);
        go->addComponent(transform);
        // addComponent 只管塞进 m_Components，owner 反向指针是
        // PostLoadResource 设的。没有它 Transform 的 m_ParentObject 是 nullptr，
        // 层级相关的东西（以及后面 GameObject 侧的查询）会走空路径。
        transform->PostLoadResource(go);
        out_owner = go;
        return transform;
    }

    void TestTransformDirections()
    {
        ObjectManager* om = GET_SYSTEM(ObjectManager);
        if (om == nullptr)
        {
            om = new ObjectManager();
            SystemRegistry::GetInstance().RegisterSystem(om);
            om->Initialize();
        }
        Check(GET_SYSTEM(ObjectManager) != nullptr, "S0", "ObjectManager is reachable");

        GameObject* owner_a = nullptr;
        Transform* t = MakeTransform(*om, owner_a);
        Check(t != nullptr, "S1", "a Transform can be produced");
        if (t == nullptr)
        {
            return;
        }

        t->SetPosition(Vector3(0.f, 0.f, 0.f));
        t->SetRotation(Quaternion::IDENTITY);

        // ZEngine 是 Z-up、forward = -Y。这三条是整套约定的锚点：写错成 Unity 的
        // +Z / +Y 的话，脚本里 `GetForward() * speed * dt` 会让角色朝侧面走。
        Check(Near(t->GetForward(), Vector3(0.f, -1.f, 0.f)), "S2", "identity: forward is -Y");
        Check(Near(t->GetUp(), Vector3(0.f, 0.f, 1.f)), "S3", "identity: up is +Z");
        Check(Near(t->GetRight(), Vector3(-1.f, 0.f, 0.f)), "S4", "identity: right is -X (X is left)");
        Check(Near(t->GetLeft(), Vector3(1.f, 0.f, 0.f)), "S5", "identity: left is +X");
        Check(Near(t->GetBack(), Vector3(0.f, 1.f, 0.f)), "S6", "identity: back is +Y");
        Check(Near(t->GetDown(), Vector3(0.f, 0.f, -1.f)), "S7", "identity: down is -Z");
        Check(Near(t->GetForward(), Vector3::NEGATIVE_UNIT_Y) && Near(t->GetUp(), Vector3::UNIT_Z),
              "S8", "and they match the constants the engine itself uses");

        // --- RotateAxis：绕世界 +Z 转 90° ---
        t->SetRotation(Quaternion::IDENTITY);
        t->RotateAxis(Vector3::UNIT_Z, 90.f);
        Check(Near(t->GetForward(), Vector3(1.f, 0.f, 0.f)), "S9", "yaw +90 about Z turns forward to +X");
        Check(Near(t->GetUp(), Vector3(0.f, 0.f, 1.f)), "S10", "and leaves up alone");

        // --- SetForward / SetUp ---
        t->SetRotation(Quaternion::IDENTITY);
        t->SetForward(Vector3(0.f, 1.f, 0.f));
        Check(Near(t->GetForward(), Vector3(0.f, 1.f, 0.f)), "S11", "SetForward aims the facing direction");
        Check(Near(t->GetUp(), Vector3(0.f, 0.f, 1.f)), "S12", "and keeps up at world +Z by default");

        t->SetUp(Vector3(0.f, 1.f, 0.f));
        Check(Near(t->GetUp(), Vector3(0.f, 1.f, 0.f)), "S13", "SetUp aims the up direction");
        Check(Near(t->GetForward(), Vector3(0.f, 1.f, 0.f)) == false && Near(t->GetForward().length(), 1.f),
              "S14", "facing stays a unit vector (it cannot stay put: up and forward are perpendicular)");

        // 退化输入：forward 与 up 平行时没有唯一解，要的是"某个合法解"而不是 NaN
        t->SetRotation(Quaternion::IDENTITY);
        t->SetForward(Vector3(0.f, 0.f, 1.f));
        Check(Near(t->GetForward(), Vector3(0.f, 0.f, 1.f)) && Near(t->GetUp().length(), 1.f),
              "S15", "SetForward straight up is solvable, not NaN");

        // --- SetRight ---
        t->SetRotation(Quaternion::IDENTITY);
        t->SetRight(Vector3::UNIT_X);
        Check(Near(t->GetRight(), Vector3(1.f, 0.f, 0.f)), "S15a", "SetRight aims the right direction");
        Check(Near(t->GetUp(), Vector3(0.f, 0.f, 1.f)), "S15b", "and keeps the up hint at world +Z");
        // 右手系：X=left=-right, Y=-forward, Z=up，且 X×Y=Z。展开后可用的两条是
        //   up × right = forward   和   forward × up = right
        // 写成左手系的话"朝向看着对"但镜像了，位移和旋转都会反，而且极难发现。
        Check(Near(t->GetUp().crossProduct(t->GetRight()), t->GetForward()),
              "S15c", "right/up/forward still form a RIGHT-handed basis");
        Check(Near(t->GetRight().crossProduct(t->GetUp()), t->GetForward()) == false,
              "S15c2", "and the mirrored cross product is NOT accepted (catches a flipped sign)");
        // 退化：right 与 up 平行
        t->SetRotation(Quaternion::IDENTITY);
        t->SetRight(Vector3(0.f, 0.f, 1.f));
        Check(Near(t->GetRight(), Vector3(0.f, 0.f, 1.f)) && Near(t->GetUp().length(), 1.f),
              "S15d", "SetRight parallel to up is solvable, not NaN");

        // --- 欧拉角 ---
        t->SetRotation(Quaternion::IDENTITY);
        t->SetEulerAngles(Vector3(0.f, 0.f, 90.f));
        Check(Near(t->GetForward(), Vector3(1.f, 0.f, 0.f)),
              "S16", "SetEulerAngles(0,0,90) matches RotateAxis(Z, 90)");
        const Vector3 read_back = t->GetEulerAngles();
        Check(Near(read_back, Vector3(0.f, 0.f, 90.f)), "S17", "and GetEulerAngles reads it back");

        t->SetEulerAngles(Vector3(10.f, 20.f, 30.f));
        Check(Near(t->GetEulerAngles(), Vector3(10.f, 20.f, 30.f), 0.01f),
              "S18", "a mixed euler triple round-trips");

        // --- Translate：自身轴 vs 世界轴 ---
        t->SetPosition(Vector3(0.f, 0.f, 0.f));
        t->SetEulerAngles(Vector3(0.f, 0.f, 90.f));   // forward 变成 +X
        t->Translate(Vector3(0.f, -2.f, 0.f));        // 沿自身 -Y（= forward）走 2
        Check(Near(t->GetPosition(), Vector3(2.f, 0.f, 0.f)),
              "S19", "Translate follows the object's OWN axes");

        t->SetPosition(Vector3(0.f, 0.f, 0.f));
        t->TranslateWorld(Vector3(0.f, -2.f, 0.f));
        Check(Near(t->GetPosition(), Vector3(0.f, -2.f, 0.f)),
              "S20", "TranslateWorld ignores the rotation");

        // --- Rotate（自身轴）---
        t->SetPosition(Vector3(0.f, 0.f, 0.f));
        t->SetRotation(Quaternion::IDENTITY);
        t->Rotate(Vector3(0.f, 0.f, 90.f));
        Check(Near(t->GetForward(), Vector3(1.f, 0.f, 0.f)), "S21", "Rotate spins around its own axes");

        // --- RotateAround ---
        t->SetPosition(Vector3(1.f, 0.f, 0.f));
        t->SetRotation(Quaternion::IDENTITY);
        t->RotateAround(Vector3(0.f, 0.f, 0.f), Vector3::UNIT_Z, 90.f);
        Check(Near(t->GetPosition(), Vector3(0.f, 1.f, 0.f)),
              "S22", "RotateAround moves the position around the pivot");
        Check(Near(t->GetForward(), Vector3(1.f, 0.f, 0.f)),
              "S23", "and turns the object too (not a slide that keeps facing one way)");

        // --- LookAt ---
        t->SetPosition(Vector3(0.f, 0.f, 0.f));
        t->SetRotation(Quaternion::IDENTITY);
        t->LookAt(Vector3(0.f, -5.f, 0.f));
        Check(Near(t->GetForward(), Vector3(0.f, -1.f, 0.f)), "S24", "LookAt aims the facing at the target");

        // 退化目标必须"什么都不做"，而不是"重置成单位朝向"。只在前一步的姿态
        // 恰好是单位姿态时，两者看不出区别 —— 所以这里先转一个非单位姿态。
        t->SetEulerAngles(Vector3(0.f, 0.f, 90.f));   // forward = +X
        const Vector3 before_self = t->GetForward();
        Check(Near(before_self, Vector3(1.f, 0.f, 0.f)), "S25a", "(the fixture really is non-identity)");
        t->LookAt(t->GetPosition());
        Check(Near(t->GetForward(), before_self),
              "S25", "LookAt at its own position is a no-op, not NaN");
        Check(Near(t->GetForward(), Vector3(0.f, -1.f, 0.f)) == false,
              "S25b", "and it did NOT silently snap back to the identity orientation");

        // --- 层级 ---
        GameObject* owner_parent = nullptr;
        GameObject* owner_child = nullptr;
        GameObject* owner_grand = nullptr;
        GameObject* owner_sibling = nullptr;
        GameObject* owner_sibling2 = nullptr;
        Transform* parent = MakeTransform(*om, owner_parent);
        Transform* child = MakeTransform(*om, owner_child);
        Transform* grandchild = MakeTransform(*om, owner_grand);
        Transform* sibling = MakeTransform(*om, owner_sibling);
        Transform* sibling2 = MakeTransform(*om, owner_sibling2);
        Check(parent != nullptr && child != nullptr && grandchild != nullptr && sibling != nullptr &&
                  sibling2 != nullptr,
              "S26", "five transforms exist");
        if (parent == nullptr || child == nullptr || grandchild == nullptr || sibling == nullptr ||
            sibling2 == nullptr)
        {
            return;
        }
        child->SetParent(parent, false);
        sibling->SetParent(parent, false);
        sibling2->SetParent(parent, false);
        grandchild->SetParent(child, false);

        Check(grandchild->IsChildOf(parent), "S27", "IsChildOf walks more than one level");
        Check(child->IsChildOf(parent), "S28", "and the direct case");
        Check(!parent->IsChildOf(child), "S29", "a parent is not a child of its child");
        Check(!parent->IsChildOf(nullptr), "S30", "IsChildOf(null) is false");
        // 三个儿子才是"边遍历边改"这个 bug 的**最小**触发条件，两个不够：
        // range-for 的 end() 只在循环开始时求一次值，erase 之后它仍指向旧的第 2 个
        // 槽位；而 erase 只是把后面的元素前移、并不清空尾部字节。于是第二轮恰好
        // 又读到了那个还没解开的儿子 —— 表现"完全正确"。三个儿子时中间那个会被
        // 前移盖掉、彻底跳过。
        Check(parent->GetChildrenCount() == 3, "S31", "the parent has three children");

        parent->DetachChildren();
        Check(parent->GetChildrenCount() == 0, "S32", "DetachChildren empties the parent");
        // 只解一层：孙子仍挂在儿子身上（Unity 同）。
        Check(child->GetChildrenCount() == 1, "S33", "and only one level -- the grandchild stays put");
        Check(!grandchild->IsChildOf(parent), "S34", "the grandchild is no longer under the parent");
        Check(sibling->GetParent() == nullptr, "S35", "and EVERY child was detached, not just the first");
        Check(sibling2->GetParent() == nullptr, "S36", "including the last one");
    }

    // ---------------------------------------------------------------- U ----
    /// 造一个挂在关卡运行时表上的 GameObject（Instantiate 产物的同款登记方式）。
    /// 手工 `new GameObject()` 的 m_Id 是 0，两个对象的 id 会撞，登记进不去。
    GameObject* MakeRuntimeObject(ObjectManager& om, Level& level, Transform** out_transform)
    {
        // GObjectID 必须在构造时给（GameObject 只有带 id 的构造函数），不能事后
        // 设：运行时表按 id 建索引，两个 id=0 的对象会互相覆盖。
        GameObject* go = new GameObject(ObjectIDAllocator::Alloc());
        Transform* transform = new Transform();
        om.AllocateAndAssignInstanceID(go);
        om.AllocateAndAssignInstanceID(transform);
        go->addComponent(transform);
        transform->PostLoadResource(go);
        if (!level.RegisterRuntimeGObject(go))
        {
            return nullptr;
        }
        if (out_transform != nullptr)
        {
            *out_transform = transform;
        }
        return go;
    }

    /// 把要用到的类型注册进 TypeManager。没有它 `Cast<Transform>()` 恒为 nullptr，
    /// 于是 `GameObject::GetTransform()` 返回 nullptr，任何依赖"对象上有
    /// Transform"的断言都不成立 —— 而且失败方式是段错误，不是断言红。
    void RegisterRuntimeTypesForTest()
    {
        TypeManager& type_manager = TypeManager::GetInstance();
        type_manager.Initialize();
        RegisterKlass<Component>();
        RegisterKlass<Transform>();
        RegisterKlass<GameObject>();
        RegisterKlass<Random>();
        type_manager.InitializeAllTypes();
    }

    void TestScenes()
    {
        RegisterRuntimeTypesForTest();
        // --- 没有 WorldManager 时的降级：必须返回空 / false，不能崩 --------
        Check(GET_SYSTEM(WorldManager) == nullptr, "U0", "(the fixture starts with no WorldManager)");
        Check(SceneManager::GetSceneCount() == 0, "U1", "no world -> zero scenes, not a crash");
        Check(SceneManager::GetActiveScene() == nullptr, "U2", "and no active scene");
        Check(SceneManager::GetSceneAt(0) == nullptr, "U3", "GetSceneAt is null");
        Check(SceneManager::GetSceneByName("Main") == nullptr, "U4", "GetSceneByName is null");
        Check(SceneManager::GetSceneByPath("any/where") == nullptr, "U5", "GetSceneByPath is null");
        Check(!SceneManager::IsSceneLoaded("any/where"), "U6", "IsSceneLoaded is false");
        Check(!SceneManager::LoadScene("any/where"), "U7", "LoadScene reports failure");
        Check(!SceneManager::LoadSceneAdditive("any/where"), "U8", "LoadSceneAdditive reports failure");
        Check(!SceneManager::UnloadScene("any/where"), "U9", "UnloadScene reports failure");
        Check(!SceneManager::ReloadActiveScene(), "U10", "ReloadActiveScene reports failure");

        // --- Scene 本身：绑一张真实的 Level --------------------------------
        ObjectManager* om = GET_SYSTEM(ObjectManager);
        Check(om != nullptr, "U11", "ObjectManager is reachable");

        Level level;
        level.setLevelResUrl("Assets/Scenes/Main.level");

        Scene scene;
        Check(!scene.IsValid(), "U12", "an unbound Scene is not valid");
        Check(scene.GetName().empty(), "U13", "and has an empty name (not a crash)");
        Check(scene.GetPath().empty(), "U14", "and an empty path");
        Check(!scene.IsLoaded(), "U15", "and is not loaded");
        Check(scene.GetRootCount() == 0, "U16", "and has no roots");
        Check(scene.GetRootGameObjects().empty(), "U17", "and an empty root list");

        scene.Bind(&level);
        Check(scene.IsValid(), "U18", "binding makes it valid");
        Check(scene.GetName() == "Main", "U19", "the name is the URL stem without the extension");
        Check(scene.GetPath() == "Assets/Scenes/Main.level", "U20", "and the path is the URL");
        Check(!scene.IsLoaded(), "U21", "a Level that was never loaded is not loaded");

        Check(level.LoadObjectGraph({}, false), "U22", "an empty object graph builds");
        Check(scene.IsLoaded(), "U23", "and then the Scene reports loaded");

        // --- 根对象：两张表都要看 ------------------------------------------
        Transform* transform_a = nullptr;
        Transform* transform_b = nullptr;
        GameObject* object_a = MakeRuntimeObject(*om, level, &transform_a);
        GameObject* object_b = MakeRuntimeObject(*om, level, &transform_b);
        Check(object_a != nullptr && object_b != nullptr && transform_a != nullptr && transform_b != nullptr,
              "U24", "two runtime objects are registered");
        if (object_a == nullptr || object_b == nullptr)
        {
            return;
        }

        Check(scene.GetRootCount() == 2, "U25", "both are roots (no parent)");
        Check(scene.GetRootGameObjects().size() == 2, "U26", "and the list agrees with the count");

        object_b->GetTransform()->SetParent(transform_a, false);
        Check(scene.GetRootCount() == 1, "U27", "parenting one under the other leaves one root");
        // 反过来再验一次：只数了"没有父节点"的对象，而不是"表里的对象数"。
        Check(scene.GetRootGameObjects().size() == 1 && scene.GetRootGameObjects().front() == object_a,
              "U28", "and the remaining root is the parent");

        // --- 名字解析的边界 ------------------------------------------------
        Level no_extension;
        no_extension.setLevelResUrl("Assets/Scenes/Boss");
        Scene plain;
        plain.Bind(&no_extension);
        Check(plain.GetName() == "Boss", "U29", "a URL with no extension keeps its stem");

        Level windows_path;
        windows_path.setLevelResUrl("Assets\\Scenes\\Dungeon.level");
        Scene backslash;
        backslash.Bind(&windows_path);
        Check(backslash.GetName() == "Dungeon", "U30", "a backslash separator is handled too");

        // --- SceneManager：装一个 WorldManager ----------------------------
        WorldManager* world = new WorldManager();
        SystemRegistry::GetInstance().RegisterSystem(world);
        Check(world->Initialize(), "U31", "WorldManager initializes");
        Check(SceneManager::GetSceneCount() == 0, "U32", "a fresh world has no loaded scenes");
        Check(SceneManager::GetActiveScene() == nullptr, "U33", "and no active scene");
        Check(SceneManager::GetSceneAt(-1) == nullptr, "U34", "GetSceneAt(-1) is null, not clamped to 0");
        Check(!SceneManager::UnloadScene("Assets/Scenes/Main.level"),
              "U35", "unloading a scene that was never loaded fails");
    }

    // ---------------------------------------------------------------- V ----
    void TestSceneWiring()
    {
        const std::string header = ReadFile("Source/Runtime/Scripting/ScriptScene/ScriptScene.h");
        Check(!header.empty(), "V0", "ScriptScene.h is readable");
        Check(CountOccurrences(header, "class Scene :") >= 1, "V1", "Scene is declared");
        Check(CountOccurrences(header, "class SceneManager :") >= 1, "V2", "SceneManager is declared");
        // Unity 里 name / path / rootCount / isLoaded 是属性。
        Check(CountOccurrences(header, "ZSCRIPTPROPERTY(Name=\"rootCount\"") >= 1,
              "V3", "rootCount is a script property");
        Check(CountOccurrences(header, "ZSCRIPTPROPERTY(Name=\"isLoaded\"") >= 1,
              "V4", "isLoaded is a script property");
        // 没有 buildIndex：ZEngine 没有 Build Settings 场景列表，编一个下标出来
        // 会让脚本依赖上一个语义不同的数字。这条断言守的是"别手滑加上"——
        // 注意头文件**会**在注释里提到 buildIndex（说明为什么没有），所以判的是
        // "没发布"而不是"没出现"。
        Check(CountOccurrences(header, "GetBuildIndex") == 0 &&
                  CountOccurrences(header, "ZSCRIPTPROPERTY(Name=\"buildIndex\"") == 0,
              "V5", "buildIndex is deliberately NOT published");
        // 说明必须有：不加注释的话下一个人会当成遗漏直接补上（然后给个假值）。
        Check(CountOccurrences(header, "Build Settings") >= 1,
              "V6", "and the omission is documented");

        const std::string source = ReadFile("Source/Runtime/Scripting/ScriptScene/ScriptScene.cpp");
        Check(!source.empty(), "V7", "ScriptScene.cpp is readable");
        // 缓存按 Level* 而不是 URL：Level 会被重载，按 URL 缓存会让脚本手里的
        // Scene 一直指向旧的那张。
        Check(CountOccurrences(source, "eastl::unordered_map<Level*, std::unique_ptr<Scene>>") >= 1,
              "V8", "Scene instances are cached per Level*, not per URL");
        // 根对象必须同时看关卡对象表和运行时表 —— 只看前者会漏掉 Instantiate 产物。
        Check(CountOccurrences(source, "getAllGObjects()") >= 1 &&
                  CountOccurrences(source, "GetRuntimeGObjects()") >= 1,
              "V9", "root enumeration looks at BOTH object tables");
        // 卸载活动场景必须被拒绝：那会让引擎进入"没有活动关卡"的状态。
        Check(CountOccurrences(source, "getCurrentActiveLevel()") >= 2,
              "V10", "unloading the ACTIVE scene is refused");

        const std::string world_header = ReadFile("Source/Runtime/Function/Framework/World/WorldManager.h");
        Check(CountOccurrences(world_header, "GetLoadedLevels() const") >= 1,
              "V11", "WorldManager exposes the loaded Level list");
        Check(CountOccurrences(world_header, "GetLoadedLevelByUrl") >= 1,
              "V12", "and a by-URL lookup");

        // 下面两条 headless **行为级测不到**：没有真实关卡文件就永远加载不出
        // 场景，于是 GetSceneAt 的越界分支和 UnloadScene 的"活动场景"分支都进不
        // 去（U34 / U35 只覆盖了"还没有场景"那条早退）。所以退到源码级，但判得
        // 很窄 —— 不是"出现了某个字符串"，而是"禁止的那种写法没出现"。
        //
        // 越界夹紧到第 0 张是最阴的一类：脚本里 `for (i=0..sceneCount)` 写错时
        // 它会让"我以为有 3 张其实只有 1 张"悄悄变成"又拿到第 0 张"。
        Check(CountOccurrences(source, "index >= static_cast<int>(levels.size())") >= 1 &&
                  CountOccurrences(source, "index = 0;") == 0,
              "V14", "GetSceneAt out of range returns null instead of clamping to 0");
        // 卸载活动场景会让引擎进入"没有活动关卡"的状态。
        Check(CountOccurrences(source, "target == world->getCurrentActiveLevel()") >= 1,
              "V15", "UnloadScene refuses the ACTIVE scene");

        const std::string level_header = ReadFile("Source/Runtime/Function/Framework/Level/Level.h");
        Check(CountOccurrences(level_header, "bool IsLoaded() const") >= 1,
              "V13", "Level exposes IsLoaded (Scene.isLoaded reads it)");
    }

    // ---------------------------------------------------------------- W ----
    /// 脚本侧 `Random` 门面的**行为**。
    ///
    /// E 组测的是 `UnityRandom`（实现），这一组测的是脚本真正能叫到的那个名字。
    /// 最关键的一条是两者打在**同一条全局流**上：门面自带一份状态的话，C++ 侧
    /// 摇出来的数和脚本侧摇出来的数会静默分叉，而"回放对不上 / 种子复现不出来"
    /// 过很久才会被怀疑到这上面 —— 两边单看都完全正确。
    void TestRandomFacade()
    {
        // --- 门面是注册过、脚本能按名字找到的类型 ------------------------
        // factory 非空才算数：只声明不注册的话 Type 是 `[UNREGISTERED]` 占位
        //（说明 gen.cpp 根本没链进来），而 ClassNameToType 依然查得到它。
        const Type* random_type = TypeManager::GetInstance().ClassNameToType("Random");
        Check(random_type != nullptr, "W0", "scripts can look the class up by the name 'Random'");
        if (random_type != nullptr)
        {
            Check(std::strcmp(random_type->GetName(), "Random") == 0, "W1",
                  "and it is spelled Random (Unity's spelling, not UnityRandom)");
            Check(random_type->GetFactory() != nullptr, "W2",
                  "and it has a factory, so the generated registrar really is linked in");
        }

        // --- 同一条流 ---------------------------------------------------
        UnityRandom::InitState(12345);
        float via_impl[8];
        for (float& v : via_impl)
        {
            v = UnityRandom::value();
        }
        Random::InitState(12345);
        bool same_stream = true;
        for (float v : via_impl)
        {
            if (Random::value() != v)
            {
                same_stream = false;
            }
        }
        Check(same_stream, "W3",
              "the facade drives the SAME global stream as UnityRandom (not a private one)");

        // 同一条流的另一半：状态也必须是同一份，否则存档/回放只能还原半边。
        UnityRandom::InitState(4242);
        for (int i = 0; i < 3; ++i)
        {
            UnityRandom::value();
        }
        Check(Random::GetState() == static_cast<int64_t>(UnityRandom::GetState()), "W4",
              "GetState() reads the one shared state, not a mirrored copy");

        // --- 可复现（脚本侧自己也要能验证） -----------------------------
        Random::InitState(20240607);
        float first_run[6];
        for (float& v : first_run)
        {
            v = Random::value();
        }
        Random::InitState(20240607);
        bool identical = true;
        for (float v : first_run)
        {
            if (Random::value() != v)
            {
                identical = false;
            }
        }
        Check(identical, "W5", "the same seed reproduces the same sequence through the facade");

        // --- 存档 / 回放：一个 int64 就能续上 ---------------------------
        Random::InitState(31337);
        for (int i = 0; i < 5; ++i)
        {
            Random::value();
        }
        const int64_t saved = Random::GetState();
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
        Check(restored, "W6", "SetState() resumes the stream exactly through the facade");

        // --- value() ----------------------------------------------------
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
        Check(in_range, "W7", "value() stays in [0, 1) over 10000 draws");

        // --- int 不含 max / float 含 max -------------------------------
        Random::InitState(99);
        int int_min = 1000;
        int int_max = -1000;
        for (int i = 0; i < 20000; ++i)
        {
            const int v = Random::RangeInt(0, 10);
            int_min = (v < int_min) ? v : int_min;
            int_max = (v > int_max) ? v : int_max;
        }
        Check(int_min == 0, "W8", "RangeInt(0, 10) reaches 0");
        Check(int_max == 9, "W9", "RangeInt(0, 10) NEVER reaches 10 -- the int max is exclusive");
        // 偏移非零的区间：只测 [0,10) 的话，"忘了加 min"这种错看不出来。
        Random::InitState(100);
        bool negative_range_ok = true;
        int neg_min = 1000;
        int neg_max = -1000;
        for (int i = 0; i < 20000; ++i)
        {
            const int v = Random::RangeInt(-7, -4);
            if (v < -7 || v >= -4)
            {
                negative_range_ok = false;
            }
            neg_min = (v < neg_min) ? v : neg_min;
            neg_max = (v > neg_max) ? v : neg_max;
        }
        Check(negative_range_ok && neg_min == -7 && neg_max == -5, "W10",
              "RangeInt(-7, -4) covers [-7, -5] -- the offset is applied, max excluded");
        Check(Random::RangeInt(5, 5) == 5, "W11", "an empty int range returns min");
        Check(Random::RangeInt(7, 2) == 7, "W12", "an inverted int range returns min");

        Random::InitState(101);
        bool float_in_range = true;
        float float_max = -1000.0f;
        for (int i = 0; i < 20000; ++i)
        {
            const float v = Random::RangeFloat(2.0f, 3.0f);
            float_max = (v > float_max) ? v : float_max;
            if (v < 2.0f || v > 3.0f)
            {
                float_in_range = false;
            }
        }
        Check(float_in_range, "W13", "RangeFloat(2, 3) stays inside both bounds");
        Check(float_max > 2.99f, "W14", "RangeFloat can get arbitrarily close to 3 -- max is INCLUSIVE");
        // 同一个偏移区间换浮点：能取到 -4，和 W10 的整数版正好对上，把不对称钉死。
        Random::InitState(102);
        bool float_negative_ok = true;
        for (int i = 0; i < 20000; ++i)
        {
            const float v = Random::RangeFloat(-7.0f, -4.0f);
            if (v < -7.0f || v > -4.0f)
            {
                float_negative_ok = false;
            }
        }
        Check(float_negative_ok, "W15", "RangeFloat(-7, -4) stays inside both bounds");

        // --- Chance -----------------------------------------------------
        Random::InitState(3);
        Check(!Random::Chance(0.0f), "W16", "Chance(0) is never true");
        Check(Random::Chance(1.0f), "W17", "Chance(1) is always true");
        Check(!Random::Chance(-0.5f), "W18", "a negative probability is never true");
        Check(Random::Chance(2.0f), "W19", "a probability above 1 is always true");
        Random::InitState(4);
        int trues = 0;
        for (int i = 0; i < 20000; ++i)
        {
            if (Random::Chance(0.25f))
            {
                ++trues;
            }
        }
        Check(trues > 4700 && trues < 5300, "W20", "Chance(0.25) fires about a quarter of the time");

        // --- 形状 -------------------------------------------------------
        Random::InitState(555);
        bool sphere_inside = true;
        double sphere_sq_sum = 0.0;
        for (int i = 0; i < 20000; ++i)
        {
            const float sq = Random::insideUnitSphere().squaredLength();
            sphere_sq_sum += sq;
            if (sq > 1.0f)
            {
                sphere_inside = false;
            }
        }
        Check(sphere_inside, "W21", "insideUnitSphere() never leaves the unit ball");
        // **按体积**均匀，不是按半径。E[r²] = 3/5 = 0.6；"随机方向 × 随机半径"
        // 那种写法 E[r²] = 1/3 —— 它在球心堆点，一用来散布生成点就能看出来，
        // 但单看"有没有出球"完全测不出（W21 照样绿）。
        const double sphere_mean_sq = sphere_sq_sum / 20000.0;
        Check(sphere_mean_sq > 0.55 && sphere_mean_sq < 0.65, "W22",
              "insideUnitSphere() is uniform BY VOLUME (mean r^2 is 3/5, not 1/3)");

        bool on_sphere = true;
        for (int i = 0; i < 2000; ++i)
        {
            if (std::fabs(Random::onUnitSphere().length() - 1.0f) > 0.01f)
            {
                on_sphere = false;
            }
        }
        Check(on_sphere, "W23", "onUnitSphere() is on the surface (length 1)");

        bool circle_inside = true;
        double circle_sq_sum = 0.0;
        for (int i = 0; i < 20000; ++i)
        {
            const float sq = Random::insideUnitCircle().squaredLength();
            circle_sq_sum += sq;
            if (sq > 1.0f)
            {
                circle_inside = false;
            }
        }
        Check(circle_inside, "W24", "insideUnitCircle() never leaves the unit disc");
        // 二维同理：E[r²] = 1/2（按面积），按半径则是 1/3。
        const double circle_mean_sq = circle_sq_sum / 20000.0;
        Check(circle_mean_sq > 0.46 && circle_mean_sq < 0.54, "W25",
              "insideUnitCircle() is uniform BY AREA (mean r^2 is 1/2, not 1/3)");

        bool on_circle = true;
        for (int i = 0; i < 2000; ++i)
        {
            if (std::fabs(Random::onUnitCircle().length() - 1.0f) > 0.01f)
            {
                on_circle = false;
            }
        }
        Check(on_circle, "W26", "onUnitCircle() is on the circle (length 1)");

        // --- rotation ---------------------------------------------------
        Random::InitState(777);
        bool unit_rotation = true;
        double abs_w_sum = 0.0;
        for (int i = 0; i < 4000; ++i)
        {
            const Quaternion q = Random::rotation();
            const float sq = q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z;
            if (std::fabs(sq - 1.0f) > 0.01f)
            {
                unit_rotation = false;
            }
            abs_w_sum += std::fabs(q.w);
        }
        Check(unit_rotation, "W27", "rotation() returns a unit quaternion");
        // 均匀分布在 SO(3) 上：|w| 的期望是 4/(3*pi) ≈ 0.424。
        //   * 恒等四元数恒等于 1（"根本没随机"）
        //   * "随机三个欧拉角"会在万向节极点附近过采样，期望明显偏离
        // 只测"是单位四元数"两种都测不出来。
        const double mean_abs_w = abs_w_sum / 4000.0;
        Check(mean_abs_w > 0.39 && mean_abs_w < 0.46, "W28",
              "rotation() is uniform over SO(3) (mean |w| is 4/(3*pi), not 1)");

        // --- InitUnseeded -----------------------------------------------
        Random::InitState(1);
        const int64_t seeded_state = Random::GetState();
        Random::InitUnseeded();
        Check(Random::GetState() != seeded_state, "W29", "InitUnseeded() moves off the seeded state");
        Check(Random::GetState() != 0, "W30", "and never leaves the state at 0");
        const float unseeded = Random::value();
        Check(unseeded >= 0.0f && unseeded < 1.0f, "W31", "and the stream it starts is still in [0, 1)");
    }

    // ---------------------------------------------------------------- X ----
    void TestRandomWiring()
    {
        const std::string header = ReadFile("Source/Runtime/Scripting/ScriptRandom/ScriptRandom.h");
        Check(!header.empty(), "X0", "ScriptRandom.h is readable");
        Check(CountOccurrences(header, "class Random :") >= 1, "X1", "the script-facing class is declared");
        Check(CountOccurrences(header, "ZFUNCTION()") >= 13, "X2", "all 13 entry points carry ZFUNCTION()");
        // 全静态 ⇒ 一个属性都发布不出来（ZSCRIPTPROPERTY 的 getter 不允许
        // static）。守的是"别有人顺手加一个"：它会在 codegen 期炸，而报错指向
        // 的是**生成**文件，看源码的人会一头雾水。
        // 注意带括号：`ZSCRIPTPROPERTY` 这个词在**注释**里也出现（说明为什么
        // 发布不出来），只搜裸词会永远为真，等于没测。
        Check(CountOccurrences(header, "ZSCRIPTPROPERTY(") == 0, "X3",
              "no script properties -- every entry point is static");
        Check(CountOccurrences(header, "RangeInt") >= 2, "X4", "RangeInt is declared AND documented");
        Check(CountOccurrences(header, "RangeFloat") >= 2, "X5", "RangeFloat is declared AND documented");
        // 不对称必须**复刻并且写明**。缺了说明，下一个人会当 bug "修"掉 —— 那会
        // 静默改变每一个移植过来的脚本摇出来的数，而且没有任何编译错误。
        Check(CountOccurrences(header, "max 不含") >= 1 && CountOccurrences(header, "max 含") >= 1,
              "X6", "the int-excludes / float-includes max asymmetry is documented");
        Check(CountOccurrences(header, "UnityRandom") >= 2, "X7",
              "the header points at the real implementation (this file is only a name)");

        const std::string source = ReadFile("Source/Runtime/Scripting/ScriptRandom/ScriptRandom.cpp");
        Check(!source.empty(), "X8", "ScriptRandom.cpp is readable");
        // 门面必须**纯转发**：在这里掺进任何自己的算法，C++ 侧和脚本侧立刻分叉，
        // 而且两边单看都对。13 个方法 = 13 次转发。
        Check(CountOccurrences(source, "UnityRandom::") >= 13, "X9",
              "every facade method forwards to UnityRandom");
        // 门面里不许出现第二个随机源 —— 那就是分叉的入口。
        Check(CountOccurrences(source, "<random>") == 0 && CountOccurrences(source, "std::mt19937") == 0 &&
                  CountOccurrences(source, "std::rand") == 0,
              "X10", "the facade brings in no RNG of its own");

        // 实现是**改名**过的：原来就叫 `class Random`，既一个 ZFUNCTION 都没有
        //（脚本根本用不了）又不派生自 Object（挂不上 ZCLASS）。留一条断言防止
        // 改回去 —— 两个同名类属于 ODR 违规，症状是随机崩而不是编译错。
        const std::string impl_header = ReadFile("Source/Runtime/Core/Math/UnityRandom.h");
        Check(!impl_header.empty(), "X11", "UnityRandom.h is readable");
        Check(CountOccurrences(impl_header, "class UnityRandom") >= 1, "X12",
              "the implementation class is UnityRandom");
        Check(CountOccurrences(impl_header, "class Random") == 0, "X13",
              "and is NOT also called Random (two same-named classes is an ODR violation)");
        const std::string impl_source = ReadFile("Source/Runtime/Core/Math/UnityRandom.cpp");
        Check(CountOccurrences(impl_source, "UnityRandom::s_State") >= 1, "X14",
              "the single global state lives on UnityRandom");
    }

    // ---------------------------------------------------------------- T ----
    void TestTransformWiring()
    {
        const std::string header = ReadFile("Source/Runtime/Function/Framework/Component/Transform/Transform.h");
        Check(!header.empty(), "T0", "Transform.h is readable");
        // 轴向约定必须写在头文件里：这是最容易"A 组按 -Y 写、B 组按 +Z 用"的一处，
        // 而且两边都编译得过。
        Check(CountOccurrences(header, "NEGATIVE_UNIT_Y") >= 1, "T1", "forward is documented as -Y");
        Check(CountOccurrences(header, "Z-up") >= 1, "T2", "and the engine is documented as Z-up");
        Check(CountOccurrences(header, "GetForward") >= 2, "T3", "GetForward is declared AND commented");
        Check(CountOccurrences(header, "SetForward") >= 1, "T4", "SetForward is declared");
        Check(CountOccurrences(header, "LookAt") >= 1, "T5", "LookAt is declared");
        Check(CountOccurrences(header, "RotateAround") >= 1, "T6", "RotateAround is declared");
        Check(CountOccurrences(header, "GetEulerAngles") >= 1, "T7", "euler angles are exposed");
        Check(CountOccurrences(header, "DetachChildren") >= 1, "T8", "DetachChildren is declared");

        const std::string source = ReadFile("Source/Runtime/Function/Framework/Component/Transform/Transform.cpp");
        Check(!source.empty(), "T9", "Transform.cpp is readable");
        // DetachChildren 必须先把子节点抄一份：SetParent 会改动 m_Children，
        // 边遍历边改会跳过元素（"只解开了一半"）。
        Check(CountOccurrences(source, "std::vector<PPtr<Transform>> children = m_Children;") >= 1,
              "T10", "DetachChildren iterates a COPY of the child list");
        // 绕世界轴 = 左乘，绕自身轴 = 右乘。写反了表现是"物体绕着奇怪的轴转"，
        // 而单看某一帧看不出来。
        Check(CountOccurrences(source, "delta * GetRotation()") >= 2,
              "T11", "world-axis rotation multiplies on the left");
        Check(CountOccurrences(source, "GetLocalRotation() * EulerToQuaternion") >= 1,
              "T12", "self-axis rotation multiplies on the right");
        Check(CountOccurrences(source, "isZeroLength()") >= 3,
              "T13", "degenerate axes / zero-length look targets are guarded");

        // Unity 里 forward/up/right/eulerAngles 是**属性**（`t.forward = v`），
        // 不是方法。只导出 GetForward/SetForward 的话脚本得写 t.SetForward(v)，
        // 而且 t.forward 在 .d.ts 里根本不存在 —— tsc 直接报"属性不存在"。
        Check(CountOccurrences(header, "ZSCRIPTPROPERTY(Name=\"forward\"") >= 1,
              "T14", "forward is published as a script PROPERTY, not just a method");
        Check(CountOccurrences(header, "ZSCRIPTPROPERTY(Name=\"eulerAngles\"") >= 1,
              "T15", "eulerAngles is published as a script property");
        Check(CountOccurrences(header, "ZSCRIPTPROPERTY(Name=\"childCount\"") >= 1,
              "T16", "childCount is published as a script property");
        Check(CountOccurrences(header, "ZSCRIPTPROPERTY(Name=\"parent\"") >= 1,
              "T17", "parent is readable as a script property");
        Check(CountOccurrences(header, "SetRight") >= 2,
              "T18", "SetRight is declared AND documented (Unity's transform.right is writable)");
        // GetChildrenCount 同时是属性 childCount 的 getter：它必须是 const 的
        // ZFUNCTION，否则 ZHeaderTool 找不到匹配的访问器（生成期报错）。
        Check(CountOccurrences(header, "int GetChildrenCount() const") >= 1,
              "T19", "the childCount getter is the const ZFUNCTION one");
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

    // Behaviour.Invoke resolves its timer through GET_SYSTEM(CoroutineScheduler),
    // so the registry has to hold one -- the coroutine group ticks a local
    // instance and never publishes it.
    CoroutineScheduler* invoke_scheduler = new CoroutineScheduler();
    SystemRegistry::GetInstance().RegisterSystem(invoke_scheduler);
    Check(invoke_scheduler->Initialize(), "A0d", "the registered coroutine scheduler is initialized");

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
    TestInvokes();
    TestInvokeWiring();
    // 顺序有讲究：TestScreenFacade 的 M0 断言"WindowSystem 还没注册"，所以它
    // 必须跑在 TestApplicationFacade（会注册一堆系统）之前。
    TestScreenFacade();
    TestApplicationFacade();
    TestApplicationScreenWiring();
    TestDebugFacade();
    TestContactEvents();
    TestContactEventWiring();
    TestTransformDirections();
    TestTransformWiring();
    TestScenes();
    TestSceneWiring();
    TestRandomFacade();
    TestRandomWiring();
    TestWiringGate();

    std::printf("\n=== %d checks, %d failures ===\n", g_Checks, g_Failures);
    return (g_Failures == 0) ? 0 : 1;
}
