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
#include "Runtime/Function/AI/Navigation/NavMeshAgentComponent.h"
#include "Runtime/Function/AI/Navigation/NavigationSystem.h"
#include "Runtime/Function/Audio/AudioDecoder.h"
#include "Runtime/Function/Audio/AudioSystem.h"
#include "Runtime/Function/Framework/Component/Audio/AudioSourceComponent.h"
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
#include "Runtime/Function/Framework/Component/Rigidbody/RigidbodyComponent.h"
#include "Runtime/Function/Framework/Component/Motor/MotorComponent.h"
#include "Runtime/Function/Controller/CharacterController.h"
#include "Runtime/Function/Physics/PhysicsScene.h"
#include "Runtime/Resource/ResType/Components/RigidBody.h"
#include "Runtime/Function/PlayerSettings/PlayerSettings.h"
#include "Runtime/Function/Render/WindowSystem.h"
#include "Runtime/Project/ProjectInfo.h"
#include "Runtime/Resource/Config/ConfigManager.h"
#include "Runtime/Function/Render/RenderCamera.h"
#include "Runtime/Function/Render/RenderSystem.h"
#include "Runtime/Scripting/ScriptCamera/ScriptCamera.h"
#include "Runtime/Scripting/ScriptMath/ScriptMath.h"
#include "Runtime/Scripting/ScriptCollider/ScriptCollider.h"
#include "Runtime/Scripting/ScriptPlayerPrefs/ScriptPlayerPrefs.h"
#include "Runtime/Resource/UserPreferences/UserPreferences.h"
#include "Runtime/Resource/ResType/Data/BasicShape.h"
#include "Runtime/Scripting/ScriptDebug/ScriptDebug.h"
#include "Runtime/Scripting/ScriptRandom/ScriptRandom.h"
#include "Runtime/Scripting/ScriptScene/ScriptScene.h"
#include "Runtime/Scripting/ScriptNavigation/ScriptNavMesh.h"
#include "Runtime/Scripting/ScriptResources/ScriptResources.h"
// `Runtime::Color` 是 Color 的脚本门面（值类型仍是全局的 `::Color`），所以本文件
// 里凡指值类型一律写 `::Color`，避免与门面混淆。
#include "Runtime/Scripting/ScriptColor/ScriptColor.h"
#include "Runtime/Scripting/ScriptVector/ScriptVector.h"
#include "Runtime/Resource/Asset/RuntimeAssetManager.h"
#include "Runtime/File/FileSystem.h"
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
template<> void RegisterKlass<Camera>();
template<> void RegisterKlass<Ray>();
// 角色控制器那组要 `Cast<Capsule>`：`PhysicsScene::IsOverlap` 走 toShape()，而
// toShape 按运行时类型分发。`TypeOf<Capsule>()` 的实体在 Capsule.gen.cpp 里，
// 本 exe 不编 .gen.cpp（/WHOLEARCHIVE 会把它带进来），但 RegisterKlass 必须显式
// 调一次 —— 否则 TypeContainer<Capsule>::m_Type 还是空，Cast 一解引用就崩。
template<> void RegisterKlass<Capsule>();
// Collider 门面要 `Cast<Box/Sphere/Capsule>`（读几何）和 `Cast<RigidBodyComponent>`
// （找形状挂在哪），同样需要显式注册一次，否则 TypeContainer 是空的。
template<> void RegisterKlass<Box>();
template<> void RegisterKlass<Sphere>();
template<> void RegisterKlass<Geometry>();
template<> void RegisterKlass<RigidBodyShape>();
template<> void RegisterKlass<RigidBodyComponent>();
template<> void RegisterKlass<Collider>();
template<> void RegisterKlass<PlayerPrefs>();

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

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

    /// 一块直投 Jolt 的盒子，带显式 Unity 层。不走 RigidBodyComponentRes 是为了
    /// 不把对象 / 反射层拖进来；那条路多出来的层管线由 I 组覆盖。
    ///
    /// 半尺寸可调：角色控制器那组需要**比角色大得多**的地板和墙 —— 0.5 的固定
    /// 半尺寸下，角色往旁边走两步就出了地板边缘，于是"没被挡住"和"地板太短"
    /// 分不出来。
    uint32_t SpawnBoxSized(PhysicsScene& in_scene,
                           const Vector3& in_position,
                           const Vector3& in_half_extents,
                           int32_t in_layer,
                           bool in_dynamic)
    {
        JPH::BodyInterface& body_interface = in_scene.GetJoltPhysicsSystem()->GetBodyInterface();

        JPH::BoxShapeSettings box_settings(toVec3(in_half_extents));
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

    /// 一个 1x1x1 的盒子。I / H 两组只关心层与矩阵，尺寸无关紧要。
    uint32_t SpawnBox(PhysicsScene& in_scene, const Vector3& in_position, int32_t in_layer, bool in_dynamic)
    {
        return SpawnBoxSized(in_scene, in_position, Vector3(0.5f, 0.5f, 0.5f), in_layer, in_dynamic);
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

    bool Near(const Vector4& a, const Vector4& b, float tolerance = 1.0e-3f)
    {
        return std::fabs(a.x - b.x) < tolerance && std::fabs(a.y - b.y) < tolerance &&
               std::fabs(a.z - b.z) < tolerance && std::fabs(a.w - b.w) < tolerance;
    }

    /// 矩阵按元素比。逐元素而不是先合成一个范数：范数相近但某一项符号反了的矩阵
    /// （例如 y 翻了）会被"整体大小差不多"放过去，而那正是这里要抓的错。
    bool Near(const Matrix4x4& a, const Matrix4x4& b, float tolerance = 1.0e-3f)
    {
        for (int row = 0; row < 4; ++row)
        {
            for (int column = 0; column < 4; ++column)
            {
                if (std::fabs(a.m_Mat[row][column] - b.m_Mat[row][column]) >= tolerance)
                {
                    return false;
                }
            }
        }
        return true;
    }

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
        RegisterKlass<Camera>();
        RegisterKlass<Ray>();
        RegisterKlass<Capsule>();
        RegisterKlass<Box>();
        RegisterKlass<Sphere>();
        RegisterKlass<Geometry>();
        RegisterKlass<RigidBodyShape>();
        RegisterKlass<RigidBodyComponent>();
        RegisterKlass<Collider>();
        RegisterKlass<PlayerPrefs>();
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

    // ---------------------------------------------------------------- Y ----
    /// 脚本侧 `Camera` / `Ray` 的行为。
    ///
    /// 这一组最要紧的是**屏幕坐标的朝向**和 **z 的含义**。两处写反都编译得过、
    /// 跑得起来，表现分别是"点哪都点反"和"只在画面边缘错"。
    ///
    /// 夹具：一台摆在原点、朝 +Y 看、up = +Z 的相机（`RenderCamera` 自己的基是
    /// X=right / Y=forward / Z=up）。near 0.1 / far 1000 / aspect 16:9 / 垂直
    /// fov 60° —— 注意 `RenderCamera` 的默认值是 near=1000、far=0.1（**反的**），
    /// 不显式设的话投影出来的深度是反的，而画面照常能出。
    /// RenderCamera 不可拷贝（ZCLASS 的 Object 基类删了拷贝构造），所以按引用
    /// 填，不能按值返回。
    void MakeRenderCamera(RenderCamera& rc)
    {
        rc.m_Znear = 0.1f;
        rc.m_Zfar = 1000.0f;
        rc.SetAspect(16.0f / 9.0f);
        rc.setFOVy(60.0f);
        rc.LookAt(Vector3(0.f, 0.f, 0.f), Vector3(0.f, 10.f, 0.f), Vector3(0.f, 0.f, 1.f));
    }

    void TestCameraFacade()
    {
        // --- 门面是注册过、脚本能按名字找到的类型 ------------------------
        const Type* camera_type = TypeManager::GetInstance().ClassNameToType("Camera");
        Check(camera_type != nullptr && camera_type->GetFactory() != nullptr, "Y0",
              "scripts can look up a factored class named 'Camera'");
        const Type* ray_type = TypeManager::GetInstance().ClassNameToType("Ray");
        Check(ray_type != nullptr && ray_type->GetFactory() != nullptr, "Y1",
              "and one named 'Ray'");

        // --- 没有 RenderSystem 时的降级：null / 0 / 空，不是崩 -----------
        Check(GET_SYSTEM(RenderSystem) == nullptr, "Y2", "(the fixture has no RenderSystem)");
        Check(Camera::GetMain() == nullptr, "Y3", "no render system -> GetMain() is null");
        Check(Camera::GetCount() == 0, "Y4", "and zero cameras");
        Check(Camera::GetCameraAt(0) == nullptr, "Y5", "GetCameraAt(0) is null");
        Check(Camera::GetCameraAt(-1) == nullptr, "Y6", "GetCameraAt(-1) is null");
        Check(Camera::GetAllCameras().empty(), "Y7", "and the list is empty");

        // --- 未绑定的 Camera：一切返回"空"，不是未定义 -------------------
        Camera unbound;
        Check(!unbound.IsValid(), "Y8", "an unbound Camera is not valid");
        Check(Near(unbound.GetFieldOfView(), 0.0f), "Y9", "and reads 0 fov (not a stale value)");
        Check(Near(unbound.GetAspect(), 0.0f), "Y10", "and 0 aspect");
        Check(Near(unbound.WorldToScreenPoint(Vector3::UNIT_Z), Vector3::ZERO, 1.0e-6f), "Y11",
              "WorldToScreenPoint returns zero rather than NaN");
        Check(unbound.ScreenPointToRay(Vector2(0.5f, 0.5f)) == nullptr, "Y12",
              "ScreenPointToRay returns null");
        Check(unbound.GetPixelWidth() > 0 && unbound.GetPixelHeight() > 0, "Y13",
              "pixel size falls back to the screen size");

        // --- 绑上一台真实的 RenderCamera --------------------------------
        RenderCamera rc;
        MakeRenderCamera(rc);
        Camera cam;
        cam.Bind(&rc);
        Check(cam.IsValid(), "Y14", "binding makes the Camera valid");
        Check(cam.GetPixelWidth() == Screen::GetWidth() && cam.GetPixelHeight() == Screen::GetHeight(),
              "Y15", "pixel size is the screen size");

        // --- 投影参数 ----------------------------------------------------
        // 垂直 fov：60 进 60 出。返回水平 fov 的话这里会是 91.5 —— 而 Unity 的
        // fieldOfView 是垂直的，移植过来的脚本算视锥高度时会整个算错。
        Check(Near(cam.GetFieldOfView(), 60.0f, 0.01f), "Y16", "fieldOfView is the VERTICAL fov");
        Check(std::fabs(cam.GetFieldOfView() - 91.5f) > 1.0f, "Y17",
              "and is not the horizontal one (91.5 at 16:9)");
        // 改 fov 必须真的改到投影上。只写 m_Fovy 的话下一次 SetAspect 会把它盖
        // 掉，读出来还是 60，但画面纹丝不动 —— 静默的错行为。
        const Vector3 before_fov = cam.WorldToScreenPoint(Vector3(0.f, 10.f, 1.f));
        cam.SetFieldOfView(30.0f);
        Check(Near(cam.GetFieldOfView(), 30.0f, 0.01f), "Y18", "SetFieldOfView(30) reads back 30");
        const Vector3 after_fov = cam.WorldToScreenPoint(Vector3(0.f, 10.f, 1.f));
        // 视场**变窄**，同一个世界偏移量在屏幕上占的比例反而**变大**（60° 时偏
        // 62px，30° 时偏 134px）。方向写反的断言会通过"没变"的实现，所以这里连
        // 大小一起判。
        Check(std::fabs(after_fov.y - static_cast<float>(cam.GetPixelHeight()) * 0.5f) >
                  std::fabs(before_fov.y - static_cast<float>(cam.GetPixelHeight()) * 0.5f) + 1.0f,
              "Y19", "and the projection actually narrowed (a narrower fov pushes the point OUT)");
        cam.SetFieldOfView(60.0f);

        cam.SetNearClipPlane(0.5f);
        Check(Near(cam.GetNearClipPlane(), 0.5f, 1.0e-6f), "Y20", "near clip round-trips");
        cam.SetNearClipPlane(0.0f);
        Check(cam.GetNearClipPlane() > 0.0f, "Y21",
              "a zero near clip is clamped, not accepted (w would be 0 -> NaN everywhere)");
        cam.SetNearClipPlane(0.1f);
        cam.SetFarClipPlane(500.0f);
        Check(Near(cam.GetFarClipPlane(), 500.0f, 1.0e-6f), "Y22", "far clip round-trips");
        cam.SetFarClipPlane(0.05f);
        Check(cam.GetFarClipPlane() > cam.GetNearClipPlane(), "Y23",
              "and a far clip below near is pushed above it (inverted depth flips the scene silently)");
        cam.SetFarClipPlane(1000.0f);

        cam.SetAspect(1.0f);
        Check(Near(cam.GetAspect(), 1.0f, 1.0e-6f), "Y24", "aspect round-trips");
        cam.SetAspect(16.0f / 9.0f);

        Check(!cam.GetOrthographic(), "Y25", "the fixture is a perspective camera");
        cam.SetOrthographicSize(7.5f);
        Check(Near(cam.GetOrthographicSize(), 7.5f, 1.0e-6f), "Y26", "orthographicSize round-trips");
        cam.SetOrthographic(true);
        Check(cam.GetOrthographic(), "Y27", "and orthographic round-trips");
        cam.SetOrthographic(false);

        // --- 屏幕坐标的朝向 ----------------------------------------------
        const float w = static_cast<float>(cam.GetPixelWidth());
        const float h = static_cast<float>(cam.GetPixelHeight());
        const Vector3 centre = cam.WorldToScreenPoint(Vector3(0.f, 10.f, 0.f));
        Check(Near(centre.x, w * 0.5f, 0.5f) && Near(centre.y, h * 0.5f, 0.5f), "Y28",
              "a point straight ahead lands in the middle of the screen");
        // 世界里的"上"必须落在**更大**的屏幕 y 上（Unity 的屏幕原点在左下角）。
        // 反了的话 ScreenPointToRay 会点天花板选中地板，而且不报错。
        const Vector3 above = cam.WorldToScreenPoint(Vector3(0.f, 10.f, 1.f));
        Check(above.y > h * 0.5f + 1.0f, "Y29",
              "world up maps to a LARGER screen y (bottom-left origin)");
        Check(Near(above.x, w * 0.5f, 0.5f), "Y30", "and does not move sideways");
        const Vector3 right = cam.WorldToScreenPoint(Vector3(1.f, 10.f, 0.f));
        Check(right.x > w * 0.5f + 1.0f, "Y31", "camera right maps to a larger screen x");
        Check(Near(right.y, h * 0.5f, 0.5f), "Y32", "and does not move vertically");

        // --- z 是"沿视线的深度"，不是欧氏距离 ----------------------------
        Check(Near(centre.z, 10.0f, 0.01f), "Y33", "z is the depth straight ahead");
        const Vector3 twice = cam.WorldToScreenPoint(Vector3(0.f, 20.f, 0.f));
        Check(Near(twice.z, 20.0f, 0.01f), "Y34", "and doubles with the distance");
        // 按欧氏距离取 z 的话下面这条会在**画面边缘**错（正中间两者相等，只测中
        // 间就漏了）：点在 (6, 10, 4)，欧氏距离 7.2，投影到视线方向是 10。
        const Vector3 off_axis = cam.WorldToScreenPoint(Vector3(6.f, 10.f, 4.f));
        Check(Near(off_axis.z, 10.0f, 0.01f), "Y35",
              "off-centre: z is the depth ALONG the view direction, not the euclidean distance");

        // --- 往返 --------------------------------------------------------
        // 正中间一个点测不出"深度按投影还是按欧氏距离"，必须带上偏心点。
        const Vector3 samples[3] = {Vector3(0.f, 10.f, 0.f), Vector3(6.f, 10.f, 4.f),
                                    Vector3(-3.f, 25.f, -2.f)};
        bool round_trips = true;
        for (const Vector3& p : samples)
        {
            const Vector3 back = cam.ScreenToWorldPoint(cam.WorldToScreenPoint(p));
            if (!Near(back, p, 0.05f))
            {
                round_trips = false;
            }
        }
        Check(round_trips, "Y36",
              "ScreenToWorldPoint inverts WorldToScreenPoint (incl. off-centre)");

        // --- 视口坐标 ----------------------------------------------------
        const Vector3 vp = cam.WorldToViewportPoint(Vector3(0.f, 10.f, 0.f));
        Check(Near(vp.x, 0.5f, 1.0e-3f) && Near(vp.y, 0.5f, 1.0e-3f), "Y37",
              "the centre is viewport (0.5, 0.5)");
        const Vector3 vp_above = cam.WorldToViewportPoint(Vector3(0.f, 10.f, 1.f));
        Check(vp_above.y > 0.5f && vp_above.y <= 1.0f, "Y38", "and world up is above 0.5");
        bool viewport_round_trips = true;
        for (const Vector3& p : samples)
        {
            const Vector3 back = cam.ViewportToWorldPoint(cam.WorldToViewportPoint(p));
            if (!Near(back, p, 0.05f))
            {
                viewport_round_trips = false;
            }
        }
        Check(viewport_round_trips, "Y39", "ViewportToWorldPoint inverts WorldToViewportPoint");
        // 视口 = 屏幕 / 尺寸，两条路径必须给出同一个世界点。
        const Vector3 via_screen = cam.ScreenToWorldPoint(Vector3(vp.x * w, vp.y * h, vp.z));
        const Vector3 via_viewport = cam.ViewportToWorldPoint(vp);
        Check(Near(via_screen, via_viewport, 0.05f), "Y40", "screen and viewport paths agree");

        // --- 射线 --------------------------------------------------------
        Ray* centre_ray = cam.ViewportPointToRay(Vector2(0.5f, 0.5f));
        Check(centre_ray != nullptr, "Y41", "ViewportPointToRay returns a ray");
        if (centre_ray == nullptr)
        {
            return;
        }
        Check(Near(centre_ray->GetOrigin(), rc.position(), 1.0e-3f), "Y42",
              "the perspective ray starts at the camera position");
        Check(Near(centre_ray->GetDirection(), rc.forward(), 1.0e-3f), "Y43",
              "and the centre ray points along the camera's forward");
        Check(Near(centre_ray->GetDirection().length(), 1.0f, 1.0e-3f), "Y44",
              "the direction is normalized");
        Check(Near(centre_ray->GetPoint(10.0f), Vector3(0.f, 10.f, 0.f), 0.05f), "Y45",
              "Ray.GetPoint(distance) walks along the ray");
        // 右上角：x 朝右、z 朝上。这里翻一次就点反了。
        Ray* corner_ray = cam.ViewportPointToRay(Vector2(0.9f, 0.9f));
        Check(corner_ray != nullptr && corner_ray->GetDirection().x > 0.0f &&
                  corner_ray->GetDirection().z > 0.0f,
              "Y46", "the top-right viewport corner aims right and up");
        // 正交相机没有"会聚点"：起点必须落在近平面上，否则整条射线偏出视锥。
        cam.SetOrthographic(true);
        Ray* ortho_ray = cam.ViewportPointToRay(Vector2(0.5f, 0.5f));
        Check(ortho_ray != nullptr && !Near(ortho_ray->GetOrigin(), rc.position(), 1.0e-3f) &&
                  Near(ortho_ray->GetDirection(), rc.forward(), 1.0e-3f),
              "Y47",
              "an orthographic ray starts off the camera position and keeps the forward direction");
        cam.SetOrthographic(false);
        // 屏幕路径和视口路径给出同一条射线。
        Ray* via_screen_ray = cam.ScreenPointToRay(Vector2(w * 0.5f, h * 0.5f));
        Check(via_screen_ray != nullptr && Near(via_screen_ray->GetDirection(), rc.forward(), 1.0e-3f),
              "Y48", "ScreenPointToRay at the screen centre matches the viewport ray");
    }

    // ---------------------------------------------------------------- Z ----
    void TestCameraWiring()
    {
        const std::string header = ReadFile("Source/Runtime/Scripting/ScriptCamera/ScriptCamera.h");
        Check(!header.empty(), "Z0", "ScriptCamera.h is readable");
        Check(CountOccurrences(header, "class Camera :") >= 1, "Z1", "Camera is declared");
        Check(CountOccurrences(header, "class Ray :") >= 1, "Z2", "Ray is declared");
        Check(CountOccurrences(header, "ZFUNCTION()") >= 30, "Z3",
              "all 30 entry points carry ZFUNCTION()");
        Check(CountOccurrences(header, "ZSCRIPTPROPERTY(") >= 8, "Z4",
              "the Unity properties are published (fieldOfView, nearClipPlane, ...)");
        // 屏幕朝向是最容易写反的一处，而写反了**不报任何错**。守的是"翻转这件事
        // 在头文件里写清楚了"。
        Check(CountOccurrences(header, "左下角") >= 2, "Z5",
              "the bottom-left screen origin is documented");

        const std::string source = ReadFile("Source/Runtime/Scripting/ScriptCamera/ScriptCamera.cpp");
        Check(!source.empty(), "Z6", "ScriptCamera.cpp is readable");
        // Y 翻转必须真的在代码里。只断言"有个函数叫这名"没用 —— 判的是那一行算术。
        Check(CountOccurrences(source, "0.5f - ndc_y * 0.5f") >= 1, "Z7",
              "screen y is flipped from NDC y (0.5 - ndc*0.5)");
        // 缓存按 RenderCamera*，不是按序号：序号会随增删相机滑动，脚本手里那个
        // Camera 会突然指向另一台。
        Check(CountOccurrences(source, "unordered_map<RenderCamera*") >= 2, "Z8",
              "Camera and Ray instances are cached per RenderCamera*");
        // 越界夹紧到第 0 台是最阴的一类：脚本 `for (i=0..count)` 写错时会静默地又
        // 拿到第 0 台。headless 没有 RenderSystem 走不到那条分支，所以判源码。
        Check(CountOccurrences(source, "index >= static_cast<int>(cameras.size())") >= 1 &&
                  CountOccurrences(source, "index = 0;") == 0,
              "Z9", "GetCameraAt out of range returns null instead of clamping to 0");
        // 必须走 NoJitter：TAA 抖动会让拾取射线每帧偏半个像素。
        //
        // ⚠️ 后半句原来是 `->GetProjectionMatrix() == 0`（"门面里一次都不许出现
        // 带抖动的投影"）。加了 `projectionMatrix` 访问器之后它必然出现一次，
        // 所以改成"**恰好**一次，且就是那个访问器"。写成 `== 0` 的话这条门禁会
        // 在每次新增任何投影相关 API 时误报，而写成"不判"就失去了意义 —— 判个数
        // 才是既准确又仍能抓到"有人把坐标换算换成了带抖动的那张"的写法。
        Check(CountOccurrences(source, "GetProjectionMatrixNoJitter()") >= 2 &&
                  CountOccurrences(source, "m_RenderCamera->GetProjectionMatrix()") == 1,
              "Z10", "conversion uses the un-jittered projection (picking must not wobble)");
        // w≈0 的守卫：漏了就是 NaN 顺着脚本的算术传遍整个 UI。
        Check(CountOccurrences(source, "std::fabs(clip.w) < 1.0e-6f") >= 2, "Z11",
              "degenerate w is guarded in both directions");
        // 深度必须按"沿视线投影"算，不能是欧氏距离。两者只在正中间相等。
        Check(CountOccurrences(source, "dotProduct(m_RenderCamera->forward())") >= 2, "Z12",
              "depth is measured along the view direction, not as a distance");

        const std::string camera_header = ReadFile("Source/Runtime/Function/Render/RenderCamera.h");
        Check(!camera_header.empty(), "Z13", "RenderCamera.h is readable");
        // `Camera.fieldOfView = v` 依赖这个 setter：没有它，门面只能直接写
        // m_Fovy，而下一次 SetAspect 就会把那个值盖掉 —— 静默失效。
        Check(CountOccurrences(camera_header, "void setFOVy(float fovy)") >= 1, "Z14",
              "RenderCamera exposes a vertical-FOV setter");
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

    // ---------------------------------------------------------------- G ----
    /// 刚体的 Unity 面：质量 / 阻尼 / 重力 / 力矩 / 偏心力 / 传送 / 睡眠。
    ///
    /// 这一组测的是 `PhysicsScene` 上那批新方法（真正的逻辑都在这里），
    /// `RigidBodyComponent` 上的同名包装测"没有物理场景时的降级"和"写回序列化
    /// 字段"两件事 —— 组件其余那些一行转发由 O 组的接线门守着。
    ///
    /// 为什么组件层拿不到一台活的刚体
    /// --------------------------------
    /// `RigidBodyComponent` 通过 `WorldManager::GetCurrentActivePhysicsScene()`
    /// 找场景，而"活动关卡"只能由加载流程设置（`m_CurrentActiveLevel` 是私有
    /// 的，没有 setter）。headless 加载不出真实关卡，所以组件在这一组里永远走
    /// "没有场景"那条早退 —— 和 U 组（Scene）同一类限制。
    void TestRigidbody()
    {
        PhysicsManager* physics_manager = GET_SYSTEM(PhysicsManager);
        Check(physics_manager != nullptr, "G0", "PhysicsManager is reachable");
        if (physics_manager == nullptr)
        {
            return;
        }

        std::shared_ptr<PhysicsScene> scene = physics_manager->CreatePhysicsScene(Vector3(0.f, 0.f, -9.81f)).lock();
        Check(scene != nullptr, "G1", "a physics scene can be created");
        if (!scene)
        {
            return;
        }

        const uint32_t body = SpawnBox(*scene, Vector3(0.f, 0.f, 50.f), 0, true);
        Check(body != kInvalidPhysicsBodyId, "G2", "a dynamic body can be spawned");
        if (body == kInvalidPhysicsBodyId)
        {
            return;
        }

        // --- 质量 --------------------------------------------------------
        float mass = 0.f;
        Check(scene->GetMass(body, mass) && mass > 0.f, "G3", "a dynamic body has a positive mass");
        Check(scene->SetMass(body, 12.0f) && scene->GetMass(body, mass) && Near(mass, 12.0f, 0.01f),
              "G4", "mass round-trips (12 kg in, 12 kg out)");
        // 0 / 负质量必须被夹住：倒数质量是 inf，一步积分之后就是 NaN 位置，而一个
        // NaN 物体会把整个 broadphase 拖垮（表现是"物理突然全不动了"，还不报错）。
        Check(scene->SetMass(body, 0.0f) && scene->GetMass(body, mass) && mass > 0.0f && mass < 1.0f,
              "G5", "a zero mass is clamped to a tiny positive, not accepted as inf");
        Check(scene->SetMass(body, -5.0f) && scene->GetMass(body, mass) && mass > 0.0f,
              "G6", "and so is a negative one");
        scene->SetMass(body, 12.0f);
        // 静态刚体没有质量：它是"不可推动"的，不是"质量无穷大但可写"。
        const uint32_t static_body = SpawnBox(*scene, Vector3(0.f, 30.f, 0.f), 0, false);
        Check(static_body != kInvalidPhysicsBodyId, "G7", "a static body can be spawned");
        Check(!scene->SetMass(static_body, 10.0f), "G8", "a static body refuses a mass");
        float static_mass = -1.f;
        Check(!scene->GetMass(static_body, static_mass) || static_mass == 0.f, "G9",
              "and reports none");

        // --- 重力 --------------------------------------------------------
        float factor = -1.f;
        Check(scene->GetGravityFactor(body, factor) && Near(factor, 1.0f), "G10",
              "gravity is on by default");
        Check(scene->SetGravityFactor(body, 0.0f) && scene->GetGravityFactor(body, factor) &&
                  Near(factor, 0.0f),
              "G11", "useGravity = false sets the factor to 0");
        Check(scene->SetGravityFactor(body, 1.0f) && scene->GetGravityFactor(body, factor) &&
                  Near(factor, 1.0f),
              "G12", "and back to 1");

        // --- 阻尼 --------------------------------------------------------
        float damping = -1.f;
        Check(scene->SetLinearDamping(body, 0.0f) && scene->GetLinearDamping(body, damping) &&
                  Near(damping, 0.0f),
              "G13", "drag round-trips");
        Check(scene->SetLinearDamping(body, 2.5f) && scene->GetLinearDamping(body, damping) &&
                  Near(damping, 2.5f, 0.001f),
              "G14", "and takes the value that was set (not a default)");
        Check(scene->SetAngularDamping(body, 3.5f) && scene->GetAngularDamping(body, damping) &&
                  Near(damping, 3.5f, 0.001f),
              "G15", "angularDrag round-trips");
        Check(!scene->SetLinearDamping(static_body, 1.0f), "G16", "a static body refuses damping");
        scene->SetLinearDamping(body, 0.0f);
        scene->SetAngularDamping(body, 0.0f);

        // --- 阻尼 / 力矩 / 偏心力：真的进模拟了吗 ------------------------
        // 关掉重力，把物体放在空中，隔离出"只有我们施加的那个作用"。
        scene->SetGravityFactor(body, 0.0f);

        // 无阻尼：一步之后速度还在。有阻尼：明显掉下来。只测"读回来对不对"的
        // 话，一个把 damping 写进字段却没接进积分的实现是全绿的。
        scene->SetLinearVelocity(body, Vector3(10.f, 0.f, 0.f));
        for (int i = 0; i < 20; ++i)
        {
            scene->Tick(1.0f / 60.0f);
        }
        Vector3 velocity = Vector3::ZERO;
        Check(scene->GetLinearVelocity(body, velocity) && std::fabs(velocity.x) > 9.0f, "G17",
              "with no drag the velocity survives 20 steps");

        scene->SetLinearVelocity(body, Vector3(10.f, 0.f, 0.f));
        scene->SetLinearDamping(body, 8.0f);
        for (int i = 0; i < 20; ++i)
        {
            scene->Tick(1.0f / 60.0f);
        }
        Check(scene->GetLinearVelocity(body, velocity) && std::fabs(velocity.x) < 3.0f, "G18",
              "with drag 8 the same 20 steps eat most of it");
        scene->SetLinearDamping(body, 0.0f);

        // 力矩：转起来。
        scene->SetAngularVelocity(body, Vector3::ZERO);
        scene->AddTorque(body, Vector3(0.f, 0.f, 500.f));
        scene->Tick(1.0f / 60.0f);
        Check(scene->GetAngularVelocity(body, velocity) && std::fabs(velocity.z) > 0.01f, "G19",
              "AddTorque actually spins the body");

        // 偏心力 vs 中心力：这是 `AddForceAtPosition` 存在的全部理由 —— 它会带出
        // 力矩，而 `AddForce` 不会。两者都"推动了物体"，所以只看线速度分不出。
        scene->SetLinearVelocity(body, Vector3::ZERO);
        scene->SetAngularVelocity(body, Vector3::ZERO);
        scene->AddForce(body, Vector3(5000.f, 0.f, 0.f));
        scene->Tick(1.0f / 60.0f);
        Vector3 after_centre_force = Vector3::ZERO;
        scene->GetAngularVelocity(body, after_centre_force);
        Check(after_centre_force.length() < 0.01f, "G20", "AddForce at the centre produces no spin");

        scene->SetLinearVelocity(body, Vector3::ZERO);
        scene->SetAngularVelocity(body, Vector3::ZERO);
        // 作用在物体"上方一角"：偏心臂会让力和力矩同时出现。
        Vector3 body_position = Vector3::ZERO;
        scene->GetRigidBodyPosition(body, body_position);
        scene->AddForceAtPosition(body, Vector3(5000.f, 0.f, 0.f),
                                  body_position + Vector3(0.f, 0.f, 0.5f));
        scene->Tick(1.0f / 60.0f);
        Vector3 after_offset_force = Vector3::ZERO;
        scene->GetAngularVelocity(body, after_offset_force);
        Check(after_offset_force.length() > 0.01f, "G21",
              "AddForceAtPosition off-centre DOES spin it (that is the whole point)");

        // --- 质量必须连惯性张量一起缩 ------------------------------------
        // 只写倒数质量的话惯性停在旧值，"改质量"于是对转动**毫无影响**：一个
        // 12kg 的箱子带着 1000kg 的转动惯量，推起来对了、转起来纹丝不动。
        //
        // 判据不依赖任何解析常数：惯性正比于质量 ⇒ 同一个角冲量下，100 倍的
        // 质量得到约 1/100 的角速度。惯性没跟着变的话两者**相等**。
        scene->SetLinearVelocity(body, Vector3::ZERO);
        scene->SetAngularVelocity(body, Vector3::ZERO);
        scene->WakeRigidBody(body);
        scene->SetMass(body, 12.0f);
        scene->AddAngularImpulse(body, Vector3(0.f, 0.f, 60.f));
        scene->Tick(1.0f / 60.0f);
        Vector3 spin_light = Vector3::ZERO;
        const bool read_light = scene->GetAngularVelocity(body, spin_light);

        scene->SetLinearVelocity(body, Vector3::ZERO);
        scene->SetAngularVelocity(body, Vector3::ZERO);
        scene->WakeRigidBody(body);
        scene->SetMass(body, 1200.0f);
        scene->AddAngularImpulse(body, Vector3(0.f, 0.f, 60.f));
        scene->Tick(1.0f / 60.0f);
        Vector3 spin_heavy = Vector3::ZERO;
        const bool read_heavy = scene->GetAngularVelocity(body, spin_heavy);

        Check(read_light && read_heavy && spin_light.length() > 0.01f && spin_heavy.length() > 0.0f &&
                  (spin_light.length() / spin_heavy.length()) > 20.0f,
              "G44", "mass scales the INERTIA too: 100x the mass spins ~100x slower");
        scene->SetMass(body, 12.0f);

        // --- 传送 / 摆位 --------------------------------------------------
        Check(scene->SetRigidBodyPosition(body, Vector3(3.f, 4.f, 5.f)) &&
                  scene->GetRigidBodyPosition(body, body_position) &&
                  Near(body_position, Vector3(3.f, 4.f, 5.f), 0.01f),
              "G22", "SetRigidBodyPosition teleports the body");
        Quaternion body_rotation = Quaternion::IDENTITY;
        Check(scene->SetRigidBodyRotation(body, Quaternion(Radian(Degree(90.f)), Vector3::UNIT_Z)) &&
                  scene->GetRigidBodyRotation(body, body_rotation) &&
                  Near(body_rotation.getW(), std::cos(Math_PI / 4.0f), 0.01f),
              "G23", "SetRigidBodyRotation sets the orientation");

        // --- MoveKinematic -----------------------------------------------
        // 只有运动学物体会"推着别人走"，也只有它才用 MoveKinematic。传送
        // （SetRigidBodyPosition）不会推 —— 这是两者的分界。
        RigidBodyActorType motion_type = RigidBodyActorType::static_body;
        Check(scene->SetRigidBodyMotionType(body, RigidBodyActorType::kinematic) &&
                  scene->GetRigidBodyMotionType(body, motion_type) &&
                  motion_type == RigidBodyActorType::kinematic,
              "G24", "a body can be switched to kinematic");
        scene->SetRigidBodyPosition(body, Vector3(0.f, 0.f, 50.f));
        Check(scene->MoveKinematic(body, Vector3(1.f, 0.f, 50.f), Quaternion::IDENTITY, 1.0f / 60.0f),
              "G25", "MoveKinematic accepts a target pose");
        scene->Tick(1.0f / 60.0f);
        Check(scene->GetRigidBodyPosition(body, body_position) && body_position.x > 0.5f, "G26",
              "and one step later the body has moved toward it");
        // delta_time 为 0 不能算出 inf 的速度。
        Check(scene->MoveKinematic(body, Vector3(2.f, 0.f, 50.f), Quaternion::IDENTITY, 0.0f), "G27",
              "a zero delta_time is clamped instead of producing an infinite velocity");
        scene->Tick(1.0f / 60.0f);
        Check(scene->GetRigidBodyPosition(body, body_position) && std::isfinite(body_position.x),
              "G28", "and the body is still finite afterwards");
        scene->SetRigidBodyMotionType(body, RigidBodyActorType::dynamic);

        // --- 睡眠 ---------------------------------------------------------
        bool is_sleeping = false;
        Check(scene->SleepRigidBody(body) && scene->IsSleeping(body, is_sleeping) && is_sleeping,
              "G29", "a body can be put to sleep and reports it");
        Check(scene->WakeRigidBody(body) && scene->IsSleeping(body, is_sleeping) && !is_sleeping,
              "G30", "and woken up again");

        // --- 无效 id：一律 false，不是崩 ---------------------------------
        float ignored_float = 0.f;
        bool ignored_bool = false;
        Vector3 ignored_vec = Vector3::ZERO;
        Check(!scene->SetMass(kInvalidPhysicsBodyId, 1.0f) &&
                  !scene->GetMass(kInvalidPhysicsBodyId, ignored_float) &&
                  !scene->SetGravityFactor(kInvalidPhysicsBodyId, 1.0f) &&
                  !scene->SetLinearDamping(kInvalidPhysicsBodyId, 1.0f) &&
                  !scene->SetAngularDamping(kInvalidPhysicsBodyId, 1.0f) &&
                  !scene->AddTorque(kInvalidPhysicsBodyId, Vector3::UNIT_Z) &&
                  !scene->AddAngularImpulse(kInvalidPhysicsBodyId, Vector3::UNIT_Z) &&
                  !scene->AddForceAtPosition(kInvalidPhysicsBodyId, Vector3::UNIT_X, ignored_vec) &&
                  !scene->AddImpulseAtPosition(kInvalidPhysicsBodyId, Vector3::UNIT_X, ignored_vec) &&
                  !scene->SetRigidBodyPosition(kInvalidPhysicsBodyId, ignored_vec) &&
                  !scene->SetRigidBodyRotation(kInvalidPhysicsBodyId, Quaternion::IDENTITY) &&
                  !scene->MoveKinematic(kInvalidPhysicsBodyId, ignored_vec, Quaternion::IDENTITY, 0.1f) &&
                  !scene->IsSleeping(kInvalidPhysicsBodyId, ignored_bool) &&
                  !scene->SleepRigidBody(kInvalidPhysicsBodyId) &&
                  !scene->WakeRigidBody(kInvalidPhysicsBodyId),
              "G31", "every new call rejects an invalid body id instead of crashing");

        // --- 组件侧：没有活动物理场景时的降级 ----------------------------
        WorldManager* world = GET_SYSTEM(WorldManager);
        Check(world != nullptr && world->GetCurrentActivePhysicsScene().lock() == nullptr, "G32",
              "(the component fixture has no active physics scene)");
        RigidBodyComponent rigidbody;
        Check(!rigidbody.HasBody(), "G33", "a component with no scene has no body");
        Check(Near(rigidbody.GetDrag(), 0.0f), "G34", "drag reads 0");
        Check(Near(rigidbody.GetAngularDrag(), 0.0f), "G35", "angularDrag reads 0");
        Check(rigidbody.GetUseGravity(), "G36", "useGravity reads true (Unity's default)");
        // 没有刚体时"没在模拟"是真的 —— 它没有在动。返回 true 而不是 false，这样
        // "如果它睡着就叫醒它"的逻辑不会漏掉一个其实根本还没建出来的物体。
        Check(rigidbody.IsSleeping(), "G37", "IsSleeping reads true when there is nothing simulating");
        Check(Near(rigidbody.GetPosition(), Vector3::ZERO, 1.0e-6f), "G38",
              "position reads zero rather than NaN");
        // 全部写操作必须是安全的空操作。
        rigidbody.SetMass(4.0f);
        rigidbody.SetDrag(0.5f);
        rigidbody.SetAngularDrag(0.5f);
        rigidbody.SetUseGravity(false);
        rigidbody.AddTorque(Vector3::UNIT_Z);
        rigidbody.AddAngularImpulse(Vector3::UNIT_Z);
        rigidbody.AddForceAtPosition(Vector3::UNIT_X, Vector3::UNIT_Z);
        rigidbody.AddImpulseAtPosition(Vector3::UNIT_X, Vector3::UNIT_Z);
        rigidbody.AddExplosionForce(100.0f, Vector3::ZERO, 5.0f);
        rigidbody.AddExplosionForce(100.0f, Vector3::ZERO, 0.0f);
        rigidbody.AddExplosionForce(100.0f, Vector3::ZERO, -1.0f);
        rigidbody.SetPosition(Vector3::UNIT_X);
        rigidbody.SetRotation(Quaternion::IDENTITY);
        rigidbody.MovePosition(Vector3::UNIT_X);
        rigidbody.MoveRotation(Quaternion::IDENTITY);
        rigidbody.Sleep();
        rigidbody.WakeUp();
        Check(true, "G39", "every Unity-named method is safe with no body (they ran above)");

        // --- 序列化字段：改质量必须落进存档 ------------------------------
        // 只改活刚体的话，存盘再读回来质量就变回作者设的那个值，而没有任何警告。
        rigidbody.SetMass(8.0f);
        Check(Near(rigidbody.GetRigidbodyRes().m_InverseMass, 1.0f / 8.0f, 1.0e-6f), "G40",
              "SetMass writes the serialized field back (a save keeps the new mass)");

        // --- isKinematic：走的是作者值，不依赖活刚体 ---------------------
        rigidbody.SetActorType(RigidBodyActorType::dynamic);
        Check(!rigidbody.GetIsKinematic(), "G41", "a dynamic body is not kinematic");
        rigidbody.SetIsKinematic(true);
        Check(rigidbody.GetIsKinematic() && rigidbody.GetActorType() == RigidBodyActorType::kinematic,
              "G42", "SetIsKinematic(true) makes it kinematic");
        rigidbody.SetIsKinematic(false);
        Check(!rigidbody.GetIsKinematic() && rigidbody.GetActorType() == RigidBodyActorType::dynamic,
              "G43", "and back (Unity turns it into a dynamic body, not a static one)");
    }

    // ---------------------------------------------------------------- O ----
    void TestRigidbodyWiring()
    {
        const std::string scene_header = ReadFile("Source/Runtime/Function/Physics/PhysicsScene.h");
        Check(!scene_header.empty(), "O0", "PhysicsScene.h is readable");
        // 质量必须连惯性张量一起标定：只改倒数质量的话，一个 10kg 的箱子会带着
        // 1kg 的转动惯量 —— 推起来对了，转起来飞快。
        Check(CountOccurrences(scene_header, "SetMass(uint32_t body_id, float mass)") >= 1, "O1",
              "the scene exposes a mass setter");
        Check(CountOccurrences(scene_header, "AddForceAtPosition") >= 1 &&
                  CountOccurrences(scene_header, "AddTorque") >= 1 &&
                  CountOccurrences(scene_header, "MoveKinematic") >= 1 &&
                  CountOccurrences(scene_header, "SetGravityFactor") >= 1 &&
                  CountOccurrences(scene_header, "SetLinearDamping") >= 1 &&
                  CountOccurrences(scene_header, "SetAngularDamping") >= 1,
              "O2", "and the torque / off-centre / kinematic-move / gravity / damping entries");

        const std::string scene_source = ReadFile("Source/Runtime/Function/Physics/PhysicsScene.cpp");
        Check(!scene_source.empty(), "O3", "PhysicsScene.cpp is readable");
        // 惯性必须一起缩。`SetInverseMass` 单独出现说明有人"顺手改了一下"。
        Check(CountOccurrences(scene_source, "ScaleToMass") >= 1, "O4",
              "mass is scaled together with the inertia tensor (ScaleToMass)");
        // 0 质量 → inf 倒数质量 → NaN 位置 → 整个 broadphase 塌掉。
        Check(CountOccurrences(scene_source, "kMinMass") >= 2, "O5",
              "a non-positive mass is clamped (kMinMass is defined AND used)");
        // delta_time 为 0 时 MoveKinematic 算出的速度是 inf。
        Check(CountOccurrences(scene_source, "kMinDeltaTime") >= 2, "O6",
              "a zero delta_time is clamped before MoveKinematic");
        // 施力路径的**顺序**是致命的：BodyInterface::ActivateBody 自己会拿写锁，
        // 在持有 BodyLockWrite 时调它属于同优先级重入 —— Jolt 断言之后就是同一
        // 线程重入同一把 mutex，直接死锁（表现是"施一次力，物理整个停住"，而且
        // 不报错）。判的是"先把动态性探完、锁外唤醒"这个结构还在。
        Check(CountOccurrences(scene_source, "BodyLockRead probe_lock") >= 1 &&
                  CountOccurrences(scene_source, "inType > mutexes") >= 1,
              "O15", "AddForce/AddImpulse activate OUTSIDE the body write lock (re-entrance deadlocks)");

        const std::string component_header =
            ReadFile("Source/Runtime/Function/Framework/Component/Rigidbody/RigidbodyComponent.h");
        Check(!component_header.empty(), "O7", "RigidbodyComponent.h is readable");
        Check(CountOccurrences(component_header, "ZFUNCTION()") >= 36, "O8",
              "all 36 entry points carry ZFUNCTION()");
        // Unity 里 mass / drag / useGravity / isKinematic / velocity / position
        // 全是**属性**（`rb.velocity = v`）。只给方法的话脚本得写
        // `rb.SetLinearVelocity(v)`，而 `rb.velocity` 在 .d.ts 里根本不存在。
        Check(CountOccurrences(component_header, "ZSCRIPTPROPERTY(Name=\"mass\"") >= 1 &&
                  CountOccurrences(component_header, "ZSCRIPTPROPERTY(Name=\"drag\"") >= 1 &&
                  CountOccurrences(component_header, "ZSCRIPTPROPERTY(Name=\"useGravity\"") >= 1 &&
                  CountOccurrences(component_header, "ZSCRIPTPROPERTY(Name=\"isKinematic\"") >= 1 &&
                  CountOccurrences(component_header, "ZSCRIPTPROPERTY(Name=\"velocity\"") >= 1 &&
                  CountOccurrences(component_header, "ZSCRIPTPROPERTY(Name=\"position\"") >= 1,
              "O9", "the Unity properties are published, not just the methods");
        Check(CountOccurrences(component_header, "AddExplosionForce") >= 2, "O10",
              "AddExplosionForce is declared AND documented as single-body");

        const std::string component_source =
            ReadFile("Source/Runtime/Function/Framework/Component/Rigidbody/RigidbodyComponent.cpp");
        Check(!component_source.empty(), "O11", "RigidbodyComponent.cpp is readable");
        // 每个 Unity 名字都必须真的转发到场景上 —— 一个"忘了接线"的方法是静默
        // 的空函数，编译得过也跑得过。
        Check(CountOccurrences(component_source, "physics_scene->SetMass(m_RigidbodyId") >= 1 &&
                  CountOccurrences(component_source, "physics_scene->SetGravityFactor(m_RigidbodyId") >= 1 &&
                  CountOccurrences(component_source, "physics_scene->SetLinearDamping(m_RigidbodyId") >= 1 &&
                  CountOccurrences(component_source, "physics_scene->SetAngularDamping(m_RigidbodyId") >= 1 &&
                  CountOccurrences(component_source, "physics_scene->AddTorque(m_RigidbodyId") >= 1 &&
                  CountOccurrences(component_source, "physics_scene->AddAngularImpulse(m_RigidbodyId") >= 1 &&
                  CountOccurrences(component_source, "physics_scene->AddForceAtPosition(m_RigidbodyId") >= 1 &&
                  CountOccurrences(component_source, "physics_scene->AddImpulseAtPosition(m_RigidbodyId") >= 1 &&
                  CountOccurrences(component_source, "physics_scene->SetRigidBodyPosition(m_RigidbodyId") >= 1 &&
                  CountOccurrences(component_source, "physics_scene->SetRigidBodyRotation(m_RigidbodyId") >= 1 &&
                  CountOccurrences(component_source, "physics_scene->MoveKinematic(m_RigidbodyId") >= 2 &&
                  CountOccurrences(component_source, "physics_scene->SleepRigidBody(m_RigidbodyId") >= 1 &&
                  CountOccurrences(component_source, "physics_scene->WakeRigidBody(m_RigidbodyId") >= 1,
              "O12", "every Unity-named method forwards to the scene with the body id");
        // 改质量必须同时写序列化字段，否则存盘再读回来就变回作者设的值。
        Check(CountOccurrences(component_source, "m_RigidbodyRes.m_InverseMass =") >= 1, "O13",
              "SetMass also writes the serialized field (a save keeps the new mass)");
        // AddExplosionForce 的两个除零：半径 0 和"炸在质心上"。
        Check(CountOccurrences(component_source, "explosion_radius <= 0.f") >= 1 &&
                  CountOccurrences(component_source, "distance < 1.0e-5f") >= 1,
              "O14", "AddExplosionForce guards both zero-radius and zero-distance");
    }

    // --------------------------------------------------------------- AA ----
    /// 角色控制器（Unity 的 `CharacterController`）。
    ///
    /// 这一组测的是 `CharacterController` 上那批新方法 —— **全部**真实逻辑都在
    /// 那里。`MotorComponent` 上的同名包装是薄壳：它要靠 `WorldManager` 找活动
    /// 场景，而活动关卡只能由加载流程设置（没有 setter），headless 下永远走
    /// "没有场景"那条路。和 Rigidbody 切片同一个取舍：逻辑放在显式收
    /// `PhysicsScene*` 的方法里，组件层由 AB 组的接线门守着。
    void TestCharacterController()
    {
        PhysicsManager* physics_manager = GET_SYSTEM(PhysicsManager);
        Check(physics_manager != nullptr, "AA0", "PhysicsManager is reachable");
        if (physics_manager == nullptr)
        {
            return;
        }

        std::shared_ptr<PhysicsScene> scene = physics_manager->CreatePhysicsScene(Vector3(0.f, 0.f, -9.81f)).lock();
        Check(scene != nullptr, "AA1", "a physics scene can be created");
        if (!scene)
        {
            return;
        }

        // 地板：顶面在 z=0 的一块大板。墙：x ∈ [2.5, 3.5] 的一块竖板。
        // 都比角色大得多 —— 否则"没被挡住"和"地板太短"分不出来。
        const uint32_t ground =
            SpawnBoxSized(*scene, Vector3(0.f, 0.f, -0.5f), Vector3(20.f, 20.f, 0.5f), 0, false);
        Check(ground != kInvalidPhysicsBodyId, "AA2", "a ground slab can be spawned");
        const uint32_t wall =
            SpawnBoxSized(*scene, Vector3(3.f, 0.f, 2.f), Vector3(0.5f, 20.f, 2.f), 0, false);
        Check(wall != kInvalidPhysicsBodyId, "AA3", "a wall can be spawned");

        ObjectManager* om = GET_SYSTEM(ObjectManager);
        if (om == nullptr)
        {
            om = new ObjectManager();
            SystemRegistry::GetInstance().RegisterSystem(om);
            om->Initialize();
        }
        Check(om != nullptr, "AA4", "ObjectManager is reachable");
        if (om == nullptr)
        {
            return;
        }

        // 用 NewObject 而不是 new + AllocateAndAssignInstanceID：控制器析构时走
        // MemoryManager::DestroyObject，必须和 NewObject 配对（mimalloc 对 CRT
        // 堆），混用是堆损坏。这里也让控制器一直活着（同文件其它夹具的取舍）。
        Capsule* capsule = om->NewObject<Capsule>();
        Check(capsule != nullptr, "AA5", "a capsule can be allocated through ObjectManager");
        if (capsule == nullptr)
        {
            return;
        }
        capsule->m_Radius = 0.3f;
        capsule->m_HalfHeight = 0.7f;

        CharacterController* controller = new CharacterController();
        controller->Initialize(capsule);
        Check(controller->HasShape(), "AA6", "Initialize builds a shape for the capsule");

        // --- 尺寸换算：height 是含帽总高 --------------------------------
        Check(Near(controller->GetCapsuleHeight(), 2.0f, 0.001f), "AA7",
              "height = 2 * (halfHeight + radius) — caps included, not the cylinder");
        Check(Near(controller->GetCapsuleRadius(), 0.3f, 0.001f), "AA8", "radius round-trips");
        Check(Near(controller->GetCapsuleCenter().z, 1.0f, 0.001f), "AA9",
              "center is half the height above the feet (origin is at the sole)");
        controller->SetCapsuleHeight(3.0f);
        Check(Near(controller->GetCapsuleHeight(), 3.0f, 0.001f) &&
                  Near(controller->GetCapsuleRadius(), 0.3f, 0.001f),
              "AA10", "SetHeight keeps the radius and re-solves the half height");
        // 比一个球的直径还矮 ⇒ 夹住。不夹的话会算出负半高，胶囊翻过来。
        controller->SetCapsuleHeight(0.2f);
        Check(controller->GetCapsuleHeight() > 0.55f && controller->GetCapsuleHeight() < 0.65f, "AA11",
              "a height below one sphere diameter is clamped, not accepted as negative");
        // 半径 0 会让 Jolt 直接断言。
        controller->SetCapsuleRadius(0.0f);
        Check(controller->GetCapsuleRadius() > 0.0f, "AA12", "a zero radius is clamped, not accepted");
        controller->SetCapsuleCenter(Vector3(0.f, 0.f, 1.2f));
        Check(Near(controller->GetCapsuleCenter().z, 1.2f, 0.001f), "AA13",
              "SetCenter re-solves the half height so the centre lands where asked");

        // 复位成标准身材，后面的碰撞检查都基于它。
        controller->SetCapsuleRadius(0.3f);
        controller->SetCapsuleHeight(2.0f);
        Check(Near(controller->GetCapsuleHeight(), 2.0f, 0.001f), "AA14", "and the size can be restored");

        // --- 着地探测 ----------------------------------------------------
        Check(controller->ProbeGrounded(scene.get(), Vector3(0.f, 0.f, 0.f), 0.08f), "AA15",
              "feet on the slab reads grounded");
        Check(!controller->ProbeGrounded(scene.get(), Vector3(0.f, 0.f, 5.f), 0.08f), "AA16",
              "five metres up reads airborne");
        // skinWidth 必须真的参与：脚底离地 5cm，skin=0.08 算贴地，skin=0 不算。
        // 只测"贴着地面"的话，一个把 skin 存起来不用的实现是全绿的。
        Check(controller->ProbeGrounded(scene.get(), Vector3(0.f, 0.f, 0.05f), 0.08f), "AA17",
              "a 5 cm gap is still grounded with skinWidth 0.08");
        Check(!controller->ProbeGrounded(scene.get(), Vector3(0.f, 0.f, 0.05f), 0.0f), "AA18",
              "and airborne with skinWidth 0 — the skin is consumed, not stored");
        Check(!controller->ProbeGrounded(nullptr, Vector3(0.f, 0.f, 0.f), 0.08f), "AA19",
              "no scene means 'unknown', reported as false (not a guess of true)");

        // --- Move：碰撞标志 ---------------------------------------------
        Vector3 landed = Vector3::ZERO;
        const int32_t down = controller->MoveWithFlags(scene.get(), Vector3(0.f, 0.f, 0.f),
                                                       Vector3(0.f, 0.f, -0.5f), true, 0.f, landed);
        Check((down & 4) != 0, "AA20", "a blocked downward move reports CollisionFlags.Below");
        Check(Near(landed.z, 0.0f, 0.001f), "AA21", "and the feet stay on the slab");

        const int32_t up = controller->MoveWithFlags(scene.get(), Vector3(0.f, 0.f, 0.f),
                                                     Vector3(0.f, 0.f, 0.5f), true, 0.f, landed);
        Check(up == 0, "AA22", "an unobstructed move reports no flags");
        Check(Near(landed.z, 0.5f, 0.001f), "AA23", "and actually moves");

        // 朝着墙推进：2.0 到得了（胶囊 x 上限 2.3 < 2.5），再 0.4 就被挡。
        controller->MoveWithFlags(scene.get(), Vector3(0.f, 0.f, 0.f), Vector3(2.0f, 0.f, 0.f), true, 0.f,
                                  landed);
        Check(Near(landed.x, 2.0f, 0.001f), "AA24", "a move short of the wall goes through");
        const int32_t into_wall =
            controller->MoveWithFlags(scene.get(), landed, Vector3(0.4f, 0.f, 0.f), true, 0.f, landed);
        Check((into_wall & 1) != 0, "AA25", "a blocked sideways move reports CollisionFlags.Sides");

        // --- 逐轴推进 ⇒ 贴墙滑行 ----------------------------------------
        // 合并判定的实现会整段取消，x 和 z 一起归零。这条断言是"逐轴"这个决定
        // 唯一的守护者：它漏了就退化成"角色被墙粘住"。
        const Vector3 before_slide = landed;
        const int32_t slide = controller->MoveWithFlags(scene.get(), before_slide,
                                                        Vector3(0.4f, 0.f, 0.5f), true, 0.f, landed);
        Check((slide & 1) != 0, "AA26", "the blocked axis still reports Sides");
        Check(Near(landed.x, before_slide.x, 0.001f), "AA27", "and does not move");
        Check(Near(landed.z, before_slide.z + 0.5f, 0.001f), "AA28",
              "while the free axis still moves — that is sliding along the wall");

        // --- 上方：撞顶 ---------------------------------------------------
        // 在头顶正上方压一块板，角色跳起来会撞到。
        SpawnBoxSized(*scene, Vector3(0.f, 0.f, 2.4f), Vector3(2.f, 2.f, 0.2f), 0, false);
        const int32_t ceiling = controller->MoveWithFlags(scene.get(), Vector3(0.f, 0.f, 0.f),
                                                          Vector3(0.f, 0.f, 0.6f), true, 0.f, landed);
        Check((ceiling & 2) != 0, "AA29", "a blocked upward move reports CollisionFlags.Above");

        // --- minMoveDistance / detectCollisions --------------------------
        const int32_t dropped = controller->MoveWithFlags(scene.get(), Vector3(0.f, 0.f, 0.f),
                                                          Vector3(0.f, 0.f, -0.001f), true, 0.01f, landed);
        Check(dropped == 0 && Near(landed.z, 0.0f, 0.0001f), "AA30",
              "minMoveDistance discards the whole motion (position untouched)");
        const int32_t ghost = controller->MoveWithFlags(scene.get(), Vector3(0.f, 0.f, 0.f),
                                                        Vector3(0.f, 0.f, -0.5f), false, 0.f, landed);
        Check(ghost == 0 && Near(landed.z, -0.5f, 0.001f), "AA31",
              "detectCollisions = false walks straight through the floor");

        // --- 改尺寸要真的改动碰撞体 --------------------------------------
        // 这是 `RefreshShapeTransform` 唯一的守护者。原点在脚底，所以形状要把胶囊
        // 抬高 `halfHeight + radius`；只改 Capsule 的两个字段而忘了重写局部变换的
        // 话，碰撞体还挂在旧高度上 —— 表现是"蹲下了却照样撞头"，而且**不报任何错**
        // （`IsOverlap` 每次都从当前几何重建 Jolt 形状，所以看起来是生效的）。
        //
        // 判据必须挑一个"抬高量"能左右结果的位置：矮天花板（底面 z=1.6）。
        // 蹲到 1.5 m 时胶囊顶端 1.5 < 1.6 ⇒ 过得去；忘刷新的话顶端还是 1.75 ⇒ 撞。
        SpawnBoxSized(*scene, Vector3(0.f, 0.f, 1.65f), Vector3(2.f, 2.f, 0.05f), 0, false);
        controller->SetCapsuleHeight(1.5f);
        const int32_t crouch = controller->MoveWithFlags(scene.get(), Vector3(0.f, 0.f, 0.f),
                                                         Vector3(0.f, 0.f, 0.05f), true, 0.f, landed);
        Check(crouch == 0 && Near(landed.z, 0.05f, 0.001f), "AA32",
              "a crouched capsule fits under a 1.6 m ceiling — the shape transform was refreshed");
        controller->SetCapsuleHeight(2.0f);
        const int32_t stand = controller->MoveWithFlags(scene.get(), Vector3(0.f, 0.f, 0.f),
                                                        Vector3(0.f, 0.f, 0.05f), true, 0.f, landed);
        Check((stand & 2) != 0, "AA33", "and the full-height one does not");

        // --- SimpleMove：纯函数那一半 ------------------------------------
        // 带着一个"刚从高处落下"的速度进场：初值是 0 的话，"落地是否归零"这条
        // 断言对任何实现都成立（变异 M12 就是这么漏过去的）。
        float fall = -7.3f;
        const Vector3 on_ground = CharacterController::ComputeSimpleMoveMotion(
            Vector3(1.f, 0.f, 5.f), 9.81f, 1.0f / 60.0f, true, fall);
        Check(Near(on_ground.z, 0.0f, 0.0001f), "AA34", "SimpleMove ignores the vertical component of speed");
        Check(Near(on_ground.x, 1.f / 60.f, 0.0001f), "AA35", "and converts m/s into a per-step motion");
        Check(Near(fall, 0.f, 0.0001f), "AA36",
              "landing resets the accumulated fall speed (a stale -7.3 would tunnel through the floor)");

        float airborne_fall = 0.f;
        const Vector3 first = CharacterController::ComputeSimpleMoveMotion(
            Vector3(0.f, 0.f, 0.f), 9.81f, 1.0f / 60.0f, false, airborne_fall);
        const Vector3 second = CharacterController::ComputeSimpleMoveMotion(
            Vector3(0.f, 0.f, 0.f), 9.81f, 1.0f / 60.0f, false, airborne_fall);
        Check(first.z < 0.f, "AA37", "airborne falls");
        Check(second.z < first.z * 1.9f, "AA38",
              "and accelerates — the fall speed accumulates instead of being recomputed");
        Check(CharacterController::ComputeSimpleMoveMotion(Vector3(0.f, 0.f, 0.f), 9.81f, 0.f, false,
                                                          airborne_fall)
                  .squaredLength() == 0.f,
              "AA39", "a zero delta time yields no motion (a paused frame must not fall)");
    }

    // --------------------------------------------------------------- AB ----
    /// 角色控制器的接线门：组件层那层薄壳**有没有接上**。
    ///
    /// AA 组证明了 `CharacterController` 的逻辑是对的；这一组证明脚本真的能碰到
    /// 它。只做前者的话，"实现对了但没接进组件"这种静默失败是绿的 —— 这正是
    /// `MotorComponent` 此前十年的状态（一整套移动实现，0 个 ZFUNCTION）。
    void TestCharacterControllerWiring()
    {
        const std::string motor_header = ReadFile("Source/Runtime/Function/Framework/Component/Motor/MotorComponent.h");
        Check(!motor_header.empty(), "AB0", "MotorComponent.h is readable");
        Check(CountOccurrences(motor_header, "class MotorComponent : public Component") >= 1, "AB1",
              "and it is the component that carries the motor");
        // 带括号数：数裸词永远为真（注释里也会出现）。
        Check(CountOccurrences(motor_header, "ZFUNCTION()") == 21, "AB2",
              "the component exposes exactly 21 script methods (a dropped ZFUNCTION is a silent API loss)");
        Check(CountOccurrences(motor_header, "ZSCRIPTPROPERTY(") == 11, "AB3",
              "and exactly 11 script properties");

        // Unity 的拼写。**必须连着 ZFUNCTION() 一起数**：只数 `void SimpleMove(`
        // 的话，一个"实现还在但忘了标 ZFUNCTION"的方法是全绿的 —— 那正是
        // MotorComponent 在此之前的样子（一整套移动实现，0 个可脚本方法）。
        Check(CountOccurrences(motor_header, "ZFUNCTION()\n    int32_t Move(") >= 1, "AB4",
              "Move is declared AND exported");
        Check(CountOccurrences(motor_header, "ZFUNCTION()\n    void SimpleMove(") >= 1, "AB5",
              "SimpleMove is declared AND exported");
        Check(CountOccurrences(motor_header, "ZFUNCTION()\n    bool IsGrounded()") >= 1, "AB6",
              "isGrounded is declared AND exported");
        Check(CountOccurrences(motor_header, "\"isGrounded\"") >= 1, "AB7",
              "isGrounded is published under Unity's name");
        Check(CountOccurrences(motor_header, "\"velocity\"") >= 1, "AB8", "and so is velocity");
        Check(CountOccurrences(motor_header, "\"height\"") >= 1 &&
                  CountOccurrences(motor_header, "\"radius\"") >= 1 &&
                  CountOccurrences(motor_header, "\"center\"") >= 1,
              "AB9", "height / radius / center are all published");

        // CollisionFlags 的值必须和 Unity 对上，否则 `flags & Below` 全错。
        Check(CountOccurrences(motor_header, "enum class CollisionFlags") >= 1 &&
                  CountOccurrences(motor_header, "sides = 1") >= 1 &&
                  CountOccurrences(motor_header, "above = 2") >= 1 &&
                  CountOccurrences(motor_header, "below = 4") >= 1,
              "AB10", "CollisionFlags uses Unity's bit values (Sides=1, Above=2, Below=4)");
        Check(CountOccurrences(motor_header, "ZENUM()") >= 1, "AB11",
              "and is published to TypeScript (a bare number would be unreadable)");

        // 明确没做的两个属性必须**留在注释里说明**，不能悄悄消失。
        Check(CountOccurrences(motor_header, "slopeLimit") >= 1 &&
                  CountOccurrences(motor_header, "stepOffset") >= 1,
              "AB12", "slopeLimit / stepOffset are documented as deliberately absent");

        const std::string motor_source = ReadFile("Source/Runtime/Function/Framework/Component/Motor/MotorComponent.cpp");
        Check(!motor_source.empty(), "AB13", "MotorComponent.cpp is readable");
        // 薄壳必须把活派给 CharacterController，不能自己重算一遍 —— 两个实现
        // 迟早分叉，而且 AA 组测的就不是跑在脚本路径上的那份。
        Check(CountOccurrences(motor_source, "MoveWithFlags(") >= 1, "AB14",
              "Move delegates to CharacterController::MoveWithFlags");
        Check(CountOccurrences(motor_source, "ComputeSimpleMoveMotion(") >= 1, "AB15",
              "SimpleMove delegates to the shared solver too");
        Check(CountOccurrences(motor_source, "ProbeGrounded(") >= 1, "AB16",
              "isGrounded goes through the ray probe, not the jump state machine");
        // 没有场景/没有控制器时的降级：这些分支不是"不可能发生"。
        Check(CountOccurrences(motor_source, "world_manager == nullptr") >= 1, "AB17",
              "a missing WorldManager degrades instead of dereferencing null");

        const std::string controller_header = ReadFile("Source/Runtime/Function/Controller/CharacterController.h");
        Check(!controller_header.empty(), "AB18", "CharacterController.h is readable");
        Check(CountOccurrences(controller_header, "RefreshShapeTransform()") >= 1, "AB19",
              "dimension setters refresh the shape transform (else the collider stays at the old height)");
        Check(CountOccurrences(controller_header, "height = 2 * (half_height + radius)") >= 1, "AB20",
              "the height/halfHeight relation is written down where it is implemented");
    }

    // --------------------------------------------------------------- AC ----
    /// 碰撞体（`Collider` 门面）+ "命中带回对象"。
    ///
    /// `Collider` 是绑在 GameObject 上的**视图**（同 `Camera`），不是新组件 ——
    /// 本引擎的碰撞形状就住在 `RigidBodyComponent` 的资源里。这一组全部走资源
    /// 读，不碰物理场景，所以能真正断言形状尺寸和包围盒。
    void TestCollider()
    {
        ObjectManager* om = GET_SYSTEM(ObjectManager);
        if (om == nullptr)
        {
            om = new ObjectManager();
            SystemRegistry::GetInstance().RegisterSystem(om);
            om->Initialize();
        }
        Check(om != nullptr, "AC0", "ObjectManager is reachable");
        if (om == nullptr)
        {
            return;
        }

        // --- 空对象：有视图，但没有碰撞体 -------------------------------
        GameObject* empty_go = new GameObject();
        om->AllocateAndAssignInstanceID(empty_go);
        Transform* empty_transform = new Transform();
        om->AllocateAndAssignInstanceID(empty_transform);
        empty_go->addComponent(empty_transform);
        empty_transform->PostLoadResource(empty_go);

        Collider* none_col = Collider::FromGameObject(empty_go);
        Check(none_col != nullptr, "AC1", "FromGameObject never returns null for a live object");
        Check(none_col != nullptr && !none_col->IsValid(), "AC2",
              "an object with no RigidBody has an INVALID collider (not a null one)");
        Check(none_col != nullptr && none_col->GetShapeCount() == 0, "AC3", "and zero shapes");
        Check(Collider::FromGameObject(nullptr) == nullptr, "AC4",
              "but a null object does return null — the two cases stay distinguishable");
        Check(Collider::FromGameObject(empty_go) == none_col, "AC5",
              "the same object always yields the same Collider instance (safe to cache)");

        // --- 一个盒子 ----------------------------------------------------
        GameObject* go = new GameObject();
        om->AllocateAndAssignInstanceID(go);
        Transform* transform = new Transform();
        om->AllocateAndAssignInstanceID(transform);
        go->addComponent(transform);
        transform->PostLoadResource(go);

        RigidBodyComponent* rigidbody = new RigidBodyComponent();
        om->AllocateAndAssignInstanceID(rigidbody);
        go->addComponent(rigidbody);
        rigidbody->PostLoadResource(go);

        Box* box = om->NewObject<Box>();
        box->m_HalfExtents = Vector3(1.f, 2.f, 3.f);
        RigidBodyShape* box_shape = om->NewObject<RigidBodyShape>();
        box_shape->m_Type = RigidBodyShapeType::box;
        box_shape->m_Geometry = box;
        box_shape->m_LocalTransform = TransformTRS(Vector3(0.f, 0.f, 5.f), Quaternion::IDENTITY, Vector3::UNIT_SCALE);
        rigidbody->GetRigidbodyResMutable().m_Shapes.push_back(box_shape);

        Collider* col = Collider::FromGameObject(go);
        Check(col != nullptr && col->IsValid(), "AC6", "adding a shape makes the collider valid");
        Check(col->GetShapeCount() == 1, "AC7", "one shape is one shape");
        Check(col->GetShapeType(0) == static_cast<int32_t>(ColliderShapeType::box), "AC8",
              "and it reports ColliderShapeType.box");
        // Unity 的 BoxCollider.size 是**全尺寸**，资源里存的是半尺寸。
        Check(Near(col->GetShapeSize(0), Vector3(2.f, 4.f, 6.f), 0.001f), "AC9",
              "BoxCollider.size is the FULL size, not the half extents");
        Check(Near(col->GetShapeCenter(0), Vector3(0.f, 0.f, 5.f), 0.001f), "AC10",
              "and center is the local offset");
        Check(col->GetAttachedRigidbody() == rigidbody, "AC11", "attachedRigidbody is the owner's RigidBody");
        Check(col->GetGameObject() == go, "AC12", "and gameObject points back at the object");

        // --- 包围盒 ------------------------------------------------------
        transform->SetPosition(Vector3(10.f, 0.f, 0.f));
        Check(Near(col->GetBoundsCenter(), Vector3(10.f, 0.f, 5.f), 0.001f), "AC13",
              "bounds follow the object's position");
        Check(Near(col->GetBoundsSize(), Vector3(2.f, 4.f, 6.f), 0.001f), "AC14",
              "and keep the full size");
        Check(col->Contains(Vector3(10.f, 0.f, 5.f)), "AC15", "Contains is true at the centre");
        Check(!col->Contains(Vector3(10.f, 0.f, 20.f)), "AC16", "and false well outside");
        // 顶面上方 12 米 ⇒ 最近点落在顶面（z = 5 + 3 = 8）。
        Check(Near(col->ClosestPoint(Vector3(10.f, 0.f, 20.f)), Vector3(10.f, 0.f, 8.f), 0.001f), "AC17",
              "ClosestPoint clamps onto the box surface");

        // --- 旋转必须参与包围盒 ------------------------------------------
        // 一个 4x1x1 的盒子绕 Z 转 90° 之后，世界 AABB 应该是 1x4x1。少了旋转这一
        // 步的话包围盒还是 4x1x1 —— 一个歪着放的碰撞体会"看起来是正的"。
        box->m_HalfExtents = Vector3(2.f, 0.5f, 0.5f);
        transform->SetRotation(Quaternion::IDENTITY);
        transform->RotateAxis(Vector3::UNIT_Z, 90.f);
        const Vector3 rotated_size = col->GetBoundsSize();
        Check(Near(rotated_size, Vector3(1.f, 4.f, 1.f), 0.001f), "AC18",
              "a rotated box gets a rotated AABB (not the local one)");
        transform->SetRotation(Quaternion::IDENTITY);

        // --- 球与胶囊 ----------------------------------------------------
        Sphere* sphere = om->NewObject<Sphere>();
        sphere->m_Radius = 2.f;
        RigidBodyShape* sphere_shape = om->NewObject<RigidBodyShape>();
        sphere_shape->m_Type = RigidBodyShapeType::sphere;
        sphere_shape->m_Geometry = sphere;
        rigidbody->GetRigidbodyResMutable().m_Shapes.push_back(sphere_shape);

        Capsule* capsule = om->NewObject<Capsule>();
        capsule->m_Radius = 0.5f;
        capsule->m_HalfHeight = 1.f;
        RigidBodyShape* capsule_shape = om->NewObject<RigidBodyShape>();
        capsule_shape->m_Type = RigidBodyShapeType::capsule;
        capsule_shape->m_Geometry = capsule;
        rigidbody->GetRigidbodyResMutable().m_Shapes.push_back(capsule_shape);

        Check(col->GetShapeCount() == 3, "AC19", "a rigid body can carry several shapes (Unity same)");
        Check(col->GetShapeType(1) == static_cast<int32_t>(ColliderShapeType::sphere), "AC20", "shape 1 is a sphere");
        Check(Near(col->GetShapeRadius(1), 2.f, 0.001f), "AC21", "SphereCollider.radius round-trips");
        Check(Near(col->GetShapeSize(1), Vector3::ZERO, 0.0001f), "AC22",
              "and a sphere reports no box size (0, not the sphere's)");
        Check(col->GetShapeType(2) == static_cast<int32_t>(ColliderShapeType::capsule), "AC23", "shape 2 is a capsule");
        Check(Near(col->GetShapeRadius(2), 0.5f, 0.001f), "AC24", "CapsuleCollider.radius round-trips");
        // 同 CharacterController.height：含两端半球帽。
        Check(Near(col->GetShapeHeight(2), 3.0f, 0.001f), "AC25",
              "CapsuleCollider.height includes both caps: 2 * (1.0 + 0.5)");
        Check(!Near(col->GetShapeHeight(2), 2.0f, 0.001f), "AC26",
              "and is NOT the cylinder height (a 2.0 here is the classic off-by-caps bug)");
        // 越界：必须是 invalid，不能是 UB，也不能悄悄返回 0 号形状。
        Check(col->GetShapeType(99) == static_cast<int32_t>(ColliderShapeType::invalid), "AC27",
              "an out-of-range index reports invalid");
        Check(Near(col->GetShapeCenter(-1), Vector3::ZERO, 0.0001f), "AC28",
              "and a negative one yields zero rather than reading out of bounds");

        // --- 包围盒是所有形状的并集 --------------------------------------
        // 球半径 2 在盒子（顶面 z=8）之上 ⇒ 并集的 z 上界被推高。
        const Vector3 union_size = col->GetBoundsSize();
        Check(union_size.z > 6.0f, "AC29", "bounds are the union of every shape, not just the first");

        // --- 降级：没有物理场景时 ----------------------------------------
        Check(!col->GetIsTrigger(), "AC30", "isTrigger reads false with no physics scene (not a crash, not true)");
        col->SetIsTrigger(true);
        Check(!col->GetIsTrigger(), "AC31", "and the setter is a no-op rather than a hang");

        // --- 没有碰撞体时 ClosestPoint 必须返回点自己 --------------------
        // 返回零向量会被当成"包围盒在原点" —— 那是一个完全错误的答案，而且看
        // 起来像"碰撞体在原点"而不是"没有碰撞体"。
        Check(Near(none_col->ClosestPoint(Vector3(7.f, 8.f, 9.f)), Vector3(7.f, 8.f, 9.f), 0.0001f), "AC37",
              "ClosestPoint on an invalid collider returns the point itself (not zero)");

        // --- body id -> 对象 --------------------------------------------
        PhysicsManager* physics_manager = GET_SYSTEM(PhysicsManager);
        Check(physics_manager != nullptr, "AC32", "PhysicsManager is reachable");
        if (physics_manager != nullptr)
        {
            physics_manager->RegisterBodyOwner(4242u, go);
            Check(physics_manager->FindBodyOwner(4242u) == go, "AC33", "a registered body resolves to its owner");
            Check(physics_manager->FindBodyOwner(4243u) == nullptr, "AC34", "an unknown body resolves to null");
            Check(Collider::FromBodyId(4242u) == col, "AC35",
                  "Collider.FromBodyId lands on the same view as FromGameObject");
            Check(Collider::FromBodyId(kInvalidPhysicsBodyId) == nullptr, "AC36",
                  "and the sentinel body id returns null");
            physics_manager->UnregisterBodyOwner(4242u);
        }
    }

    // --------------------------------------------------------------- AD ----
    /// 碰撞体的接线门：`RaycastHit.gameObject` 那条链（命中 → 绑定 → d.ts）断在
    /// 任何一环都是"脚本拿到 undefined"，而 C++ 侧一切正常。
    void TestColliderWiring()
    {
        const std::string query_header = ReadFile("Source/Runtime/Function/Physics/PhysicsQuery.h");
        Check(!query_header.empty(), "AD0", "PhysicsQuery.h is readable");
        Check(CountOccurrences(query_header, "GameObject* gameObject") >= 1, "AD1",
              "RaycastHit carries a GameObject back-pointer");

        const std::string physics_source = ReadFile("Source/Runtime/Function/Physics/Physics.cpp");
        Check(!physics_source.empty(), "AD2", "Physics.cpp is readable");
        // 只在一个汇流点赋值 —— 20 个查询方法全靠它。改成逐个方法赋值迟早漏一个。
        Check(CountOccurrences(physics_source, "out.gameObject = ResolveBodyOwner(in_hit.body_id)") >= 1, "AD3",
              "the single ToRaycastHit funnel fills gameObject");
        Check(CountOccurrences(physics_source, "FindBodyOwner(") >= 1, "AD4",
              "and resolves it through PhysicsManager's body-owner table");

        const std::string binding_header = ReadFile("Source/Runtime/Scripting/Binding/ScriptBinding.h");
        Check(!binding_header.empty(), "AD5", "ScriptBinding.h is readable");
        // 数 **ToJs 那一行**：只数 `"gameObject"` 的话，FromJs 那一行也命中，
        // 于是"转换出去时漏了这个字段"的实现是全绿的（变异 M13 就是这么过的）。
        Check(CountOccurrences(binding_header, "Converter<GameObject*>::ToJs") >= 1, "AD6",
              "the RaycastHit converter writes gameObject to JS");
        // ⚠️ 声明顺序：`Converter<GameObject*>` 是非依赖名字，在模板**定义点**绑定。
        // Converter<RaycastHit> 必须排在 Object 派生指针那个特化之后，否则它绑到
        // 主模板上、static_assert 直接红。这个门守的是结构，不是字符串。
        Check(binding_header.find("Object-derived pointers") < binding_header.find("struct Converter<RaycastHit>"),
              "AD7", "Converter<RaycastHit> is declared AFTER the Object-pointer specialization");

        const std::string binding_source = ReadFile("Source/Runtime/Scripting/Binding/ScriptBinding.cpp");
        Check(!binding_source.empty(), "AD8", "ScriptBinding.cpp is readable");
        Check(CountOccurrences(binding_source, "gameObject: GameObject | null;") >= 1, "AD9",
              "and the .d.ts declares it (else TypeScript sees undefined)");

        const std::string collider_header = ReadFile("Source/Runtime/Scripting/ScriptCollider/ScriptCollider.h");
        Check(!collider_header.empty(), "AD10", "ScriptCollider.h is readable");
        Check(CountOccurrences(collider_header, "ZFUNCTION(") == 17, "AD11",
              "Collider exposes exactly 17 script methods");
        Check(CountOccurrences(collider_header, "ZSCRIPTPROPERTY(") == 6, "AD12",
              "and exactly 6 script properties");
        Check(CountOccurrences(collider_header, "ZENUM()") >= 1 &&
                  CountOccurrences(collider_header, "box = 0") >= 1 &&
                  CountOccurrences(collider_header, "sphere = 1") >= 1 &&
                  CountOccurrences(collider_header, "capsule = 2") >= 1,
              "AD13", "ColliderShapeType is published with explicit values");
        // Unity 的拼写。
        Check(CountOccurrences(collider_header, "\"isTrigger\"") >= 1, "AD14", "isTrigger is published");
        Check(CountOccurrences(collider_header, "\"attachedRigidbody\"") >= 1, "AD15",
              "attachedRigidbody is published");
        Check(CountOccurrences(collider_header, "\"boundsCenter\"") >= 1 &&
                  CountOccurrences(collider_header, "\"boundsSize\"") >= 1,
              "AD16", "bounds are published as center + size");
        // 本引擎"Collider 不是组件"这件事必须写在门面文件里，否则下一个人会去
        // GetComponent("Collider") 然后得到 null。
        Check(CountOccurrences(collider_header, "不是独立组件") >= 1, "AD17",
              "the header explains why Collider is a view, not a component");

        const std::string collider_source = ReadFile("Source/Runtime/Scripting/ScriptCollider/ScriptCollider.cpp");
        Check(!collider_source.empty(), "AD18", "ScriptCollider.cpp is readable");
        // 包围盒从资源算，不查场景 —— 这条决定了本组能不能测。
        Check(CountOccurrences(collider_source, "shape->m_LocalTransform.m_Position") >= 1, "AD19",
              "bounds are computed from the authored shapes (not from a physics query)");
        // ClosestPoint 是 AABB 上的，不是精确几何 —— 差别必须记录在案。
        Check(CountOccurrences(collider_header, "AABB") >= 1, "AD20",
              "the AABB (not exact-geometry) nature of ClosestPoint is documented");
    }

    // --------------------------------------------------------------- AE ----
    /// `PlayerPrefs`（Unity 同名静态类）。
    ///
    /// `UserPreferences` 早就实现完整了（int/float/string/bool + 落盘），但它是
    /// IEngineSystem 而不是 ZCLASS，脚本一个字都碰不到 —— 想存个最高分只能自己
    /// 写文件。这一组先测"没有偏好系统时的降级"（默认分支），再注册一台**不调
    /// Initialize** 的实例测真实读写。
    ///
    /// 为什么不 Initialize
    /// -------------------
    /// `Initialize()` 需要 `GET_SYSTEM(PlayerSettings)` 且会真的在用户目录下建目录
    /// 读文件。不 Initialize 的话 `m_PreferencesFilePath` 是空的，所有读写都在内存
    /// 里跑 —— 行为完全一致，且不碰磁盘。这也正好顺带验证了 `save()` 的空路径守卫。
    void TestPlayerPrefs()
    {
        // --- 降级：还没有 UserPreferences -------------------------------
        Check(GET_SYSTEM(UserPreferences) == nullptr, "AE0", "(the fixture starts with no UserPreferences)");
        Check(PlayerPrefs::GetInt("nope", 42) == 42, "AE1",
              "GetInt falls back to the default when there is no preference system");
        Check(Near(PlayerPrefs::GetFloat("nope", 1.5f), 1.5f, 0.0001f), "AE2", "and so does GetFloat");
        Check(PlayerPrefs::GetString("nope", "fallback") == "fallback", "AE3", "and GetString");
        Check(PlayerPrefs::GetBool("nope", true), "AE4", "and GetBool");
        Check(!PlayerPrefs::HasKey("nope"), "AE5",
              "HasKey reports false (saying true would make a script think it can read)");
        // 写必须是安全的空操作，不是崩。
        PlayerPrefs::SetInt("k", 1);
        PlayerPrefs::SetFloat("k", 1.f);
        PlayerPrefs::SetString("k", "v");
        PlayerPrefs::SetBool("k", true);
        PlayerPrefs::DeleteKey("k");
        PlayerPrefs::DeleteAll();
        PlayerPrefs::Save();
        Check(true, "AE6", "every write degrades to a no-op instead of crashing");

        // --- 装一台真的（不 Initialize，纯内存）--------------------------
        UserPreferences* prefs = new UserPreferences();
        SystemRegistry::GetInstance().RegisterSystem(prefs);
        Check(GET_SYSTEM(UserPreferences) == prefs, "AE7", "a UserPreferences can be registered");

        // 往返
        PlayerPrefs::SetInt("score", 1234);
        Check(PlayerPrefs::GetInt("score", 0) == 1234, "AE8", "an int round-trips");
        PlayerPrefs::SetFloat("volume", 0.75f);
        Check(Near(PlayerPrefs::GetFloat("volume", 0.f), 0.75f, 0.0001f), "AE9", "a float round-trips");
        PlayerPrefs::SetString("name", "zing");
        Check(PlayerPrefs::GetString("name", "") == "zing", "AE10", "a string round-trips");
        PlayerPrefs::SetBool("muted", true);
        Check(PlayerPrefs::GetBool("muted", false), "AE11", "a bool round-trips");

        // 默认值：键不存在时**必须**返回传入的默认值。返回 0 / "" 的话"第一次进
        // 游戏"这个分支永远走不到 —— 那是 PlayerPrefs 最经典的用法。
        Check(PlayerPrefs::GetInt("missing", 7) == 7, "AE12", "a missing int returns the given default");
        Check(Near(PlayerPrefs::GetFloat("missing", 2.5f), 2.5f, 0.0001f), "AE13", "and so does a float");
        Check(PlayerPrefs::GetString("missing", "def") == "def", "AE14", "and a string");
        Check(PlayerPrefs::GetBool("missing", true), "AE15", "and a bool");

        // HasKey 必须区分"没存过"和"存了个默认值"。
        Check(PlayerPrefs::HasKey("score"), "AE16", "HasKey is true for a stored key");
        Check(!PlayerPrefs::HasKey("missing"), "AE17", "and false for one that was never set");
        PlayerPrefs::SetInt("zero", 0);
        Check(PlayerPrefs::HasKey("zero"), "AE18",
              "a key stored as 0 still exists (GetInt == 0 is not the same question)");

        // 类型串味：写 int 读 float 不该崩，也不该返回垃圾。
        Check(Near(PlayerPrefs::GetFloat("score", -1.f), 1234.0f, 0.0001f), "AE19",
              "reading an int as a float widens it, it does not return garbage");

        // 删除
        PlayerPrefs::DeleteKey("score");
        Check(!PlayerPrefs::HasKey("score"), "AE20", "DeleteKey removes just that key");
        Check(PlayerPrefs::HasKey("name"), "AE21", "and leaves the others alone");

        // Save 在路径为空时必须安静地什么都不做（不 Initialize 的状态），否则
        // create_directories("") 抛异常被 catch 吞掉 —— 一个静默的失败。
        PlayerPrefs::Save();
        Check(PlayerPrefs::HasKey("name"), "AE22", "Save with no path is a no-op, not a wipe");

        PlayerPrefs::DeleteAll();
        Check(!PlayerPrefs::HasKey("name") && !PlayerPrefs::HasKey("volume") && !PlayerPrefs::HasKey("zero"),
              "AE23", "DeleteAll clears every key");
        // 删完还能继续写 —— SetObject 之后 allocator 必须还可用。
        PlayerPrefs::SetInt("after", 5);
        Check(PlayerPrefs::GetInt("after", 0) == 5, "AE24",
              "the document still accepts writes after DeleteAll");

        // 不析构也不注销：SystemRegistry 没有 UnregisterSystem，而进程马上就退出。
        // 与同文件其它夹具（new ObjectManager / new PhysicsManager）同样的取舍。
    }

    // --------------------------------------------------------------- AF ----
    /// PlayerPrefs 的接线门。
    void TestPlayerPrefsWiring()
    {
        const std::string prefs_header = ReadFile("Source/Runtime/Scripting/ScriptPlayerPrefs/ScriptPlayerPrefs.h");
        Check(!prefs_header.empty(), "AF0", "ScriptPlayerPrefs.h is readable");
        Check(CountOccurrences(prefs_header, "ZFUNCTION()") == 12, "AF1",
              "PlayerPrefs exposes exactly 12 script methods (a dropped ZFUNCTION is a silent API loss)");
        // Unity 的拼写，一个都不能少。
        Check(CountOccurrences(prefs_header, "static void SetInt(") >= 1 &&
                  CountOccurrences(prefs_header, "static int GetInt(") >= 1,
              "AF2", "SetInt / GetInt are declared");
        Check(CountOccurrences(prefs_header, "static void SetFloat(") >= 1 &&
                  CountOccurrences(prefs_header, "static float GetFloat(") >= 1,
              "AF3", "SetFloat / GetFloat are declared");
        Check(CountOccurrences(prefs_header, "static void SetString(") >= 1 &&
                  CountOccurrences(prefs_header, "static eastl::string GetString(") >= 1,
              "AF4", "SetString / GetString are declared");
        Check(CountOccurrences(prefs_header, "static bool HasKey(") >= 1 &&
                  CountOccurrences(prefs_header, "static void DeleteKey(") >= 1 &&
                  CountOccurrences(prefs_header, "static void DeleteAll(") >= 1 &&
                  CountOccurrences(prefs_header, "static void Save(") >= 1,
              "AF5", "HasKey / DeleteKey / DeleteAll / Save are all declared");
        // 默认值参数是 PlayerPrefs 的一半意义所在（"第一次进游戏"）。
        Check(CountOccurrences(prefs_header, "int default_value = 0") >= 1, "AF6",
              "GetInt takes a default value parameter");
        // 明文存储这件事必须写在门面文件里 —— 拿它存令牌是常见误用。
        Check(CountOccurrences(prefs_header, "明文") >= 1, "AF7",
              "the header warns that this is plaintext storage");

        const std::string prefs_source = ReadFile("Source/Runtime/Scripting/ScriptPlayerPrefs/ScriptPlayerPrefs.cpp");
        Check(!prefs_source.empty(), "AF8", "ScriptPlayerPrefs.cpp is readable");
        // 每一个入口都必须先解析系统再判空 —— 少一处就是一个崩溃，而不是"读不到"。
        Check(CountOccurrences(prefs_source, "ResolvePreferences()") >= 13, "AF9",
              "every entry point resolves the system (no unguarded GET_SYSTEM deref)");

        const std::string user_header = ReadFile("Source/Runtime/Resource/UserPreferences/UserPreferences.h");
        Check(!user_header.empty(), "AF10", "UserPreferences.h is readable");
        Check(CountOccurrences(user_header, "void DeleteAll();") >= 1, "AF11",
              "UserPreferences grew DeleteAll (it did not exist before this slice)");

        const std::string user_source = ReadFile("Source/Runtime/Resource/UserPreferences/UserPreferences.cpp");
        Check(!user_source.empty(), "AF12", "UserPreferences.cpp is readable");
        // 空路径守卫：没 Initialize 时 save() 不该去 create_directories("")。
        Check(CountOccurrences(user_source, "m_PreferencesFilePath.empty()") >= 1, "AF13",
              "save() guards the empty path instead of throwing into a swallowed catch");
    }

    // --------------------------------------------------------------- AG ----
    /// `Component` 的 Unity 门面。
    ///
    /// Unity 里脚本 99% 的"我在哪 / 我身上有什么"都写在 Component 上，而本引擎的
    /// Component 之前只有 3 个 ZFUNCTION —— TS 里 `this.GetComponent(...)` 压根
    /// 不存在，只能绕成 `this.component.gameObject.GetComponent(...)`，而这两者
    /// 在 Unity 里从来就是一句话。这一组钉两件事：
    ///
    ///   1. 转发**真的到了宿主**（不是空实现）—— 靠三级层级里 A/C 有刚体、
    ///      B 没有，让"含自己"和"向上找"给出两个不同答案；
    ///   2. **没有宿主时是空值而不是崩** —— `m_ParentObject == nullptr` 是组件
    ///      生命周期里真实存在的一段窗口（New 出来到 PostLoadResource 之间）。
    void TestComponentFacade()
    {
        ObjectManager* om = GET_SYSTEM(ObjectManager);
        if (om == nullptr)
        {
            om = new ObjectManager();
            SystemRegistry::GetInstance().RegisterSystem(om);
            om->Initialize();
        }
        Check(om != nullptr, "AG0", "ObjectManager is reachable");
        if (om == nullptr)
        {
            return;
        }

        // A(root) -> B(mid) -> C(leaf)。A 和 C 各挂一个 RigidBodyComponent，
        // B 不挂：这样"InParent 含自己"和"向上找"是两个不同的答案，能分得清
        // 实现到底是先查了自己还是直接跳到父级（后者在 Unity 里是错的）。
        auto make_object = [om](const char* in_name) -> GameObject* {
            GameObject* object = new GameObject();
            om->AllocateAndAssignInstanceID(object);
            Transform* transform = new Transform();
            om->AllocateAndAssignInstanceID(transform);
            object->addComponent(transform);
            transform->PostLoadResource(object);
            object->SetName(in_name);
            return object;
        };
        auto add_rigidbody = [om](GameObject* in_object) -> RigidBodyComponent* {
            RigidBodyComponent* rigidbody = new RigidBodyComponent();
            om->AllocateAndAssignInstanceID(rigidbody);
            in_object->addComponent(rigidbody);
            rigidbody->PostLoadResource(in_object);
            return rigidbody;
        };

        GameObject* object_a = make_object("A");
        GameObject* object_b = make_object("B");
        GameObject* object_c = make_object("C");
        object_b->GetTransform()->SetParent(object_a->GetTransform(), false);
        object_c->GetTransform()->SetParent(object_b->GetTransform(), false);
        RigidBodyComponent* rigidbody_a = add_rigidbody(object_a);
        RigidBodyComponent* rigidbody_c = add_rigidbody(object_c);

        // --- 两个 Unity 属性 -------------------------------------------
        Check(object_a->GetTransform()->GetParentObject() == object_a, "AG1",
              "gameObject points back at the host object");
        // Component.transform 同义于 gameObject.transform：拿自己的 Transform
        // 当被测组件，返回它自己，这一条同时验了"转发"和"自洽"。
        Check(object_c->GetTransform()->GetTransform() == object_c->GetTransform(), "AG2",
              "transform on a Transform is itself");

        // Unity 的 Component.name **就是 GameObject 的名字**，不是组件自己的
        // Object::name（这里是空串）—— 少了转发的话这一条立刻红。
        Check(object_a->GetTransform()->GetName() == "A", "AG3",
              "name is the host GameObject's name, not the component's own (empty) field");
        // 写必须穿透：只改自己那个字段的话，脚本 `this.name = "x"` 是句空话。
        object_b->GetTransform()->SetName("B2");
        Check(object_b->GetName() == "B2", "AG4", "SetName writes through to the GameObject");
        Check(object_b->GetTransform()->GetName() == "B2", "AG5", "and reads back the same");

        // --- tag -------------------------------------------------------
        Check(object_a->GetTransform()->GetTag() == TagLayerManager::kUntagged, "AG6",
              "tag forwards to the host (Untagged by default)");
        Check(object_a->GetTransform()->CompareTagName("Untagged"), "AG7", "CompareTag agrees");
        Check(object_a->GetTransform()->SetTagByName("Respawn"), "AG8",
              "SetTag on a component renames the host's tag");
        Check(object_a->GetTag() == "Respawn", "AG9", "and the host sees it");
        Check(!object_a->GetTransform()->CompareTagName("Untagged"), "AG10",
              "so the old tag no longer matches");
        Check(!object_a->GetTransform()->SetTagByName("NoSuchTag"), "AG11",
              "an unregistered tag is refused, not silently accepted");
        object_a->SetTagByName(TagLayerManager::kUntagged);

        // --- 查询：范围才是这些方法的全部区别 --------------------------
        Check(object_c->GetTransform()->GetComponentByTypeName("RigidBodyComponent") == rigidbody_c,
              "AG12", "GetComponent finds the host's own component");
        Check(object_b->GetTransform()->GetComponentByTypeName("RigidBodyComponent") == nullptr,
              "AG13", "and returns null where the host has none");
        Check(object_b->GetTransform()->GetComponentInParentByTypeName("RigidBodyComponent") == rigidbody_a,
              "AG14", "InParent walks up when the host has none");
        // ⚠️ 含自己 —— 直接跳到父级的实现在这里会给出 A 的刚体，而 Unity 给 C 的。
        Check(object_c->GetTransform()->GetComponentInParentByTypeName("RigidBodyComponent") == rigidbody_c,
              "AG15", "InParent INCLUDES self (Unity's GetComponentInParent does)");
        const std::vector<Component*> up_from_c =
            object_c->GetTransform()->GetComponentsInParentByTypeName("RigidBodyComponent");
        Check(up_from_c.size() == 2 && up_from_c[0] == rigidbody_c && up_from_c[1] == rigidbody_a,
              "AG16", "GetComponentsInParent is nearest-first (self, then A)");
        Check(object_a->GetTransform()->GetComponentsInParentByTypeName("RigidBodyComponent").size() == 1,
              "AG17", "and stops at the root");

        const std::vector<Component*> down_from_a =
            object_a->GetTransform()->GetComponentsInChildrenByTypeName("RigidBodyComponent");
        Check(down_from_a.size() == 2 && down_from_a[0] == rigidbody_a && down_from_a[1] == rigidbody_c,
              "AG18", "GetComponentsInChildren is pre-order (parent before child)");
        // GetComponents 只查自己 —— 与 InChildren 唯一的区别就是范围。
        Check(object_a->GetTransform()->GetComponentsByTypeName("RigidBodyComponent").size() == 1,
              "AG19", "GetComponents is self-only (it is NOT InChildren)");
        Check(object_a->GetTransform()->GetComponentsByTypeName("Transform").size() == 1, "AG20",
              "one Transform on the host");
        Check(object_a->GetTransform()->GetComponentsByTypeName("NoSuchType").empty(), "AG21",
              "an unknown type name yields an empty list (not a crash, not null)");
        // 基类也要能查：`GetComponents("Component")` 得捞到所有组件。
        Check(object_a->GetComponentsByTypeName("Component").size() == 2, "AG22",
              "the plural query matches base types too (Component finds every component)");

        // --- GameObject 一侧的复数查询 ---------------------------------
        Check(object_a->GetComponentsInParentByTypeName("Transform").size() == 1, "AG23",
              "GetComponentsInParent on the root is just itself");
        Check(object_c->GetComponentsInParentByTypeName("Transform").size() == 3, "AG24",
              "and walks the whole chain up (C, B, A)");

        // --- 消息：只验"路由到宿主"，投递本身在 F 组 --------------------
        Check(object_a->GetTransform()->SendMessageByName("OnNothing") == 0, "AG25",
              "SendMessage routes through the host");
        Check(object_a->GetTransform()->BroadcastMessageByName("OnNothing") == 0, "AG26",
              "BroadcastMessage routes through the host");
        Check(object_a->GetTransform()->SendMessageUpwardsByName("OnNothing") == 0, "AG27",
              "SendMessageUpwards routes through the host");

        // --- 没有宿主的组件：必须是空值，不能是崩 ----------------------
        // 这段窗口是真实存在的（MemoryManager 造出来 → PostLoadResource 挂上），
        // 脚本没理由知道它，但撞上时必须拿到合法空值。
        Component orphan;
        Check(orphan.GetTransform() == nullptr, "AG28", "a hostless component has no transform");
        Check(orphan.GetName().empty(), "AG29", "and falls back to its own (empty) Object::name");
        Check(orphan.GetTag() == TagLayerManager::kUntagged, "AG30",
              "its tag is Untagged — not an empty string (an empty tag makes CompareTag(\"\") match everything)");
        // 与 GetTag 必须自洽：返回 "Untagged" 却说不匹配就是自相矛盾的 API。
        Check(orphan.CompareTagName("Untagged"), "AG31", "CompareTag agrees with GetTag when hostless");
        Check(!orphan.CompareTagName("Respawn"), "AG32", "and does not match anything else");
        Check(!orphan.SetTagByName("Respawn"), "AG33",
              "SetTag reports false rather than promising a tag it cannot keep");
        Check(orphan.GetComponentByTypeName("Transform") == nullptr, "AG34", "no component to find");
        Check(orphan.GetComponentsByTypeName("Transform").empty(), "AG35", "and the plural form is empty");
        Check(orphan.GetComponentInChildrenByTypeName("Transform") == nullptr, "AG36", "no children either");
        Check(orphan.GetComponentsInChildrenByTypeName("Transform").empty(), "AG37", "still empty");
        Check(orphan.GetComponentInParentByTypeName("Transform") == nullptr, "AG38", "no parent either");
        Check(orphan.GetComponentsInParentByTypeName("Transform").empty(), "AG39", "still empty");
        Check(orphan.SendMessageByName("X") == 0 && orphan.BroadcastMessageByName("X") == 0 &&
                  orphan.SendMessageUpwardsByName("X") == 0,
              "AG40", "and every message reports 'nobody received it'");
    }

    // --------------------------------------------------------------- AH ----
    /// Component 门面的接线门。
    ///
    /// 这里的每个方法都是"转发一行"，所以门要守的不是算法而是**有没有被发布** ——
    /// 少一个 ZFUNCTION 在 C++ 侧毫无症状（编译过、跑得过），只是 TS 里那个名字
    /// 不存在。这类丢失只能靠源码级计数拦。
    void TestComponentFacadeWiring()
    {
        const std::string component_header =
            ReadFile("Source/Runtime/Function/Framework/Component/Component.h");
        Check(!component_header.empty(), "AH0", "Component.h is readable");
        // 18 = 3 个原有的（GetParentObject / isEnabled / SetEnabled）+ 本切片的 15 个。
        Check(CountOccurrences(component_header, "ZFUNCTION(") == 18, "AH1",
              "Component exposes exactly 18 script methods");
        Check(CountOccurrences(component_header, "ZSCRIPTPROPERTY(") == 4, "AH2",
              "and exactly 4 script properties (name / tag / transform / gameObject)");
        // Unity 的拼写，一个都不能少。
        Check(CountOccurrences(component_header, "\"GetComponents\"") >= 1 &&
                  CountOccurrences(component_header, "\"GetComponentsInParent\"") >= 1 &&
                  CountOccurrences(component_header, "\"GetComponentInChildren\"") >= 1 &&
                  CountOccurrences(component_header, "\"GetComponentsInChildren\"") >= 1 &&
                  CountOccurrences(component_header, "\"GetComponentInParent\"") >= 1,
              "AH3", "the Unity query spellings are all published");
        Check(CountOccurrences(component_header, "\"transform\"") >= 1 &&
                  CountOccurrences(component_header, "\"gameObject\"") >= 1 &&
                  CountOccurrences(component_header, "\"CompareTag\"") >= 1,
              "AH4", "transform / gameObject / CompareTag are published");
        // Component.h 只能前向声明 Transform —— 谁把 include 加进去谁就会撞上
        // 循环依赖，这条注释是给下一个人的。
        Check(CountOccurrences(component_header, "class Transform;") >= 1, "AH5",
              "Transform is forward-declared (including it would be a cycle)");

        const std::string component_source =
            ReadFile("Source/Runtime/Function/Framework/Component/Component.cpp");
        Check(!component_source.empty(), "AH6", "Component.cpp is readable");
        // 每个入口一处判空。少一处就是一个崩溃，而且是在"组件刚造出来还没挂上"
        // 这种最难复现的时序上崩。
        Check(CountOccurrences(component_source, "m_ParentObject != nullptr") >= 15, "AH7",
              "every forwarded entry point guards the hostless window");
        Check(CountOccurrences(component_source, "GameObject.h") >= 1 &&
                  CountOccurrences(component_source, "Transform/Transform.h") >= 1,
              "AH8", "the .cpp is where the host's full definition becomes reachable");

        const std::string go_header = ReadFile("Source/Runtime/BaseClasses/GameObject.h");
        Check(!go_header.empty(), "AH9", "GameObject.h is readable");
        Check(CountOccurrences(go_header, "\"GetComponents\"") >= 1 &&
                  CountOccurrences(go_header, "\"GetComponentsInParent\"") >= 1,
              "AH10", "GameObject publishes the two new plural queries");

        const std::string go_source = ReadFile("Source/Runtime/BaseClasses/GameObject.cpp");
        Check(!go_source.empty(), "AH11", "GameObject.cpp is readable");
        // 3 = 定义 1 + GetComponentsInParent 里的两处调用（自己、每个祖先）。
        // ⚠️ 数**不带类名限定**的调用：那个复数查询是通过 `this->` 调的，限定名
        // 只出现在定义处，数 "GameObject::" 前缀会永远停在 1。
        Check(CountOccurrences(go_source, "GetComponentsByTypeName(") >= 3, "AH12",
              "GetComponents is both defined and called (GetComponentsInParent funnels through it)");
        Check(CountOccurrences(go_source, "GameObject::GetComponentsInParentByTypeName") >= 1, "AH13",
              "GetComponentsInParent is defined");
        // 基类匹配：名字相等的实现会让 GetComponents("Component") 返回空。
        Check(CountOccurrences(go_source, "wanted->IsBaseOf") >= 2, "AH14",
              "the plural query matches on IsBaseOf, not on string equality");
    }

    // --------------------------------------------------------------- AI ----
    /// `Mathf` 的标量函数补全。
    ///
    /// 补全前 Mathf 只有 8 个函数，而标量数学是脚本里最容易被顺手用到的东西 ——
    /// 缺一个 `SmoothStep` 或 `Repeat`，玩法代码就只能拿裸的三角函数现拼，拼出来
    /// 的那一份和引擎其它部分不是一个口径。
    ///
    /// 这些是纯函数，所以断言都挑**能区分实现**的点：平局取偶、负数取模归正、
    /// 跨 0° 走最短弧、a==b 不产 NaN —— 而不是"代进去等于个值"。
    void TestMathf()
    {
        // --- 取整：平局规则才是重点 -------------------------------------
        Check(Near(Mathf::Abs(-3.5f), 3.5f, 0.0001f) && Near(Mathf::Abs(3.5f), 3.5f, 0.0001f) &&
                  Mathf::Abs(0.f) == 0.f,
              "AI0", "Abs is the magnitude");
        Check(Near(Mathf::Ceil(1.2f), 2.f, 0.0001f) && Near(Mathf::Ceil(-1.2f), -1.f, 0.0001f), "AI1",
              "Ceil goes toward +inf (not toward zero)");
        Check(Near(Mathf::Floor(1.8f), 1.f, 0.0001f) && Near(Mathf::Floor(-1.2f), -2.f, 0.0001f), "AI2",
              "Floor goes toward -inf");
        // ⚠️ 平局取偶：Round(2.5) 是 2，不是 3。std::round 会给 3。
        Check(Near(Mathf::Round(2.5f), 2.f, 0.0001f) && Near(Mathf::Round(1.5f), 2.f, 0.0001f) &&
                  Near(Mathf::Round(-2.5f), -2.f, 0.0001f),
              "AI3", "Round breaks ties to even (2.5 -> 2, not 3)");
        Check(Near(Mathf::Round(1.4f), 1.f, 0.0001f), "AI4", "and rounds normally otherwise");
        Check(Mathf::CeilToInt(1.2f) == 2 && Mathf::CeilToInt(-1.8f) == -1, "AI5", "CeilToInt truncates up");
        Check(Mathf::FloorToInt(1.8f) == 1 && Mathf::FloorToInt(-1.2f) == -2, "AI6", "FloorToInt truncates down");
        Check(Mathf::RoundToInt(2.5f) == 2, "AI7", "RoundToInt inherits the tie-to-even rule");

        // --- 三角 / 幂 ---------------------------------------------------
        Check(Near(Mathf::Sin(0.f), 0.f, 1e-5f) && Near(Mathf::Sin(Math_PI * 0.5f), 1.f, 1e-5f), "AI8",
              "Sin takes radians");
        Check(Near(Mathf::Cos(0.f), 1.f, 1e-5f) && Near(Mathf::Cos(Math_PI), -1.f, 1e-5f), "AI9",
              "Cos takes radians");
        Check(Near(Mathf::Tan(Math_PI * 0.25f), 1.f, 1e-5f), "AI10", "Tan takes radians");
        Check(Near(Mathf::Asin(1.f), Math_PI * 0.5f, 1e-4f), "AI11", "Asin returns radians");
        Check(Near(Mathf::Acos(1.f), 0.f, 1e-4f), "AI12", "Acos returns radians");
        Check(Near(Mathf::Atan(1.f), Math_PI * 0.25f, 1e-5f), "AI13", "Atan returns radians");
        // y 在前 —— 参数顺序写反在 I 象限里"看起来是对的"。
        Check(Near(Mathf::Atan2(1.f, 0.f), Math_PI * 0.5f, 1e-5f) && Near(Mathf::Atan2(0.f, 1.f), 0.f, 1e-5f),
              "AI14", "Atan2 is (y, x) like C and Unity");
        Check(Near(Mathf::Sqrt(16.f), 4.f, 1e-5f), "AI15", "Sqrt");
        Check(Near(Mathf::Pow(2.f, 10.f), 1024.f, 1e-3f), "AI16", "Pow");
        Check(Near(Mathf::Exp(0.f), 1.f, 1e-5f), "AI17", "Exp(0) is 1");
        // Log 是**自然对数** —— 拿 Log10 的实现顶替在这里必红。
        Check(Near(Mathf::Log(std::exp(1.f)), 1.f, 1e-4f) && Near(Mathf::Log10(1000.f), 3.f, 1e-4f), "AI18",
              "Log is the natural log and Log10 is base 10");
        Check(Mathf::Max(3.f, 7.f) == 7.f && Mathf::Min(3.f, 7.f) == 3.f, "AI19", "Max / Min");
        Check(Near(Mathf::HalfPi(), Math_PI * 0.5f, 1e-6f) && Near(Mathf::TwoPi(), Math_PI * 2.f, 1e-5f), "AI20",
              "HalfPi / TwoPi");

        // --- 环绕：负数必须归正 -----------------------------------------
        // fmod 在这里是错的（fmod(-1,3) == -1），而"角度算出来是负的"是常态。
        Check(Near(Mathf::Repeat(3.5f, 3.f), 0.5f, 1e-5f), "AI21", "Repeat wraps past the length");
        Check(Near(Mathf::Repeat(-1.f, 3.f), 2.f, 1e-5f), "AI22",
              "and maps negatives into [0, length) — fmod would give -1");
        Check(Near(Mathf::PingPong(0.f, 3.f), 0.f, 1e-5f) && Near(Mathf::PingPong(3.f, 3.f), 3.f, 1e-5f) &&
                  Near(Mathf::PingPong(4.f, 3.f), 2.f, 1e-5f),
              "AI23", "PingPong bounces inside [0, length]");

        // --- 角度：跨 0° 必须走短弧 -------------------------------------
        // 直接相减给 -340，正确答案是 +20 —— 差的那一下就是"转一大圈"。
        Check(Near(Mathf::DeltaAngle(350.f, 10.f), 20.f, 1e-4f), "AI24",
              "DeltaAngle takes the short way across 0 degrees");
        Check(Near(Mathf::DeltaAngle(10.f, 350.f), -20.f, 1e-4f), "AI25", "and is signed");
        Check(Near(Mathf::LerpAngle(350.f, 10.f, 0.5f), 360.f, 1e-4f), "AI26",
              "LerpAngle crosses 0 rather than sweeping backwards");
        Check(Near(Mathf::LerpAngle(10.f, 350.f, 0.5f), 0.f, 1e-4f), "AI27", "and the other way too");
        Check(Near(Mathf::MoveTowardsAngle(350.f, 10.f, 5.f), 355.f, 1e-4f), "AI28",
              "MoveTowardsAngle steps forward across 0");
        Check(Near(Mathf::MoveTowardsAngle(350.f, 10.f, 100.f), 10.f, 1e-4f), "AI29",
              "and snaps to the target when the step is big enough");

        // --- 插值 --------------------------------------------------------
        Check(Near(Mathf::LerpUnclamped(0.f, 10.f, 1.5f), 15.f, 1e-4f), "AI30",
              "LerpUnclamped extrapolates past 1");
        Check(Near(Mathf::InverseLerp(0.f, 10.f, 2.5f), 0.25f, 1e-4f), "AI31",
              "InverseLerp is Lerp's inverse");
        Check(Near(Mathf::InverseLerp(10.f, 0.f, 2.5f), 0.75f, 1e-4f), "AI32", "and respects a > b");
        // a == b 时必须给 0，不是 NaN —— NaN 参与的任何比较都是 false，会一路
        // 静默传下去，最后表现为"这个值莫名不动了"。
        Check(Mathf::InverseLerp(5.f, 5.f, 5.f) == 0.f, "AI33",
              "InverseLerp on a zero-length range gives 0, not NaN");
        Check(Near(Mathf::SmoothStep(0.f, 10.f, 0.5f), 5.f, 1e-4f), "AI34", "SmoothStep is symmetric");
        Check(Near(Mathf::SmoothStep(0.f, 10.f, -5.f), 0.f, 1e-4f) &&
                  Near(Mathf::SmoothStep(0.f, 10.f, 5.f), 10.f, 1e-4f),
              "AI35", "and clamps t (unlike Lerp)");

        // --- 比较 / 位 ---------------------------------------------------
        Check(Mathf::Approximately(1.f, 1.f + 1e-7f) && !Mathf::Approximately(1.f, 1.001f), "AI36",
              "Approximately is a relative+absolute test");
        // 0 附近必须走绝对判据：纯相对比较会让 0 和 1e-9 "相等"。
        Check(Mathf::Approximately(0.f, 0.f) && !Mathf::Approximately(0.f, 1e-3f), "AI37",
              "and still discriminates near zero");
        Check(Mathf::IsPowerOfTwo(8) && Mathf::IsPowerOfTwo(1) && !Mathf::IsPowerOfTwo(7) &&
                  !Mathf::IsPowerOfTwo(0),
              "AI38", "IsPowerOfTwo rejects 0 and non-powers");
        Check(Mathf::NextPowerOfTwo(7) == 8 && Mathf::NextPowerOfTwo(8) == 8 &&
                  Mathf::NextPowerOfTwo(1) == 1,
              "AI39", "NextPowerOfTwo is inclusive of exact powers");
        Check(Mathf::ClosestPowerOfTwo(7) == 8 && Mathf::ClosestPowerOfTwo(9) == 8 &&
                  Mathf::ClosestPowerOfTwo(3) == 4,
              "AI40", "ClosestPowerOfTwo picks the nearer one");

        // --- Perlin ------------------------------------------------------
        bool in_range = true;
        bool varies = false;
        for (int i = 0; i < 256; ++i)
        {
            const float n = Mathf::PerlinNoise(static_cast<float>(i) * 0.13f, static_cast<float>(i) * 0.29f);
            if (n < 0.f || n > 1.f)
            {
                in_range = false;
            }
            if (std::fabs(n - 0.5f) > 0.01f)
            {
                varies = true;
            }
        }
        Check(in_range, "AI41", "PerlinNoise stays inside [0, 1]");
        Check(varies, "AI42", "and actually varies across the domain");
        // 逐位相等，不是"接近" —— Near(x, x, 0) 是 `0 < 0`，恒假。
        Check(Mathf::PerlinNoise(3.7f, -1.25f) == Mathf::PerlinNoise(3.7f, -1.25f), "AI43",
              "and is deterministic (same input, bit-identical output)");
        // 格点上梯度贡献为 0 ⇒ 值恒为 0.5。这条同时钉住了插值权重的实现。
        Check(Near(Mathf::PerlinNoise(2.f, 5.f), 0.5f, 1e-5f), "AI44",
              "PerlinNoise is 0.5 on lattice points");

        // --- 色彩空间 ----------------------------------------------------
        Check(Mathf::GammaToLinearSpace(0.f) == 0.f && Near(Mathf::GammaToLinearSpace(1.f), 1.f, 1e-5f), "AI45",
              "GammaToLinearSpace fixes the endpoints");
        // sRGB 0.5 ≈ 线性 0.214 —— 纯 pow(x, 2.2) 给 0.2176，差在这条能看出来。
        Check(Near(Mathf::GammaToLinearSpace(0.5f), 0.2140f, 0.001f), "AI46",
              "GammaToLinearSpace(0.5) is 0.214, not pow(0.5, 2.2)");
        Check(Near(Mathf::LinearToGammaSpace(Mathf::GammaToLinearSpace(0.5f)), 0.5f, 1e-4f), "AI47",
              "and LinearToGammaSpace round-trips it");
        Check(Mathf::LinearToGammaSpace(0.f) == 0.f, "AI48", "LinearToGammaSpace(0) is 0");

        // --- 常量 --------------------------------------------------------
        Check(Mathf::Epsilon() > 0.f && Mathf::Epsilon() < 1e-3f, "AI49", "Epsilon is a tiny positive number");
        Check(Mathf::Infinity() > 1e30f && Mathf::NegativeInfinity() < -1e30f, "AI50",
              "Infinity / NegativeInfinity");
    }

    // --------------------------------------------------------------- AJ ----
    /// Mathf 补全的接线门。
    void TestMathfWiring()
    {
        const std::string math_header = ReadFile("Source/Runtime/Scripting/ScriptMath/ScriptMath.h");
        Check(!math_header.empty(), "AJ0", "ScriptMath.h is readable");
        // 48 是 VectorMath + QuaternionMath 原有的；Mathf 这一块是 49。
        Check(CountOccurrences(math_header, "ZFUNCTION()") == 98, "AJ1",
              "ScriptMath declares exactly 98 script methods (a dropped ZFUNCTION is a silent API loss)");
        // Unity 的拼写，一个都不能少。
        Check(CountOccurrences(math_header, "static float Abs(") >= 1 &&
                  CountOccurrences(math_header, "static float Ceil(") >= 1 &&
                  CountOccurrences(math_header, "static float Floor(") >= 1 &&
                  CountOccurrences(math_header, "static float Round(") >= 1,
              "AJ2", "Abs / Ceil / Floor / Round are declared");
        Check(CountOccurrences(math_header, "static float Atan2(") >= 1 &&
                  CountOccurrences(math_header, "static float Log10(") >= 1 &&
                  CountOccurrences(math_header, "static float Pow(") >= 1,
              "AJ3", "Atan2 / Log10 / Pow are declared");
        Check(CountOccurrences(math_header, "static float Repeat(") >= 1 &&
                  CountOccurrences(math_header, "static float PingPong(") >= 1 &&
                  CountOccurrences(math_header, "static float DeltaAngle(") >= 1 &&
                  CountOccurrences(math_header, "static float LerpAngle(") >= 1 &&
                  CountOccurrences(math_header, "static float MoveTowardsAngle(") >= 1,
              "AJ4", "the wrap-around family is declared");
        Check(CountOccurrences(math_header, "static float InverseLerp(") >= 1 &&
                  CountOccurrences(math_header, "static float SmoothStep(") >= 1 &&
                  CountOccurrences(math_header, "static bool Approximately(") >= 1,
              "AJ5", "InverseLerp / SmoothStep / Approximately are declared");
        Check(CountOccurrences(math_header, "static bool IsPowerOfTwo(") >= 1 &&
                  CountOccurrences(math_header, "static int32_t NextPowerOfTwo(") >= 1 &&
                  CountOccurrences(math_header, "static int32_t ClosestPowerOfTwo(") >= 1 &&
                  CountOccurrences(math_header, "static float PerlinNoise(") >= 1,
              "AJ6", "the power-of-two family and PerlinNoise are declared");
        // 数**声明本身**：函数名在注释里也出现，只数名字分不出"声明了"和"提到了"。
        Check(CountOccurrences(math_header, "static float GammaToLinearSpace(") >= 1 &&
                  CountOccurrences(math_header, "static float LinearToGammaSpace(") >= 1,
              "AJ7", "both colour-space conversions are declared");
        // 平局取偶这条规则必须写进源码，否则下一个人会"修好"它。
        Check(CountOccurrences(math_header, "取偶") >= 1, "AJ8",
              "the tie-to-even rule of Round is documented");
        // 静态属性做不出来（getter 不允许 static）—— 这个偏差必须记录，
        // 否则脚本会去写 `Mathf.Infinity` 然后拿到 undefined。
        Check(CountOccurrences(math_header, "静态只读字段") >= 1, "AJ9",
              "the header records that Epsilon/Infinity are methods, not fields");
        // SmoothDamp 缺的是 TS 的 ref 参数，不是忘了写。
        Check(CountOccurrences(math_header, "SmoothDamp") >= 1, "AJ10",
              "the header records why SmoothDamp is absent (no out params in TS)");
        // Perlin 不是 Unity 的逐位实现 —— 不写会被当成 bug。
        Check(CountOccurrences(math_header, "不是 Unity 的逐位实现") >= 1, "AJ11",
              "the header records that PerlinNoise is not bit-identical to Unity's");
    }

    // --------------------------------------------------------------- AK ----
    /// `VectorMath` / `QuaternionMath` 补齐 Unity 的 Vector3 / Quaternion 静态面。
    ///
    /// 断言都挑**能区分实现**的点：Slerp 与 Lerp 的路径差别（长度塌不塌）、
    /// 反向插值会不会产 NaN、RotateTowards 的角度单位、Project 与
    /// ProjectOnPlane 差一个减法 —— 而不是"代进去等于个值"。
    void TestVectorQuaternionExtras()
    {
        const Vector3 unit_x = Vector3::UNIT_X;
        const Vector3 unit_y = Vector3::UNIT_Y;
        const Vector3 unit_z = Vector3::UNIT_Z;

        // --- LerpUnclamped ---------------------------------------------
        Check(Near(VectorMath::LerpUnclamped(Vector3::ZERO, unit_x * 10.f, 1.5f), unit_x * 15.f, 1e-4f),
              "AK0", "LerpUnclamped extrapolates past 1");

        // --- Slerp ------------------------------------------------------
        // 路径差别就是这一条：Lerp 走弦，中点长度塌到 cos45 ≈ 0.707；
        // Slerp 走弧，长度全程是 1。
        Check(Near(VectorMath::Magnitude(VectorMath::Slerp(unit_x, unit_y, 0.5f)), 1.f, 1e-3f), "AK1",
              "Slerp keeps the magnitude (it walks the arc, not the chord)");
        Check(Near(VectorMath::Magnitude(VectorMath::Lerp(unit_x, unit_y, 0.5f)), 0.7071f, 1e-3f), "AK2",
              "while Lerp collapses it to cos(45) -- that is the whole difference");
        Check(Near(VectorMath::Slerp(unit_x, unit_y, 0.f), unit_x, 1e-4f) &&
                  Near(VectorMath::Slerp(unit_x, unit_y, 1.f), unit_y, 1e-4f),
              "AK3", "Slerp hits both endpoints");
        Check(Near(VectorMath::Slerp(unit_x, unit_y, -1.f), unit_x, 1e-4f) &&
                  Near(VectorMath::Slerp(unit_x, unit_y, 3.f), unit_y, 1e-4f),
              "AK4", "and clamps t");
        // 长度单独插值：2 与 4 的中点必须是 3。
        Check(Near(VectorMath::Magnitude(VectorMath::Slerp(unit_x * 2.f, unit_y * 4.f, 0.5f)), 3.f, 1e-3f),
              "AK5", "Slerp interpolates the magnitude separately");
        // 退化：同向时旋转轴是零向量，必须退回 Lerp 而不是归一化出 NaN。
        Check(Near(VectorMath::Slerp(unit_x, unit_x * 3.f, 0.5f), unit_x * 2.f, 1e-4f), "AK6",
              "Slerp on parallel vectors falls back to Lerp");
        const Vector3 zero_slerp = VectorMath::Slerp(Vector3::ZERO, unit_x * 2.f, 0.5f);
        Check(Near(zero_slerp, unit_x, 1e-4f) && !std::isnan(zero_slerp.x), "AK7",
              "Slerp from a zero vector is a Lerp, not a NaN");
        // 反向：旋转轴不唯一，必须给 fallback 轴，否则 getRotationTo 出 NaN。
        const Vector3 flipped = VectorMath::Slerp(unit_x, unit_x * -1.f, 0.5f);
        Check(!std::isnan(flipped.x) && !std::isnan(flipped.y) && !std::isnan(flipped.z) &&
                  Near(VectorMath::Magnitude(flipped), 1.f, 1e-3f),
              "AK8", "Slerp between opposite vectors stays on the unit sphere (no NaN)");

        // --- RotateTowards ----------------------------------------------
        // ⚠️ 弧度。0.1 rad ≈ 5.7°，不是 0.1°。
        const Vector3 rotated = VectorMath::RotateTowards(unit_x, unit_y, 0.1f, 0.f);
        Check(Near(VectorMath::AngleRad(unit_x, rotated), 0.1f, 1e-3f), "AK9",
              "RotateTowards advances by max_radians_delta (radians, not degrees)");
        Check(Near(VectorMath::Angle(unit_x, rotated), 5.7296f, 0.01f), "AK10", "and 0.1 rad really is 5.73 deg");
        // 一步够大就直接到位，不在目标附近抖。
        Check(Near(VectorMath::RotateTowards(unit_x, unit_y, 10.f, 0.f), unit_y, 1e-4f), "AK11",
              "RotateTowards snaps to the target when the step is big enough");
        // 长度是**另一个**参数管的，与转多少无关。
        Check(Near(VectorMath::Magnitude(VectorMath::RotateTowards(unit_x, unit_y * 2.f, 0.1f, 0.5f)), 1.5f,
                   1e-3f),
              "AK12", "RotateTowards moves the magnitude by its own budget");
        // 零向量没有方向可转，退到 MoveTowards。
        Check(Near(VectorMath::RotateTowards(Vector3::ZERO, unit_y * 2.f, 0.1f, 10.f), unit_y * 2.f, 1e-4f),
              "AK13", "RotateTowards from a zero vector degenerates to MoveTowards");

        // --- ProjectOnPlane ---------------------------------------------
        const Vector3 some(1.f, 2.f, 3.f);
        Check(Near(VectorMath::ProjectOnPlane(some, unit_z), Vector3(1.f, 2.f, 0.f), 1e-4f), "AK14",
              "ProjectOnPlane drops the component along the normal");
        // Project + ProjectOnPlane 必须**加起来等于原向量** —— 这正是"差一个减法"。
        Check(Near(VectorMath::Project(some, unit_z) + VectorMath::ProjectOnPlane(some, unit_z), some, 1e-4f),
              "AK15", "Project + ProjectOnPlane is the original vector");
        Check(Near(VectorMath::ProjectOnPlane(some, unit_z * 5.f), Vector3(1.f, 2.f, 0.f), 1e-4f), "AK16",
              "and the normal need not be unit length");

        // --- SignedAngle -------------------------------------------------
        Check(Near(VectorMath::SignedAngle(unit_x, unit_y, unit_z), 90.f, 1e-3f), "AK17",
              "SignedAngle is positive one way round the axis");
        Check(Near(VectorMath::SignedAngle(unit_y, unit_x, unit_z), -90.f, 1e-3f), "AK18",
              "and negative the other way");
        Check(Near(VectorMath::SignedAngle(unit_x, unit_y, unit_z * -1.f), -90.f, 1e-3f), "AK19",
              "flipping the axis flips the sign");
        Check(Near(std::fabs(VectorMath::SignedAngle(unit_x, unit_y, unit_z)),
                   VectorMath::Angle(unit_x, unit_y), 1e-3f),
              "AK20", "and its magnitude is the unsigned Angle");

        // --- QuaternionMath ----------------------------------------------
        Check(Near(QuaternionMath::Dot(Quaternion::IDENTITY, Quaternion::IDENTITY), 1.f, 1e-5f), "AK21",
              "Quaternion Dot(identity, identity) is 1");
        // q 与 -q 是同一个朝向，但点积是 -1 —— 判"是否同朝向"必须用绝对值。
        const Quaternion half_turn = QuaternionMath::AngleAxis(90.f, unit_z);
        // 旋转四元数是单位的，所以 Dot(q, q) == 1（不是 Dot(q, q⁻¹)：逆是共轭，
        // w 不变而 xyz 反号，点积是 w² - |v|²，对 90° 旋转等于 0）。
        Check(Near(QuaternionMath::Dot(half_turn, half_turn), 1.f, 1e-4f), "AK22",
              "Dot(q, q) is 1 for a rotation quaternion");
        Check(Near(QuaternionMath::Dot(half_turn, Quaternion(half_turn.w * -1.f, half_turn.x * -1.f,
                                                             half_turn.y * -1.f, half_turn.z * -1.f)),
                   -1.f, 1e-4f),
              "AK23", "while Dot(q, -q) is -1 even though they are the same orientation");
        const Quaternion normalized = QuaternionMath::Normalize(Quaternion(2.f, 0.f, 0.f, 0.f));
        Check(Near(QuaternionMath::Dot(normalized, Quaternion::IDENTITY), 1.f, 1e-4f), "AK24",
              "Normalize scales a non-unit quaternion down to unit length");
        const Quaternion zero_normalized = QuaternionMath::Normalize(Quaternion(0.f, 0.f, 0.f, 0.f));
        Check(!std::isnan(zero_normalized.w) && Near(zero_normalized.w, 1.f, 1e-5f), "AK25",
              "Normalize of a zero quaternion gives the identity, not NaN");

        const Quaternion quarter = QuaternionMath::AngleAxis(90.f, unit_z);
        const Quaternion stepped = QuaternionMath::RotateTowards(Quaternion::IDENTITY, quarter, 10.f);
        Check(Near(QuaternionMath::Angle(Quaternion::IDENTITY, stepped), 10.f, 0.01f), "AK26",
              "Quaternion RotateTowards advances by max_degrees_delta (degrees)");
        Check(QuaternionMath::Angle(QuaternionMath::RotateTowards(Quaternion::IDENTITY, quarter, 500.f),
                                    quarter) < 1e-3f,
              "AK27", "and reaches the target when the step is big enough");
        Check(QuaternionMath::Angle(QuaternionMath::RotateTowards(quarter, quarter, 10.f), quarter) < 1e-5f,
              "AK28", "RotateTowards from the target returns the target (no jitter, no NaN)");
        Check(Near(QuaternionMath::Rotate(QuaternionMath::FromToRotation(unit_x, unit_y), unit_x), unit_y,
                   1e-4f),
              "AK29", "QuaternionMath.FromToRotation actually rotates one onto the other");
        Check(Near(QuaternionMath::Dot(QuaternionMath::FromToRotation(unit_x, unit_y),
                                       VectorMath::FromToRotation(unit_x, unit_y)),
                   1.f, 1e-4f),
              "AK30", "and is the same rotation VectorMath.FromToRotation returns");
    }

    // --------------------------------------------------------------- AL ----
    void TestVectorQuaternionExtrasWiring()
    {
        const std::string math_header = ReadFile("Source/Runtime/Scripting/ScriptMath/ScriptMath.h");
        Check(!math_header.empty(), "AL0", "ScriptMath.h is readable");
        Check(CountOccurrences(math_header, "ZFUNCTION()") == 98, "AL1",
              "ScriptMath declares exactly 98 script methods");
        Check(CountOccurrences(math_header, "static Vector3 Slerp(") >= 1 &&
                  CountOccurrences(math_header, "static Vector3 LerpUnclamped(") >= 1 &&
                  CountOccurrences(math_header, "static Vector3 RotateTowards(") >= 1 &&
                  CountOccurrences(math_header, "static Vector3 ProjectOnPlane(") >= 1 &&
                  CountOccurrences(math_header, "static float SignedAngle(") >= 1,
              "AL2", "the new VectorMath methods are all declared");
        Check(CountOccurrences(math_header, "static float Dot(const Quaternion&") >= 1 &&
                  CountOccurrences(math_header, "static Quaternion Normalize(") >= 1 &&
                  CountOccurrences(math_header, "static Quaternion RotateTowards(") >= 1 &&
                  CountOccurrences(math_header, "static Quaternion FromToRotation(") >= 1,
              "AL3", "the new QuaternionMath methods are all declared");
        // ⚠️ 两个 RotateTowards 的**单位不一样**（弧度 vs 度）。这是 Unity 自己的
        // 不一致，照搬比统一安全，但必须写在源码里，否则会被"修好"。
        Check(CountOccurrences(math_header, "max_radians_delta") >= 1 &&
                  CountOccurrences(math_header, "max_degrees_delta") >= 1,
              "AL4", "both RotateTowards parameter units are present (radians vs degrees)");
        // 反向插值的 fallback 轴：不给的话 getRotationTo 出 NaN。
        Check(CountOccurrences(math_header, "fallback") >= 1, "AL5",
              "the antiparallel fallback axis is there");
        // 零长度 / 零向量必须显式守卫，且理由要写下来。
        Check(CountOccurrences(math_header, "NaN") >= 3, "AL6",
              "the degenerate-case guards are documented as NaN avoidance");
    }

    // --------------------------------------------------------------- AM ----
    /// 对象生命周期：`Destroy(obj, delay)` / `DestroyImmediate` / 按类型全场查找。
    ///
    /// 这一组能测，靠的是把逻辑放进**吃一张明确 Level** 的入口
    /// （`GameObject::CollectComponentsOfTypeIn` / `Level::QueueDestroy(id, delay)`）。
    /// 活动关卡只能经 `WorldManager::getCurrentActiveLevel()` 拿到，而它没有
    /// setter，headless 下恒为 null —— 只测 ZFUNCTION 那层壳的话，等于什么也没测。
    void TestObjectLifecycle()
    {
        ObjectManager* om = GET_SYSTEM(ObjectManager);
        if (om == nullptr)
        {
            om = new ObjectManager();
            SystemRegistry::GetInstance().RegisterSystem(om);
            om->Initialize();
        }
        Check(om != nullptr, "AM0", "ObjectManager is reachable");

        // 壳层那条"没有活动关卡"的分支必须是真的，否则下面的降级断言是空的。
        WorldManager* world = GET_SYSTEM(WorldManager);
        Check(world == nullptr || world->getCurrentActiveLevel() == nullptr, "AM1",
              "(there is no active level in this process, so the null-level path is the real one)");

        Level level;
        level.setLevelResUrl("Assets/Scenes/Lifecycle.level");

        // 空对象 / 空关卡：必须是安全的空操作。这两条在壳上是**测不到**的
        // （关卡检查先返回），所以只能在这儿测。
        GameObject::DestroyIn(&level, nullptr, 1.f, false);
        Transform* transform_h = nullptr;
        GameObject* object_h = MakeRuntimeObject(*om, level, &transform_h);
        GameObject::DestroyIn(nullptr, object_h, 1.f, false);
        Check(object_h != nullptr && level.FindRuntimeGObject(object_h->GetID()) == object_h, "AM1d",
              "DestroyIn tolerates a null object and a null level (neither is touched)");

        // --- 销毁：真实行为走 DestroyIn（吃一张明确的 Level）-------------
        // 壳（DestroyObject / DestroyObjectImmediate）在 headless 下永远走"没有
        // 活动关卡"那条路，测不到任何行为；所以行为在这里测，壳只测"不崩"。
        Transform* transform_f = nullptr;
        GameObject* object_f = MakeRuntimeObject(*om, level, &transform_f);
        if (object_f != nullptr)
        {
            // ⚠️ id 必须在销毁**之前**存下来：FlushPendingDestroys 会真的 delete
            // 掉这个对象，之后再碰 `object_f->` 任何东西都是 use-after-free。
            // 它通常"能跑"（mimalloc 不立刻把页还回去），于是崩溃只在某次堆布局
            // 下才出现 —— 这正是最难查的那类 bug。
            const GObjectID id_f = object_f->GetID();
            GameObject::DestroyIn(&level, object_f, 1.f, false);
            Check(level.GetDelayedDestroyCount() == 1, "AM1a",
                  "Destroy(obj, t) parks the object on a timer instead of killing it now");
            level.TickDestroyTimers(1.1f);
            level.FlushPendingDestroys();
            Check(level.FindRuntimeGObject(id_f) == nullptr, "AM1b",
                  "and it dies once the timer runs out and the frame flushes");
        }

        // DestroyImmediate：**当场**就没了（同一条调用里，不需要等 Tick）。
        Transform* transform_g = nullptr;
        GameObject* object_g = MakeRuntimeObject(*om, level, &transform_g);
        if (object_g != nullptr)
        {
            const GObjectID id_g = object_g->GetID();  // 同上：销毁前存
            GameObject::DestroyIn(&level, object_g, 0.f, true);
            Check(level.FindRuntimeGObject(id_g) == nullptr, "AM1c",
                  "DestroyImmediate is immediate -- no Tick, no flush by the caller");
        }

        // --- 延迟销毁：倒计时 -------------------------------------------
        Transform* transform_a = nullptr;
        GameObject* object_a = MakeRuntimeObject(*om, level, &transform_a);
        Check(object_a != nullptr, "AM2", "a runtime object can be registered on the level");
        if (object_a == nullptr)
        {
            return;
        }
        const GObjectID id_a = object_a->GetID();

        level.QueueDestroy(id_a, 1.f);
        Check(level.GetDelayedDestroyCount() == 1, "AM3", "Destroy(obj, t) parks the object on a timer");
        level.TickDestroyTimers(0.4f);
        Check(level.GetDelayedDestroyCount() == 1 && level.FindRuntimeGObject(id_a) != nullptr, "AM4",
              "and it survives a partial tick (0.4 of 1.0)");
        level.TickDestroyTimers(0.4f);
        Check(level.GetDelayedDestroyCount() == 1, "AM5", "and still at 0.8");
        // 到点只是"移进帧末队列"，**不等于**已经销毁 —— 销毁仍然统一在帧末做，
        // 否则在 Tick 中途销毁会让遍历的迭代器失效。
        level.TickDestroyTimers(0.3f);
        Check(level.GetDelayedDestroyCount() == 0 && level.FindRuntimeGObject(id_a) != nullptr, "AM6",
              "crossing the deadline only moves it to the end-of-frame queue");
        level.FlushPendingDestroys();
        Check(level.FindRuntimeGObject(id_a) == nullptr, "AM7", "and the frame-end flush actually destroys it");

        // --- delay <= 0 就是原来的"这一帧末" -----------------------------
        Transform* transform_b = nullptr;
        GameObject* object_b = MakeRuntimeObject(*om, level, &transform_b);
        const GObjectID id_b = (object_b != nullptr) ? object_b->GetID() : k_invalid_gobject_id;
        level.QueueDestroy(id_b, 0.f);
        Check(level.GetDelayedDestroyCount() == 0, "AM8",
              "Destroy(obj, 0) skips the timer entirely (same path as before)");
        level.FlushPendingDestroys();
        Check(level.FindRuntimeGObject(id_b) == nullptr, "AM9", "and dies at the end of this frame");

        // --- 重复请求：保留更早的时间点 ----------------------------------
        // 覆盖成更晚的时间会让对象比第一次请求的时刻活得更久 —— 那不是"再请求
        // 一次"的意思。
        Transform* transform_c = nullptr;
        GameObject* object_c = MakeRuntimeObject(*om, level, &transform_c);
        const GObjectID id_c = (object_c != nullptr) ? object_c->GetID() : k_invalid_gobject_id;
        level.QueueDestroy(id_c, 2.f);
        level.QueueDestroy(id_c, 5.f);
        level.TickDestroyTimers(2.1f);
        Check(level.GetDelayedDestroyCount() == 0, "AM10",
              "a second, later request does not push the deadline out");
        level.FlushPendingDestroys();
        Check(level.FindRuntimeGObject(id_c) == nullptr, "AM11", "and it dies on the first deadline");

        // --- 按类型全场查找 ----------------------------------------------
        Transform* transform_d = nullptr;
        GameObject* object_d = MakeRuntimeObject(*om, level, &transform_d);
        Transform* transform_e = nullptr;
        GameObject* object_e = MakeRuntimeObject(*om, level, &transform_e);
        RigidBodyComponent* rigidbody_e = new RigidBodyComponent();
        om->AllocateAndAssignInstanceID(rigidbody_e);
        object_e->addComponent(rigidbody_e);
        rigidbody_e->PostLoadResource(object_e);

        const std::vector<Component*> bodies =
            GameObject::CollectComponentsOfTypeIn(&level, "RigidBodyComponent");
        Check(bodies.size() == 1 && bodies[0] == rigidbody_e, "AM12",
              "FindObjectsOfType finds the one component of that type in the whole level");
        // 数**相对于关卡里活着的对象数**，不写死数字：上面几段会往同一张表里
        // 加加减减，写死的话任何一处顺序变动都会让这两条红，而红的原因和被测的
        // 东西无关。
        const size_t live_objects = level.GetRuntimeGObjects().size();
        Check(GameObject::CollectComponentsOfTypeIn(&level, "Transform").size() == live_objects, "AM13",
              "and one transform per live object");
        // 基类也要算命中 —— 与 GameObject.GetComponents 同一个判据。
        Check(GameObject::CollectComponentsOfTypeIn(&level, "Component").size() == live_objects + 1, "AM14",
              "a base type matches every component (one per object, plus the rigidbody)");
        Check(GameObject::CollectComponentsOfTypeIn(&level, "NoSuchType").empty(), "AM15",
              "an unknown type name is empty, not a crash");
        Check(GameObject::CollectComponentsOfTypeIn(nullptr, "Transform").empty(), "AM16",
              "a null level is empty too (the shells pass one every time there is no active level)");
        Check(object_d != nullptr && object_e != nullptr, "AM17", "(both search fixture objects are alive)");

        // --- 壳层：没有活动关卡时必须安全 --------------------------------
        // 这里测的是"不崩"，不是"有用" —— 真正的行为在上面用明确的 Level 测过了。
        GameObject::DestroyObject(object_d, 1.f);
        GameObject::DestroyObjectImmediate(object_e);
        Check(true, "AM18", "the Destroy shells degrade to a no-op instead of crashing with no level");
        Check(GameObject::FindFirstComponentOfType("Transform") == nullptr, "AM19",
              "FindObjectOfType returns null with no active level");
        Check(GameObject::FindAllComponentsOfType("Transform").empty(), "AM20",
              "and FindObjectsOfType returns an empty list");
        GameObject::DestroyObject(nullptr, 1.f);
        GameObject::DestroyObjectImmediate(nullptr);
        Check(true, "AM21", "and both accept a null object");
    }

    // --------------------------------------------------------------- AN ----
    void TestObjectLifecycleWiring()
    {
        const std::string go_header = ReadFile("Source/Runtime/BaseClasses/GameObject.h");
        Check(!go_header.empty(), "AN0", "GameObject.h is readable");
        Check(CountOccurrences(go_header, "ZFUNCTION(") == 32, "AN1",
              "GameObject exposes exactly 32 script methods");
        // Unity 的拼写。
        Check(CountOccurrences(go_header, "\"DestroyImmediate\"") >= 1 &&
                  CountOccurrences(go_header, "\"FindObjectOfType\"") >= 1 &&
                  CountOccurrences(go_header, "\"FindObjectsOfType\"") >= 1,
              "AN2", "DestroyImmediate / FindObject(s)OfType are published");
        // ⚠️ delay 是必填的：ZFUNCTION 没有可选参数，这条必须写在头文件里，
        // 否则下一个人会以为 Destroy(obj) 还成立。
        Check(CountOccurrences(go_header, "static void DestroyObject(GameObject* gobject, float delay = 0.f)") >= 1,
              "AN3", "Destroy takes an explicit delay (there are no optional script parameters)");
        // 逻辑必须在"给一张 Level"的版本里，否则 headless 测不到。
        Check(CountOccurrences(go_header, "CollectComponentsOfTypeIn") >= 1, "AN4",
              "the type search has a level-explicit core");
        Check(CountOccurrences(go_header, "static void DestroyIn(Level* level") >= 1, "AN4b",
              "and so does destruction");

        const std::string go_source = ReadFile("Source/Runtime/BaseClasses/GameObject.cpp");
        Check(!go_source.empty(), "AN5", "GameObject.cpp is readable");
        // 两个外壳 + 定义本身；数**不带类名限定**的名字（调用是 `CollectComponentsOfTypeIn(`）。
        Check(CountOccurrences(go_source, "CollectComponentsOfTypeIn(") >= 3, "AN6",
              "both type-search shells funnel through the level-explicit core");
        Check(CountOccurrences(go_source, "level->QueueDestroy(gobject->GetID(), delay)") >= 1, "AN7",
              "Destroy(obj, t) really forwards the delay to the level");
        // DestroyImmediate 必须复用队列，不能另写一套销毁。
        Check(CountOccurrences(go_source, "level->FlushPendingDestroys()") >= 1, "AN8",
              "DestroyImmediate reuses the frame-end path instead of reimplementing destruction");
        // 两个壳 + 定义本身。
        Check(CountOccurrences(go_source, "DestroyIn(") >= 3, "AN8b",
              "both Destroy shells funnel through DestroyIn");

        const std::string level_header = ReadFile("Source/Runtime/Function/Framework/Level/Level.h");
        Check(!level_header.empty(), "AN9", "Level.h is readable");
        Check(CountOccurrences(level_header, "void QueueDestroy(GObjectID go_id, float delay_seconds);") >= 1,
              "AN10", "Level has the delayed QueueDestroy");
        Check(CountOccurrences(level_header, "void TickDestroyTimers(float delta_time);") >= 1, "AN11",
              "and a separately drivable timer tick");
        Check(CountOccurrences(level_header, "m_DelayedDestroys") >= 1, "AN12",
              "backed by its own timer list (not mixed into m_PendingDestroys)");

        const std::string level_source = ReadFile("Source/Runtime/Function/Framework/Level/Level.cpp");
        Check(!level_source.empty(), "AN13", "Level.cpp is readable");
        // 接线门：计时器必须**真的被 Tick 驱动**，不然延迟销毁永远不会发生。
        Check(CountOccurrences(level_source, "TickDestroyTimers(delta_time);") >= 1, "AN14",
              "Level::Tick drives the destroy timers (an undriven timer is a silent no-op)");
        // 游戏内时间而不是挂钟，这条要写下来。
        Check(CountOccurrences(level_source, "游戏内") >= 1, "AN15",
              "the delay is documented as scaled game time, not wall-clock");
    }

    // --------------------------------------------------------------- AO ----
    /// Unity 的 `PhysicMaterial`：摩擦 / 弹性 / 两种组合模式。
    ///
    /// 组合规则（`CombineSurfaceValues`）是**纯函数**、不碰 Jolt，所以这一组能在
    /// 没有物理世界的情况下把规则本身测干净。这是有意的：组合模式是整个特性里
    /// 最容易搞错又最难在真世界里复现的部分（它要两个特定材质接触才走一次），
    /// 所以规则必须能单独断言。
    /// 一张只有一个 1x1x1 盒子、带指定表面材质的 res。
    ///
    /// 走 `PhysicsScene::CreateRigidBody` 而不是 SpawnBox（后者直接造 Jolt body，
    /// 绕开了"res -> body"这一段，而那正是这个特性唯一可能没接上的地方）。
    RigidBodyComponentRes MakeBoxRes(ObjectManager& om,
                                     float friction,
                                     float bounciness,
                                     PhysicMaterialCombine friction_combine,
                                     PhysicMaterialCombine bounce_combine)
    {
        RigidBodyComponentRes res;
        res.SetActorType(RigidBodyActorType::dynamic);
        res.m_InverseMass = 0.1f;  // 10 kg
        res.m_Friction = friction;
        res.m_Bounciness = bounciness;
        res.SetFrictionCombine(friction_combine);
        res.SetBounceCombine(bounce_combine);

        // ⚠️ 必须经 ObjectManager::NewObject 而不是裸 `new`：PPtr<Geometry> 是按
        // InstanceID 解析的，裸 new 出来的对象 id 为 0，`toShape` 里那次
        // `static_cast<const Geometry*>(m_Geometry)` 拿到 null，建体静默失败
        // （只是一条 LOG_ERROR，测试表现为 "body id 无效"）。
        Box* box_geometry = om.NewObject<Box>();
        box_geometry->m_HalfExtents = Vector3(0.5f, 0.5f, 0.5f);

        RigidBodyShape* shape = om.NewObject<RigidBodyShape>();
        shape->m_Type = RigidBodyShapeType::box;
        shape->m_Geometry = box_geometry;
        res.m_Shapes.push_back(shape);
        return res;
    }

    TransformTRS MakeBoxTransform(const Vector3& position)
    {
        return TransformTRS(Vector3d(position), Quaternion::IDENTITY, Vector3(1.f, 1.f, 1.f));
    }

    void TestPhysicMaterialCombine()
    {
        using Combine = PhysicMaterialCombine;

        // --- 四种模式各自的算术 -------------------------------------------
        Check(Near(CombineSurfaceValues(0.2f, Combine::Average, 0.8f, Combine::Average), 0.5f, 1e-5f),
              "AO0", "Average is the mean of the two");
        Check(Near(CombineSurfaceValues(0.2f, Combine::Minimum, 0.8f, Combine::Minimum), 0.2f, 1e-5f),
              "AO1", "Minimum takes the smaller");
        Check(Near(CombineSurfaceValues(0.2f, Combine::Maximum, 0.8f, Combine::Maximum), 0.8f, 1e-5f),
              "AO2", "Maximum takes the larger");
        Check(Near(CombineSurfaceValues(0.5f, Combine::Multiply, 0.8f, Combine::Multiply), 0.4f, 1e-5f),
              "AO3", "Multiply is the product (0.5 * 0.8 = 0.4)");

        // 交换律：两种模式相同时结果必须与参数顺序无关。求解器给的两个 body 顺序
        // 不保证（broadphase 决定），不对称的组合会让同一个接触在两帧之间跳值。
        Check(Near(CombineSurfaceValues(0.8f, Combine::Minimum, 0.2f, Combine::Minimum),
                   CombineSurfaceValues(0.2f, Combine::Minimum, 0.8f, Combine::Minimum), 1e-6f),
              "AO4", "and every mode is symmetric in its two arguments");

        // --- 优先级：Average < Minimum < Multiply < Maximum ----------------
        // Unity 文档的原文规则：两个面模式不同时**取优先级高的那一个**，不是"各
        // 让一步"。顺序即优先级，所以有效模式就是 max。
        Check(Near(CombineSurfaceValues(0.2f, Combine::Average, 0.8f, Combine::Maximum), 0.8f, 1e-5f),
              "AO5", "Average vs Maximum -> Maximum wins");
        Check(Near(CombineSurfaceValues(0.2f, Combine::Average, 0.8f, Combine::Minimum), 0.2f, 1e-5f),
              "AO6", "Average vs Minimum -> Minimum wins");
        Check(Near(CombineSurfaceValues(0.5f, Combine::Average, 0.8f, Combine::Multiply), 0.4f, 1e-5f),
              "AO7", "Average vs Multiply -> Multiply wins");
        Check(Near(CombineSurfaceValues(0.2f, Combine::Minimum, 0.8f, Combine::Maximum), 0.8f, 1e-5f),
              "AO8", "Minimum vs Maximum -> Maximum wins");
        Check(Near(CombineSurfaceValues(0.5f, Combine::Minimum, 0.8f, Combine::Multiply), 0.4f, 1e-5f),
              "AO9", "Minimum vs Multiply -> Multiply wins");

        // 反着传也要一样：优先级判定不能依赖参数顺序，否则同一对物体换个 broadphase
        // 顺序就换了一种手感。
        Check(Near(CombineSurfaceValues(0.8f, Combine::Maximum, 0.2f, Combine::Average),
                   CombineSurfaceValues(0.2f, Combine::Average, 0.8f, Combine::Maximum), 1e-6f),
              "AO10", "priority does not depend on argument order");

        // --- 默认：必须是 Unity 的 0.6 / 0 / Average -----------------------
        RigidBodyComponentRes res;
        Check(Near(res.m_Friction, 0.6f, 1e-6f), "AO11", "a fresh res carries Unity's default friction (0.6)");
        Check(Near(res.m_Bounciness, 0.f, 1e-6f), "AO12", "and zero bounce, like Unity's default PhysicMaterial");
        Check(res.GetFrictionCombine() == Combine::Average && res.GetBounceCombine() == Combine::Average,
              "AO13", "and Average for both combines");

        // 越界值必须回到 Average 而不是变成野枚举：存档里这个键可能根本不存在
        // （老资源），也可能被手改成 7。
        Check(ToPhysicMaterialCombine(0) == Combine::Average && ToPhysicMaterialCombine(7) == Combine::Average &&
                  ToPhysicMaterialCombine(-3) == Combine::Average,
              "AO14", "an out-of-range combine mode reads as Average, not garbage");
        Check(ToPhysicMaterialCombine(static_cast<int>(Combine::Maximum)) == Combine::Maximum,
              "AO15", "and the in-range ones round-trip");

        // --- user data 打包 -----------------------------------------------
        // 组合模式搭在 Jolt body 的 user data 上（Jolt 没有别的槽位），所以打包
        // /解包必须是双射 —— 解错了就是一个"手感莫名其妙"的 bug，而且没法从日志
        // 里看出来。
        const uint64_t packed = PackCombineModes(Combine::Multiply, Combine::Maximum);
        Check(FrictionCombineFromUserData(packed) == Combine::Multiply, "AO16",
              "the friction combine survives the user-data round trip");
        Check(BounceCombineFromUserData(packed) == Combine::Maximum, "AO17",
              "and so does the bounce combine (separate bits, not the same field)");
        // 默认值（全是 0）必须解出 Average/Average：一个没写过 user data 的 body
        // （例如测试里直接 SpawnBox 造的）不该被当成野值。
        Check(FrictionCombineFromUserData(0) == Combine::Average && BounceCombineFromUserData(0) == Combine::Average,
              "AO18", "a zero user datum means Average/Average, which is the Unity default");
    }

    /// 物理材质在**真 Jolt body** 上的落地（写进去、读出来、夹住越界值）。
    ///
    /// 上面 AO 组只测规则；这一组测"规则真的接到了物理上"—— 一个从来没被
    /// CreateRigidBody 读过的材质字段，和没有这个字段是一样的。
    void TestPhysicMaterial()
    {
        PhysicsManager* physics_manager = GET_SYSTEM(PhysicsManager);
        Check(physics_manager != nullptr, "AP0", "PhysicsManager is reachable");
        if (physics_manager == nullptr)
        {
            return;
        }

        std::shared_ptr<PhysicsScene> scene = physics_manager->CreatePhysicsScene(Vector3(0.f, 0.f, -9.81f)).lock();
        Check(scene != nullptr, "AP1", "a physics scene can be created");
        if (!scene)
        {
            return;
        }

        const uint32_t body = SpawnBox(*scene, Vector3(0.f, 0.f, 50.f), 0, true);
        Check(body != kInvalidPhysicsBodyId, "AP2", "a dynamic body can be spawned");
        if (body == kInvalidPhysicsBodyId)
        {
            return;
        }

        // --- 读写往返 ------------------------------------------------------
        float friction = -1.f;
        float restitution = -1.f;
        Check(scene->GetBodySurface(body, friction, restitution), "AP3", "the surface of a live body is readable");
        Check(scene->SetBodyFriction(body, 0.25f) && scene->SetBodyRestitution(body, 0.75f), "AP4",
              "friction and restitution can be written");
        Check(scene->GetBodySurface(body, friction, restitution) && Near(friction, 0.25f, 1e-5f) &&
                  Near(restitution, 0.75f, 1e-5f),
              "AP5", "and read back unchanged (they are real Jolt body state, not a side table)");

        // 摩擦与弹性必须是**两个独立的量**：合成后摩擦为负会让 Jolt 的行为未定义。
        Check(scene->SetBodyFriction(body, 0.9f) && scene->GetBodySurface(body, friction, restitution) &&
                  Near(restitution, 0.75f, 1e-5f),
              "AP6", "writing friction does not disturb restitution");

        // --- 组合模式写进 user data ----------------------------------------
        Check(scene->SetBodyCombineModes(body, PhysicMaterialCombine::Multiply, PhysicMaterialCombine::Maximum),
              "AP7", "the combine modes can be written to a live body");
        // 读数只能间接验证（没有 getter），所以这里验的是"打包函数与写入用的是同一
        // 套编码"：把同样的值喂给打包函数，再断言 Jolt 侧解出来一致。
        Check(FrictionCombineFromUserData(PackCombineModes(PhysicMaterialCombine::Multiply,
                                                           PhysicMaterialCombine::Maximum)) ==
                  PhysicMaterialCombine::Multiply,
              "AP8", "the encoding the scene writes is the one the combine callback decodes");

        // --- 不存在的 body：必须是 false，不是崩 ---------------------------
        Check(!scene->GetBodySurface(kInvalidPhysicsBodyId, friction, restitution), "AP9",
              "an invalid body id reports failure rather than reading freed memory");
        Check(!scene->SetBodyFriction(kInvalidPhysicsBodyId, 0.5f), "AP10", "and refuses a write");

        // --- 组件层 --------------------------------------------------------
        // 组件是脚本真正碰的那一层。这里没有活动关卡（WorldManager 没有 setter），
        // 所以活体写入那条分支走不到 —— 但 res 的读写与夹紧是纯状态，能测。
        RigidBodyComponent body_component;
        Check(Near(body_component.GetFriction(), 0.6f, 1e-6f), "AP11",
              "a fresh RigidBodyComponent reports Unity's default friction");
        Check(Near(body_component.GetBounciness(), 0.f, 1e-6f), "AP12", "and zero bounce");
        Check(body_component.GetFrictionCombine() == PhysicMaterialCombine::Average &&
                  body_component.GetBounceCombine() == PhysicMaterialCombine::Average,
              "AP13", "with Average for both combines");

        body_component.SetFriction(0.15f);
        Check(Near(body_component.GetFriction(), 0.15f, 1e-6f), "AP14", "friction round-trips");
        // 负摩擦夹到 0：Jolt 明确要求合成后的值不能为负，而脚本里写 -0.1 几乎肯定
        // 是笔误。夹住比把 NaN/未定义行为留到求解器里好。
        body_component.SetFriction(-0.5f);
        Check(body_component.GetFriction() >= 0.f, "AP15", "a negative friction is clamped, not stored");
        // 弹性夹到 [0,1]：>1 会真的往系统里加能量，弹一次比一次高。
        body_component.SetBounciness(0.4f);
        Check(Near(body_component.GetBounciness(), 0.4f, 1e-6f), "AP16", "bounciness round-trips");
        body_component.SetBounciness(3.f);
        Check(body_component.GetBounciness() <= 1.f, "AP17", "bounciness above 1 is clamped");
        body_component.SetBounciness(-1.f);
        Check(body_component.GetBounciness() >= 0.f, "AP18", "and below 0 is clamped");

        // 组合模式：**先设弹性、后设摩擦**，然后两个一起断言。
        //
        // 顺序是有意的（变异 M13 就是被原来那个顺序放过去的）：如果两个模式共用
        // 一份存储，"设摩擦把弹性冲掉"这件事只有在**弹性先设**的时候才看得见 ——
        // 先设摩擦、后设弹性的话，被冲掉的值马上又被覆盖了，断言全绿。
        body_component.SetBounceCombine(PhysicMaterialCombine::Minimum);
        body_component.SetFrictionCombine(PhysicMaterialCombine::Maximum);
        Check(body_component.GetFrictionCombine() == PhysicMaterialCombine::Maximum, "AP19",
              "the friction combine round-trips");
        Check(body_component.GetBounceCombine() == PhysicMaterialCombine::Minimum, "AP20",
              "and setting the friction combine leaves the bounce combine alone");
        // 反方向也验一次：只验一个方向的话，"设弹性把摩擦冲掉"同样看不见。
        body_component.SetBounceCombine(PhysicMaterialCombine::Multiply);
        Check(body_component.GetFrictionCombine() == PhysicMaterialCombine::Maximum, "AP20a",
              "and setting the bounce combine leaves the friction combine alone");

        // --- 建体时真的读 res ----------------------------------------------
        // 这是整个特性唯一一条"行为级"的接线断言：前面读写的是 SpawnBox 直接造的
        // Jolt body，而脚本改的是 **res**。res -> Jolt 这一段如果没接上，前面
        // 全绿而特性等于不存在（能存档、能改脚本，但物理永远用默认值）。
        ObjectManager* body_om = GET_SYSTEM(ObjectManager);
        Check(body_om != nullptr, "AP20b", "ObjectManager is reachable for authoring a res");
        if (body_om == nullptr)
        {
            return;
        }

        const uint32_t authored = scene->CreateRigidBody(
            MakeBoxTransform(Vector3(0.f, 0.f, 80.f)),
            MakeBoxRes(*body_om, 0.11f, 0.62f, PhysicMaterialCombine::Minimum, PhysicMaterialCombine::Multiply), 0);
        Check(authored != kInvalidPhysicsBodyId, "AP21", "a body can be created from an authored res");
        if (authored != kInvalidPhysicsBodyId)
        {
            float authored_friction = -1.f;
            float authored_bounce = -1.f;
            Check(scene->GetBodySurface(authored, authored_friction, authored_bounce) &&
                      Near(authored_friction, 0.11f, 1e-5f) && Near(authored_bounce, 0.62f, 1e-5f),
                  "AP22", "CreateRigidBody puts the authored friction and bounce on the body");

            // 反过来验一次：默认值不能"恰好"等于被测值，否则这条断言在材质没被
            // 应用时也会绿。
            const uint32_t plain = scene->CreateRigidBody(
                MakeBoxTransform(Vector3(0.f, 0.f, 90.f)),
                MakeBoxRes(*body_om, 0.6f, 0.f, PhysicMaterialCombine::Average, PhysicMaterialCombine::Average), 0);
            float plain_friction = -1.f;
            Check(plain != kInvalidPhysicsBodyId && scene->GetBodySurface(plain, plain_friction, authored_bounce) &&
                      std::fabs(plain_friction - authored_friction) > 0.1f,
                  "AP23", "and two different materials really do land on two different bodies");
        }
    }

    void TestPhysicMaterialWiring()
    {
        const std::string rigidbody_res = ReadFile("Source/Runtime/Resource/ResType/Components/RigidBody.h");
        Check(!rigidbody_res.empty(), "AQ0", "RigidBody.h is readable");
        Check(CountOccurrences(rigidbody_res, "enum class PhysicMaterialCombine") >= 1, "AQ1",
              "the combine enum exists");
        // 组合规则必须是**函数**而不是散在 Jolt 回调里的 if：散在回调里就没法在
        // 没有物理世界的情况下测（AO 组就是这么测的）。
        Check(CountOccurrences(rigidbody_res, "inline float CombineSurfaceValues(") >= 1, "AQ2",
              "the combine rule is one free function, not duplicated in the solver callback");

        const std::string scene_source = ReadFile("Source/Runtime/Function/Physics/PhysicsScene.cpp");
        Check(!scene_source.empty(), "AQ3", "PhysicsScene.cpp is readable");
        // 接线门 1：建体时必须**读** res 里的材质，否则这个特性等于不存在
        // （能存档、能改脚本、但物理永远用 Jolt 的默认值）。
        Check(CountOccurrences(scene_source, "body_settings.mFriction = rigidbody_actor_res.m_Friction;") >= 1,
              "AQ4", "CreateRigidBody applies the authored friction");
        Check(CountOccurrences(scene_source, "body_settings.mRestitution = rigidbody_actor_res.m_Bounciness;") >= 1,
              "AQ5", "and the authored bounce");
        Check(CountOccurrences(scene_source, "body_settings.mUserData =") >= 1, "AQ6",
              "and packs the combine modes into the body's user data");
        // 接线门 2：组合函数必须**装到 Jolt 上**。不装的话 Jolt 用自己那套
        // （sqrt(f1*f2) / max(r1,r2)），跟 Unity 四种模式都不一样，而症状只是
        // "手感不太对"，没有任何日志。
        Check(CountOccurrences(scene_source, "SetCombineFriction(") >= 1, "AQ7",
              "the friction combine is installed on the Jolt system");
        Check(CountOccurrences(scene_source, "SetCombineRestitution(") >= 1, "AQ8",
              "and so is the restitution combine");

        const std::string scene_header = ReadFile("Source/Runtime/Function/Physics/PhysicsScene.h");
        Check(!scene_header.empty(), "AQ9", "PhysicsScene.h is readable");
        Check(CountOccurrences(scene_header, "bool SetBodyFriction(uint32_t body_id, float friction);") >= 1,
              "AQ10", "a live body's friction can be changed");
        Check(CountOccurrences(scene_header, "bool SetBodyRestitution(uint32_t body_id, float restitution);") >= 1,
              "AQ11", "and its restitution");

        // 接线门 3：slim 构建（无 Jolt）必须有对应的空实现，否则那里是
        // EXC_BAD_ACCESS 而不是"物理关掉了"。
        const std::string scene_null = ReadFile("Source/Runtime/Function/Physics/PhysicsSceneNull.cpp");
        Check(!scene_null.empty(), "AQ12", "PhysicsSceneNull.cpp is readable");
        Check(CountOccurrences(scene_null, "bool PhysicsScene::SetBodyFriction(") >= 1, "AQ13",
              "the Jolt-free build stubs SetBodyFriction");
        Check(CountOccurrences(scene_null, "bool PhysicsScene::SetBodyCombineModes(") >= 1, "AQ14",
              "and SetBodyCombineModes");

        const std::string component_header =
            ReadFile("Source/Runtime/Function/Framework/Component/Rigidbody/RigidbodyComponent.h");
        Check(!component_header.empty(), "AQ15", "RigidbodyComponent.h is readable");
        Check(CountOccurrences(component_header, "ZSCRIPTPROPERTY(Name=\"friction\"") >= 1, "AQ16",
              "friction is a script property, not just a method");
        Check(CountOccurrences(component_header, "ZSCRIPTPROPERTY(Name=\"bounciness\"") >= 1, "AQ17",
              "and so is bounciness");
        // 存档门：只加了字段没进 Transfer() 的话，存盘再读回来材质就丢了。
        Check(CountOccurrences(rigidbody_res, "transfer.Transfer(m_Friction, \"friction\");") >= 1, "AQ18",
              "friction is serialized, not just declared");
        Check(CountOccurrences(rigidbody_res, "transfer.Transfer(m_FrictionCombine, \"friction_combine\");") >= 1,
              "AQ19", "and so is the friction combine mode");
    }

    // --------------------------------------------------------------- AR ----
    /// Camera 门面补全：屏幕 <-> 视口、三张矩阵、ResetAspect、pixelRect、朝向。
    ///
    /// 之前 Unity 的六个坐标互转里**只有穿过相机的四条**，屏幕与视口之间得先绕道
    /// 世界（一次额外的矩阵求逆 + 白带的浮点误差）；而 `worldToCameraMatrix` /
    /// `cameraToWorldMatrix` / `projectionMatrix` 完全不存在 —— 自定义渲染、把世界
    /// 点投到 UI、手算视锥都要它们。
    void TestCameraExtras()
    {
        // --- 未绑定的 Camera：返回"空"，不是未定义 -----------------------
        Camera unbound;
        Check(Near(unbound.GetWorldToCameraMatrix(), Matrix4x4::IDENTITY), "AR0",
              "an unbound Camera reads an identity world-to-camera matrix (not garbage)");
        Check(Near(unbound.GetCameraToWorldMatrix(), Matrix4x4::IDENTITY), "AR1", "and an identity inverse");
        Check(Near(unbound.GetProjectionMatrix(), Matrix4x4::IDENTITY), "AR2", "and an identity projection");
        Check(Near(unbound.GetPixelRect(), Vector4(0.f, 0.f, 0.f, 0.f)), "AR3", "and an empty pixel rect");
        // 屏幕 <-> 视口只依赖**屏幕尺寸**、不依赖相机，所以未绑定时照样是一次真
        // 换算。断言"返回 ZERO"是错的（那等于把一次坐标转换静默变成没做），
        // 这里要的是"有结果且不出现 NaN"。
        const Vector3 unbound_viewport = unbound.ScreenToViewportPoint(Vector3(1.f, 2.f, 3.f));
        Check(std::isfinite(unbound_viewport.x) && std::isfinite(unbound_viewport.y) &&
                  Near(unbound_viewport.z, 3.f, 1e-6f),
              "AR4", "screen<->viewport needs no camera, so an unbound Camera still converts (no NaN)");
        Check(unbound.GetPosition() == Vector3::ZERO, "AR5", "and the position is the origin");

        // --- 绑上一台真实相机 -------------------------------------------
        RenderCamera rc;
        MakeRenderCamera(rc);
        Camera cam;
        cam.Bind(&rc);
        Check(cam.IsValid(), "AR6", "the Camera is bound");

        const float width = static_cast<float>(cam.GetPixelWidth());
        const float height = static_cast<float>(cam.GetPixelHeight());
        Check(width > 0.f && height > 0.f, "AR7", "(the screen has a pixel size)");

        // --- 屏幕 <-> 视口 ----------------------------------------------
        const Vector3 screen_point(width * 0.25f, height * 0.75f, 12.f);
        const Vector3 viewport = cam.ScreenToViewportPoint(screen_point);
        Check(Near(viewport.x, 0.25f, 1e-4f) && Near(viewport.y, 0.75f, 1e-4f), "AR8",
              "ScreenToViewportPoint divides by the pixel size");
        // z 是**深度**，不是第三个坐标：除以屏幕高度的话 ScreenToWorldPoint 拿到
        // 的深度会整体缩到 1/height，来回一趟就不再回到原点（而且只在乎深的地方
        // 才明显）。
        Check(Near(viewport.z, 12.f, 1e-4f), "AR9", "and leaves the depth alone (it is not a coordinate)");
        Check(Near(cam.ViewportToScreenPoint(viewport), screen_point, 1e-3f), "AR10",
              "ViewportToScreenPoint is its inverse");

        // 交叉验证：屏幕/视口这两条转换必须和已有的世界 <-> 屏幕 / 世界 <-> 视口
        // 是同一套缩放。各自单独测的话，"屏幕->视口" 用了 width/height 反过来也
        // 能通过自己的往返测试（它是自洽的错）。
        const Vector3 world_probe(3.f, 14.f, 2.f);
        const Vector3 probe_screen = cam.WorldToScreenPoint(world_probe);
        const Vector3 probe_viewport = cam.WorldToViewportPoint(world_probe);
        Check(Near(cam.ScreenToViewportPoint(probe_screen), probe_viewport, 1e-3f), "AR11",
              "screen->viewport agrees with world->viewport for the same world point");
        Check(Near(cam.ViewportToScreenPoint(probe_viewport), probe_screen, 1e-3f), "AR12",
              "and viewport->screen agrees with world->screen");

        // --- 矩阵 --------------------------------------------------------
        const Matrix4x4 world_to_camera = cam.GetWorldToCameraMatrix();
        const Matrix4x4 camera_to_world = cam.GetCameraToWorldMatrix();
        Check(!Near(world_to_camera, Matrix4x4::IDENTITY), "AR13", "the world-to-camera matrix is a real view matrix");
        // 必须**真的互为逆**，不只是"两个都不是单位阵"。用矩阵乘法验，而不是各
        // 自看点：一个只差平移的实现也能让两条"不是单位阵"的断言全绿。
        Check(Near(world_to_camera * camera_to_world, Matrix4x4::IDENTITY, 1e-3f), "AR14",
              "cameraToWorldMatrix really is the inverse of worldToCameraMatrix");
        // 顺带验一次"点变换回去还是自己"：纯矩阵相等有数值巧合的可能，点没有。
        const Vector4 world_h(world_probe.x, world_probe.y, world_probe.z, 1.f);
        const Vector4 round_tripped = camera_to_world * (world_to_camera * world_h);
        Check(Near(round_tripped, world_h, 1e-3f), "AR15",
              "so a world point survives world->camera->world unchanged");

        Check(!Near(cam.GetProjectionMatrix(), Matrix4x4::IDENTITY), "AR16", "the projection matrix is a real one");
        // TAA 关着的时候两张必须一样：那正是"没有抖动"的意思。
        Check(!rc.IsTaaEnabled(), "AR17", "(TAA is off on this camera)");
        Check(Near(cam.GetProjectionMatrix(), cam.GetNonJitteredProjectionMatrix(), 1e-6f), "AR18",
              "with TAA off the jittered and non-jittered projections are the same matrix");

        // 正交 + 开 TAA + 有抖动时，两张必须**不一样** —— 否则"非抖动"这一路根本
        // 没实现，而拾取会因为每帧差半个像素而点不中小物体。
        //
        // 用正交相机而不是透视相机来验：引擎当前只在正交分支里调了
        // ApplyTaaJitter（透视分支没调），拿透视相机会得到"两张相同"的假绿。
        // 那处不一致是渲染侧既有的，不在这个切片里改，但它决定了这条断言只能用
        // 正交相机写 —— 所以写下来了。
        RenderCamera taa_rc;
        MakeRenderCamera(taa_rc);
        taa_rc.SetOrthographic(true);
        taa_rc.SetTaaEnabled(true);
        taa_rc.SetTaaJitter(Vector2(0.01f, -0.01f));
        Camera taa_cam;
        taa_cam.Bind(&taa_rc);
        Check(!Near(taa_cam.GetProjectionMatrix(), taa_cam.GetNonJitteredProjectionMatrix(), 1e-6f), "AR19",
              "with TAA on, the non-jittered projection really differs from the jittered one");

        // --- ResetAspect --------------------------------------------------
        cam.SetAspect(1.0f);
        Check(Near(cam.GetAspect(), 1.0f, 1e-4f), "AR20", "SetAspect(1) sticks");
        cam.ResetAspect();
        // 回到**屏幕**宽高比：只把 aspect 清成 0 的话，下一次 GetProjectionMatrix
        // 会走 GetPersProjMatrix（用 m_Fovy），和 SetAspect 之前的表现又不一样。
        Check(Near(cam.GetAspect(), width / height, 1e-3f), "AR21",
              "ResetAspect goes back to the screen aspect ratio");

        // --- pixelRect ----------------------------------------------------
        const Vector4 rect = cam.GetPixelRect();
        Check(rect.z > 0.f && rect.w > 0.f, "AR22", "pixelRect has a positive size");
        // 一台占满渲染目标的相机，矩形就该是整块屏。宽高用反了的话这条会红。
        Check(Near(rect.z, width, 1.f) && Near(rect.w, height, 1.f), "AR23",
              "and for a full-target camera it is the whole screen");

        // --- 朝向 ---------------------------------------------------------
        Check(Near(cam.GetForward().length(), 1.f, 1e-3f) && Near(cam.GetUp().length(), 1.f, 1e-3f) &&
                  Near(cam.GetRight().length(), 1.f, 1e-3f),
              "AR24", "forward / up / right are unit vectors");
        // 三者必须两两正交：LookAt 的 up 与视线不垂直时，右手系会歪，"贴着相机右
        // 方向偏移 N 米"就会偏到别的方向上去。
        Check(std::fabs(cam.GetForward().dotProduct(cam.GetUp())) < 1e-3f &&
                  std::fabs(cam.GetForward().dotProduct(cam.GetRight())) < 1e-3f &&
                  std::fabs(cam.GetUp().dotProduct(cam.GetRight())) < 1e-3f,
              "AR25", "and mutually orthogonal (a real right-handed basis)");
        // 只验"单位 + 两两正交"是不够的：把 forward 和 up 换一下三者仍然全部通过，
        // 而"沿视线往前 5 米"就变成了"往上 5 米"。所以还要对上 LookAt 的目标。
        Check(Near(cam.GetForward(), Vector3(0.f, 1.f, 0.f), 1e-3f), "AR26",
              "forward really is the direction of the LookAt target");
        Check(Near(cam.GetUp(), Vector3(0.f, 0.f, 1.f), 1e-3f), "AR27",
              "and up is the up that LookAt was given (Z-up)");
    }

    void TestCameraExtrasWiring()
    {
        const std::string header = ReadFile("Source/Runtime/Scripting/ScriptCamera/ScriptCamera.h");
        Check(!header.empty(), "AS0", "ScriptCamera.h is readable");
        // 接线门：矩阵必须是**方法声明**，不是只在 .cpp 里写了个函数。脚本只看
        // ZFUNCTION，漏一个就整条 API 不存在，而且不报错。
        Check(CountOccurrences(header, "Matrix4x4 GetWorldToCameraMatrix() const;") >= 1, "AS1",
              "worldToCameraMatrix is declared on the facade");
        Check(CountOccurrences(header, "Matrix4x4 GetCameraToWorldMatrix() const;") >= 1, "AS2",
              "and cameraToWorldMatrix");
        Check(CountOccurrences(header, "Matrix4x4 GetProjectionMatrix() const;") >= 1, "AS3",
              "and projectionMatrix");
        Check(CountOccurrences(header, "Vector3 ScreenToViewportPoint(") >= 1, "AS4",
              "ScreenToViewportPoint is declared");
        Check(CountOccurrences(header, "Vector3 ViewportToScreenPoint(") >= 1, "AS5",
              "ViewportToScreenPoint is declared");

        const std::string source = ReadFile("Source/Runtime/Scripting/ScriptCamera/ScriptCamera.cpp");
        Check(!source.empty(), "AS6", "ScriptCamera.cpp is readable");
        // cameraToWorld 必须**求逆**而不是用 position+rotation 现组装：LWC 下 view
        // 矩阵还带一块相对当前 tile 的平移，组装出来的和渲染用的不是同一张。
        Check(CountOccurrences(source, "GetViewMatrix().inverse()") >= 1, "AS7",
              "cameraToWorldMatrix is the inverse of the view matrix, not a re-assembled one");
        // 屏幕 -> 视口不能顺手把 z 也除了：z 是深度。
        Check(CountOccurrences(source, "screen_point.x / width, screen_point.y / height, screen_point.z") >= 1,
              "AS8", "ScreenToViewportPoint leaves the depth untouched");
        Check(CountOccurrences(source, "m_RenderCamera->ResetAspect();") >= 1, "AS9",
              "ResetAspect reaches the render camera (it is not a no-op)");
    }

    // ---- 合成导航关卡（Z-up）----------------------------------------------
    //
    // 与 `Function/AI/Test/NavMeshTest.cpp` 同一套做法：`NavMesh::Build` 只吃一个
    // 函数指针，就是为了能这样脱离物理单测。整个 AT 组因此不需要 PhysicsManager、
    // 不需要 WorldManager，也不需要激活关卡。
    struct NavBox
    {
        float min_x {0.f};
        float max_x {0.f};
        float min_y {0.f};
        float max_y {0.f};
        float min_z {0.f};
        float max_z {0.f};
    };

    std::vector<NavBox> g_nav_boxes;

    bool NavRayBoxHit(const Vector3& origin, const Vector3& direction, float length, const NavBox& box,
                      float& out_t)
    {
        //  slabs：返回 [0, length] 内最近的正向命中。
        float t_min = 0.f;
        float t_max = length;
        const float o[3] = {origin.x, origin.y, origin.z};
        const float d[3] = {direction.x, direction.y, direction.z};
        const float b_min[3] = {box.min_x, box.min_y, box.min_z};
        const float b_max[3] = {box.max_x, box.max_y, box.max_z};

        for (int axis = 0; axis < 3; ++axis)
        {
            if (std::fabs(d[axis]) < 1e-8f)
            {
                if (o[axis] < b_min[axis] || o[axis] > b_max[axis])
                {
                    return false;
                }
                continue;
            }
            const float inv = 1.f / d[axis];
            float t1 = (b_min[axis] - o[axis]) * inv;
            float t2 = (b_max[axis] - o[axis]) * inv;
            if (t1 > t2)
            {
                const float swap = t1;
                t1 = t2;
                t2 = swap;
            }
            if (t1 > t_min)
            {
                t_min = t1;
            }
            if (t2 < t_max)
            {
                t_max = t2;
            }
            if (t_min > t_max)
            {
                return false;
            }
        }
        out_t = t_min;
        return true;
    }

    bool NavSyntheticRaycast(const Vector3& origin, const Vector3& direction, float length,
                             Vector3& out_hit_position)
    {
        float best_t = length;
        bool found = false;
        for (const NavBox& box : g_nav_boxes)
        {
            float t = 0.f;
            if (NavRayBoxHit(origin, direction, length, box, t) && t < best_t)
            {
                best_t = t;
                found = true;
            }
        }
        if (!found)
        {
            return false;
        }
        out_hit_position = origin + direction * best_t;
        return true;
    }

    NavBox MakeNavBox(float cx, float cy, float hx, float hy, float min_z, float max_z)
    {
        NavBox box;
        box.min_x = cx - hx;
        box.max_x = cx + hx;
        box.min_y = cy - hy;
        box.max_y = cy + hy;
        box.min_z = min_z;
        box.max_z = max_z;
        return box;
    }

    /// 地板铺满网格；一道墙挡住 x ∈ [-0.5, 0.5]、y ∈ [-10, 0]，只在 y > 0 留缺口。
    /// AGENT_START → AGENT_GOAL 的直线**一定**穿墙，所以"走到终点"这件事本身就
    /// 证明了转向真的沿航点走。
    void ResetNavLevel()
    {
        g_nav_boxes.clear();
        g_nav_boxes.push_back(MakeNavBox(0.f, 0.f, 20.f, 20.f, -1.f, 0.f));   // 地板，顶面 z = 0
        g_nav_boxes.push_back(MakeNavBox(0.f, -5.f, 0.5f, 5.f, 0.f, 3.f));    // 墙
    }

    NavMeshBuildSettings NavFlatSettings()
    {
        NavMeshBuildSettings settings;
        settings.origin = Vector3(0.f, 0.f, 0.f);
        settings.cell_size = 1.f;
        settings.cells_x = 20;
        settings.cells_y = 20;
        settings.probe_height = 10.f;
        settings.probe_depth = 20.f;
        settings.agent_radius = 0.4f;
        settings.agent_height = 0.9f;
        settings.max_step_height = 0.6f;
        settings.max_slope = 0.7f;
        return settings;
    }

    // --------------------------------------------------------------- AT ----
    /// 导航代理（`NavMeshAgentComponent`）+ `Navigation` 静态门面。
    ///
    /// 补这个之前引擎有网格、有寻路，但没有"会走路的东西"：AI 层拿到航点数组后
    /// 各自现写推进逻辑。下面绝大多数断言盯的是**推进**这件事本身（不穿墙、不超
    /// 速、会绕、会减速、会停），因为寻路已经有 NavMeshTest 单测了。
    void TestNavMeshAgent()
    {
        const Vector3 agent_start(-5.f, -5.f, 0.f);
        const Vector3 agent_goal(5.f, -5.f, 0.f);
        const float dt = 1.f / 60.f;

        // --- 建一张网格 ---------------------------------------------------
        ResetNavLevel();
        NavigationSystem* nav = new NavigationSystem();
        SystemRegistry::GetInstance().RegisterSystem(nav);
        Check(GET_SYSTEM(NavigationSystem) == nav, "AT0", "NavigationSystem is registered");

        const int walkable = nav->BuildWithSampler(NavFlatSettings(), &NavSyntheticRaycast);
        Check(walkable > 0, "AT1", "the synthetic level builds a navmesh");
        // 墙必须真的把直线挡掉：不然下面所有"绕过去"的断言都是空转。
        //
        // 注意不能靠"可走格数变少"来判断 —— 墙顶那一格也是 walkable 的（网格按
        // 射线打到的第一个面取样，墙顶是一座孤岛），所以它只改了高度、没减格子。
        // 真正挡路的是 `IsConnected` 里的 max_step_height：孤岛与地面差 3 米，
        // 连不上。直接问"这个点站不站得住"才是这件事的判据。
        Check(!nav->IsStandable(Vector3(0.f, -5.f, 0.f)), "AT2",
              "and the wall really blocks the straight line (that point is not standable)");
        Check(nav->IsStandable(Vector3(-5.f, -5.f, 0.f)), "AT2a",
              "while the start is standable (so the check above is not just 'nothing is')");
        Check(nav->IsReady(), "AT3", "and the system reports ready");
        Check(Navigation::IsReady(), "AT4", "the script facade sees it too");
        Check(Navigation::GetWalkableCellCount() == walkable, "AT5", "and reports the same cell count");

        ObjectManager* om = GET_SYSTEM(ObjectManager);
        Check(om != nullptr, "AT6", "ObjectManager is reachable");

        Level level;
        level.setLevelResUrl("Assets/Scenes/Nav.level");
        level.LoadObjectGraph({}, false);
        Transform* transform = nullptr;
        GameObject* go = MakeRuntimeObject(*om, level, &transform);
        Check(go != nullptr && transform != nullptr, "AT7", "a runtime object with a Transform exists");
        if (go == nullptr || transform == nullptr)
        {
            return;
        }

        NavMeshAgentComponent* agent = new NavMeshAgentComponent();
        om->AllocateAndAssignInstanceID(agent);
        go->addComponent(agent);
        agent->PostLoadResource(go);

        transform->SetPosition(agent_start);
        Check(agent->IsOnNavMesh(), "AT8", "an agent standing on the floor is on the navmesh");
        Check(!agent->HasPath(), "AT9", "and has no path before a destination is set");
        Check(Near(agent->GetRemainingDistance(), 0.f), "AT10", "so remainingDistance is 0");
        Check(agent->GetVelocity() == Vector3::ZERO, "AT11", "and the velocity is zero");

        // --- 绕过墙 --------------------------------------------------------
        Check(agent->SetDestination(agent_goal), "AT12", "SetDestination finds a path around the wall");
        Check(agent->HasPath(), "AT13", "and the path is live");
        Check(agent->GetPathStatus() == 0, "AT14", "and it is a complete path (PathComplete)");
        Check(agent->GetWaypointCount() >= 2, "AT15", "with more than one waypoint (it really bends)");
        Check(Near(agent->GetDestination(), agent_goal), "AT16", "and the destination is remembered");
        Check(agent->GetRemainingDistance() > 10.f, "AT17",
              "remainingDistance follows the detour, not the straight line (which is 10)");

        float max_y = -1000.f;
        float max_step = 0.f;
        bool entered_wall = false;
        float speed_when_close = -1.f;
        Vector3 previous = transform->GetPosition();
        int ticks = 0;
        while (agent->HasPath() && ticks < 3000)
        {
            agent->Tick(dt);
            ++ticks;
            const Vector3 p = transform->GetPosition();
            max_y = std::max(max_y, p.y);
            max_step = std::max(max_step, (p - previous).length());
            // 墙占 x ∈ [-0.5, 0.5]、y ∈ [-10, 0]。留 0.1 余量给离散化。
            if (std::fabs(p.x) < 0.6f && p.y < 0.f)
            {
                entered_wall = true;
            }
            if (speed_when_close < 0.f && agent->GetRemainingDistance() < 0.2f)
            {
                speed_when_close = agent->GetVelocity().length();
            }
            previous = p;
        }

        Check(ticks < 3000, "AT18", "the agent reaches the goal in bounded time");
        Check(!entered_wall, "AT19", "and never walks through the wall");
        Check(max_y > 0.4f, "AT20", "it detours through the gap at y > 0 (steering follows the waypoints)");
        Check(Near(transform->GetPosition(), agent_goal, 0.8f), "AT21", "and ends up at the destination");
        Check(max_step <= agent->GetSpeed() * dt + 1e-3f, "AT22", "no frame moves faster than speed * dt");
        Check(!agent->HasPath(), "AT23", "the path is cleared on arrival");
        Check(Near(agent->GetRemainingDistance(), 0.f), "AT24", "remainingDistance is 0 on arrival");
        Check(agent->GetVelocity() == Vector3::ZERO, "AT25", "and it stopped");
        // 拐角必须真的走到：把中间航点的"算走过"半径放宽到 0.5 会让代理抄近路，
        // 在真实关卡里表现为贴着墙角斜插然后卡住。这条是唯一能抓到它的判据
        // （不穿墙、能到点那两条都抓不到 —— 切掉的那一小段本来就在空地上）。
        // --- 航点贴合度：不能抄近路切掉拐角 --------------------------------
        //
        // 用**低速 + 高加速**来量：正常速度 3.5 m/s 下转弯的横向偏移有 0.4 米（惯性，
        // 是物理上对的），那个量级会把"切拐角"完全淹没掉。把速度压到 1、加速拉到 20，
        // 惯性偏移降到 3cm 量级，剩下的偏差就只反映"算走过"的半径了。
        //
        // 半径放宽到 stoppingDistance（0.5）时，最近的拐角会被切掉约 0.2 米 —— 在
        // 真实关卡里就是贴着墙角斜插然后卡住。不穿墙 / 能到点那两条断言都抓不到它
        // （被切掉的那一小段本来就在空地上），只有这条抓得到。
        agent->Warp(agent_start);
        agent->SetSpeed(1.f);
        agent->SetAcceleration(20.f);
        Check(agent->SetDestination(agent_goal), "AT25a", "(a slow-speed pass is set up)");
        std::vector<Vector3> planned;
        for (int i = 0; i < agent->GetWaypointCount(); ++i)
        {
            planned.push_back(agent->GetWaypoint(i));
        }
        std::vector<float> closest_to_waypoint(planned.size(), 1.0e9f);
        int slow_ticks = 0;
        while (agent->HasPath() && slow_ticks < 4000)
        {
            agent->Tick(dt);
            ++slow_ticks;
            const Vector3 p = transform->GetPosition();
            // 终点不算：它是靠 stoppingDistance 判到达的，本来就差得到 0.5。
            for (std::size_t i = 0; i + 1 < planned.size(); ++i)
            {
                closest_to_waypoint[i] = std::min(closest_to_waypoint[i], (p - planned[i]).length());
            }
        }
        float worst_waypoint_miss = 0.f;
        for (std::size_t i = 0; i + 1 < planned.size(); ++i)
        {
            worst_waypoint_miss = std::max(worst_waypoint_miss, closest_to_waypoint[i]);
        }
        Check(slow_ticks < 4000, "AT25b", "and it still arrives");
        Check(worst_waypoint_miss < 0.12f, "AT25c",
              "the agent passes within 12cm of every intermediate waypoint (no corner cutting)");
        agent->SetSpeed(3.5f);
        agent->SetAcceleration(8.f);

        // --- 自动减速 ------------------------------------------------------
        //
        // 只断言"到点速度 < 最大速度"是不够的（离散化下几乎必然成立），所以同时
        // 卡住制动曲线 sqrt(2·a·d) —— 关掉 autoBraking 时速度会一直是 3.5，两条
        // 一起才抓得住"根本没减速"。
        Check(speed_when_close >= 0.f, "AT26", "(the agent was sampled close to the goal)");
        Check(speed_when_close < agent->GetSpeed() * 0.9f, "AT27",
              "autoBraking slows it down below max speed before arrival");
        Check(speed_when_close <= std::sqrt(2.f * agent->GetAcceleration() * 0.25f) + 1e-2f, "AT28",
              "and by the braking curve, not by an arbitrary factor");

        // --- 起步是加速，不是瞬移 ------------------------------------------
        transform->SetPosition(agent_start);
        agent->SetSpeed(3.5f);
        agent->SetAcceleration(8.f);
        Check(agent->SetDestination(Vector3(-2.f, -5.f, 0.f)), "AT29", "a short straight destination is accepted");
        const Vector3 before_first_tick = transform->GetPosition();
        agent->Tick(dt);
        const float first_step = (transform->GetPosition() - before_first_tick).length();
        Check(first_step > 0.f, "AT30", "the first frame does move");
        Check(first_step <= agent->GetAcceleration() * dt * dt + 1e-3f, "AT31",
              "but only by acceleration * dt^2 (it accelerates from rest, it does not teleport)");

        // --- Stop / Resume -------------------------------------------------
        transform->SetPosition(agent_start);
        agent->SetDestination(agent_goal);
        for (int i = 0; i < 30; ++i)
        {
            agent->Tick(dt);
        }
        const Vector3 moved_to = transform->GetPosition();
        Check((moved_to - agent_start).length() > 0.1f, "AT32", "the agent really moves before Stop");
        agent->Stop();
        Check(agent->IsStopped(), "AT33", "Stop sets isStopped");
        Check(agent->GetVelocity() == Vector3::ZERO, "AT34", "and zeroes the velocity at once");
        for (int i = 0; i < 60; ++i)
        {
            agent->Tick(dt);
        }
        Check(Near(transform->GetPosition(), moved_to, 1e-3f), "AT35", "and it does not drift while stopped");
        agent->Resume();
        Check(!agent->IsStopped(), "AT36", "Resume clears isStopped");
        int guard = 0;
        while (agent->HasPath() && guard < 3000)
        {
            agent->Tick(dt);
            ++guard;
        }
        Check(Near(transform->GetPosition(), agent_goal, 0.8f), "AT37",
              "and it finishes the same destination afterwards (the destination was kept)");

        // --- Warp / Move ---------------------------------------------------
        // 先建一条路再 Warp：直接断言"Warp 之后没有路"在"本来就没路"时是空转。
        agent->SetDestination(agent_start);
        Check(agent->HasPath(), "AT37a", "(a path is live before the Warp)");
        Check(agent->Warp(Vector3(0.f, 8.f, 0.f)), "AT38", "Warp onto the floor reports standable");
        Check(Near(transform->GetPosition(), Vector3(0.f, 8.f, 0.f)), "AT39", "and the transform moved");
        Check(!agent->HasPath(), "AT40", "and the old path is dropped");
        Check(!agent->Warp(Vector3(1000.f, 1000.f, 0.f)), "AT41", "Warp far off the mesh reports not standable");
        Check(!agent->IsOnNavMesh(), "AT42", "and IsOnNavMesh is false out there");

        agent->Warp(Vector3(0.f, 8.f, 0.f));
        const Vector3 before_move = transform->GetPosition();
        agent->Move(Vector3(1.f, 0.f, 0.f));
        Check(Near(transform->GetPosition(), before_move + Vector3(1.f, 0.f, 0.f)), "AT43",
              "Move applies a relative offset");
        Check(!agent->HasPath(), "AT44", "and creates no path (it is not a walk)");

        // --- updatePosition ------------------------------------------------
        agent->Warp(agent_start);
        agent->SetUpdatePosition(false);
        agent->SetDestination(agent_goal);
        for (int i = 0; i < 10; ++i)
        {
            agent->Tick(dt);
        }
        Check(Near(transform->GetPosition(), agent_start, 1e-4f), "AT45",
              "with updatePosition off the transform does not move");
        Check(agent->GetVelocity().squaredLength() > 0.f, "AT46",
              "but the velocity is still computed (that is what a controller consumes)");
        agent->SetUpdatePosition(true);

        // --- updateRotation / 朝向是本地 -Y --------------------------------
        agent->Warp(Vector3(-8.f, -5.f, 0.f));
        agent->SetUpdateRotation(false);
        agent->SetDestination(Vector3(-1.f, -5.f, 0.f));
        for (int i = 0; i < 5; ++i)
        {
            agent->Tick(dt);
        }
        const Quaternion rotation_before = transform->GetRotation();
        for (int i = 0; i < 20; ++i)
        {
            agent->Tick(dt);
        }
        // 比四元数本身没意义（q 与 -q 是同一个旋转），比它转出来的前向量才有。
        const Vector3 facing_before = rotation_before * Vector3::NEGATIVE_UNIT_Y;
        const Vector3 facing_after = transform->GetRotation() * Vector3::NEGATIVE_UNIT_Y;
        Check(Near(facing_after, facing_before, 1e-4f), "AT47",
              "with updateRotation off the rotation is untouched");
        agent->SetUpdateRotation(true);
        for (int i = 0; i < 90; ++i)
        {
            agent->Tick(dt);
        }
        // 朝 +X 走：本地 -Y 必须转到 +X。只验"转了"不够 —— 转反了也是"转了"。
        const Vector3 facing = transform->GetRotation() * Vector3::NEGATIVE_UNIT_Y;
        Check(facing.x > 0.9f, "AT48", "facing local -Y ends up along +X, the direction of travel");
        Check(!agent->GetVelocity().isZero(), "AT49", "(it was still moving when facing was sampled)");

        // --- 目标不可达 / 部分可达 ------------------------------------------
        agent->Warp(agent_start);
        agent->ResetPath();
        // 墙**里**的目标：会被吸附到墙边最近的可达格 —— Unity 的 PathPartial。
        Check(agent->SetDestination(Vector3(0.f, -5.f, 0.f)), "AT50",
              "a destination inside the wall still yields a path (to the nearest reachable cell)");
        Check(agent->GetPathStatus() == 1, "AT51", "and the status is PathPartial, not PathComplete");
        Check(agent->IsPathComplete() == false, "AT52", "so IsPathComplete is false");
        // 网格外的目标：一个可站立格都没有 => PathInvalid。
        Check(!agent->SetDestination(Vector3(500.f, 500.f, 0.f)), "AT53",
              "a destination far outside the grid finds no path");
        Check(agent->GetPathStatus() == 2, "AT54", "and the status is PathInvalid");
        Check(agent->HasDestination(), "AT55",
              "but the destination is still recorded (so autoRepath can retry later)");

        // --- Navigation 静态门面 -------------------------------------------
        NavMeshPath* path = Navigation::CalculatePath(agent_start, agent_goal);
        Check(path != nullptr, "AT56", "CalculatePath returns a path object");
        Check(path->GetStatus() == 0, "AT57", "and it is complete");
        Check(path->GetCornerCount() >= 2, "AT58", "with several corners (it bends around the wall)");
        Check(Near(path->GetCorner(0), agent_start, 1.5f), "AT59", "the first corner is at the source");
        Check(Near(path->GetCorner(path->GetCornerCount() - 1), agent_goal, 0.01f), "AT60",
              "and the last one is the exact target (agents arrive on the target, not a cell centre)");
        Check(path->GetCorner(-1) == Vector3::ZERO && path->GetCorner(999) == Vector3::ZERO, "AT61",
              "an out-of-range corner is the zero vector, not a crash");
        path->ClearCorners();
        Check(path->GetCornerCount() == 0 && path->GetStatus() == 2, "AT62",
              "ClearCorners empties it and invalidates the status");

        NavMeshPath* partial = Navigation::CalculatePath(agent_start, Vector3(0.f, -5.f, 0.f));
        Check(partial != nullptr && partial->GetStatus() == 1, "AT63",
              "the facade reports PathPartial for a target inside the wall");

        NavMeshHit* blocked = Navigation::Raycast(agent_start, agent_goal);
        Check(blocked != nullptr && blocked->IsHit(), "AT64",
              "Raycast across the wall reports blocked");
        Check(blocked != nullptr && blocked->GetDistance() > 3.f && blocked->GetDistance() < 6.f, "AT65",
              "and the blocking point is where the wall is (about 4 units along a 10-unit ray)");
        NavMeshHit* open = Navigation::Raycast(Vector3(-5.f, 8.f, 0.f), Vector3(5.f, 8.f, 0.f));
        Check(open != nullptr && !open->IsHit(), "AT66", "while a corridor clear of the wall is not blocked");

        NavMeshHit* sampled = Navigation::SamplePosition(Vector3(0.f, -5.f, 0.f), 5.f);
        Check(sampled != nullptr && sampled->IsHit(), "AT67", "SamplePosition inside a wall finds nearby floor");
        Check(sampled != nullptr && sampled->GetDistance() > 0.f && sampled->GetDistance() <= 5.f, "AT68",
              "and reports how far it had to move");
        NavMeshHit* too_far = Navigation::SamplePosition(Vector3(0.f, -5.f, 0.f), 0.5f);
        Check(too_far != nullptr && !too_far->IsHit(), "AT69",
              "but not when the nearest walkable point is outside the radius");

        // --- Resume 必须立刻重算 --------------------------------------------
        //
        // 停着的时候网格可能已经重建过，旧航点是按旧网格算的。只清 isStopped 而
        // 不重算的话，Resume 之后会继续沿一条可能已经失效的路走。
        agent->Warp(agent_start);
        agent->SetDestination(agent_goal);
        agent->ResetPath();  // 路没了，目标还在 —— 就是"网格重建过"那种情形
        agent->Stop();
        agent->Resume();
        agent->Tick(dt);
        Check(agent->HasPath(), "AT70",
              "Resume forces an immediate repath when the path was dropped while stopped");

        // --- 速度上限：把 speed 改小必须立刻生效 ----------------------------
        //
        // 不写这条的话 `if (velocity > speed) clamp` 是**死代码**（desired 本身已经
        // 不超过 speed），删掉它测试照样全绿 —— 于是没人知道它到底在不在。
        agent->Warp(agent_start);
        agent->SetDestination(Vector3(-2.f, -5.f, 0.f));
        // 45 帧 = 0.75s：还在匀速段（3 米的路约 1.23s 走完，过了 48 帧就开始制动了）。
        for (int i = 0; i < 45; ++i)
        {
            agent->Tick(dt);
        }
        Check(agent->GetVelocity().length() > 3.f, "AT71", "(the agent is running at full speed)");
        agent->SetSpeed(1.f);
        agent->Tick(dt);
        Check(agent->GetVelocity().length() <= 1.f + 1e-3f, "AT72",
              "lowering speed clamps the velocity at once (it does not coast down)");
        agent->SetSpeed(3.5f);

        // --- remainingDistance 要扣掉 stoppingDistance ----------------------
        //
        // 只在"到达之后 == 0"上验是不够的：到达时路径已经清空，
        // `ComputeRemainingDistance` 自己就返回 0，扣不扣都一样。必须在**走的过程
        // 中**量一次。
        agent->Warp(agent_start);
        agent->SetDestination(Vector3(-2.f, -5.f, 0.f));
        Check(agent->GetRemainingDistance() > 2.f && agent->GetRemainingDistance() < 3.f, "AT73",
              "mid-walk remainingDistance is the path length minus the stopping distance "
              "(a 3m path reads under 3m)");
    }

    // --------------------------------------------------------------- AU ----
    void TestNavMeshAgentWiring()
    {
        const std::string header = ReadFile("Source/Runtime/Function/AI/Navigation/NavMeshAgentComponent.h");
        Check(!header.empty(), "AU0", "NavMeshAgentComponent.h is readable");
        // 接线门：Tick 必须是**重写**。忘了 override 的话编译能过、运行没人调，
        // 代理一动不动而所有寻路断言依然全绿 —— 这是本切片最容易犯的错。
        Check(CountOccurrences(header, "void Tick(float delta_time) override;") >= 1, "AU1",
              "the agent overrides Component::Tick (otherwise it never runs)");
        Check(CountOccurrences(header, "ZFUNCTION()") >= 20, "AU2",
              "the agent publishes a real script surface");
        Check(CountOccurrences(header, "ZPROPERTY()") >= 10, "AU3", "and every authored parameter is serializable");
        Check(CountOccurrences(header, "ZSCRIPTPROPERTY(Name=\"destination\"") >= 1, "AU4",
              "destination is a script property");
        Check(CountOccurrences(header, "ZSCRIPTPROPERTY(Name=\"remainingDistance\"") >= 1, "AU5",
              "and so is remainingDistance");

        const std::string source = ReadFile("Source/Runtime/Function/AI/Navigation/NavMeshAgentComponent.cpp");
        Check(!source.empty(), "AU6", "NavMeshAgentComponent.cpp is readable");
        // 必须真的去问 NavigationSystem 要路，而不是自己沿直线走。
        Check(CountOccurrences(source, "navigation_system->FindPath(") >= 1, "AU7",
              "the agent asks NavigationSystem for a path");
        // 转向必须走引擎自己的前向量约定（-Y），不是重新定义一个 yaw。
        Check(CountOccurrences(source, "Vector3::NEGATIVE_UNIT_Y") >= 1, "AU8",
              "facing uses the engine's -Y forward convention");
        // 停止时不能一帧归零速度（会表现为撞墙 + 速度曲线断裂）。
        Check(CountOccurrences(source, "m_Acceleration * delta_time") >= 2, "AU9",
              "both accelerating and braking are rate-limited by acceleration");

        const std::string facade = ReadFile("Source/Runtime/Scripting/ScriptNavigation/ScriptNavMesh.h");
        Check(!facade.empty(), "AU10", "ScriptNavMesh.h is readable");
        Check(CountOccurrences(facade, "ZENUM()") >= 1, "AU11", "the path status enum is published");
        Check(CountOccurrences(facade, "class NavMeshPath :") >= 1, "AU12", "NavMeshPath exists");
        Check(CountOccurrences(facade, "class NavMeshHit :") >= 1, "AU13", "and NavMeshHit");
        Check(CountOccurrences(facade, "static NavMeshPath* CalculatePath(") >= 1, "AU14",
              "Navigation.CalculatePath is declared");
        Check(CountOccurrences(facade, "static NavMeshHit* SamplePosition(") >= 1, "AU15",
              "and SamplePosition");
        Check(CountOccurrences(facade, "static NavMeshHit* Raycast(") >= 1, "AU16", "and Raycast");

        // `BuildWithSampler` 是"无头环境能建网格"的口子；没有它测试只能去伪造
        // WorldManager。同时它也必须真的落到同一个 m_NavMesh 上。
        const std::string nav_header = ReadFile("Source/Runtime/Function/AI/Navigation/NavigationSystem.h");
        Check(CountOccurrences(nav_header, "BuildWithSampler") >= 1, "AU17",
              "NavigationSystem exposes a sampler-based build");
        const std::string nav_source = ReadFile("Source/Runtime/Function/AI/Navigation/NavigationSystem.cpp");
        Check(CountOccurrences(nav_source, "m_NavMesh.Build(settings, raycast)") >= 1, "AU18",
              "and it builds the system's own mesh (so FindPath sees it immediately)");
    }

    // =====================================================================
    // AW / AV -- AudioSource 对齐 Unity（`time` / `timeSamples` / `PlayDelayed`
    // / `Pause` / `UnPause` / `mute` / `volume` / `PlayOneShot`）。
    //
    // 无头下没有声卡，`AudioMixer::Render()` 没人调，于是播放进度恒为 0 —— 直接
    // 断言进度只会测到"自己的假设"。所以这里**手动 Render**：渲染掉多少秒，就
    // 等价于声卡把这段时间播过去了。顺带"幅度"也能量了：mute / volume 到底有没
    // 有真的落到 DSP 上，看输出峰值就知道，不用去猜参数推没推过去。
    // =====================================================================

    constexpr uint32_t kAudioTestRate = 48000u;
    constexpr uint32_t kAudioTestChannels = 2u;

    /// 渲染掉 in_seconds 秒，返回输出峰值（0 = 全程静音）。
    float RenderAudio(AudioSystem& in_system, float in_seconds)
    {
        std::vector<float> buffer(1024u * kAudioTestChannels);
        float peak = 0.0f;
        uint32_t remaining = static_cast<uint32_t>(in_seconds * static_cast<float>(kAudioTestRate) + 0.5f);
        while (remaining > 0u)
        {
            const uint32_t chunk = std::min(remaining, 1024u);
            std::fill(buffer.begin(), buffer.end(), 0.0f);
            in_system.GetMixer().Render(buffer.data(), chunk, kAudioTestChannels, kAudioTestRate);
            for (uint32_t index = 0u; index < chunk * kAudioTestChannels; ++index)
            {
                peak = std::max(peak, std::fabs(buffer[index]));
            }
            remaining -= chunk;
        }
        return peak;
    }

    /// 在临时目录烘一个单声道正弦当 clip。
    ///
    /// 仓库里没有现成音频素材，而 `AudioSystem::GetClip()` 只认能解码的文件 ——
    /// 拿一个不存在的路径去测，只会走到"加载失败"那条路，下面所有播放断言都是
    /// 空转（而且全是绿的全空转）。
    std::string BakeTestClip(float in_seconds)
    {
        AudioClipData clip;
        clip.m_SampleRate = kAudioTestRate;
        clip.m_Channels = 1u;
        clip.m_Name = "parity_tone";
        const size_t frames = static_cast<size_t>(in_seconds * static_cast<float>(kAudioTestRate));
        clip.m_Samples.resize(frames);
        for (size_t index = 0u; index < frames; ++index)
        {
            clip.m_Samples[index] = std::sin(2.0f * 3.14159265f * 440.0f * static_cast<float>(index) /
                                             static_cast<float>(kAudioTestRate));
        }

        std::vector<uint8_t> encoded;
        std::string error;
        if (!EncodeWavS16(clip, encoded, &error))
        {
            std::printf("      (cannot encode the synthetic clip: %s)\n", error.c_str());
            return std::string();
        }

        const std::string path = (std::filesystem::temp_directory_path() / "zengine_audio_parity_tone.wav").string();
        std::ofstream out(path, std::ios::binary);
        if (!out)
        {
            return std::string();
        }
        out.write(reinterpret_cast<const char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
        out.close();
        return path;
    }

    void TestAudioSource()
    {
        const std::string clip_path = BakeTestClip(0.5f);
        Check(!clip_path.empty(), "AW1", "a synthetic wav clip exists on disk");
        if (clip_path.empty())
        {
            return;
        }

        // 不调 Initialize()：那会真的去开 WASAPI。这里要的只是 mixer，而 mixer
        // 不需要设备 —— 进度由测试自己 Render 推进，比让声卡线程来得更可控。
        if (GET_SYSTEM(AudioSystem) == nullptr)
        {
            SystemRegistry::GetInstance().RegisterSystem(new AudioSystem());
        }
        AudioSystem* audio = GET_SYSTEM(AudioSystem);
        Check(audio != nullptr, "AW2", "AudioSystem is reachable");
        if (audio == nullptr)
        {
            return;
        }

        ObjectManager* om = GET_SYSTEM(ObjectManager);
        Check(om != nullptr, "AW3", "ObjectManager is reachable");
        if (om == nullptr)
        {
            return;
        }

        Level level;
        level.setLevelResUrl("Assets/Scenes/Audio.level");
        level.LoadObjectGraph({}, false);
        Transform* transform = nullptr;
        GameObject* go = MakeRuntimeObject(*om, level, &transform);
        Check(go != nullptr, "AW4", "a runtime object with a Transform exists");
        if (go == nullptr)
        {
            return;
        }

        Runtime::AudioSourceComponent* source = new Runtime::AudioSourceComponent();
        om->AllocateAndAssignInstanceID(source);
        go->addComponent(source);
        source->PostLoadResource(go);

        source->SetClipPath(clip_path.c_str());
        Check(source->GetClipPath() == clip_path.c_str(), "AW5", "the clip path round-trips");

        // volume / pitch / mute 都是 ZPROPERTY（私有字段 + ZHeaderTool 生成的
        // 访问器），脚本就是走这对访问器写的 —— 测试也这么写，才是同一条路径。
        // 2D：下面要量"峰值 == volume"，不能掺距离衰减。
        source->ZhtScriptSet_m_SpatialBlend(0.f);
        source->ZhtScriptSet_m_Volume(1.f);
        source->ZhtScriptSet_m_Pitch(1.f);
        source->ZhtScriptSet_m_Mute(false);
        source->ZhtScriptSet_m_Loop(false);

        Check(Near(source->GetClipLength(), 0.5f, 0.01f), "AW6", "clip length matches the file");
        source->ZhtScriptSet_m_Pitch(2.f);
        Check(Near(source->GetClipLength(), 0.25f, 0.01f), "AW7", "pitch 2 halves the audible length");
        source->ZhtScriptSet_m_Pitch(1.f);

        // --- Play ---------------------------------------------------------
        const AudioVoiceId voice = source->Play();
        Check(voice != kInvalidAudioVoiceId, "AW8", "Play() starts a voice");
        Check(source->GetVoiceId() == voice, "AW9", "and the component remembers it");
        Check(source->IsPlaying(), "AW10", "IsPlaying() is true right after Play()");
        Check(!source->IsPaused(), "AW11", "and it is not paused");
        Check(Near(source->GetTime(), 0.f), "AW12", "time is 0 before anything has rendered");
        Check(source->GetTimeSamples() == 0, "AW13", "and so is timeSamples");

        // --- 进度：只有 Render 会推进它 ------------------------------------
        RenderAudio(*audio, 0.1f);
        Check(Near(source->GetTime(), 0.1f, 0.01f), "AW14", "rendering 0.1 s advances time by 0.1 s");
        Check(source->GetTimeSamples() > 4000 && source->GetTimeSamples() < 5600, "AW15",
              "timeSamples tracks it in source frames (4800 at 48 kHz)");
        Check(Near(static_cast<float>(audio->GetVoiceDuration(voice)), 0.5f, 0.01f), "AW16",
              "the mixer reports the clip duration for that voice");
        // 倍速也会缩短"听感时长"，mixer 侧同样要折算（组件的 GetClipLength 是另一
        // 条路，别只测了一条）。
        source->ZhtScriptSet_m_Pitch(2.f);
        source->Play();
        Check(Near(static_cast<float>(audio->GetVoiceDuration(source->GetVoiceId())), 0.25f, 0.01f), "AW16a",
              "and so does the mixer's own duration (pitch 2 = half as long)");
        source->ZhtScriptSet_m_Pitch(1.f);
        source->Play();

        // --- 跳转 ----------------------------------------------------------
        source->SetTime(0.25f);
        Check(Near(source->GetTime(), 0.25f, 0.01f), "AW17", "SetTime seeks");
        // 负数也要夹：负播放头会让 `static_cast<uint64_t>(playhead)` 变成一个巨大
        // 的下标，同样是越界读。
        source->SetTime(-5.f);
        Check(source->GetTimeSamples() == 0, "AW17a", "a negative seek clamps to 0");
        source->SetTime(100.f);
        Check(Near(source->GetTime(), 0.5f, 0.01f), "AW18", "and seeking past the end clamps to the end");
        // 严格小于帧数，不只是"约等于时长"：播放头 == 帧数 时 Render() 里
        // `m_Samples[frame_index * channels]` 是一次越界读。
        Check(source->GetTimeSamples() == 23999, "AW18a",
              "it clamps to the last playable frame, not to the frame count (that would read past the end)");
        RenderAudio(*audio, 0.05f);
        Check(!source->IsPlaying(), "AW19", "a non-looping clip ends once it reaches the end");
        // 组件还没 Tick，所以句柄还挂着 —— 而 mixer 已经不认识这条 voice 了。
        // `GetTime()` 必须把 -1（"没了"）翻译成 0，不能原样给脚本。
        Check(Near(source->GetTime(), 0.f), "AW19a",
              "a voice that ended on its own reports time 0, not the -1 sentinel");

        // --- 暂停 / 继续 ----------------------------------------------------
        source->Play();
        RenderAudio(*audio, 0.1f);
        const float before_pause = source->GetTime();
        source->Pause();
        Check(source->IsPaused(), "AW20", "Pause() reports paused");
        // Unity 的语义：`AudioSource.isPlaying` 在 Pause() 之后是 **false**（文档
        // 明说）。所以"暂停中"不等于"没了"，两件事必须分开问。
        Check(!source->IsPlaying(), "AW21", "and isPlaying is false while paused (Unity semantics)");
        // 而 Tick 必须认得暂停中的 voice：原来它只问 IsPlaying，于是暂停一帧之后
        // 句柄就被当成"播完了"丢掉，`UnPause()` 从此没有对象可以恢复 —— 静默失效。
        for (int frame = 0; frame < 10; ++frame)
        {
            source->Tick(1.f / 60.f);
        }
        Check(source->IsPaused(), "AW22", "ticking a paused source keeps the voice (pause is not stop)");
        Check(RenderAudio(*audio, 0.2f) < 1.0e-6f, "AW23", "a paused voice renders silence");
        Check(Near(source->GetTime(), before_pause, 0.005f), "AW24", "and does not advance");
        source->UnPause();
        Check(!source->IsPaused(), "AW25", "UnPause() resumes");
        RenderAudio(*audio, 0.1f);
        Check(source->GetTime() > before_pause + 0.05f && source->GetTime() < before_pause + 0.2f, "AW26",
              "and it continues from where it stopped, not from 0");

        // --- Stop ----------------------------------------------------------
        source->Stop();
        Check(!source->IsPlaying(), "AW27", "Stop() ends the voice");
        Check(Near(source->GetTime(), 0.f), "AW28", "and time reads 0 for a source that is not playing");
        // -1 而不是 0：**0 是合法的播放位置**，拿它当"没这条 voice"会让"还没开始"
        // 和"已经在播"分不开。
        Check(audio->GetVoicePlaybackTime(voice) == AudioMixer::kUnknownPlaybackTime, "AW29",
              "a dead voice reports -1, not 0 (0 is a real position)");

        // --- 一条音源只占一条 voice -----------------------------------------
        const AudioVoiceId first = source->Play();
        const AudioVoiceId second = source->Play();
        Check(first != second && second != kInvalidAudioVoiceId, "AW30", "a second Play() starts a new voice");
        Check(audio->GetMixer().GetActiveVoiceCount() == 1, "AW31",
              "and the first one was released (one voice per source, not a stack)");

        // --- PlayOneShot 不抢本音源的 voice ---------------------------------
        const AudioVoiceId owner = source->GetVoiceId();
        const AudioVoiceId one_shot = source->PlayOneShot(clip_path.c_str(), 1.f);
        Check(one_shot != kInvalidAudioVoiceId, "AW32", "PlayOneShot starts a voice");
        Check(one_shot != owner, "AW33", "a different one");
        Check(source->GetVoiceId() == owner, "AW34", "and the source still owns its own voice");
        Check(audio->GetMixer().GetActiveVoiceCount() == 2, "AW35", "both are alive at once (that is the point)");

        // --- 一次性音效不该继承 loop ------------------------------------------
        //
        // `PlayOneShot` 用的是本组件的 params，但 loop 必须强行关掉：叠一个循环
        // 体上去就再也停不下来了，而且它是"一次性"的，没人会去 Stop 它。
        source->ZhtScriptSet_m_Loop(true);
        audio->StopAll();
        source->Play();
        const AudioVoiceId looping = source->GetVoiceId();
        const AudioVoiceId shot = source->PlayOneShot(clip_path.c_str(), 1.f);
        Check(shot != looping && shot != kInvalidAudioVoiceId, "AW35a", "a looping source can still fire a one-shot");
        RenderAudio(*audio, 0.6f);
        Check(audio->IsPlaying(looping), "AW35b", "the source's own looping voice survives past one clip length");
        Check(!audio->IsPlaying(shot), "AW35c", "and the one-shot does not inherit loop (it would never stop)");

        // --- 幅度：mute / volume 必须真的落到 DSP 上 --------------------------
        //
        // 这是本切片那个真 bug 的判据：Tick 里原来有一条 `m_SpatialBlend <= 0`
        // 的提前返回，于是 **2D 音源改 volume / pitch / mute 完全不生效** ——
        // 字段是脚本直接写的，绕过了任何 setter，唯一的生效路径就是 Tick 推参数。
        // 3D 音源因为要更新位置顺带生效了，所以这个 bug 一直没人发现。
        audio->StopAll();
        source->ZhtScriptSet_m_Loop(true);  // 量幅度时不能让它播完
        source->Play();
        source->Tick(1.f / 60.f);
        const float full_peak = RenderAudio(*audio, 0.05f);
        // 不是 1.0：2D 声源在正中，走等功率声像（左右各 0.707），这是对的 ——
        // 阈值按 0.6 取，判的是"有没有声音"而不是"是不是 1.0"。
        Check(full_peak > 0.6f, "AW36", "an unmuted 2D source at volume 1 renders at equal-power pan level");

        source->ZhtScriptSet_m_Volume(0.25f);
        source->Tick(1.f / 60.f);
        const float quarter_peak = RenderAudio(*audio, 0.05f);
        Check(Near(quarter_peak / full_peak, 0.25f, 0.02f), "AW37",
              "dropping volume to 0.25 really scales the output (2D sources are pushed too)");

        source->ZhtScriptSet_m_Volume(1.f);
        source->ZhtScriptSet_m_Mute(true);
        source->Tick(1.f / 60.f);
        Check(RenderAudio(*audio, 0.05f) < 1.0e-6f, "AW38", "mute silences the source");
        source->ZhtScriptSet_m_Mute(false);
        source->Tick(1.f / 60.f);
        Check(RenderAudio(*audio, 0.05f) > 0.6f, "AW39",
              "and unmuting restores it (mute never destroyed the volume)");

        // --- PlayDelayed -----------------------------------------------------
        audio->StopAll();
        source->PlayDelayed(0.5f);
        Check(source->IsPlaying(), "AW40", "PlayDelayed() registers the voice immediately");
        Check(Near(source->GetTime(), 0.f), "AW41", "and time is 0 (it has not started yet)");
        Check(RenderAudio(*audio, 0.25f) < 1.0e-6f, "AW42", "the wait is silent");
        Check(Near(source->GetTime(), 0.f), "AW43", "and still 0 half the delay later");
        // 再渲染 0.4 s：延迟（剩 0.25）在第 12 个 1024 帧块上跑完，剩下的 0.144 s
        // 才是真正播出去的。容差留到 0.3，专门用来区分"从 0 开始"和"把等待的
        // 0.65 s 全算进播放头"（后者会得到 0.65）。
        const float after_delay_peak = RenderAudio(*audio, 0.4f);
        Check(after_delay_peak > 0.6f, "AW44", "once the delay is over it is audible");
        Check(source->GetTime() > 0.05f && source->GetTime() < 0.3f, "AW45",
              "and it starts counting from 0, not from the 0.65 s already elapsed");

        audio->StopAll();
    }

    void TestAudioSourceWiring()
    {
        const std::string header = ReadFile("Source/Runtime/Function/Framework/Component/Audio/AudioSourceComponent.h");
        Check(!header.empty(), "AV0", "AudioSourceComponent.h is readable");
        Check(CountOccurrences(header, "ZFUNCTION()") >= 15, "AV1", "the source publishes a real script surface");
        Check(CountOccurrences(header, "void PlayDelayed(") >= 1, "AV2", "PlayDelayed is declared");
        Check(CountOccurrences(header, "AudioVoiceId PlayOneShot(") >= 1, "AV3", "PlayOneShot is declared");
        Check(CountOccurrences(header, "bool IsPaused() const") >= 1, "AV4", "IsPaused is declared");
        Check(CountOccurrences(header, "bool m_Mute") >= 1, "AV5", "mute is a serialized field");

        const std::string source = ReadFile("Source/Runtime/Function/Framework/Component/Audio/AudioSourceComponent.cpp");
        Check(!source.empty(), "AV6", "AudioSourceComponent.cpp is readable");
        // 静音必须折进 params，而不是让脚本去改 volume（改了原值就丢了，取消静音
        // 恢复不回去）。
        Check(CountOccurrences(source, "params.m_Volume = m_Mute ? 0.0f : m_Volume;") >= 1, "AV7",
              "mute is folded into the play params");
        // 2D 音源也必须每帧推参数。`m_SpatialBlend <= 0.0f` 这个 token 特意带
        // `.0f`：注释里提到的是不带后缀的写法，用带后缀的才不会命中注释。
        Check(CountOccurrences(source, "m_SpatialBlend <= 0.0f") == 0, "AV8",
              "there is no 2D early return in Tick (it would mute every parameter change)");
        Check(CountOccurrences(source, "audio_system->UpdateVoice(m_VoiceId, BuildPlayParams());") >= 1, "AV9",
              "Tick pushes the params every frame");
        // Play() 必须是"延迟 0"，而不是自己再抄一遍播放逻辑（两份逻辑必然漂移）。
        Check(CountOccurrences(source, "return PlayWithDelay(0.0f);") >= 1, "AV10",
              "Play() is the zero-delay case of PlayWithDelay");
        // Wwise 的 event 没有"延迟 post"这回事，只能组件自己数完再发。
        Check(CountOccurrences(source, "m_HasWwisePendingDelay") >= 4, "AV11",
              "the Wwise route counts its own delay down");
        // `IsPlaying()` 在暂停时是 false，所以 Tick 不能只问它 —— 否则暂停一帧就
        // 把句柄当"播完了"丢掉，UnPause() 从此没有对象可恢复。
        Check(CountOccurrences(source, "audio_system->IsVoicePaused(m_VoiceId)") >= 1, "AV22",
              "Tick treats a paused voice as alive");

        const std::string mixer_header = ReadFile("Source/Runtime/Function/Audio/AudioMixer.h");
        Check(CountOccurrences(mixer_header, "m_DelayRemaining") >= 1, "AV12", "the mixer holds the pending delay");
        Check(CountOccurrences(mixer_header, "kUnknownPlaybackTime") >= 2, "AV13",
              "and reports a missing voice as -1, not 0");
        const std::string mixer_source = ReadFile("Source/Runtime/Function/Audio/AudioMixer.cpp");
        Check(CountOccurrences(mixer_source, "voice.m_DelayRemaining = (in_params.m_DelaySeconds") >= 1, "AV14",
              "Play() seeds it from the requested delay once");
        Check(CountOccurrences(mixer_source, "voice.m_DelayRemaining -= ") >= 1, "AV15", "and Render counts it down");
        Check(CountOccurrences(mixer_source, "voice->m_Clip->GetDuration() / pitch") >= 1, "AV16",
              "the voice duration accounts for pitch");
        Check(CountOccurrences(mixer_source, "last_playable_frame") >= 2, "AV17",
              "and seeking clamps to the last playable frame (frame_count would read out of bounds)");

        const std::string common = ReadFile("Source/Runtime/Function/Audio/AudioCommon.h");
        Check(CountOccurrences(common, "m_DelaySeconds") >= 1, "AV18", "the delay lives in the play params");

        const std::string system_header = ReadFile("Source/Runtime/Function/Audio/AudioSystem.h");
        Check(CountOccurrences(system_header, "GetVoicePlaybackTime") >= 1, "AV19",
              "AudioSystem exposes the playhead");
        Check(CountOccurrences(system_header, "SetVoicePlaybackTime") >= 1, "AV20", "and seeking");
        Check(CountOccurrences(system_header, "IsVoicePaused") >= 1, "AV21", "and the paused state");
    }

    // =====================================================================
    // AX / AY -- Resources（Unity 的 `Resources.Load`）。
    //
    // 引擎有完整的素材管线，但那全是 C++ 侧的：组件在 PostLoadResource 里按路径
    // 取素材，脚本什么都拿不到。于是"运行时动态取一个 prefab / 配置"这件事在
    // 脚本世界里根本做不到。
    //
    // 这一组用**绝对路径**驱动解析（相对路径要 ConfigManager / ProjectInfo 提供
    // 搜索根，那两个的初始化依赖 CommandSystem 与磁盘上的 ini，无头下不可控），
    // 解析规则本身因此完全确定。
    // =====================================================================

    void TestResources()
    {
        // 类型表还没建：TestScenes()（它调 RegisterRuntimeTypesForTest）排在
        // TestApplicationFacade 之后，而本组必须排在它之前 —— 见 main() 里的顺序
        // 注释。没有类型表 `ObjectManager::Produce(TypeOf<GameObject>())` 拿不到
        // 工厂函数。
        RegisterRuntimeTypesForTest();

        if (GET_SYSTEM(FileSystem) == nullptr)
        {
            SystemRegistry::GetInstance().RegisterSystem(new FileSystem());
        }
        // AssetManager 是抽象类（InsertPathNameInternal 是纯虚），运行时侧的实现
        // 是 RuntimeAssetManager。
        if (GET_SYSTEM(AssetManager) == nullptr)
        {
            SystemRegistry::GetInstance().RegisterSystem(new RuntimeAssetManager());
        }
        Check(GET_SYSTEM(AssetManager) != nullptr, "AX1", "an AssetManager is reachable");

        // --- 解析 ----------------------------------------------------------
        std::error_code ec;
        const std::filesystem::path probe_dir = std::filesystem::temp_directory_path() / "zengine_resource_probe";
        std::filesystem::create_directories(probe_dir, ec);
        const std::filesystem::path probe = probe_dir / "probe.zasset";
        {
            std::ofstream out(probe, std::ios::binary);
            out << "not a real asset";
        }
        Check(std::filesystem::exists(probe), "AX2", "a probe file exists");

        const std::string probe_str = probe.generic_string();
        Check(Resources::Exists(probe_str.c_str()), "AX3", "an absolute path resolves as-is");
        Check(!Resources::GetResolvedPath(probe_str.c_str()).empty(), "AX4", "and reports where it went");

        const std::filesystem::path no_extension = probe_dir / "probe";
        Check(Resources::Exists(no_extension.generic_string().c_str()), "AX5",
              "an extension-less path is probed (Unity writes them without one)");
        Check(Resources::GetResolvedPath(no_extension.generic_string().c_str()) ==
                  Resources::GetResolvedPath(probe_str.c_str()),
              "AX6", "both spellings resolve to the same file (the key is the resolved path)");

        Check(!Resources::Exists(""), "AX7", "an empty path resolves to nothing");
        Check(!Resources::Exists((probe_dir / "nope").generic_string().c_str()), "AX8",
              "a missing file does not resolve");
        Check(Resources::GetResolvedPath((probe_dir / "nope").generic_string().c_str()).empty(), "AX9",
              "and reports nothing");
        Check(Resources::GetTypeName(nullptr).empty(), "AX10", "GetTypeName(null) is empty");

        // 找不到必须是 nullptr 而不是"看起来成功" —— 见 FileSystem::loadAssetJson
        // 那个 `return true;`：它在一个返回 T* 的函数里，等于 (T*)1。
        Check(Resources::Load((probe_dir / "nope").generic_string().c_str()) == nullptr, "AX11",
              "loading a missing asset returns null (not a bogus non-null pointer)");

        // --- 一张真写的 YAML 资产 -------------------------------------------
        //
        // 仓库里那些 .zasset 是 cooked 产物，读它要 SerializedFile + FileCacher
        // + MemoryManager 全套初始化，无头下拿不到（第一次试就是直接段错误）。
        // 所以改用引擎自己的写出器**现写**一张 .scene：写出和读回走的是同一套
        // YAML 图模型，`Resources.Load` 走的加载路径是真的（类型名从文件里读、
        // ObjectManager 按类型 Produce、按图反序列化），只是素材来源换成了这一张。
        if (GET_SYSTEM(ObjectManager) == nullptr)
        {
            SystemRegistry::GetInstance().RegisterSystem(new ObjectManager());
        }
        ObjectManager* om = GET_SYSTEM(ObjectManager);
        Check(om != nullptr, "AX12", "ObjectManager is reachable");
        if (om == nullptr)
        {
            return;
        }

        GameObject* const authored = static_cast<GameObject*>(om->Produce(TypeOf<GameObject>(), 0));
        Check(authored != nullptr, "AX13", "a GameObject can be produced to author an asset");
        if (authored == nullptr)
        {
            return;
        }
        om->AllocateAndAssignInstanceID(authored);
        authored->SetName("ResourcesProbe");

        const std::filesystem::path scene_path = std::filesystem::temp_directory_path() / "zengine_resources_probe.scene";
        std::filesystem::remove(scene_path, ec);
        std::vector<Object*> graph_objects {authored};
        std::vector<int64_t> graph_ids {1};
        Check(GET_SYSTEM(AssetManager)->WriteObjectsToYaml(scene_path, graph_objects.data(), graph_ids.data(), 1),
              "AX14", "the engine authors the probe asset");
        Check(std::filesystem::exists(scene_path, ec), "AX15", "and it is on disk");

        const std::string scene_str = scene_path.generic_string();
        Object* const first = Resources::Load(scene_str.c_str());
        Check(first != nullptr, "AX16", "Resources.Load reads the asset back");
        if (first == nullptr)
        {
            return;
        }
        Check(Resources::GetTypeName(first) == "GameObject", "AX17",
              "and it decodes as the type the file declares");
        Check(Resources::Load(scene_str.c_str()) == first, "AX18",
              "a second Load returns the cached instance (Unity caches)");
        Check(Resources::GetLoadedCount() >= 1, "AX19", "the cache holds it");

        Check(Resources::LoadTyped(scene_str.c_str(), "NoSuchAssetType") == nullptr, "AX20",
              "an unknown type name is rejected");
        Check(Resources::LoadTyped(scene_str.c_str(), "GameObject") != nullptr, "AX21",
              "and the declared type name loads it");

        const int before_unload = Resources::GetLoadedCount();
        Check(Resources::UnloadAsset(first), "AX22", "UnloadAsset drops the cached entry");
        Check(Resources::GetLoadedCount() == before_unload - 1, "AX23", "exactly one entry goes away");
        Check(Resources::Load(scene_str.c_str()) != nullptr, "AX24", "so the next Load reads it again");
        Check(Resources::GetLoadedCount() == before_unload, "AX25", "and the cache is refilled");

        Check(Resources::UnloadUnusedAssets() >= 1, "AX26", "UnloadUnusedAssets reports what it dropped");
        Check(Resources::GetLoadedCount() == 0, "AX27", "and the cache is empty afterwards");
    }

    void TestResourcesWiring()
    {
        const std::string header = ReadFile("Source/Runtime/Scripting/ScriptResources/ScriptResources.h");
        Check(!header.empty(), "AY0", "ScriptResources.h is readable");
        Check(CountOccurrences(header, "class Resources :") >= 1, "AY1", "the Resources facade exists");
        Check(CountOccurrences(header, "ZFUNCTION()") >= 8, "AY2", "and publishes a real script surface");

        const std::string source = ReadFile("Source/Runtime/Scripting/ScriptResources/ScriptResources.cpp");
        Check(!source.empty(), "AY3", "ScriptResources.cpp is readable");
        // Unity 的约定：素材放在 Resources/ 子目录里，且调用时不带扩展名。
        Check(CountOccurrences(source, "\"Resources\"") >= 1, "AY4",
              "the Unity Resources/ folder convention is honoured");
        // `.zasset` 里刻着真实类型名；只按扩展名分派会解出一个裸 Object。
        Check(CountOccurrences(source, "GetAssetTypeName(resolved)") >= 1, "AY5",
              "the type stamped in the file drives the decode");
        Check(CountOccurrences(source, "CacheKey(resolved)") >= 2, "AY6", "loading goes through the path cache");
        // 缓存键必须是解析后的绝对路径：按调用方传进来的拼写缓存，`a/b` 与
        // `A/b` 在 Windows 上是同一个文件却会读出两份对象。
        Check(CountOccurrences(source, "std::string key = resolved.generic_string();") >= 1, "AY7",
              "the cache key is the resolved absolute path, not the caller's spelling");

        // `FileSystem::loadAssetJson` 原来在一个返回 `AssetType*` 的函数里
        // `return true;` —— 那就是 `(AssetType*)1`：非空、完全无效。
        const std::string fs = ReadFile("Source/Runtime/File/FileSystem.h");
        Check(!fs.empty(), "AY8", "FileSystem.h is readable");
        Check(CountOccurrences(fs, "return obj;") >= 2, "AY9",
              "loadAssetJson returns the object, not `true` (which decayed to (T*)1)");
    }

    // ------------------------------------------------------------- AZ ------
    // Color：Unity 的 `Color` 静态面 + 值类型跨边界。
    //
    // 这里测的是 C++ 实现（插值是否夹、HSV 往返是否闭合）；"静态属性能不能在
    // JS 里真的取到"（pesapi Variables -> 构造器）由 ScriptVMSmokeTest 的 I 组
    // 端到端验。两头都要：只验 C++ 会漏掉属性根本没装到对象上，只验 JS 会漏掉
    // 数值本身算错。
    namespace
    {
        bool SameColor(const ::Color& a, const ::Color& b, float eps = 0.0005f)
        {
            return std::fabs(a.r - b.r) <= eps && std::fabs(a.g - b.g) <= eps && std::fabs(a.b - b.b) <= eps &&
                   std::fabs(a.a - b.a) <= eps;
        }

        ::Color MakeColor(float r, float g, float b, float a)
        {
            ::Color c;
            c.r = r;
            c.g = g;
            c.b = b;
            c.a = a;
            return c;
        }
    }  // namespace

    void TestColor()
    {
        // ---- 常量 ----
        Check(SameColor(Runtime::Color::GetClear(), MakeColor(0, 0, 0, 0)), "AZ1",
              "Color.clear is (0,0,0,0) -- fully transparent, not black");
        Check(SameColor(Runtime::Color::GetWhite(), MakeColor(1, 1, 1, 1)), "AZ2", "Color.white is (1,1,1,1)");
        // 黑是 (0,0,0,1)：alpha 为 0 的话"黑色"会画不出来 —— 这是最容易写错的一个。
        Check(SameColor(Runtime::Color::GetBlack(), MakeColor(0, 0, 0, 1)), "AZ3",
              "Color.black is opaque (alpha 1), not transparent");
        Check(SameColor(Runtime::Color::GetRed(), MakeColor(1, 0, 0, 1)), "AZ4", "Color.red");
        Check(SameColor(Runtime::Color::GetGreen(), MakeColor(0, 1, 0, 1)), "AZ5", "Color.green");
        Check(SameColor(Runtime::Color::GetBlue(), MakeColor(0, 0, 1, 1)), "AZ6", "Color.blue");
        // Unity 的 yellow 是 RGB(255,235,4)，不是纯 (1,1,0)。
        Check(SameColor(Runtime::Color::GetYellow(), MakeColor(1.0f, 235.0f / 255.0f, 4.0f / 255.0f, 1.0f)), "AZ7",
              "Color.yellow matches Unity's RGB(255,235,4), not (1,1,0)");
        Check(SameColor(Runtime::Color::GetCyan(), MakeColor(0, 1, 1, 1)), "AZ8", "Color.cyan");
        Check(SameColor(Runtime::Color::GetMagenta(), MakeColor(1, 0, 1, 1)), "AZ9", "Color.magenta");
        Check(SameColor(Runtime::Color::GetGray(), MakeColor(0.5f, 0.5f, 0.5f, 1.0f)), "AZ10", "Color.gray is 50%");
        Check(SameColor(Runtime::Color::GetGrey(), Runtime::Color::GetGray()), "AZ11",
              "gray and grey are the same value (Unity keeps both spellings)");

        // ---- Lerp ----
        const ::Color black = Runtime::Color::GetBlack();
        const ::Color white = Runtime::Color::GetWhite();
        // t 夹到 [0,1]：不夹的话 t=2 会得到 2 —— 那不是插值，是外推，接下去写进
        // 材质就溢出。
        Check(SameColor(Runtime::Color::Lerp(black, white, 2.0f), white), "AZ12",
              "Lerp clamps t > 1 to the end colour");
        Check(SameColor(Runtime::Color::Lerp(black, white, -1.0f), black), "AZ13",
              "Lerp clamps t < 0 to the start colour");
        Check(SameColor(Runtime::Color::Lerp(black, white, 0.5f), MakeColor(0.5f, 0.5f, 0.5f, 1.0f)), "AZ14",
              "Lerp at t = 0.5 is the midpoint");
        Check(SameColor(Runtime::Color::LerpUnclamped(black, white, 2.0f), MakeColor(2, 2, 2, 1.0f)), "AZ15",
              "LerpUnclamped extrapolates past the end colour");
        // alpha 也要插：两个不同透明度的颜色插值后 alpha 不动是最典型的漏项。
        Check(SameColor(Runtime::Color::Lerp(MakeColor(0, 0, 0, 0), MakeColor(1, 1, 1, 1), 0.25f),
                        MakeColor(0.25f, 0.25f, 0.25f, 0.25f)),
              "AZ16", "Lerp interpolates alpha too, not just rgb");

        // ---- HSV ----
        Check(SameColor(Runtime::Color::HSVToRGB(0.25f, 0.0f, 0.7f), MakeColor(0.7f, 0.7f, 0.7f, 1.0f)), "AZ17",
              "HSVToRGB with s = 0 is grey regardless of h");
        // 负饱和度（脚本里除错出来的常见值）也必须退化成灰，而不是算出 > v 的通道。
        Check(SameColor(Runtime::Color::HSVToRGB(0.25f, -0.5f, 0.7f), MakeColor(0.7f, 0.7f, 0.7f, 1.0f)), "AZ17b",
              "a negative saturation degrades to grey instead of overshooting");
        Check(SameColor(Runtime::Color::HSVToRGB(0.0f, 1.0f, 1.0f), MakeColor(1, 0, 0, 1)), "AZ18",
              "h = 0 is red");
        Check(SameColor(Runtime::Color::HSVToRGB(1.0f / 3.0f, 1.0f, 1.0f), MakeColor(0, 1, 0, 1)), "AZ19",
              "h = 1/3 is green");
        Check(SameColor(Runtime::Color::HSVToRGB(2.0f / 3.0f, 1.0f, 1.0f), MakeColor(0, 0, 1, 1)), "AZ20",
              "h = 2/3 is blue");
        // 色相是环绕的：1.25 与 0.25 同色。
        Check(SameColor(Runtime::Color::HSVToRGB(1.25f, 1.0f, 1.0f), Runtime::Color::HSVToRGB(0.25f, 1.0f, 1.0f)),
              "AZ21", "hue wraps: h = 1.25 is the same colour as h = 0.25");
        Check(SameColor(Runtime::Color::HSVToRGB(-0.25f, 1.0f, 1.0f), Runtime::Color::HSVToRGB(0.75f, 1.0f, 1.0f)),
              "AZ22", "and a negative hue wraps the other way");
        // ⚠️ 只测 -0.25 分不出对错：`hue < 0 就 +1` 这一句自己就能把它救回来，
        // 于是 `h - std::floor(h)` 那一步是死代码也没人发现。跨过 -1 才是真正
        // 需要 floor 的地方（floor(-1.25) == -2，不是 -1）。变异测试里
        // "去掉 floor" 曾经全绿 —— 这条是它的回归门。
        Check(SameColor(Runtime::Color::HSVToRGB(-1.25f, 1.0f, 1.0f), Runtime::Color::HSVToRGB(0.75f, 1.0f, 1.0f)),
              "AZ28", "a hue below -1 wraps too (std::floor, not a bare +1)");

        // 往返：RGB -> HSV -> RGB 必须闭合，否则"取色 -> 改饱和 -> 写回"会把
        // 颜色慢慢漂掉（每次往返丢一点，几十帧后肉眼可见）。
        for (int i = 0; i < 6; ++i)
        {
            const float h = static_cast<float>(i) / 6.0f + 0.02f;
            const ::Color rgb = Runtime::Color::HSVToRGB(h, 0.6f, 0.9f);
            const Vector3 hsv = Runtime::Color::RGBToHSV(rgb);
            Check(std::fabs(hsv.x - h) < 0.002f && std::fabs(hsv.y - 0.6f) < 0.002f && std::fabs(hsv.z - 0.9f) < 0.002f,
                  "AZ23", "RGBToHSV(HSVToRGB(h,s,v)) round-trips (6 hues sampled)");
            Check(SameColor(Runtime::Color::HSVToRGB(hsv.x, hsv.y, hsv.z), rgb), "AZ24",
                  "and HSVToRGB(RGBToHSV(c)) returns the original colour");
        }

        // 灰的 H 是 1/3 —— Unity 的 `HSVToRGBHelper` 走 else 分支（offset = 2），
        // diff == 0 时 H = offset，/6 得 1/3。抄成 0 就和 Unity 对不上了。
        const Vector3 gray_hsv = Runtime::Color::RGBToHSV(Runtime::Color::GetGray());
        Check(std::fabs(gray_hsv.x - 1.0f / 3.0f) < 1e-6f && std::fabs(gray_hsv.y) < 1e-6f &&
                  std::fabs(gray_hsv.z - 0.5f) < 1e-6f,
              "AZ25", "grey is (h=1/3, s=0, v=0.5) -- Unity's own convention, h is NOT 0");
        // 只有纯黑（V == 0）的 H 才是 0，那是 `V != 0` 那条判据。
        const Vector3 black_hsv = Runtime::Color::RGBToHSV(Runtime::Color::GetBlack());
        Check(std::fabs(black_hsv.x) < 1e-6f && std::fabs(black_hsv.y) < 1e-6f && std::fabs(black_hsv.z) < 1e-6f,
              "AZ25b", "black is (0,0,0) -- the V == 0 branch, not the grey one");
        const Vector3 red_hsv = Runtime::Color::RGBToHSV(Runtime::Color::GetRed());
        Check(std::fabs(red_hsv.x) < 1e-6f && std::fabs(red_hsv.y - 1.0f) < 1e-6f && std::fabs(red_hsv.z - 1.0f) < 1e-6f,
              "AZ26", "pure red is (h=0, s=1, v=1)");
        const Vector3 blue_hsv = Runtime::Color::RGBToHSV(Runtime::Color::GetBlue());
        Check(std::fabs(blue_hsv.x - 2.0f / 3.0f) < 0.002f, "AZ27", "pure blue sits at h = 2/3");
    }

    void TestColorWiring()
    {
        const std::string header = ReadFile("Source/Runtime/Scripting/ScriptColor/ScriptColor.h");
        Check(!header.empty(), "BA0", "ScriptColor.h is readable");
        Check(CountOccurrences(header, "class Color : public Object") >= 1, "BA1",
              "the Color facade is a ZCLASS (so ZHeaderTool publishes it as `declare class Color`)");
        // 值类型与门面同名，靠命名空间分开；门面里所有值类型必须写全 `::Color`。
        Check(CountOccurrences(header, "namespace Runtime") >= 1, "BA2",
              "the facade lives in a namespace (a global `Color` would collide with the value type)");
        Check(CountOccurrences(header, "static ::Color GetRed()") >= 1, "BA3",
              "constants are static ZFUNCTIONs (the getter a ZSCRIPTPROPERTY needs)");
        Check(CountOccurrences(header, "ZSCRIPTPROPERTY(Name = \"red\", Getter = \"GetRed\")") >= 1, "BA4",
              "and are published as Unity's lowercase static properties (Color.red)");

        const std::string bind_h = ReadFile("Source/Runtime/Scripting/Binding/ScriptBinding.h");
        Check(!bind_h.empty(), "BA5", "ScriptBinding.h is readable");
        Check(CountOccurrences(bind_h, "struct Converter<Color>") >= 1, "BA6",
              "Color has a ZScript::Converter, so it can cross into scripts at all");
        Check(CountOccurrences(bind_h, "bool isStatic = false;") >= 1, "BA7",
              "ZScriptPropertyEntry carries isStatic (Properties vs Variables)");
        Check(CountOccurrences(bind_h, "struct PropertyCallback") >= 1, "BA8",
              "property accessors use their own trampoline (sharing MethodCallback trips LNK1179)");

        const std::string bind_cpp = ReadFile("Source/Runtime/Scripting/Binding/ScriptBinding.cpp");
        Check(!bind_cpp.empty(), "BA9", "ScriptBinding.cpp is readable");
        Check(CountOccurrences(bind_cpp, "declare interface Color") >= 1, "BA10",
              "the .d.ts declares Color as a value type (interface, not class)");
        // 静态属性走 pesapi 的 Variables 表、装到构造器；传错 is_static 它就
        // 哪儿都不出现（不报错、不崩，只是 undefined）。
        Check(CountOccurrences(bind_cpp, "prop.isStatic ? 1 : 0") >= 1, "BA11",
              "static properties are registered with is_static = 1 (pesapi Variables)");
        Check(CountOccurrences(bind_cpp, "instance_prop_count, static_prop_count") >= 1, "BA12",
              "and the two property tables are sized separately");
        Check(CountOccurrences(bind_cpp, "if (prop.isStatic)") >= 1, "BA13",
              "the .d.ts writes `static` before `readonly`");

        const std::string codegen = ReadFile("tools/ZHeaderTool/zht/codegen.py");
        Check(!codegen.empty(), "BA14", "codegen.py is readable");
        Check(CountOccurrences(codegen, "prop.isStatic ? 1 : 0") == 0 &&
                  CountOccurrences(codegen, "sp.is_static else \"false\"") >= 1,
              "BA15", "codegen stamps isStatic into the generated property entry");
        // 这条是 LNK1179 的回归门：静态访问器再从方法表里发一遍，MSVC 就会把
        // MethodCallback 编进两个 COMDAT 段，链接器直接拒收整个 obj。
        Check(CountOccurrences(codegen, "m.is_static and m.name in static_accessors") >= 1, "BA16",
              "static accessors are kept out of the method table (otherwise: duplicate COMDAT)");

        const std::string parser = ReadFile("tools/ZHeaderTool/zht/parser.py");
        Check(!parser.empty(), "BA17", "parser.py is readable");
        Check(CountOccurrences(parser, "getter '%s' must not be static") == 0, "BA18",
              "ZSCRIPTPROPERTY no longer rejects a static getter");
        Check(CountOccurrences(parser, "\"Color\": \"Color\"") >= 1, "BA19",
              "a ZPROPERTY(Color) field is marshallable");
    }

    // --------------------------------------------------------------- BB ----
    // Unity 的静态字段（`Time.deltaTime` / `Screen.width` / `Mathf.PI` /
    // `Application.dataPath`）：拼写是对的，但"装没装上"在 C++ 侧看不见 ——
    // 装错表（pesapi 的 Properties 而不是 Variables）不报错、不崩，只是 JS 里
    // 永远 undefined。所以这里查源码里的声明，ScriptVMSmokeTest 的 D/J 两组查
    // 运行时。
    void TestStaticFieldWiring()
    {
        const std::string time = ReadFile("Source/Runtime/Scripting/ScriptTime/ScriptTime.h");
        Check(!time.empty(), "BB0", "ScriptTime.h is readable");
        Check(CountOccurrences(time, "ZSCRIPTPROPERTY(Name = \"deltaTime\", Getter = \"GetDeltaTime\")") >= 1,
              "BB1", "Time.deltaTime is a static script property (Unity's spelling)");
        // 可写属性必须带 setter —— Unity 里 `Time.timeScale = 0` 就是暂停。
        Check(CountOccurrences(time, "Setter = \"SetTimeScale\"") >= 1, "BB2",
              "Time.timeScale is writable (Setter), not readonly");
        // 一个静态访问器只能挂一个属性：挂两次 ⇒ PropertyCallback 进两个 COMDAT
        // 段 ⇒ LNK1179。所以 realtimeSinceStartup 必须有自己的转发函数。
        Check(CountOccurrences(time, "GetRealtimeSinceStartup") >= 2, "BB3",
              "realtimeSinceStartup has its own getter (a shared one trips LNK1179)");
        // 过时的注释比没注释更坑：它会让人继续写 Get* 那一套。
        Check(CountOccurrences(time, "ZSCRIPTPROPERTY 不支持") == 0 &&
                  CountOccurrences(time, "getter 不允许 static") == 0,
              "BB4", "ScriptTime.h no longer claims static properties are impossible");

        const std::string screen = ReadFile("Source/Runtime/Scripting/ScriptScreen/ScriptScreen.h");
        Check(CountOccurrences(screen, "ZSCRIPTPROPERTY(Name = \"width\", Getter = \"GetWidth\")") >= 1,
              "BB5", "Screen.width is a static script property");
        Check(CountOccurrences(screen, "ZSCRIPTPROPERTY(Name = \"dpi\", Getter = \"GetDpi\")") >= 1,
              "BB6", "Screen.dpi is a static script property");

        const std::string mathf = ReadFile("Source/Runtime/Scripting/ScriptMath/ScriptMath.h");
        Check(CountOccurrences(mathf, "ZSCRIPTPROPERTY(Name = \"PI\", Getter = \"Pi\")") >= 1,
              "BB7", "Mathf.PI is a static script property (Unity's capital spelling)");
        Check(CountOccurrences(mathf, "ZSCRIPTPROPERTY(Name = \"Deg2Rad\", Getter = \"GetDeg2Rad\")") >= 1,
              "BB8", "Mathf.Deg2Rad is the constant Unity has, not a function");
        Check(CountOccurrences(mathf, "ZSCRIPTPROPERTY(Name = \"Infinity\", Getter = \"Infinity\")") >= 1,
              "BB9", "Mathf.Infinity is a static script property");

        const std::string app = ReadFile("Source/Runtime/Application/Application.h");
        Check(CountOccurrences(app, "ZSCRIPTPROPERTY(Name = \"dataPath\", Getter = \"GetDataPath\")") >= 1,
              "BB10", "Application.dataPath is a static script property");
        Check(CountOccurrences(app, "ZSCRIPTPROPERTY(Name = \"isPlaying\", Getter = \"GetIsPlaying\")") >= 1,
              "BB11", "Application.isPlaying is a static script property");
        Check(CountOccurrences(app, "Setter = \"SetTargetFrameRate\"") >= 1, "BB12",
              "Application.targetFrameRate is writable");

        // 方法表里不该再有静态访问器（见 BA16）——这次是"旧拼写是否真的撤掉了"。
        const std::string codegen = ReadFile("tools/ZHeaderTool/zht/codegen.py");
        Check(CountOccurrences(codegen, "m.is_static and m.name in static_accessors") >= 1, "BB13",
              "codegen still keeps static accessors out of the method table");
    }

    // --------------------------------------------------------------- BC ----
    // `Vector3` / `Quaternion` 的静态面（`Vector3.zero`、`Quaternion.identity`）。
    // C++ 侧看不见"装没装进 d.ts"，所以这里查源码，ScriptVMSmokeTest 的 K 组查
    // 运行时。
    void TestVectorFacadeWiring()
    {
        const std::string header = ReadFile("Source/Runtime/Scripting/ScriptVector/ScriptVector.h");
        const std::string source = ReadFile("Source/Runtime/Scripting/ScriptVector/ScriptVector.cpp");
        Check(!header.empty(), "BC0", "ScriptVector.h is readable");
        // 门面必须与值类型同名，但得住进命名空间（同 Runtime::Color）。
        Check(CountOccurrences(header, "namespace Runtime") >= 1, "BC1",
              "the Vector3 facade is namespaced (a global one would collide with the value type)");
        Check(CountOccurrences(header, "class Vector3 : public Object") >= 1 &&
                  CountOccurrences(header, "class Quaternion : public Object") >= 1,
              "BC2", "both value types have an Object-derived facade (ZCLASS requirement)");
        // 值类型一律全称：命名空间里裸 `Vector3` 指的是门面类。
        Check(CountOccurrences(header, "static ::Vector3 GetZero()") >= 1, "BC3",
              "constants return a fully-qualified ::Vector3");
        Check(CountOccurrences(header, "ZSCRIPTPROPERTY(Name = \"zero\", Getter = \"GetZero\")") >= 1,
              "BC4", "Vector3.zero is a static script property (Unity's spelling)");
        Check(CountOccurrences(header, "ZSCRIPTPROPERTY(Name = \"identity\", Getter = \"GetIdentity\")") >= 1,
              "BC5", "Quaternion.identity is a static script property");
        // 轴向常量：Z-up。写反了脚本会把物体平推到墙里，而且不报错。
        Check(CountOccurrences(source, "Make(0.0f, 0.0f, 1.0f)") >= 1, "BC6",
              "up is +Z (Z-up engine, not Unity's +Y) -- see the file header");
        Check(CountOccurrences(source, "Make(0.0f, -1.0f, 0.0f)") >= 1, "BC7",
              "forward is -Y (the camera's default facing)");
        // 实现必须转发，不许重写一份（两份实现迟早漂移）。
        Check(!source.empty(), "BC8", "ScriptVector.cpp is readable");
        Check(CountOccurrences(source, "VectorMath::") >= 10 &&
                  CountOccurrences(source, "QuaternionMath::") >= 8,
              "BC9", "the facades forward to VectorMath / QuaternionMath (no second implementation)");
        // Unity 的 `Vector3.Lerp` 夹 t —— 门面按 Unity 来，`VectorMath.Lerp` 不夹。
        Check(CountOccurrences(source, "clamped > 1.0f") >= 1, "BC10",
              "Vector3.Lerp clamps t like Unity's (LerpUnclamped is the other one)");
    }

    // --------------------------------------------------------------- BD ----
    // `Vector3.right` 与 `transform.right` 必须是同一边。
    //
    // 这两处约定是**分别**写下来的（`Transform.h` 的轴向注释 vs
    // `ScriptVector.h` 的轴向表），任一边改了另一边不会知道。左/右写反尤其阴：
    // 两边都还是单位向量、都还是互相垂直，所有"是不是单位向量"的断言全绿，只有
    // "把两边放在一起比"才分得出来。（本切片第一版就是 `Vector3.right = +X`
    // 而 `Transform::GetRight() = -X`。）
    void TestAxisConventionAgreement()
    {
        ObjectManager* om = GET_SYSTEM(ObjectManager);
        if (om == nullptr)
        {
            om = new ObjectManager();
            SystemRegistry::GetInstance().RegisterSystem(om);
            om->Initialize();
        }
        GameObject* owner = nullptr;
        Transform* t = MakeTransform(*om, owner);
        Check(t != nullptr, "BD0", "a Transform is reachable for the axis cross-check");

        // 未旋转：transform 的方向就是世界轴。
        Check(Near(Runtime::Vector3::GetUp(), t->GetUp()), "BD1",
              "Vector3.up == transform.up (identity)");
        Check(Near(Runtime::Vector3::GetForward(), t->GetForward()), "BD2",
              "Vector3.forward == transform.forward (identity)");
        Check(Near(Runtime::Vector3::GetRight(), t->GetRight()), "BD3",
              "Vector3.right == transform.right (identity) -- +X is LEFT here");
        Check(Near(Runtime::Vector3::GetLeft(), t->GetLeft()), "BD4",
              "Vector3.left == transform.left (identity)");
        Check(Near(Runtime::Vector3::GetDown(), t->GetDown()), "BD5",
              "Vector3.down == transform.down (identity)");
        Check(Near(Runtime::Vector3::GetBack(), t->GetBack()), "BD6",
              "Vector3.back == transform.back (identity)");

        // 六个方向两两互补：不能有两个常量落到同一根轴上。
        Check(Near(Runtime::Vector3::GetRight(), Runtime::Vector3::GetLeft()) == false &&
                  Near(Runtime::Vector3::GetUp(), Runtime::Vector3::GetDown()) == false &&
                  Near(Runtime::Vector3::GetForward(), Runtime::Vector3::GetBack()) == false,
              "BD7", "the six axis constants are three distinct pairs, not three copies");
        // 右手系：up × right = forward（与 Transform 那边的 S13 同一条）。
        Check(Near(Runtime::Vector3::GetUp().crossProduct(Runtime::Vector3::GetRight()),
                   Runtime::Vector3::GetForward()),
              "BD8", "up x right == forward (right-handed, same as transform)");
    }

    // ---------------------------------------------------------------- BE ---
    // Script coroutines: StartCoroutine / StopCoroutine / StopAllCoroutines.

    /// Stands in for a script-side generator.
    ///
    /// `Behaviour` sees a script coroutine through exactly three hooks and one
    /// opaque integer, so a class that answers those hooks with a canned yield
    /// sequence *is* a script coroutine as far as the engine is concerned. That
    /// is what lets this group test the adapter (does `yield new
    /// WaitForEndOfFrame()` land in the EndOfFrame phase?) without standing up a
    /// whole JS VM -- the VM is ScriptVMSmokeTest's job.
    class CoroutineProbe : public Behaviour
    {
        REGISTER_CLASS(CoroutineProbe)

    public:
        /// false -> "this Behaviour has no script side", which every plain C++
        /// Behaviour answers by default.
        bool has_script = true;

        /// false -> "the body threw" / "the value is gone".
        bool step_ok = true;

        /// One entry per `yield`, in order. Past the end the body is done.
        eastl::vector<ScriptYield> body;

        // -- observations -------------------------------------------------
        eastl::vector<eastl::string> started;
        int steps = 0;
        int finishes = 0;
        int released = 0;
        int double_release = 0;

        /// The invariant that matters: a generator that has been released must
        /// never be polled again. In the real (JS) implementation that is a
        /// value ref that has already been dropped.
        int polled_after_release = 0;

        bool StartScriptCoroutine(const char* method_name, uint64_t& out_script_id) override
        {
            if (!has_script)
            {
                return false;
            }
            started.emplace_back(method_name);
            gen_steps.push_back(0);
            out_script_id = static_cast<uint64_t>(gen_steps.size());
            return true;
        }

        bool StepScriptCoroutine(uint64_t script_id, ScriptYield& out) override
        {
            if (!step_ok || script_id == 0 || script_id > gen_steps.size())
            {
                return false;
            }
            if (gen_steps[script_id - 1] == kReleased)
            {
                ++polled_after_release;
                return false;
            }
            const int step = gen_steps[script_id - 1];
            if (step >= static_cast<int>(body.size()))
            {
                out.done = true;
                ++finishes;
                return true;
            }
            out = body[static_cast<std::size_t>(step)];
            gen_steps[script_id - 1] = step + 1;
            ++steps;
            return true;
        }

        void EndScriptCoroutine(uint64_t script_id) override
        {
            if (script_id == 0 || script_id > gen_steps.size())
            {
                return;
            }
            if (gen_steps[script_id - 1] == kReleased)
            {
                ++double_release;
                return;
            }
            gen_steps[script_id - 1] = kReleased;
            ++released;
        }

    private:
        static constexpr int kReleased = -1;
        eastl::vector<int> gen_steps;
    };

    void AdvanceUpdate(int frames, float scaled, float unscaled)
    {
        CoroutineScheduler* scheduler = GET_SYSTEM(CoroutineScheduler);
        for (int i = 0; i < frames && scheduler != nullptr; ++i)
        {
            scheduler->TickUpdate(scaled, unscaled);
        }
    }

    void AdvancePhase(int frames, CoroutinePhase phase)
    {
        CoroutineScheduler* scheduler = GET_SYSTEM(CoroutineScheduler);
        for (int i = 0; i < frames && scheduler != nullptr; ++i)
        {
            switch (phase)
            {
                case CoroutinePhase::kFixedUpdate: scheduler->TickFixedUpdate(); break;
                case CoroutinePhase::kLateUpdate: scheduler->TickLateUpdate(); break;
                case CoroutinePhase::kEndOfFrame: scheduler->TickEndOfFrame(); break;
                case CoroutinePhase::kUpdate:
                default: scheduler->TickUpdate(0.f, 0.f); break;
            }
        }
    }

    void TestScriptCoroutines()
    {
        CoroutineScheduler* scheduler = GET_SYSTEM(CoroutineScheduler);
        Check(scheduler != nullptr, "BE0", "CoroutineScheduler is reachable");
        if (scheduler == nullptr)
        {
            return;
        }
        scheduler->StopAll();

        // --- a Behaviour with no script side ------------------------------
        {
            CoroutineProbe probe;
            probe.has_script = false;
            Check(probe.StartCoroutine("Nope") == CoroutineScheduler::kInvalidId,
                  "BE1", "no script side -> StartCoroutine refuses");
            Check(scheduler->GetRunningCount() == 0, "BE2", "and nothing is scheduled");
            Check(probe.GetCoroutineCount() == 0, "BE3", "and nothing is recorded on the behaviour");
        }

        // --- rejected input -----------------------------------------------
        {
            CoroutineProbe probe;
            Check(probe.StartCoroutine("") == CoroutineScheduler::kInvalidId,
                  "BE4", "an empty method name starts nothing");
            Check(scheduler->GetRunningCount() == 0, "BE5", "and schedules nothing");
        }

        // --- `yield null`: one resume per Update ---------------------------
        {
            CoroutineProbe probe;
            ScriptYield null_yield {};
            null_yield.kind = ScriptYieldKind::kNull;
            probe.body = {null_yield, null_yield};

            const uint64_t id = probe.StartCoroutine("Tick");
            Check(id != CoroutineScheduler::kInvalidId, "BE6", "StartCoroutine hands back a real id");
            Check(probe.started.size() == 1 && probe.started[0] == "Tick",
                  "BE7", "and the method name reached the script side");
            Check(probe.steps == 1, "BE8", "the first segment runs synchronously, like Unity");
            Check(probe.IsCoroutineRunning("Tick"), "BE9", "the behaviour lists it");
            Check(probe.GetCoroutineCount() == 1, "BE10", "exactly once");

            AdvanceUpdate(1, 0.1f, 0.1f);
            Check(probe.steps == 2, "BE11", "`yield null` resumes on the very next Update");

            AdvanceUpdate(1, 0.1f, 0.1f);
            Check(probe.finishes == 1, "BE12", "the body is asked one more time and reports done");
            // 必须在"没有任何人触发剪枝"的前提下断言：泵自己在出口释放，而不是
            // 等到下一次 Start / IsCoroutineRunning 顺手剪掉。写过一次没有这行的
            // 版本 —— 自然结束的协程会一直攥着 JS 值，而调用方完全看不出来。
            Check(probe.released == 1, "BE12b", "the pump releases on its own exit, unpruned");
            Check(!probe.IsCoroutineRunning("Tick"), "BE13", "so it is no longer listed");
            Check(probe.released == 1, "BE14", "and the generator is released");
            Check(probe.polled_after_release == 0, "BE15", "never polled after release");
            Check(probe.double_release == 0, "BE16", "and never released twice");
        }

        // --- WaitForSeconds: scaled time ----------------------------------
        {
            CoroutineProbe probe;
            ScriptYield wait {};
            wait.kind = ScriptYieldKind::kSeconds;
            wait.seconds = 0.25f;
            ScriptYield null_yield {};
            probe.body = {wait, null_yield};

            probe.StartCoroutine("Wait");
            AdvanceUpdate(2, 0.1f, 0.1f);  // 0.2s
            Check(probe.steps == 1, "BE17", "WaitForSeconds does not resume early");
            AdvanceUpdate(1, 0.1f, 0.1f);  // 0.3s
            Check(probe.steps == 2, "BE18", "and resumes once the scaled time elapsed");
        }

        // --- WaitForSeconds is frozen by timeScale -------------------------
        // 与下面 BE19/BE20 是互补的一对：同样的 delta（缩放 0、墙钟 0.1），
        // realtime 必须走、WaitForSeconds 必须停。只写其中一组的话，
        // "把 WaitForSeconds 实现成 WaitForSecondsRealtime" 这个变异全绿 ——
        // 试过一次，SC4 就是这么漏掉的。
        {
            CoroutineProbe probe;
            ScriptYield wait {};
            wait.kind = ScriptYieldKind::kSeconds;
            wait.seconds = 0.2f;
            ScriptYield null_yield {};
            probe.body = {wait, null_yield};

            probe.StartCoroutine("Paused");
            AdvanceUpdate(4, 0.f, 0.5f);
            Check(probe.steps == 1, "BE18b", "WaitForSeconds does not advance while timeScale is 0");
        }

        // --- WaitForSecondsRealtime: the unscaled clock -------------------
        {
            CoroutineProbe probe;
            ScriptYield wait {};
            wait.kind = ScriptYieldKind::kSecondsRealtime;
            wait.seconds = 0.2f;
            ScriptYield null_yield {};
            probe.body = {wait, null_yield};

            probe.StartCoroutine("Realtime");
            // A paused world: scaled 0, but the wall clock is running.
            AdvanceUpdate(1, 0.f, 0.1f);
            Check(probe.steps == 1, "BE19", "a realtime wait ignores a zero scaled delta");
            AdvanceUpdate(1, 0.f, 0.1f);
            Check(probe.steps == 2, "BE20", "and advances on the unscaled clock alone");
        }

        // --- the phase-carrying waits -------------------------------------
        // The whole reason ScriptYield carries a *kind* and not just a number:
        // resuming a WaitForEndOfFrame in Update still produces a value, just
        // one phase stale, which is a bug with no error anywhere.
        {
            CoroutineProbe probe;
            ScriptYield end_of_frame {};
            end_of_frame.kind = ScriptYieldKind::kEndOfFrame;
            ScriptYield null_yield {};
            probe.body = {end_of_frame, null_yield};

            probe.StartCoroutine("Late");
            AdvanceUpdate(3, 0.1f, 0.1f);
            Check(probe.steps == 1, "BE21", "WaitForEndOfFrame is NOT resumed by Update");
            AdvancePhase(1, CoroutinePhase::kEndOfFrame);
            Check(probe.steps == 2, "BE22", "and is resumed by the EndOfFrame phase");
        }
        {
            CoroutineProbe probe;
            ScriptYield fixed {};
            fixed.kind = ScriptYieldKind::kFixedUpdate;
            ScriptYield null_yield {};
            probe.body = {fixed, null_yield};

            probe.StartCoroutine("Fixed");
            AdvanceUpdate(2, 0.1f, 0.1f);
            Check(probe.steps == 1, "BE23", "WaitForFixedUpdate is not resumed by Update");
            AdvancePhase(1, CoroutinePhase::kFixedUpdate);
            Check(probe.steps == 2, "BE24", "and is resumed by the FixedUpdate phase");
        }
        {
            CoroutineProbe probe;
            ScriptYield late {};
            late.kind = ScriptYieldKind::kLateUpdate;
            ScriptYield null_yield {};
            probe.body = {late, null_yield};

            probe.StartCoroutine("LateOne");
            AdvanceUpdate(2, 0.1f, 0.1f);
            Check(probe.steps == 1, "BE25", "WaitForLateUpdate is not resumed by Update");
            AdvancePhase(1, CoroutinePhase::kLateUpdate);
            Check(probe.steps == 2, "BE26", "and is resumed by the LateUpdate phase");
        }

        // --- a broken generator is dropped, not polled forever -------------
        {
            CoroutineProbe probe;
            ScriptYield null_yield {};
            probe.body = {null_yield, null_yield};
            probe.step_ok = true;
            probe.StartCoroutine("Broken");
            probe.step_ok = false;

            const int before = scheduler->GetRunningCount();
            AdvanceUpdate(4, 0.1f, 0.1f);
            Check(scheduler->GetRunningCount() < before,
                  "BE27", "a generator that stops answering ends the coroutine");
            Check(probe.released >= 1, "BE28", "and is released rather than leaked");
        }

        // --- StopCoroutine by name ----------------------------------------
        {
            CoroutineProbe probe;
            ScriptYield null_yield {};
            probe.body = {null_yield, null_yield, null_yield};

            probe.StartCoroutine("A");
            probe.StartCoroutine("B");
            Check(scheduler->GetRunningCount() == 2, "BE29", "two coroutines are running");
            Check(probe.GetCoroutineCount() == 2, "BE30", "and both are recorded");

            probe.StopCoroutine("A");
            Check(!probe.IsCoroutineRunning("A"), "BE31", "StopCoroutine drops that name");
            Check(probe.IsCoroutineRunning("B"), "BE32", "and leaves the other alone");
            Check(scheduler->GetRunningCount() == 1, "BE33", "one is left in the scheduler");
            Check(probe.released == 1, "BE34", "the stopped generator was released");

            AdvanceUpdate(3, 0.1f, 0.1f);
            Check(probe.started.size() == 2, "BE35", "no extra generator was created");
            Check(probe.polled_after_release == 0, "BE36", "the stopped one is never polled again");
        }

        // --- StopAllCoroutines --------------------------------------------
        {
            CoroutineProbe probe;
            ScriptYield null_yield {};
            probe.body = {null_yield, null_yield, null_yield};
            probe.StartCoroutine("A");
            probe.StartCoroutine("B");
            probe.StopAllCoroutines();
            Check(scheduler->GetRunningCount() == 0, "BE37", "StopAllCoroutines empties the scheduler");
            Check(probe.GetCoroutineCount() == 0, "BE38", "and the behaviour's own list");
            Check(probe.released == 2, "BE39", "both generators released, once each");
            Check(probe.double_release == 0, "BE40", "no double release");
            AdvanceUpdate(3, 0.1f, 0.1f);
            Check(probe.polled_after_release == 0, "BE41", "and neither is polled afterwards");
        }

        // --- destruction stops everything ---------------------------------
        {
            const int before = scheduler->GetRunningCount();
            {
                CoroutineProbe temp;
                ScriptYield null_yield {};
                temp.body = {null_yield, null_yield, null_yield};
                temp.StartCoroutine("A");
                temp.StartCoroutine("B");
                Check(scheduler->GetRunningCount() == before + 2, "BE42", "two are running");
            }
            // A coroutine that outlives its Behaviour would resume into freed
            // script state on the very next frame -- the same reason the Invoke
            // group has a check for this.
            Check(scheduler->GetRunningCount() == before, "BE43", "destroying the Behaviour stops them");
        }

        scheduler->StopAll();
    }

    void TestScriptCoroutineWiring()
    {
        const std::string behaviour_header =
            ReadFile("Source/Runtime/Function/Framework/Component/Behaviour.h");
        Check(!behaviour_header.empty(), "BF0", "Behaviour.h is readable");
        Check(CountOccurrences(behaviour_header, "uint64_t StartCoroutine(") >= 1,
              "BF1", "StartCoroutine is declared");
        Check(CountOccurrences(behaviour_header, "void StopCoroutine(") >= 1,
              "BF2", "StopCoroutine is declared");
        Check(CountOccurrences(behaviour_header, "void StopAllCoroutines(") >= 1,
              "BF3", "StopAllCoroutines is declared");
        Check(CountOccurrences(behaviour_header, "IsCoroutineRunning") >= 1,
              "BF4", "IsCoroutineRunning is declared");
        Check(CountOccurrences(behaviour_header, "StartScriptCoroutine") >= 1,
              "BF5", "the script-side hook is virtual");
        Check(CountOccurrences(behaviour_header, "StepScriptCoroutine") >= 1,
              "BF6", "and so is the step hook");
        Check(CountOccurrences(behaviour_header, "EndScriptCoroutine") >= 1,
              "BF7", "and the release hook");
        Check(CountOccurrences(behaviour_header, "ScriptYield.h") >= 1,
              "BF8", "Behaviour.h takes the dependency-free yield type, not pesapi");
        // Same trap as the Invoke group: publishing `Behaviour` emits a second
        // `declare class Behaviour` and tsc fails inside the user's project.
        Check(CountOccurrences(behaviour_header, "ZFUNCTION()") == 0,
              "BF9", "Behaviour itself is still NOT published as a script class");

        const std::string behaviour_source =
            ReadFile("Source/Runtime/Function/Framework/Component/Behaviour.cpp");
        Check(!behaviour_source.empty(), "BF10", "Behaviour.cpp is readable");
        Check(CountOccurrences(behaviour_source, "ScriptCoroutinePump") >= 2,
              "BF11", "a single pump coroutine adapts generator.next() to co_yield");
        Check(CountOccurrences(behaviour_source, "kEndOfFrame") >= 1,
              "BF12", "the pump maps the EndOfFrame kind");
        Check(CountOccurrences(behaviour_source, "kSecondsRealtime") >= 1,
              "BF13", "and the realtime kind");
        Check(CountOccurrences(behaviour_source, "EndScriptCoroutine") >= 2,
              "BF14", "the release hook is actually called");
        // Two exits can end one coroutine (body finished vs. somebody stopped
        // it) and a JS value ref must be released exactly once. Without a flag
        // the second release lands on whoever owns the slot by then.
        // 2 = 读一次 + 写一次，都在 ReleasePendingGenerator 里。低于 2 说明
        // 释放又变成了"谁先到谁释放"，双释放就会落到当时占着那个槽的人身上。
        Check(CountOccurrences(behaviour_source, "generator_owned") >= 2,
              "BF14b", "release is gated on an ownership flag, not on call order");
        Check(CountOccurrences(behaviour_source, "ReleaseScriptCoroutine") >= 3,
              "BF14c", "and the pump releases on its own exit, not only on prune");

        const std::string ts_header =
            ReadFile("Source/Runtime/Function/Framework/Component/Script/TypeScriptComponent.h");
        Check(CountOccurrences(ts_header, "uint64_t StartCoroutine(const eastl::string& methodName)") >= 1,
              "BF15", "TypeScriptComponent publishes StartCoroutine");
        Check(CountOccurrences(ts_header, "void StopAllCoroutines();") >= 1,
              "BF16", "and StopAllCoroutines");
        Check(CountOccurrences(ts_header, "void StopCoroutine(const eastl::string& methodName);") >= 1,
              "BF16b", "and StopCoroutine");
        // 11 ZFUNCTIONs: HasLiveInstance, GetScriptGuid, the five Invoke-family
        // forwarders and the four coroutine ones. Counting them (rather than one
        // name) is what catches a method that was added to the class but never
        // annotated -- which compiles, links, runs, and is simply invisible to
        // scripts.
        Check(CountOccurrences(ts_header, "ZFUNCTION") >= 10,
              "BF16c", "all of them carry ZFUNCTION");
        Check(CountOccurrences(ts_header, "m_CoroutineGenerators") >= 1,
              "BF17", "the JS generators are owned on the script side");

        const std::string ts_source =
            ReadFile("Source/Runtime/Function/Framework/Component/Script/TypeScriptComponent.cpp");
        Check(!ts_source.empty(), "BF18", "TypeScriptComponent.cpp is readable");
        Check(CountOccurrences(ts_source, "StartScriptCoroutine") >= 1,
              "BF19", "it starts the generator through the hook");
        Check(CountOccurrences(ts_source, "GeneratorNext") >= 1,
              "BF20", "and steps it through ScriptEnv");
        Check(CountOccurrences(ts_source, "ReleaseCoroutineValue") >= 2,
              "BF21", "and releases it (both explicit and on teardown)");

        const std::string env_source = ReadFile("Source/Runtime/Scripting/Wrapper/ScriptEnv.cpp");
        Check(!env_source.empty(), "BF22", "ScriptEnv.cpp is readable");
        // The declaration alone is a lie: without the prototype forwarding,
        // `this.StartCoroutine(...)` type-checks and then does nothing.
        Check(CountOccurrences(env_source, "Behaviour.prototype.StartCoroutine") >= 1,
              "BF23", "the JS Behaviour prototype forwards StartCoroutine");
        Check(CountOccurrences(env_source, "Behaviour.prototype.StopAllCoroutines") >= 1,
              "BF24", "and StopAllCoroutines");
        Check(CountOccurrences(env_source, "__zWait") >= 5,
              "BF25", "the YieldInstruction globals carry the __zWait tag");
        Check(CountOccurrences(env_source, "g.WaitForEndOfFrame") >= 1,
              "BF26", "WaitForEndOfFrame is installed as a global");
        Check(CountOccurrences(env_source, "g.Coroutine") >= 1,
              "BF27", "and so is the Coroutine handle type");
        // Every tag the shim can emit must have a branch that reads it, or a
        // script can yield something the engine silently treats as one frame.
        Check(CountOccurrences(env_source, "\"secondsRealtime\"") >= 1,
              "BF28", "GeneratorNext reads the realtime tag");
        Check(CountOccurrences(env_source, "\"endOfFrame\"") >= 1,
              "BF29", "and the end-of-frame tag");

        const std::string binding_source =
            ReadFile("Source/Runtime/Scripting/Binding/ScriptBinding.cpp");
        Check(CountOccurrences(binding_source, "StartCoroutine(methodName: string): Coroutine | null") >= 1,
              "BF30", "the .d.ts declares StartCoroutine on Behaviour");
        Check(CountOccurrences(binding_source, "declare class WaitForEndOfFrame") >= 1,
              "BF31", "and WaitForEndOfFrame as a global class");
        Check(CountOccurrences(binding_source, "declare class Coroutine") >= 1,
              "BF32", "and the Coroutine handle");
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
    // TestResources 必须跑在 TestApplicationFacade **之前**：后者会注册并初始化
    // ConfigManager / ProjectInfo，而那两个正是 Resources 相对路径解析的搜索根。
    // 在它们不存在时跑，才能真正验证"绝对路径不依赖搜索根"这一条 —— 有了搜索根，
    // `std::filesystem::operator/` 遇到绝对右值本来就会整体替换，那条快速路径会
    // 被完全掩盖（变异 R3 就是这么漏掉的）。
    TestResources();
    TestResourcesWiring();
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
    TestCameraFacade();
    TestCameraWiring();
    TestRigidbody();
    TestRigidbodyWiring();
    TestCharacterController();
    TestCharacterControllerWiring();
    TestCollider();
    TestColliderWiring();
    TestPlayerPrefs();
    TestPlayerPrefsWiring();
    TestComponentFacade();
    TestComponentFacadeWiring();
    TestMathf();
    TestMathfWiring();
    TestVectorQuaternionExtras();
    TestVectorQuaternionExtrasWiring();
    TestObjectLifecycle();
    TestObjectLifecycleWiring();
    TestPhysicMaterialCombine();
    TestPhysicMaterial();
    TestPhysicMaterialWiring();
    TestCameraExtras();
    TestCameraExtrasWiring();
    TestNavMeshAgent();
    TestNavMeshAgentWiring();
    TestAudioSource();
    TestAudioSourceWiring();
    TestColor();
    TestColorWiring();
    TestStaticFieldWiring();
    TestVectorFacadeWiring();
    TestAxisConventionAgreement();
    TestScriptCoroutines();
    TestScriptCoroutineWiring();
    TestWiringGate();

    std::printf("\n=== %d checks, %d failures ===\n", g_Checks, g_Failures);
    return (g_Failures == 0) ? 0 : 1;
}
