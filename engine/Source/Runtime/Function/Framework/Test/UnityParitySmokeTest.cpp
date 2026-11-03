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
#include "Runtime/Function/Input/EnhancedInputSystem.h"
#include "Runtime/Scripting/ScriptInput/ScriptInput.h"
#include "Runtime/BaseClasses/ObjectDefines.h"
#include "Runtime/BaseClasses/ObjectManager.h"
#include "Runtime/BaseClasses/Type.h"
#include "Runtime/BaseClasses/TypeManager.h"
#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Core/Math/Matrix4.h"
#include "Runtime/Core/Math/AnimationCurve.h"
#include "Runtime/Core/Math/Frustum.h"
#include "Runtime/Core/Math/Plane.h"
#include "Runtime/Core/Math/Math.h"
#include "Runtime/Core/Math/SmoothDamp.h"
#include "Runtime/Core/Math/UnityRandom.h"
#include "Runtime/Core/Time/TimeState.h"
#include "Runtime/Core/Math/Vector4.h"
#include "Runtime/Function/Render/DebugDraw/DebugDrawGroup.h"
#include "Runtime/Function/Render/DebugDraw/DebugDrawManager.h"
#include "Runtime/Scripting/ScriptGizmos/ScriptGizmos.h"
#include "Runtime/Scripting/ScriptAnimationCurve/ScriptAnimationCurve.h"
// `Runtime::Vector2Int` / `Runtime::Vector3Int` / `Runtime::Rect` / `Runtime::RectInt`
// 是脚本门面（值类型仍是全局的 `::Vector2Int` 等），所以本文件里凡指值类型一律
// 写 `::` 前缀，避免与门面混淆。
#include "Runtime/Scripting/ScriptIntVector/ScriptIntVector.h"
// `Runtime::LayerMask` 是层的脚本门面（返回的是 number，不是 struct）。
#include "Runtime/Scripting/ScriptLayerMask/ScriptLayerMask.h"
#include "Runtime/Scripting/ScriptRect/ScriptRect.h"
// `Runtime::Plane` / `Runtime::GeometryUtility` 是 Plane 与视锥的脚本门面
// （值类型仍是全局的 `::Plane`）。
#include "Runtime/Scripting/ScriptPlane/ScriptPlane.h"
#include "Runtime/Scripting/ScriptBounds/ScriptBounds.h"
#include "Runtime/Scripting/ScriptColor/ScriptColor.h"
#include "Runtime/Scripting/ScriptSystemInfo/ScriptSystemInfo.h"
#include "Runtime/Scripting/ScriptQuality/ScriptQuality.h"
#include "Runtime/Scripting/ScriptCursor/ScriptCursor.h"
#include "Runtime/Function/Render/Texture/Texture2D.h"
#include "Runtime/Function/Render/Texture/RenderTexture.h"
#include "Runtime/Function/Framework/Component/Particle/ParticleComponent.h"
#include "Runtime/Function/AI/Navigation/NavMeshAgentComponent.h"
#include "Runtime/Function/AI/Navigation/NavigationSystem.h"
#include "Runtime/Function/Audio/AudioDecoder.h"
#include "Runtime/Function/Audio/AudioSystem.h"
#include "Runtime/Function/Framework/Component/Audio/AudioSourceComponent.h"
#include "Runtime/Function/Command/CommandSystem.h"
#include "Runtime/Function/Framework/Component/Behaviour.h"
#include "Runtime/Function/Framework/Component/Camera/CameraComponent.h"
#include "Runtime/Function/Framework/Component/Mesh/BaseRenderer.h"
#include "Runtime/Function/Framework/Component/Mesh/MeshRenderer.h"
#include "Runtime/Function/Framework/Component/Mesh/MeshFilter.h"
#include "Runtime/Function/Framework/Component/Light/LightComponent.h"
#include "Runtime/Scripting/ScriptMaterial/ScriptMaterial.h"
#include "Runtime/Scripting/ScriptMesh/ScriptMesh.h"
#include "Runtime/Function/Framework/Component/Animation/AnimationComponent.h"
#include "Runtime/Resource/ResType/Data/AnimatorController.h"
#include "Runtime/Function/Framework/Component/MouseEventDispatcher.h"
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
#include "Runtime/Function/Framework/Component/Joint/JointComponent.h"
#include "Runtime/Function/Framework/Component/Joint/HingeJointComponent.h"
#include "Runtime/Function/Framework/Component/Joint/FixedJointComponent.h"
#include "Runtime/Function/Framework/Component/Joint/SpringJointComponent.h"
#include "Runtime/Function/Framework/Component/Joint/SliderJointComponent.h"
#include "Runtime/Function/Framework/Component/Joint/CharacterJointComponent.h"
#include "Runtime/Function/Framework/Component/Joint/ConfigurableJointComponent.h"
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
#include "Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h"
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
template<> void RegisterKlass<BaseRenderer>();
template<> void RegisterKlass<CameraComponent>();
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
// 关节族：`Cast<JointComponent>` 与四个子类的 `Produce` 都要它们的
// TypeContainer 已经填上（否则 TypeOf<T>() 解引用空类型）。
template<> void RegisterKlass<JointComponent>();
template<> void RegisterKlass<HingeJointComponent>();
template<> void RegisterKlass<FixedJointComponent>();
template<> void RegisterKlass<SpringJointComponent>();
template<> void RegisterKlass<SliderJointComponent>();
template<> void RegisterKlass<CharacterJointComponent>();
template<> void RegisterKlass<ConfigurableJointComponent>();
template<> void RegisterKlass<Collider>();
template<> void RegisterKlass<PlayerPrefs>();

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
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

    /// 把 `\r\n` / 裸 `\r` 统一成 `\n`。
    ///
    /// ⚠️ 这不是洁癖。仓库里两种行尾**同时存在**（`Component.h` 是 CRLF，
    /// `GameObject.cpp` 是 LF），而且 `git checkout` 会按 `core.autocrlf` 把
    /// 文件翻成 CRLF —— 同一个文件今天和明天行尾可能不一样。
    ///
    /// 而 `ReadFile` 是 `std::ios::binary` 打开的，原样读到 `\r\n`。于是任何
    /// 用 `find("\n}\n")` 定位"函数尾"的接线门，在 CRLF 文件上**一律落空**：
    /// `substr` 退化成"从锚点到文件末尾"。后果分两种，都是静默的：
    ///   * 正向门（"这段里应该有什么"）→ 什么都找得到 → **永真**；
    ///   * 反向门（"这段里不该有什么"）→ 永远找得到 → **永假**。
    /// 实测就踩到了：`Transform.cpp` 被 `git checkout` 翻成 CRLF 之后，CW11
    /// 变成永真、CW15 变成永假，而 1985 条里只有它俩的动静。
    ///
    /// 统一之后再切函数体，结果就只取决于源码内容，不取决于 git 怎么捡出它。
    std::string NormalizeNewlines(const std::string& text)
    {
        std::string out;
        out.reserve(text.size());
        for (const char c : text)
        {
            if (c != '\r')
            {
                out.push_back(c);
            }
        }
        return out;
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
        return NormalizeNewlines(ss.str());
    }

    /// 从 `at`（函数签名处）切到本函数的结尾。
    ///
    /// ⚠️ 找不到结尾时**故意**返回"从这里到文件末尾"，而不是空串 —— 反向门
    /// （"这段里不该有什么"）在空串上是永真的，那是所有假绿里最安静的一种。
    /// 退化成整段之后反向门会红，正向门则由调用方那条
    /// `BodyIsBounded` 断言兜住，两个方向都不会静默。
    /// 函数尾 = "与签名同一缩进的一个 `}`"。
    ///
    /// ⚠️ 不能写死 `"\n}\n"`：Runtime 那批文件函数顶格（`Transform.cpp` 的
    /// `void Transform::SetParent` 在第 0 列），而**本文件**的函数缩进 4 格
    /// （在匿名 namespace 里）。写死的话后者切出来的是"从这里到 namespace 结
    /// 尾"，CW21c / CW22 于是恒真。按签名行的前导空白推导，两种风格都对。
    std::string FunctionEndMarker(const std::string& text, std::size_t at)
    {
        std::size_t line_start = text.rfind('\n', at);
        line_start = (line_start == std::string::npos) ? 0 : line_start + 1;
        std::string indent;
        for (std::size_t i = line_start; i < text.size() && (text[i] == ' ' || text[i] == '\t'); ++i)
        {
            indent.push_back(text[i]);
        }
        return "\n" + indent + "}\n";
    }

    std::string SliceFunctionBody(const std::string& text, std::size_t at)
    {
        const std::size_t end = text.find(FunctionEndMarker(text, at), at);
        return (end == std::string::npos) ? text.substr(at) : text.substr(at, end - at);
    }

    /// `SliceFunctionBody` 切出来的真的是**一个函数**，不是"半个文件"。
    ///
    /// 行尾退化（CRLF 下 `"\n}\n"` 找不到）、函数名被改掉、或函数被搬到别处，
    /// 都会让上面的 `SliceFunctionBody` 静默切错 —— 这条就是那个"门本身的门"。
    bool BodyIsBounded(const std::string& text, std::size_t at)
    {
        const std::size_t end = text.find(FunctionEndMarker(text, at), at);
        return end != std::string::npos && end > at;
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

    /// 一块**质心不在原点**的盒子（形状整体偏离 body 原点 `in_shape_offset`）。
    ///
    /// 质心测试必须有这么一个夹具：质心与 body position 重合的对称形状下，
    /// "返回局部质心"和"返回零"相等、"世界质心"和"body position"也相等 ——
    /// 于是 `GetBodyCenterOfMass` 不管怎么写都是绿的（这是本轮差点留下的白测：
    /// 一开始只用 SpawnBox，RB25 断言的是"质心为零"，而返回零的实现也过）。
    uint32_t SpawnOffsetComBox(PhysicsScene& in_scene,
                               const Vector3& in_position,
                               const Vector3& in_half_extents,
                               const Vector3& in_shape_offset)
    {
        JPH::BodyInterface& body_interface = in_scene.GetJoltPhysicsSystem()->GetBodyInterface();

        JPH::BoxShapeSettings box_settings(toVec3(in_half_extents));
        box_settings.SetEmbedded();
        JPH::Shape::ShapeResult box_result = box_settings.Create();
        if (!box_result.IsValid())
        {
            return kInvalidPhysicsBodyId;
        }
        JPH::Ref<JPH::Shape> box = box_result.Get();

        // 直接用**具体形状**那个重载：另一个重载收 ShapeSettings，于是又多一层
        // 需要活到 CreateBody 之后的栈对象。
        JPH::RotatedTranslatedShapeSettings offset_settings(toVec3(in_shape_offset),
                                                            JPH::Quat::sIdentity(),
                                                            box.GetPtr());
        offset_settings.SetEmbedded();

        JPH::BodyCreationSettings body_settings(&offset_settings,
                                                JPH::RVec3(toVec3(in_position)),
                                                JPH::Quat::sIdentity(),
                                                JPH::EMotionType::Dynamic,
                                                static_cast<JPH::ObjectLayer>(0));

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

    bool Near(const Vector2& a, const Vector2& b, float tolerance = 1.0e-3f)
    {
        return std::fabs(a.x - b.x) < tolerance && std::fabs(a.y - b.y) < tolerance;
    }

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
        RegisterKlass<BaseRenderer>();
        // ⚠️ `MeshRenderer` / `MeshFilter` / `Mesh` 必须**逐个**注册：这张表是
        // "本测试要 Cast 到哪些类型"的清单，不是"引擎有哪些类型"（生产环境走
        // `RegisterRuntimeType()` 里的 `ExecuteAllRegistrations()`，全注册）。
        // 漏了 `MeshRenderer` 的话 `Cast<BaseRenderer>(mesh_renderer)` 恒为
        // nullptr —— 于是 MeshFilter 找不到同对象的渲染器，整套接线断言全线红，
        // 而错看起来像"组件没写回"。
        RegisterKlass<MeshRenderer>();
        RegisterKlass<MeshFilter>();
        RegisterKlass<Mesh>();
        RegisterKlass<CameraComponent>();
        RegisterKlass<Capsule>();
        RegisterKlass<Box>();
        RegisterKlass<Sphere>();
        RegisterKlass<Geometry>();
        RegisterKlass<RigidBodyShape>();
        RegisterKlass<RigidBodyComponent>();
        RegisterKlass<JointComponent>();
        RegisterKlass<HingeJointComponent>();
        RegisterKlass<FixedJointComponent>();
        RegisterKlass<SpringJointComponent>();
        RegisterKlass<SliderJointComponent>();
        RegisterKlass<CharacterJointComponent>();
        RegisterKlass<ConfigurableJointComponent>();
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

    // ---------------------------------------------------------------- RB ---
    // Unity 的 Rigidbody 剩余面：constraints / 质心 / 惯性 / CCD / 点速度 / 密度。
    //
    // 顺序是刻意的：**先**把"Unity 位 <-> Jolt 掩码"这个纯映射钉死（不起物理
    // 世界），**再**测它有没有真的被用上（真场景、真落体）。反过来做的话，映射
    // 写反一位的表现是"物体在某个轴上悄悄不动了"，而"冻住没生效"和"映射写反了"
    // 长得一模一样 —— 分不开。
    void TestRigidbodyExtras()
    {
        // --- 1. 纯映射（不需要物理世界）--------------------------------
        Check(ToJoltAllowedDOFs(RigidbodyConstraints::None) == 0x3fu, "RB0",
              "no constraints -> all six Jolt DOFs allowed");
        Check(ToJoltAllowedDOFs(RigidbodyConstraints::FreezeAll) == 0u, "RB1",
              "FreezeAll -> no DOF allowed");
        // Unity 的轴序（X=2 Y=4 Z=8 / rotX=16 rotY=32 rotZ=64）和 Jolt 的轴序
        // （Tx=1 Ty=2 Tz=4 / Rx=8 Ry=16 Rz=32）**不同**，所以只能逐位搬。这三个
        // 断言就是"三个位移位分别落到三个不同的 Jolt 位上"。
        Check(ToJoltAllowedDOFs(RigidbodyConstraints::FreezePositionX) == 0x3eu, "RB2",
              "FreezePositionX clears Jolt translation X (bit 0) and only that");
        Check(ToJoltAllowedDOFs(RigidbodyConstraints::FreezePositionZ) == 0x3bu, "RB3",
              "FreezePositionZ clears Jolt translation Z (bit 2) and only that");
        Check(ToJoltAllowedDOFs(RigidbodyConstraints::FreezeRotationZ) == 0x1fu, "RB4",
              "FreezeRotationZ clears Jolt rotation Z (bit 5) and only that");
        // 组合值：Unity 的 FreezePosition / FreezeRotation 是"三个位一起"，不是
        // 一个独立的枚举值。按值搬会漏掉组合。
        Check(ToJoltAllowedDOFs(RigidbodyConstraints::FreezePosition) == 0x38u, "RB5",
              "FreezePosition leaves exactly the three rotation DOFs");
        Check(ToJoltAllowedDOFs(RigidbodyConstraints::FreezeRotation) == 0x07u, "RB6",
              "FreezeRotation leaves exactly the three translation DOFs");
        // 逆映射。
        Check(ToRigidbodyConstraints(0x3fu) == RigidbodyConstraints::None, "RB7",
              "and back: all DOFs allowed reads as no constraint");
        Check(ToRigidbodyConstraints(0u) == RigidbodyConstraints::FreezeAll, "RB8",
              "no DOF allowed reads as FreezeAll");
        Check(ToRigidbodyConstraints(ToJoltAllowedDOFs(RigidbodyConstraints::FreezeRotationY)) ==
                  RigidbodyConstraints::FreezeRotationY,
              "RB9", "a single rotation bit round-trips");
        Check(ToRigidbodyConstraints(ToJoltAllowedDOFs(RigidbodyConstraints::FreezePositionY)) ==
                  RigidbodyConstraints::FreezePositionY,
              "RB10", "and so does a single translation bit (Y, not X)");
        // 未知位（Jolt 掩码里没出现的轴）必须翻译成"冻住"，不是"放开"。
        Check(ToRigidbodyConstraints(0x3fu & ~(1u << 3)) == RigidbodyConstraints::FreezeRotationX,
              "RB11", "a missing DOF bit reads as frozen, not free");
        // CCD 的四个模式里只有 Discrete 不连续。
        Check(!UsesContinuousCollision(CollisionDetectionMode::Discrete) &&
                  UsesContinuousCollision(CollisionDetectionMode::Continuous) &&
                  UsesContinuousCollision(CollisionDetectionMode::ContinuousDynamic) &&
                  UsesContinuousCollision(CollisionDetectionMode::ContinuousSpeculative),
              "RB12", "every mode but Discrete asks for continuous collision");

        // --- 2. 真场景：映射有没有真的被用上 ----------------------------
        PhysicsManager* physics_manager = GET_SYSTEM(PhysicsManager);
        Check(physics_manager != nullptr, "RB13", "PhysicsManager is reachable");
        if (physics_manager == nullptr)
        {
            return;
        }

        std::shared_ptr<PhysicsScene> scene = physics_manager->CreatePhysicsScene(Vector3(0.f, 0.f, -9.81f)).lock();
        Check(scene != nullptr, "RB14", "a physics scene can be created");
        if (!scene)
        {
            return;
        }

        const uint32_t faller = SpawnBox(*scene, Vector3(0.f, 0.f, 50.f), 0, true);
        Check(faller != kInvalidPhysicsBodyId, "RB15", "a dynamic body can be spawned");
        if (faller == kInvalidPhysicsBodyId)
        {
            return;
        }

        // 对照组：不冻任何轴，30 步之后它必须真的往下掉。少了这一条，"冻住 Z"的
        // 断言在一个**根本不动**的场景里也是绿的（重力没生效和冻结生效长得一样）。
        StepScene(*scene, 30);
        Vector3 fallen = Vector3::ZERO;
        Check(scene->GetRigidBodyPosition(faller, fallen) && fallen.z < 49.0f, "RB16",
              "an unconstrained body falls (the control for the freeze test)");

        // 冻住 Z 位移 —— 重力正好沿 -Z，所以这是"它掉不下去"。
        scene->SetRigidBodyPosition(faller, Vector3(0.f, 0.f, 50.f));
        scene->SetLinearVelocity(faller, Vector3::ZERO);
        scene->WakeRigidBody(faller);
        Check(scene->SetBodyAllowedDOFs(faller,
                                        ToJoltAllowedDOFs(RigidbodyConstraints::FreezePositionZ),
                                        12.0f),
              "RB17", "the constraint can be pushed onto a live body");
        uint32_t read_mask = 0xffu;
        Check(scene->GetBodyAllowedDOFs(faller, read_mask) && read_mask == 0x3bu, "RB18",
              "and the body reports exactly the mask that was asked for");
        StepScene(*scene, 30);
        Vector3 frozen = Vector3::ZERO;
        Check(scene->GetRigidBodyPosition(faller, frozen) && Near(frozen.z, 50.0f, 0.01f), "RB19",
              "freezing Z actually stops the fall (not just a stored flag)");
        Vector3 frozen_velocity = Vector3::ZERO;
        Check(scene->GetLinearVelocity(faller, frozen_velocity) && Near(frozen_velocity.z, 0.0f, 1.0e-4f),
              "RB20", "and the solver never gives it a Z velocity at all");

        // 旋转同理：冻住 Rz 之后同样的力矩不再让它转。
        scene->SetBodyAllowedDOFs(faller, ToJoltAllowedDOFs(RigidbodyConstraints::FreezeRotationZ), 12.0f);
        scene->SetAngularVelocity(faller, Vector3::ZERO);
        scene->WakeRigidBody(faller);
        scene->AddTorque(faller, Vector3(0.f, 0.f, 500.f));
        scene->Tick(1.0f / 60.0f);
        Vector3 spin_frozen = Vector3::ZERO;
        Check(scene->GetAngularVelocity(faller, spin_frozen) && Near(spin_frozen.z, 0.0f, 1.0e-4f),
              "RB21", "freezing rotation Z makes the same torque produce no spin");

        scene->SetBodyAllowedDOFs(faller, ToJoltAllowedDOFs(RigidbodyConstraints::None), 12.0f);
        scene->SetAngularVelocity(faller, Vector3::ZERO);
        scene->WakeRigidBody(faller);
        scene->AddTorque(faller, Vector3(0.f, 0.f, 500.f));
        scene->Tick(1.0f / 60.0f);
        Vector3 spin_free = Vector3::ZERO;
        Check(scene->GetAngularVelocity(faller, spin_free) && std::fabs(spin_free.z) > 0.01f, "RB22",
              "unfreezing it (the control) makes the same torque spin it");

        // --- 改质量不许清掉约束 ----------------------------------------
        // `SetMassProperties` 把掩码和质量**一起**收，写死 `EAllowedDOFs::All`
        // 就是"改一次质量，constraints 悄悄没了"。表现是"设了 freezeRotation，
        // 一改质量就又开始转"。
        scene->SetBodyAllowedDOFs(faller, ToJoltAllowedDOFs(RigidbodyConstraints::FreezeRotation), 12.0f);
        Check(scene->SetMass(faller, 30.0f), "RB23", "the mass can be changed on a constrained body");
        Check(scene->GetBodyAllowedDOFs(faller, read_mask) && read_mask == 0x07u, "RB24",
              "changing the mass keeps the constraint (it does not reset to All)");
        scene->SetBodyAllowedDOFs(faller, ToJoltAllowedDOFs(RigidbodyConstraints::None), 12.0f);

        // --- 质心 / 惯性 / 体积 ----------------------------------------
        // 对称盒子的局部质心是 0，所以仅用它测不出"返回质心"和"返回零"的差别
        // —— 这里换成一块形状偏离原点的盒子，局部质心必须是那个**非零**偏移。
        const Vector3 kShapeOffset(0.f, 0.25f, 0.f);
        const uint32_t offset_body =
            SpawnOffsetComBox(*scene, Vector3(0.f, 60.f, 0.f), Vector3(0.5f, 0.25f, 0.5f), kShapeOffset);
        Check(offset_body != kInvalidPhysicsBodyId, "RB25", "a body whose shape is off-centre can be spawned");
        Vector3 local_com = Vector3::UNIT_X;
        Check(scene->GetBodyCenterOfMass(offset_body, local_com) &&
                  Near(local_com, kShapeOffset, 1.0e-4f),
              "RB26", "the local centre of mass is the shape offset (not zero)");
        Vector3 world_com = Vector3::ZERO;
        Vector3 offset_origin = Vector3::ZERO;
        Check(scene->GetBodyWorldCenterOfMass(offset_body, world_com) &&
                  scene->GetRigidBodyPosition(offset_body, offset_origin) &&
                  Near(world_com, offset_origin + kShapeOffset, 1.0e-4f),
              "RB29", "the world centre of mass is the body ORIGIN plus the local COM");
        // ⚠️ 这条是"别把两个量搞混"的钉子：Jolt 的 `Body::GetPosition()` 是物体
        // 原点（`mPosition - rotation * localCOM`），质心是 `mPosition`。形状质心
        // 不在原点时两者差 0.25 —— 用 `GetRigidBodyPosition` 顶替质心的实现会在这
        // 里偏出去，而对称盒子上完全看不出来。
        Check(Near(world_com.y, 60.25f, 0.01f) && Near(offset_origin.y, 60.0f, 0.01f), "RB30",
              "so worldCenterOfMass != position here (60.25 vs 60.00 on Y)");

        Vector3 inertia_12 = Vector3::ZERO;
        Quaternion inertia_rotation = Quaternion(99.f, 99.f, 99.f, 99.f);
        scene->SetMass(faller, 12.0f);
        Check(scene->GetBodyInertiaTensor(faller, inertia_12, inertia_rotation) &&
                  inertia_12.x > 0.f && inertia_12.y > 0.f && inertia_12.z > 0.f,
              "RB29", "a dynamic body reports a positive inertia on every axis");
        // 惯性正比于质量 —— 判据不依赖任何解析常数。惯性没跟着质量走的话两者相等。
        Vector3 inertia_24 = Vector3::ZERO;
        scene->SetMass(faller, 24.0f);
        Check(scene->GetBodyInertiaTensor(faller, inertia_24, inertia_rotation) &&
                  Near(inertia_24.x, 2.0f * inertia_12.x, 0.02f * inertia_12.x),
              "RB30", "doubling the mass doubles the inertia (SetMass scales it too)");
        Check(Near(inertia_rotation.length(), 1.0f, 1.0e-3f), "RB31",
              "the inertia rotation is a unit quaternion");

        float volume = 0.f;
        Check(scene->GetBodyVolume(faller, volume) && Near(volume, 1.0f, 0.01f), "RB32",
              "a 1x1x1 box has a volume of 1 m3 (that is what density multiplies)");
        const uint32_t bigger = SpawnBoxSized(*scene, Vector3(0.f, 20.f, 0.f), Vector3(1.f, 1.f, 1.f), 0, true);
        float bigger_volume = 0.f;
        Check(bigger != kInvalidPhysicsBodyId && scene->GetBodyVolume(bigger, bigger_volume) &&
                  Near(bigger_volume, 8.0f, 0.01f),
              "RB33", "and a 2x2x2 box has 8 (the volume really comes from the shape)");

        // --- CCD --------------------------------------------------------
        CollisionDetectionMode mode = CollisionDetectionMode::ContinuousSpeculative;
        Check(scene->GetBodyMotionQuality(faller, mode) && mode == CollisionDetectionMode::Discrete,
              "RB34", "a new body is Discrete (continuous is opt-in and costs a sweep)");
        Check(scene->SetBodyMotionQuality(faller, CollisionDetectionMode::ContinuousDynamic) &&
                  scene->GetBodyMotionQuality(faller, mode) && mode == CollisionDetectionMode::Continuous,
              "RB35", "ContinuousDynamic is accepted and reads back as Continuous");
        Check(scene->SetBodyMotionQuality(faller, CollisionDetectionMode::Discrete) &&
                  scene->GetBodyMotionQuality(faller, mode) && mode == CollisionDetectionMode::Discrete,
              "RB36", "and it can be turned back off");

        // --- 角速度上限 ---------------------------------------------------
        float default_max_angular = -1.f;
        Check(scene->GetBodyMaxAngularVelocity(faller, default_max_angular) && default_max_angular > 0.f,
              "RB37", "a body has a positive default angular velocity cap");
        Check(scene->SetBodyMaxAngularVelocity(faller, 2.0f) &&
                  scene->GetBodyMaxAngularVelocity(faller, default_max_angular) &&
                  Near(default_max_angular, 2.0f, 1.0e-4f),
              "RB38", "maxAngularVelocity round-trips");
        // 行为级：上限要真的挡住。只测"读回来对不对"的话，一个把值存进字段却不
        // 交给求解器的实现是全绿的。
        scene->SetAngularVelocity(faller, Vector3::ZERO);
        scene->SetBodyMaxAngularVelocity(faller, 1.0f);
        scene->AddAngularImpulse(faller, Vector3(0.f, 0.f, 1000.f));
        scene->Tick(1.0f / 60.0f);
        Vector3 capped_spin = Vector3::ZERO;
        Check(scene->GetAngularVelocity(faller, capped_spin) && std::fabs(capped_spin.z) <= 1.05f,
              "RB39", "with the cap at 1 rad/s a huge impulse still yields at most ~1 rad/s");
        scene->SetAngularVelocity(faller, Vector3::ZERO);
        scene->SetBodyMaxAngularVelocity(faller, 500.0f);
        scene->AddAngularImpulse(faller, Vector3(0.f, 0.f, 1000.f));
        scene->Tick(1.0f / 60.0f);
        Vector3 uncapped_spin = Vector3::ZERO;
        Check(scene->GetAngularVelocity(faller, uncapped_spin) && std::fabs(uncapped_spin.z) > 2.0f,
              "RB40", "raising the cap (the control) lets the same impulse spin it much faster");

        // --- 点速度 ------------------------------------------------------
        // 线速度为 0、绕 Z 以 5 rad/s 自转的刚体：X 方向偏 1 米的那个点，速度是
        // ω × r = (0, 5, 0)。这正是 `GetPointVelocity` 相对 `velocity` 多出来的
        // 那一半（"转着的杆子端点比质心快"）。
        scene->SetLinearVelocity(faller, Vector3::ZERO);
        scene->SetAngularVelocity(faller, Vector3(0.f, 0.f, 5.f));
        // ⚠️ 用**这个**刚体自己的质心，不要用上面那段读出来的 offset_origin ——
        // 那是另一个物体的坐标，拿它当参考点会得到一个"看起来有旋转分量但方向
        // 不对"的速度，而方向恰恰是这条断言要看的东西。
        Vector3 spin_com = Vector3::ZERO;
        Check(scene->GetBodyWorldCenterOfMass(faller, spin_com), "RB41a", "the spinner's COM is readable");
        Vector3 point_velocity = Vector3::ZERO;
        Check(scene->GetBodyPointVelocity(faller, spin_com + Vector3(1.f, 0.f, 0.f), point_velocity) &&
                  Near(point_velocity.y, 5.0f, 0.05f) && Near(point_velocity.x, 0.0f, 0.05f),
              "RB41", "a point 1 m out on X moves at omega x r = (0, 5, 0), not at the COM velocity");
        Vector3 com_velocity = Vector3::UNIT_X;
        Check(scene->GetBodyPointVelocity(faller, spin_com, com_velocity) &&
                  Near(com_velocity, Vector3::ZERO, 0.05f),
              "RB42", "the centre of mass itself moves at the linear velocity (zero here)");
        // 有平移时两者叠加，不能只算旋转分量。
        scene->SetLinearVelocity(faller, Vector3(2.f, 0.f, 0.f));
        Check(scene->GetBodyPointVelocity(faller, spin_com, com_velocity) &&
                  Near(com_velocity.x, 2.0f, 0.05f),
              "RB43", "with a linear velocity the COM point reports it (rotation is added, not replaced)");

        // --- 无效 body id 一律拒绝 ---------------------------------------
        uint32_t ignored_mask = 0;
        float ignored_float = 0.f;
        Vector3 ignored_vec = Vector3::ZERO;
        Quaternion ignored_quat = Quaternion::IDENTITY;
        CollisionDetectionMode ignored_mode = CollisionDetectionMode::Continuous;
        Check(!scene->GetBodyAllowedDOFs(kInvalidPhysicsBodyId, ignored_mask) &&
                  !scene->SetBodyAllowedDOFs(kInvalidPhysicsBodyId, 0x3fu, 1.0f) &&
                  !scene->GetBodyCenterOfMass(kInvalidPhysicsBodyId, ignored_vec) &&
                  !scene->GetBodyWorldCenterOfMass(kInvalidPhysicsBodyId, ignored_vec) &&
                  !scene->GetBodyInertiaTensor(kInvalidPhysicsBodyId, ignored_vec, ignored_quat) &&
                  !scene->GetBodyVolume(kInvalidPhysicsBodyId, ignored_float) &&
                  !scene->GetBodyMotionQuality(kInvalidPhysicsBodyId, ignored_mode) &&
                  !scene->SetBodyMotionQuality(kInvalidPhysicsBodyId, CollisionDetectionMode::Discrete) &&
                  !scene->GetBodyMaxAngularVelocity(kInvalidPhysicsBodyId, ignored_float) &&
                  !scene->SetBodyMaxAngularVelocity(kInvalidPhysicsBodyId, 1.0f) &&
                  !scene->GetBodyPointVelocity(kInvalidPhysicsBodyId, Vector3::ZERO, ignored_vec),
              "RB44", "every new call rejects an invalid body id instead of crashing");
        // 静态刚体没有 MotionProperties，也就没有"允许哪些轴"这回事。
        const uint32_t static_body = SpawnBox(*scene, Vector3(0.f, 40.f, 0.f), 0, false);
        Check(static_body != kInvalidPhysicsBodyId && !scene->GetBodyAllowedDOFs(static_body, ignored_mask) &&
                  !scene->SetBodyAllowedDOFs(static_body, 0x3fu, 1.0f) &&
                  !scene->GetBodyMaxAngularVelocity(static_body, ignored_float),
              "RB45", "a static body has no DOF mask and no angular velocity cap");

        // --- 3. 组件侧：没有活刚体时的降级 ------------------------------
        RigidBodyComponent rigidbody;
        Check(!rigidbody.HasBody(), "RB46", "(the component fixture has no body)");
        Check(rigidbody.GetConstraints() == RigidbodyConstraints::None, "RB47",
              "constraints read None before anything is set");
        Check(!rigidbody.GetFreezeRotation(), "RB48", "freezeRotation reads false");

        // 没有活体时读回**脚本设过的值**而不是 None：否则"设了没生效"和"从没设
        // 过"在读回来时完全一样。
        rigidbody.SetConstraints(RigidbodyConstraints::FreezeRotation);
        Check(rigidbody.GetConstraints() == RigidbodyConstraints::FreezeRotation, "RB49",
              "a constraint set with no body is still readable (it is not silently dropped)");
        Check(rigidbody.GetFreezeRotation(), "RB50", "and freezeRotation reads true from it");

        // freezeRotation 只动旋转位，位移位必须原样保留。
        rigidbody.SetConstraints(RigidbodyConstraints::FreezePositionZ);
        Check(!rigidbody.GetFreezeRotation(), "RB51", "freezing only Z translation does not read as freezeRotation");
        rigidbody.SetFreezeRotation(true);
        Check(rigidbody.GetFreezeRotation(), "RB52", "SetFreezeRotation(true) freezes all three rotations");
        Check((static_cast<int>(rigidbody.GetConstraints()) &
               static_cast<int>(RigidbodyConstraints::FreezePositionZ)) != 0,
              "RB53", "and it KEEPS the position freeze that was set before (it does not overwrite)");
        // 三个旋转位必须一起动：只冻一个轴的"freezeRotation"会让"物体绕另外两个
        // 轴还在翻"这种事发生。
        Check((static_cast<int>(rigidbody.GetConstraints()) &
               static_cast<int>(RigidbodyConstraints::FreezeRotation)) ==
                  static_cast<int>(RigidbodyConstraints::FreezeRotation),
              "RB54", "freezeRotation means all three rotation bits, not just one");
        rigidbody.SetFreezeRotation(false);
        Check(!rigidbody.GetFreezeRotation(), "RB55", "and it can be turned back off");
        Check((static_cast<int>(rigidbody.GetConstraints()) &
               static_cast<int>(RigidbodyConstraints::FreezeRotation)) == 0,
              "RB56", "unfreezing clears all three bits");

        // 质心 / 惯性：没有刚体时给"零"而不是 NaN。
        // ⚠️ 没有 `Near(Quaternion, Quaternion)` 重载（有 Matrix4x4 的），所以逐
        // 分量比。顺带也把"单位四元数"这件事一起钉了：一个没归一化的旋转会让
        // 惯性张量整体缩错。
        const Quaternion no_body_inertia_rotation = rigidbody.GetInertiaTensorRotation();
        Check(Near(rigidbody.GetCenterOfMass(), Vector3::ZERO, 1.0e-6f) &&
                  Near(rigidbody.GetWorldCenterOfMass(), Vector3::ZERO, 1.0e-6f) &&
                  Near(rigidbody.GetInertiaTensor(), Vector3::ZERO, 1.0e-6f) &&
                  Near(no_body_inertia_rotation.x, 0.0f, 1.0e-6f) &&
                  Near(no_body_inertia_rotation.y, 0.0f, 1.0e-6f) &&
                  Near(no_body_inertia_rotation.z, 0.0f, 1.0e-6f) &&
                  Near(no_body_inertia_rotation.w, 1.0f, 1.0e-6f),
              "RB57", "the centre of mass and the inertia read zero / identity, not NaN");

        // CCD：没有活体时读回作者值，**保留全部四个模式**（Jolt 那个两态的窄化
        // 只发生在真的落进 Jolt 之后）。
        Check(rigidbody.GetCollisionDetectionMode() == CollisionDetectionMode::Discrete, "RB58",
              "collisionDetectionMode defaults to Discrete");
        rigidbody.SetCollisionDetectionMode(CollisionDetectionMode::ContinuousDynamic);
        Check(rigidbody.GetCollisionDetectionMode() == CollisionDetectionMode::ContinuousDynamic, "RB59",
              "and round-trips at full fidelity while there is no body");

        Check(Near(rigidbody.GetMaxAngularVelocity(), 0.0f), "RB60", "maxAngularVelocity reads 0 with no body");
        Check(Near(rigidbody.GetPointVelocity(Vector3::UNIT_X), Vector3::ZERO, 1.0e-6f) &&
                  Near(rigidbody.GetRelativePointVelocity(Vector3::UNIT_X), Vector3::ZERO, 1.0e-6f),
              "RB61", "both point-velocity queries read zero with no body");
        // 全部写操作必须是安全的空操作。
        rigidbody.AddRelativeForce(Vector3::UNIT_X);
        rigidbody.AddRelativeTorque(Vector3::UNIT_Z);
        rigidbody.SetDensity(2.0f);
        Check(Near(rigidbody.GetDensity(), 0.0f), "RB62",
              "density reads 0 with no body (and every setter above was a safe no-op)");

        // --- 4. 接线门 ----------------------------------------------------
        const std::string rb_header =
            ReadFile("Source/Runtime/Function/Framework/Component/Rigidbody/RigidbodyComponent.h");
        Check(!rb_header.empty(), "RB63", "RigidbodyComponent.h is readable");
        Check(CountOccurrences(rb_header, "GetConstraints() const") >= 1 &&
                  CountOccurrences(rb_header, "SetConstraints(RigidbodyConstraints") >= 1 &&
                  CountOccurrences(rb_header, "GetFreezeRotation() const") >= 1 &&
                  CountOccurrences(rb_header, "GetCenterOfMass() const") >= 1 &&
                  CountOccurrences(rb_header, "GetInertiaTensor() const") >= 1 &&
                  CountOccurrences(rb_header, "GetCollisionDetectionMode() const") >= 1 &&
                  CountOccurrences(rb_header, "GetMaxAngularVelocity() const") >= 1 &&
                  CountOccurrences(rb_header, "GetPointVelocity(const Vector3&") >= 1 &&
                  CountOccurrences(rb_header, "AddRelativeForce(const Vector3&") >= 1 &&
                  CountOccurrences(rb_header, "SetDensity(float density)") >= 1,
              "RB64", "every Unity-named member is declared on the component");
        Check(CountOccurrences(rb_header, "Name=\"constraints\"") >= 1 &&
                  CountOccurrences(rb_header, "Name=\"freezeRotation\"") >= 1 &&
                  CountOccurrences(rb_header, "Name=\"centerOfMass\"") >= 1 &&
                  CountOccurrences(rb_header, "Name=\"collisionDetectionMode\"") >= 1 &&
                  CountOccurrences(rb_header, "Name=\"maxAngularVelocity\"") >= 1,
              "RB65", "and every one of them is published under its Unity name");

        const std::string rb_source =
            ReadFile("Source/Runtime/Function/Framework/Component/Rigidbody/RigidbodyComponent.cpp");
        Check(!rb_source.empty(), "RB66", "RigidbodyComponent.cpp is readable");
        // 接线门：声明了不算实现。SetConstraints 必须真的走到场景，且走的是
        // ToJoltAllowedDOFs —— 直接把 Unity 的值当 Jolt 掩码传下去能编译、能
        // 运行，然后在完全不相干的轴上冻住东西。
        Check(CountOccurrences(rb_source, "SetBodyAllowedDOFs(") >= 1, "RB67",
              "SetConstraints reaches the physics scene");
        Check(CountOccurrences(rb_source, "ToJoltAllowedDOFs(constraints)") >= 1, "RB68",
              "and converts Unity's bits into Jolt's mask (they are not the same bits)");
        Check(CountOccurrences(rb_source, "GetBodyMotionQuality(") >= 1 &&
                  CountOccurrences(rb_source, "SetBodyMotionQuality(") >= 1, "RB69",
              "the CCD mode is read from and written to the live body");
        Check(CountOccurrences(rb_source, "GetBodyPointVelocity(") >= 1, "RB70",
              "GetPointVelocity asks the scene, not a local field");

        // 改质量不许清掉约束（RB24 是行为级，这条保证修法留在源码里）。
        const std::string scene_source = ReadFile("Source/Runtime/Function/Physics/PhysicsScene.cpp");
        Check(!scene_source.empty(), "RB71", "PhysicsScene.cpp is readable");
        Check(CountOccurrences(scene_source, "GetAllowedDOFs()") >= 1, "RB72",
              "SetMass passes the CURRENT DOF mask through (not EAllowedDOFs::All)");

        // body 重建之后约束要补回去：Awake 里设的约束落在 body 还存在之前，不补
        // 就是"关卡一加载约束就没了"。
        Check(CountOccurrences(rb_source, "ApplyRuntimeBodySettings()") >= 4, "RB73",
              "the constraint is re-applied on every path that creates a body");

        const std::string binding_source = ReadFile("Source/Runtime/Scripting/Binding/ScriptBinding.cpp");
        Check(CountOccurrences(binding_source, "declare type Rigidbody = RigidBodyComponent;") >= 1, "RB74",
              "scripts can spell the type the Unity way");

        const std::string null_source = ReadFile("Source/Runtime/Function/Physics/PhysicsSceneNull.cpp");
        // ⚠️ 每个锚点都带上**参数表**。只写函数名的门是漏的：把符号改名成
        // `GetBodyPointVelocityRenamed` 之后，`PhysicsScene::GetBodyPointVelocity`
        // 这个子串**还在**，门照样绿 —— 而那个构建里符号就是真的没了
        // （-undefined dynamic_lookup ⇒ 第一次调用 EXC_BAD_ACCESS）。
        Check(CountOccurrences(null_source, "PhysicsScene::SetBodyAllowedDOFs(uint32_t, uint32_t, float)") >= 1 &&
                  CountOccurrences(null_source,
                                   "PhysicsScene::GetBodyInertiaTensor(uint32_t, Vector3& out_inertia, "
                                   "Quaternion& out_rotation) const") >= 1 &&
                  CountOccurrences(null_source,
                                   "PhysicsScene::GetBodyPointVelocity(uint32_t, const Vector3&, "
                                   "Vector3& out_velocity) const") >= 1,
              "RB75", "the no-physics build defines the new symbols too (a missing one is EXC_BAD_ACCESS)");

        // 相对力 / 相对力矩：不加旋转的话，`AddRelativeForce` 与 `AddForce` 是同一
        // 个函数 —— 而"车头前方"和"世界前方"在车转过之后是两回事。这条在行为层
        // 测不到（组件夹具没有活刚体），所以钉在源码上。
        Check(CountOccurrences(rb_source, "GetRotation() * force") >= 1 &&
                  CountOccurrences(rb_source, "GetRotation() * torque") >= 1,
              "RB76", "AddRelativeForce/Torque rotate by the body's own orientation");
        // 密度必须乘**体积**：写死 1.0 的话 `SetDensity(2)` 在一个 8 m3 的物体上
        // 给出 2kg 而不是 16kg，而"质量变了"在任何断言里都是真的。
        Check(CountOccurrences(rb_source, "density * volume") >= 1 &&
                  CountOccurrences(rb_source, "GetMass() / volume") >= 1,
              "RB77", "density is mass per volume, not mass per unit");
        // 相对点速度的等价定义：局部点 -> 世界点 用**物体原点**（transform 位置）
        // 而不是质心。Unity 的 `relativePoint` 是刚体局部空间里的点，局部空间的
        // 原点是 transform —— 写成质心会在复合碰撞体上整体偏一个 localCOM。
        Check(CountOccurrences(rb_source, "GetPosition() + GetRotation() * local_point") >= 1,
              "RB78", "GetRelativePointVelocity measures from the body origin, not the centre of mass");
    }

    /// 一个"能挂关节"的演员：GameObject + Transform + 带盒子形状的刚体。
    ///
    /// 不走 `AddComponentByTypeName` 是因为它**先** PostLoadResource **再**返回，
    /// 而刚体的 res（形状 / 类型）必须在那一刻之前就填好 —— 否则第一次建体会
    /// 因为没有形状而失败，且 `m_HasPendingBody` 已经是 false，再也补不回来。
    struct JointActor
    {
        GameObject* object {nullptr};
        Transform* transform {nullptr};
        RigidBodyComponent* body {nullptr};
    };

    JointActor SpawnJointActor(ObjectManager& om, const Vector3& position)
    {
        JointActor actor;

        actor.object = new GameObject();
        om.AllocateAndAssignInstanceID(actor.object);

        actor.transform = new Transform();
        om.AllocateAndAssignInstanceID(actor.transform);
        actor.object->addComponent(actor.transform);
        actor.transform->PostLoadResource(actor.object);
        actor.transform->SetPosition(position);

        Box* box = new Box();
        om.AllocateAndAssignInstanceID(box);
        RigidBodyShape* shape = new RigidBodyShape();
        om.AllocateAndAssignInstanceID(shape);
        shape->m_Type = RigidBodyShapeType::box;
        shape->m_Geometry = box;

        actor.body = new RigidBodyComponent();
        om.AllocateAndAssignInstanceID(actor.body);
        actor.body->GetRigidbodyResMutable().m_Shapes.push_back(shape);
        actor.body->GetRigidbodyResMutable().SetActorType(RigidBodyActorType::dynamic);
        actor.object->addComponent(actor.body);
        actor.body->PostLoadResource(actor.object);

        return actor;
    }

    void TestJointComponents()
    {
        // 与 `TestPhysicsQueries` 那组同一个场景：组件层走 `Physics::CurrentScene()`
        //（`PhysicsManager` 的第一张场景），所以这里也必须查同一个 —— 各用一张
        // 场景的话，"组件报告建好了"和"后端真有一个约束"会落在两个世界里。
        std::shared_ptr<PhysicsScene> scene = Physics::CurrentScene();
        Check(scene != nullptr, "JN0", "the joint fixture starts from the live physics scene");
        if (!scene)
        {
            return;
        }
        // 重力固定成 -Z：本组有"关节在扛着重力"的判断，而这张场景是前面某组
        // 建的，重力未知。
        scene->setGravity(Vector3(0.f, 0.f, -9.81f));

        // =====================================================================
        // 1. PhysicsScene 的关节运行时控制（真场景，直投 Jolt 的刚体）
        // =====================================================================

        const uint32_t hinge_a = SpawnBox(*scene, Vector3(0.f, 0.f, 0.f), 0, true);
        const uint32_t hinge_b = SpawnBox(*scene, Vector3(0.f, 0.f, 2.f), 0, true);
        Check(hinge_a != kInvalidPhysicsBodyId && hinge_b != kInvalidPhysicsBodyId,
              "JN1", "two bodies for the hinge");

        PhysicsJointDesc hinge_desc;
        hinge_desc.m_Type = PhysicsJointType::hinge;
        hinge_desc.m_BodyA = hinge_a;
        hinge_desc.m_BodyB = hinge_b;
        hinge_desc.m_PivotA = Vector3(0.f, 0.f, 1.f);
        hinge_desc.m_PivotB = Vector3(0.f, 0.f, -1.f);
        hinge_desc.m_AxisA = Vector3(0.f, 0.f, 1.f);
        hinge_desc.m_AxisB = Vector3(0.f, 0.f, 1.f);
        hinge_desc.m_AutoDetectPoint = false;
        const uint32_t hinge_id = scene->CreateJoint(hinge_desc);
        Check(hinge_id != s_InvalidJointId, "JN2", "a hinge joint can be created");

        PhysicsJointType read_type = PhysicsJointType::fixed;
        Check(scene->GetJointType(hinge_id, read_type) && read_type == PhysicsJointType::hinge, "JN3",
              "and it reports itself as a hinge (asked of Jolt, not of a side table)");
        Check(!scene->GetJointType(9999u, read_type), "JN4", "an unknown joint id reports no type");

        bool enabled = false;
        Check(scene->IsJointEnabled(hinge_id, enabled) && enabled, "JN5", "a new joint is enabled");
        Check(scene->SetJointEnabled(hinge_id, false) && scene->IsJointEnabled(hinge_id, enabled) && !enabled,
              "JN6", "SetJointEnabled(false) reads back as disabled");
        Check(scene->SetJointEnabled(hinge_id, true) && scene->IsJointEnabled(hinge_id, enabled) && enabled,
              "JN7", "and it can be switched back on");
        Check(!scene->SetJointEnabled(9999u, true), "JN8", "enabling an unknown joint is refused");

        float angle = 0.f;
        float angular_velocity = 0.f;
        Check(scene->GetJointAngle(hinge_id, angle), "JN9", "the hinge angle can be read");
        Check(scene->GetJointVelocity(hinge_id, angular_velocity), "JN10", "and so can its angular velocity");

        // 对照组：这两个方法在一个**不是**铰链的约束上必须失败。少了这一条，
        // "读角度"实现成 `out = 0; return true;` 也是绿的。
        PhysicsJointDesc fixed_desc = hinge_desc;
        fixed_desc.m_Type = PhysicsJointType::fixed;
        const uint32_t fixed_id = scene->CreateJoint(fixed_desc);
        Check(fixed_id != s_InvalidJointId, "JN11", "a fixed joint can be created");
        Check(!scene->GetJointAngle(fixed_id, angle), "JN12", "GetJointAngle refuses a joint that is not a hinge");
        Check(!scene->GetJointVelocity(fixed_id, angular_velocity), "JN13", "and so does GetJointVelocity");

        float min_limit = 0.f;
        float max_limit = 0.f;
        Check(scene->SetJointLimits(hinge_id, -1.f, 0.5f), "JN14", "hinge limits can be set");
        Check(scene->GetJointLimits(hinge_id, min_limit, max_limit) && Near(min_limit, -1.f) &&
                  Near(max_limit, 0.5f),
              "JN15", "and they read back unchanged");
        Check(!scene->SetJointLimits(hinge_id, 1.f, -1.f), "JN16", "min > max is refused, not silently swapped");
        Check(!scene->GetJointLimits(fixed_id, min_limit, max_limit), "JN17", "a fixed joint has no limits to read");

        // 速度电机与位置电机是**两个不同的目标量**：一个是角速度、一个是角度。
        Check(scene->SetJointMotor(hinge_id, true, 2.f, 10.f), "JN18", "the velocity motor can be driven");
        // 光读到 `true` 不算数：`out = 0; return true;` 也是绿的。判"读到的是真的
        // 相对角速度"最稳的办法是直接给 B 一个已知的绕轴角速度 —— 至于电机能不能
        // 推得动，那是另一回事，下面单独判。
        Check(scene->SetAngularVelocity(hinge_b, Vector3(0.f, 0.f, 3.f)), "JN18a",
              "the backend can set an angular velocity (the hinge axis is +Z here)");
        Check(scene->GetJointVelocity(hinge_id, angular_velocity) &&
                  std::fabs(std::fabs(angular_velocity) - 3.f) < 0.5f,
              "JN18b", "and the joint reports that REAL relative spin (JN10 alone would pass with 0)");
        scene->SetAngularVelocity(hinge_b, Vector3::ZERO);

        // 电机本身：A 静态、B 动态挂在铰链上，给一个够大的力矩上限（默认密度
        // 1000kg/m³ ⇒ 这个 1m 的箱子 1000kg、绕轴惯量约 167kg·m²，10N·m 推 5 步
        // 只攒得出 ~0.005rad/s —— 那种失败是"数太小"，不是"没转"，诊断会带偏）。
        const uint32_t motor_a = SpawnBox(*scene, Vector3(0.f, 0.f, 10.f), 0, false);
        const uint32_t motor_b = SpawnBox(*scene, Vector3(0.f, 0.f, 12.f), 0, true);
        PhysicsJointDesc motor_desc = hinge_desc;
        motor_desc.m_BodyA = motor_a;
        motor_desc.m_BodyB = motor_b;
        motor_desc.m_PivotA = Vector3(0.f, 0.f, 1.f);
        motor_desc.m_PivotB = Vector3(0.f, 0.f, -1.f);
        const uint32_t motor_hinge = scene->CreateJoint(motor_desc);
        Check(motor_hinge != s_InvalidJointId && scene->SetJointMotor(motor_hinge, true, 2.f, 1.0e5f),
              "JN18c", "a hinge motor against a STATIC body can be set up");
        StepScene(*scene, 10);
        float motor_spin = 0.f;
        Check(scene->GetJointVelocity(motor_hinge, motor_spin) && std::fabs(motor_spin) > 0.1f, "JN18d",
              "and it really spins the dynamic body up (the motor is not decorative)");
        scene->SetJointMotor(hinge_id, false, 0.f, 0.f);
        Check(scene->SetJointPositionMotor(hinge_id, true, 0.25f, 10.f, 2.f, 0.5f), "JN19",
              "and so can the position motor");
        Check(!scene->SetJointPositionMotor(hinge_id, true, 0.f, 10.f, 0.f, 0.f), "JN20",
              "a position motor with zero frequency is refused (MotorSettings would be invalid)");
        Check(!scene->SetJointMotor(hinge_id, true, 0.f, -1.f), "JN21", "a negative max force is refused");
        Check(!scene->SetJointMotor(fixed_id, true, 0.f, 1.f), "JN22", "a fixed joint has no motor");
        Check(scene->SetJointSpring(hinge_id, 2.f, 0.5f), "JN23", "the hinge spring can be retuned");

        // 冲量的对照组：**静态** + **动态**焊在一起，重力一直拉，所以约束每步
        // 都要使力。两个都自由落体的话冲量恒为 0，那条断言在"永远返回 0"的实现
        // 上也是绿的。
        const uint32_t held_static = SpawnBox(*scene, Vector3(10.f, 0.f, 0.f), 0, false);
        const uint32_t held_dynamic = SpawnBox(*scene, Vector3(10.f, 0.f, 2.f), 0, true);
        PhysicsJointDesc held_desc;
        held_desc.m_Type = PhysicsJointType::fixed;
        held_desc.m_BodyA = held_static;
        held_desc.m_BodyB = held_dynamic;
        const uint32_t held_id = scene->CreateJoint(held_desc);
        Check(held_id != s_InvalidJointId, "JN24", "a fixed joint between a static and a dynamic body");
        StepScene(*scene, 10);
        float linear_impulse = 0.f;
        float angular_impulse = 0.f;
        Check(scene->GetJointImpulse(held_id, linear_impulse, angular_impulse) && linear_impulse > 0.f, "JN25",
              "a joint holding a body against gravity reports a NON-ZERO impulse");
        Check(!scene->GetJointImpulse(9999u, linear_impulse, angular_impulse), "JN26",
              "an unknown joint has no impulse");

        // 拆掉之后这个 id 必须死透。
        Check(scene->RemoveJoint(hinge_id), "JN27", "a joint can be removed");
        Check(!scene->RemoveJoint(hinge_id), "JN28", "removing it twice is refused (the slot was cleared)");
        Check(!scene->GetJointType(hinge_id, read_type), "JN29", "a removed joint reports no type");
        Check(!scene->SetJointEnabled(hinge_id, true), "JN30", "and cannot be switched back on");

        PhysicsJointDesc slider_desc = hinge_desc;
        slider_desc.m_Type = PhysicsJointType::slider;
        const uint32_t slider_id = scene->CreateJoint(slider_desc);
        Check(slider_id != s_InvalidJointId, "JN31", "a slider joint can be created");
        Check(scene->SetJointLimits(slider_id, -0.5f, 0.5f) &&
                  scene->GetJointLimits(slider_id, min_limit, max_limit) && Near(min_limit, -0.5f) &&
                  Near(max_limit, 0.5f),
              "JN32", "slider limits round-trip through the same unit-less API as the hinge's");
        Check(scene->SetJointMotor(slider_id, true, 0.5f, 20.f), "JN33", "the slider motor drives to a position");

        PhysicsJointDesc distance_desc = hinge_desc;
        distance_desc.m_Type = PhysicsJointType::distance;
        const uint32_t distance_id = scene->CreateJoint(distance_desc);
        Check(distance_id != s_InvalidJointId, "JN34", "a distance (spring) joint can be created");
        Check(scene->SetJointDistance(distance_id, 1.f, 3.f) &&
                  scene->GetJointDistance(distance_id, min_limit, max_limit) && Near(min_limit, 1.f) &&
                  Near(max_limit, 3.f),
              "JN35", "the distance range round-trips");
        Check(!scene->SetJointDistance(distance_id, 3.f, 1.f), "JN36", "and min > max is refused");
        Check(!scene->SetJointDistance(slider_id, 0.f, 1.f), "JN37",
              "the distance setters refuse a joint that is not a distance constraint");

        // "拴在世界"：Jolt 没有世界这个端点，得造一个锚点 body。
        const uint32_t anchor = scene->CreateStaticAnchorBody(Vector3(0.f, 0.f, 20.f));
        Check(anchor != s_InvalidRigidbodyId, "JN38", "a world anchor body can be created");
        Vector3 anchor_position = Vector3::ZERO;
        Check(scene->GetRigidBodyPosition(anchor, anchor_position) && Near(anchor_position.z, 20.f), "JN39",
              "and it sits exactly where it was asked to");
        PhysicsJointDesc anchored_desc = hinge_desc;
        anchored_desc.m_Type = PhysicsJointType::distance;
        anchored_desc.m_BodyA = hinge_b;
        anchored_desc.m_BodyB = anchor;
        Check(scene->CreateJoint(anchored_desc) != s_InvalidJointId, "JN40",
              "a joint can be made against a world anchor (Unity's connectedBody === null)");

        // 退化限位：`useLimits = true` 而区间还是 0/0。Unity 的意思是"锁死在这个
        // 角度"，Jolt 却断言 `min != max || frequency > 0`（"你干脆用 fixed"）。
        // 后端必须自己补一条弹簧把它建出来 —— 静默丢掉限位 和 断言崩在用户机器
        // 上，都是不能接受的。
        PhysicsJointDesc locked_hinge_desc = hinge_desc;
        locked_hinge_desc.m_MinLimit = 0.f;
        locked_hinge_desc.m_MaxLimit = 0.f;
        const uint32_t locked_hinge_id = scene->CreateJoint(locked_hinge_desc);
        Check(locked_hinge_id != s_InvalidJointId, "JN41",
              "a hinge whose two limits coincide still builds (Jolt would say 'use a fixed constraint')");
        Check(scene->GetJointLimits(locked_hinge_id, min_limit, max_limit) && Near(min_limit, 0.f) &&
                  Near(max_limit, 0.f),
              "JN42", "and it keeps the degenerate range instead of silently dropping it");
        PhysicsJointDesc locked_slider_desc = slider_desc;
        locked_slider_desc.m_MinLimit = 0.f;
        locked_slider_desc.m_MaxLimit = 0.f;
        const uint32_t locked_slider_id = scene->CreateJoint(locked_slider_desc);
        Check(locked_slider_id != s_InvalidJointId &&
                  scene->GetJointLimits(locked_slider_id, min_limit, max_limit) && Near(min_limit, 0.f) &&
                  Near(max_limit, 0.f),
              "JN43", "the slider follows the same rule (0/0 locks it, it is not 'no limits')");

        // =====================================================================
        // 2. JointComponent 一族（真 GameObject + RigidBodyComponent）
        // =====================================================================

        ObjectManager* om = GET_SYSTEM(ObjectManager);
        Check(om != nullptr, "JN50", "ObjectManager is reachable");
        if (om == nullptr)
        {
            return;
        }

        const JointActor actor_a = SpawnJointActor(*om, Vector3(0.f, 0.f, 30.f));
        const JointActor actor_b = SpawnJointActor(*om, Vector3(0.f, 0.f, 32.f));
        Check(actor_a.body != nullptr && actor_a.body->HasBody(), "JN51", "the first actor has a live body");
        Check(actor_b.body != nullptr && actor_b.body->HasBody(), "JN52", "and so does the second");
        if (actor_a.body == nullptr || actor_b.body == nullptr)
        {
            return;
        }

        HingeJointComponent* hinge_joint = new HingeJointComponent();
        om->AllocateAndAssignInstanceID(hinge_joint);
        actor_a.object->addComponent(hinge_joint);
        hinge_joint->PostLoadResource(actor_a.object);

        // 没设连接体 ⇒ 挂在世界锚点上，关节**照样**建得出来。
        Check(hinge_joint->HasJoint(), "JN53", "a joint with no connected body hangs off a world anchor");
        Check(hinge_joint->GetConnectedBody() == nullptr, "JN54", "connectedBody is null when world-anchored");

        hinge_joint->SetConnectedBody(actor_b.body);
        Check(hinge_joint->GetConnectedBody() == actor_b.body, "JN55", "connectedBody round-trips");
        Check(hinge_joint->HasJoint(), "JN56", "and the joint was rebuilt against the new body");
        const uint32_t component_joint_id = hinge_joint->GetJointId();
        Check(component_joint_id != kInvalidJointId, "JN57", "the component exposes its joint id");
        Check(scene->GetJointType(component_joint_id, read_type) && read_type == PhysicsJointType::hinge, "JN58",
              "and the backend really built a hinge for a HingeJointComponent");

        // --- 限位：烘在约束里，所以要重建 ----------------------------------
        Check(!hinge_joint->GetUseLimits(), "JN59", "useLimits defaults to off");
        // 只开开关、还没挑区间：min == max == 0。这是 Jolt 断言的那一种输入，
        // 也是脚本最容易写出来的那一种（先 `useLimits = true` 再配 `limits`）。
        hinge_joint->SetUseLimits(true);
        Check(hinge_joint->HasJoint() &&
                  scene->GetJointLimits(hinge_joint->GetJointId(), min_limit, max_limit) &&
                  Near(min_limit, 0.f) && Near(max_limit, 0.f),
              "JN59b", "useLimits = true on its own locks the hinge -- it must not blow up");
        JointLimits limits;
        limits.min = -1.f;
        limits.max = 0.25f;
        hinge_joint->SetUseLimits(true);
        hinge_joint->SetLimits(limits);
        Check(hinge_joint->GetUseLimits(), "JN60", "useLimits reads back");
        Check(Near(hinge_joint->GetLimits().min, -1.f) && Near(hinge_joint->GetLimits().max, 0.25f), "JN61",
              "and so do the limits");
        const uint32_t limited_id = hinge_joint->GetJointId();
        Check(limited_id != component_joint_id, "JN62", "turning limits on REBUILT the joint (they are baked in)");
        Check(scene->GetJointLimits(limited_id, min_limit, max_limit) && Near(min_limit, -1.f) &&
                  Near(max_limit, 0.25f),
              "JN63", "and the backend really got them");
        // Jolt 断言限位区间跨 0，所以一个正数的 min 要被夹住，不是原样存下。
        JointLimits one_sided;
        one_sided.min = 0.75f;
        one_sided.max = 0.9f;
        hinge_joint->SetLimits(one_sided);
        Check(Near(hinge_joint->GetLimits().min, 0.f) && Near(hinge_joint->GetLimits().max, 0.9f), "JN64",
              "a positive min is clamped to 0 (Jolt asserts the range straddles zero)");
        // 限位还开着 ⇒ 这一次也重建。电机/弹簧的判据必须拿**重建之后**的 id，
        // 否则 `limited_id` 已经是死槽了，比对必定不等、断言也就恒假了。
        const uint32_t motor_id = hinge_joint->GetJointId();
        Check(motor_id != limited_id, "JN64b", "re-clamping the limits rebuilt the joint again");

        // --- 电机 / 弹簧：运行时可改，不重建 --------------------------------
        JointMotor motor;
        motor.targetVelocity = 3.f;
        motor.force = 12.f;
        hinge_joint->SetUseMotor(true);
        hinge_joint->SetMotor(motor);
        Check(hinge_joint->GetUseMotor() && Near(hinge_joint->GetMotor().targetVelocity, 3.f) &&
                  Near(hinge_joint->GetMotor().force, 12.f),
              "JN65", "the motor round-trips");
        Check(hinge_joint->GetJointId() == motor_id, "JN66", "and driving the motor did NOT rebuild the joint");

        JointSpring spring_settings;
        spring_settings.spring = 2.f;
        spring_settings.damper = 0.4f;
        spring_settings.targetPosition = 0.5f;
        hinge_joint->SetUseSpring(true);
        hinge_joint->SetSpring(spring_settings);
        Check(hinge_joint->GetUseSpring() && Near(hinge_joint->GetSpring().spring, 2.f) &&
                  Near(hinge_joint->GetSpring().targetPosition, 0.5f),
              "JN67", "the spring round-trips");
        Check(hinge_joint->GetJointId() == motor_id, "JN68", "and neither did the spring");

        StepScene(*scene, 3);
        float backend_angle = -999.f;
        Check(scene->GetJointAngle(hinge_joint->GetJointId(), backend_angle) &&
                  Near(hinge_joint->GetAngle(), backend_angle),
              "JN69", "HingeJoint.angle is the backend's angle, not a component-side copy");

        hinge_joint->SetEnabled(false);
        Check(scene->IsJointEnabled(hinge_joint->GetJointId(), enabled) && !enabled, "JN70",
              "disabling the component disables the Jolt constraint");
        hinge_joint->SetEnabled(true);
        Check(scene->IsJointEnabled(hinge_joint->GetJointId(), enabled) && enabled, "JN71",
              "and re-enabling switches it back on");

        // --- 断裂 ------------------------------------------------------------
        Check(hinge_joint->GetBreakForce() >= kInfiniteBreak && hinge_joint->GetBreakTorque() >= kInfiniteBreak,
              "JN72", "breakForce / breakTorque default to 'never break' (Unity's Mathf.Infinity)");
        Check(!hinge_joint->HasBroken(), "JN73", "and a fresh joint has not broken");
        hinge_joint->FixedTick(1.0f / 60.0f);
        Check(!hinge_joint->HasBroken(), "JN74", "one fixed step with an infinite threshold breaks nothing");
        // 上面那条走的是"两个阈值都无穷 ⇒ 直接返回"的捷径，`force <= breakForce`
        // 这个比较**一次都没被执行过**。给一个有限但极大的阈值，让比较真的发生。
        hinge_joint->SetBreakForce(1.0e9f);
        hinge_joint->FixedTick(1.0f / 60.0f);
        Check(!hinge_joint->HasBroken(), "JN74b", "a huge but finite breakForce still survives the step");
        // 再让它真的吃上力：两个刚体都是动态时它们一起自由落体，铰链不受力，
        // "零阈值必断"就成了看运气。A 转静态、B 吊在铰链上才是受力姿态。
        actor_a.body->SetActorType(RigidBodyActorType::static_body);
        StepScene(*scene, 4);
        float load_linear = 0.f;
        float load_angular = 0.f;
        Check(scene->GetJointImpulse(hinge_joint->GetJointId(), load_linear, load_angular) &&
                  (load_linear > 0.f || load_angular > 0.f),
              "JN74c", "the hanging hinge really carries a load (otherwise the break test is luck)");
        hinge_joint->SetBreakForce(0.f);
        hinge_joint->FixedTick(1.0f / 60.0f);
        Check(hinge_joint->HasBroken(), "JN74d", "and a zero breakForce breaks it on the very next step");
        bool broken_enabled = true;
        Check(scene->IsJointEnabled(hinge_joint->GetJointId(), broken_enabled) && !broken_enabled, "JN74e",
              "the broken joint is switched off in Jolt too, not just flagged");

        // --- 另外三个子类 ----------------------------------------------------
        FixedJointComponent* fixed_joint = new FixedJointComponent();
        om->AllocateAndAssignInstanceID(fixed_joint);
        actor_a.object->addComponent(fixed_joint);
        fixed_joint->PostLoadResource(actor_a.object);
        fixed_joint->SetConnectedBody(actor_b.body);
        Check(fixed_joint->HasJoint() && scene->GetJointType(fixed_joint->GetJointId(), read_type) &&
                  read_type == PhysicsJointType::fixed,
              "JN75", "a FixedJointComponent builds a fixed constraint");

        SpringJointComponent* spring_joint = new SpringJointComponent();
        om->AllocateAndAssignInstanceID(spring_joint);
        actor_a.object->addComponent(spring_joint);
        spring_joint->PostLoadResource(actor_a.object);
        spring_joint->SetConnectedBody(actor_b.body);
        Check(spring_joint->HasJoint() && scene->GetJointType(spring_joint->GetJointId(), read_type) &&
                  read_type == PhysicsJointType::distance,
              "JN76", "a SpringJointComponent builds a distance constraint");
        spring_joint->SetSpring(3.f);
        spring_joint->SetDamper(0.5f);
        Check(Near(spring_joint->GetSpring(), 3.f) && Near(spring_joint->GetDamper(), 0.5f), "JN77",
              "its spring / damper round-trip");
        // 没设过距离 ⇒ 后端按当前姿态推，所以读回来必须是一个真距离（不是 -1）。
        Check(spring_joint->GetMinDistance() >= 0.f && spring_joint->GetMaxDistance() >= 0.f, "JN78",
              "an unconfigured distance range reads back as a real distance, not -1");
        spring_joint->SetMaxDistance(4.f);
        Check(Near(spring_joint->GetMaxDistance(), 4.f) &&
                  scene->GetJointDistance(spring_joint->GetJointId(), min_limit, max_limit) &&
                  Near(max_limit, 4.f),
              "JN79", "and setting the max distance reaches the backend");

        SliderJointComponent* slider_joint = new SliderJointComponent();
        om->AllocateAndAssignInstanceID(slider_joint);
        actor_a.object->addComponent(slider_joint);
        slider_joint->PostLoadResource(actor_a.object);
        slider_joint->SetConnectedBody(actor_b.body);
        Check(slider_joint->HasJoint() && scene->GetJointType(slider_joint->GetJointId(), read_type) &&
                  read_type == PhysicsJointType::slider,
              "JN80", "a SliderJointComponent builds a slider constraint");
        JointLimits slider_limits;
        slider_limits.min = -0.5f;
        slider_limits.max = 0.5f;
        slider_joint->SetUseLimits(true);
        slider_joint->SetLimits(slider_limits);
        Check(scene->GetJointLimits(slider_joint->GetJointId(), min_limit, max_limit) &&
                  Near(min_limit, -0.5f) && Near(max_limit, 0.5f),
              "JN81", "and its limits reach the backend");
        slider_joint->SetMotorTargetPosition(0.25f);
        slider_joint->SetMotorForce(30.f);
        Check(Near(slider_joint->GetMotorTargetPosition(), 0.25f) && Near(slider_joint->GetMotorForce(), 30.f),
              "JN82", "the slider motor round-trips");
        // 负的力没有意义（`SetForceLimit` 给的是对称区间），必须被夹住而不是
        // 原样存下 —— 否则脚本读到 -5 会以为"反向也能使 5N 的劲"。
        slider_joint->SetMotorForce(-5.f);
        Check(Near(slider_joint->GetMotorForce(), 0.f), "JN82b",
              "a negative motor force is clamped to 0, not stored raw");

        // --- anchor / axis / autoConfigure -----------------------------------
        Check(Near(hinge_joint->GetAxis().x, 1.f) && Near(hinge_joint->GetAxis().y, 0.f) &&
                  Near(hinge_joint->GetAxis().z, 0.f),
              "JN83", "Joint.axis defaults to +X (Unity's default)");
        Check(hinge_joint->GetAutoConfigureConnectedAnchor(), "JN84",
              "autoConfigureConnectedAnchor defaults to true (Unity's default)");
        hinge_joint->SetAxis(Vector3(0.f, 1.f, 0.f));
        Check(Near(hinge_joint->GetAxis().y, 1.f) && hinge_joint->GetJointId() != limited_id, "JN85",
              "changing the axis rebuilds the joint (the axis is baked into the constraint frame)");
        // 零向量轴：宁可建不出关节，也不要建一个轴是 nan 的关节。
        hinge_joint->SetAxis(Vector3::ZERO);
        Check(!hinge_joint->HasJoint(), "JN86", "a zero axis is refused: the joint is dropped, not built wrong");
        hinge_joint->SetAxis(Vector3(0.f, 0.f, 1.f));
        Check(hinge_joint->HasJoint(), "JN87", "and a valid axis brings it back");
        hinge_joint->SetAnchor(Vector3(0.f, 0.f, 0.5f));
        Check(Near(hinge_joint->GetAnchor().z, 0.5f) && hinge_joint->HasJoint(), "JN88",
              "the anchor round-trips and rebuilds");

        // `connectedAnchor` 是这条分界唯一的判据：**有连接体时是连接体的局部
        // 空间**，关掉 autoConfigure 之后它就该真的挪动连接点。写成"永远用宿主
        // 锚点"的话，下面这条还是绿的（两个 pivot 重合 → 距离恒为 0）。
        // ⚠️ 基线必须在关掉 autoConfigure **之后**读：开着的时候两个 pivot 重合
        // （距离 0），关掉之后连接点才落到连接体的原点上。拿"开着的时候"当基线
        // 的话，"忽略 connectedAnchor"和"照做"都会让距离变大 —— 断言恒真。
        spring_joint->SetAutoConfigureConnectedAnchor(false);
        const float anchored_distance = spring_joint->GetMinDistance();
        spring_joint->SetConnectedAnchor(Vector3(0.f, 0.f, -1.f));
        Check(!spring_joint->GetAutoConfigureConnectedAnchor() &&
                  Near(spring_joint->GetConnectedAnchor().z, -1.f),
              "JN88a", "autoConfigureConnectedAnchor and connectedAnchor both round-trip");
        Check(std::fabs(spring_joint->GetMinDistance() - anchored_distance) > 0.5f, "JN88b",
              "with autoConfigure off, connectedAnchor really moves the attachment point");

        // =====================================================================
        // 2b. CharacterJoint（锥形球窝）
        // =====================================================================

        PhysicsJointDesc cone_desc = hinge_desc;
        cone_desc.m_Type = PhysicsJointType::swing_twist;
        cone_desc.m_Swing1HalfAngle = 0.5f;
        cone_desc.m_Swing2HalfAngle = 0.4f;
        cone_desc.m_TwistMinAngle = -0.3f;
        cone_desc.m_TwistMaxAngle = 0.6f;
        const uint32_t cone_id = scene->CreateJoint(cone_desc);
        Check(cone_id != s_InvalidJointId &&
                  scene->GetJointType(cone_id, read_type) && read_type == PhysicsJointType::swing_twist,
              "CJ0", "a swing-twist (cone) joint can be created and reports its own type");
        float cone_swing1 = -1.f;
        float cone_swing2 = -1.f;
        float cone_twist_min = -1.f;
        float cone_twist_max = -1.f;
        Check(scene->GetJointSwingTwistLimits(cone_id, cone_swing1, cone_swing2, cone_twist_min,
                                              cone_twist_max) &&
                  Near(cone_swing1, 0.5f) && Near(cone_swing2, 0.4f) && Near(cone_twist_min, -0.3f) &&
                  Near(cone_twist_max, 0.6f),
              "CJ0a", "and all four cone limits come back out of the backend");
        // 对照组：按类型拒绝。少了这条，"永远返回 true" 也是绿的。
        // ⚠️ 对照组必须挑一个**还活着**的关节：`hinge_id` 在上面 JN27 已经被
        // RemoveJoint 拆掉了，`JointAt` 直接返回 nullptr —— 那样这条断言会因为
        // "关节不存在"而不是"类型不对"通过，什么也没挡住。
        Check(scene->GetJointType(fixed_id, read_type) && read_type == PhysicsJointType::fixed, "CJ0b",
              "the control joint for the type refusal is a LIVE fixed joint (a removed one proves nothing)");
        Check(!scene->GetJointSwingTwistLimits(fixed_id, cone_swing1, cone_swing2, cone_twist_min,
                                               cone_twist_max),
              "CJ0c", "a fixed joint has no cone limits to read");
        Check(!scene->SetJointSwingTwistLimits(fixed_id, 0.1f, 0.1f, -0.1f, 0.1f), "CJ0d",
              "and refuses to have them written");
        // 摆动轴与扭转轴平行：Jolt 只正交化一次，cross(parallel, parallel) 是零
        // 向量，后面 .Normalized() 就是 NaN —— 一个 NaN 约束会拖垮整个 broadphase。
        PhysicsJointDesc parallel_desc = cone_desc;
        parallel_desc.m_PlaneAxisA = Vector3(0.f, 0.f, 1.f);   // 与扭转轴（+Z）平行
        parallel_desc.m_PlaneAxisB = Vector3(0.f, 0.f, 1.f);
        const uint32_t parallel_id = scene->CreateJoint(parallel_desc);
        Check(parallel_id != s_InvalidJointId, "CJ0e",
              "a swing axis parallel to the twist axis does NOT poison the constraint");
        // 后端自己也要拒：组件层的拒绝是**另一层**，两层不能互相顶替 —— 直接投
        // Jolt 的调用者（程序化布娃娃）只走后端这一层。
        Check(!scene->SetJointSwingTwistLimits(cone_id, -0.5f, 0.4f, -0.3f, 0.6f) &&
                  !scene->SetJointSwingTwistLimits(cone_id, 0.5f, 0.4f, 0.3f, 0.6f) &&
                  !scene->SetJointSwingTwistLimits(cone_id, 0.5f, 0.4f, -0.3f, -0.6f),
              "CJ0f", "the backend refuses a negative half cone angle and a twist range that misses zero");
        // NaN 判据：正交化失败会把 NaN 带进约束，一步之后整个场景的位置都是
        // NaN。光"创建成功"看不出来，必须步进之后查位置还是不是有限值。
        StepScene(*scene, 3);
        Vector3 cone_body_position = Vector3::ZERO;
        Check(scene->GetRigidBodyPosition(hinge_b, cone_body_position) &&
                  std::isfinite(cone_body_position.x) && std::isfinite(cone_body_position.y) &&
                  std::isfinite(cone_body_position.z),
              "CJ0g", "and a few steps later the bodies are still finite (a NaN constraint poisons everything)");

        CharacterJointComponent* char_joint = new CharacterJointComponent();
        om->AllocateAndAssignInstanceID(char_joint);
        actor_a.object->addComponent(char_joint);
        char_joint->PostLoadResource(actor_a.object);
        char_joint->SetConnectedBody(actor_b.body);
        Check(char_joint->HasJoint() && scene->GetJointType(char_joint->GetJointId(), read_type) &&
                  read_type == PhysicsJointType::swing_twist,
              "CJ1", "a CharacterJointComponent builds a swing-twist constraint (a cone, not a ball socket)");
        const uint32_t cone_component_id = char_joint->GetJointId();
        Check(Near(char_joint->GetSwing1Limit(), 0.f) && Near(char_joint->GetSwing2Limit(), 0.f) &&
                  Near(char_joint->GetLowTwistLimit(), 0.f) && Near(char_joint->GetHighTwistLimit(), 0.f),
              "CJ2", "the four limits default to 0 (Unity's fresh CharacterJoint)");
        Check(Near(char_joint->GetSwingAxis().y, 1.f), "CJ3", "swingAxis defaults to +Y (Unity's default)");

        // ⚠️ 与铰链相反：这里的限位**可以运行时改**，改了不该重建。
        Check(char_joint->SetSwing1Limit(0.5f) && Near(char_joint->GetSwing1Limit(), 0.5f), "CJ4",
              "swing1Limit round-trips");
        Check(scene->GetJointSwingTwistLimits(char_joint->GetJointId(), cone_swing1, cone_swing2,
                                              cone_twist_min, cone_twist_max) &&
                  Near(cone_swing1, 0.5f),
              "CJ5", "and it reached the backend");
        Check(char_joint->GetJointId() == cone_component_id, "CJ6",
              "changing a cone limit did NOT rebuild the joint (unlike a hinge's limits)");
        Check(char_joint->SetTwistSwingLimits(0.5f, 0.4f, -0.3f, 0.6f) &&
                  scene->GetJointSwingTwistLimits(char_joint->GetJointId(), cone_swing1, cone_swing2,
                                                  cone_twist_min, cone_twist_max) &&
                  Near(cone_swing1, 0.5f) && Near(cone_swing2, 0.4f) && Near(cone_twist_min, -0.3f) &&
                  Near(cone_twist_max, 0.6f),
              "CJ7", "all four at once round-trip and reach the backend");

        // 拒而不夹：夹出来的值和脚本写的不是一回事，"我明明写了 -0.5 却被当成 0"
        // 是最难查的一类。
        Check(!char_joint->SetSwing1Limit(-0.5f) && Near(char_joint->GetSwing1Limit(), 0.5f), "CJ8",
              "a negative swing half cone angle is refused, and the old value is untouched");
        Check(!char_joint->SetLowTwistLimit(0.2f) && Near(char_joint->GetLowTwistLimit(), -0.3f), "CJ9",
              "a twist range that does not straddle zero is refused (not clamped)");
        Check(!char_joint->SetHighTwistLimit(-0.2f) && Near(char_joint->GetHighTwistLimit(), 0.6f), "CJ10",
              "and so is the other side");

        // 摆动轴烘在约束的参考系里 ⇒ 改它**要**重建（与铰链那条规则一致）。
        char_joint->SetSwingAxis(Vector3(1.f, 0.f, 0.f));
        Check(Near(char_joint->GetSwingAxis().x, 1.f), "CJ11", "swingAxis round-trips");
        Check(char_joint->GetJointId() != cone_component_id && char_joint->HasJoint(), "CJ12",
              "and changing it rebuilt the joint (the axis is baked into the constraint frame)");

        // 限位弹簧
        SoftJointLimitSpring twist_spring;
        twist_spring.spring = 3.f;
        twist_spring.damper = 0.5f;
        Check(char_joint->SetTwistLimitSpring(twist_spring) &&
                  Near(char_joint->GetTwistLimitSpring().spring, 3.f) &&
                  Near(char_joint->GetTwistLimitSpring().damper, 0.5f),
              "CJ13", "twistLimitSpring round-trips");
        float spring_hz = -1.f;
        float spring_damping = -1.f;
        float twist_hz = -1.f;
        float twist_damping = -1.f;
        Check(scene->GetJointSwingTwistSpring(char_joint->GetJointId(), spring_hz, spring_damping, twist_hz,
                                              twist_damping) &&
                  Near(twist_hz, 3.f) && Near(twist_damping, 0.5f),
              "CJ14", "and it reached the backend");
        SoftJointLimitSpring swing_spring;
        swing_spring.spring = 2.f;
        swing_spring.damper = 0.25f;
        Check(char_joint->SetSwingLimitSpring(swing_spring) &&
                  scene->GetJointSwingTwistSpring(char_joint->GetJointId(), spring_hz, spring_damping,
                                                  twist_hz, twist_damping) &&
                  Near(spring_hz, 2.f) && Near(spring_damping, 0.25f),
              "CJ15", "swingLimitSpring round-trips and reaches the backend");
        SoftJointLimitSpring bad_spring;
        bad_spring.spring = -1.f;
        bad_spring.damper = 0.f;
        Check(!char_joint->SetSwingLimitSpring(bad_spring) &&
                  Near(char_joint->GetSwingLimitSpring().spring, 2.f),
              "CJ16", "a negative spring is refused, and the old value is untouched");

        // 真的在吃力 ⇒ breakForce 那条路在这个类型上也通。
        StepScene(*scene, 4);
        float cone_linear = 0.f;
        float cone_angular = 0.f;
        Check(scene->GetJointImpulse(char_joint->GetJointId(), cone_linear, cone_angular) &&
                  cone_linear > 0.f,
              "CJ17", "a swing-twist holding a body up reports a NON-ZERO impulse (breakForce needs it)");
        // ⚠️ 上面那条只证明了**线性**冲量。角冲量走的是另一条分支（twist / swingY /
        // swingZ 三个 lambda 取最大），而锥上的角冲量**默认就是 0** —— 两端轴对齐、
        // 关节点又落在质心上，重力对枢轴零力矩，压根没人在拧它。所以夹具必须
        // **让重力拧出一个绕扭转轴的力矩**，否则"角冲量永远返回 0"是绿的。
        // 而且要换一对**全新**的刚体：`actor_a` / `actor_b` 身上已经挂了
        // fixed / spring / slider，它们会把 actor_b 焊死，根本拧不动。
        const uint32_t twist_a = SpawnBox(*scene, Vector3(0.f, 80.f, 0.f), 0, false);
        // ⚠️ B 的质心必须**偏离关节点**（这里偏 +Y 两米）。关节点落在质心上的话
        // 重力对枢轴的力矩是 0，扭转根本没得可顶 —— 那就只能靠 SetAngularVelocity
        // 硬塞一个角速度，而它不唤醒 body、且求解一结束角速度就被清零，读到的
        // lambda 永远是"已经解完"的 0。让重力去拧才是持续载荷。
        const uint32_t twist_b = SpawnBox(*scene, Vector3(0.f, 82.f, 0.f), 0, true);
        Check(twist_a != kInvalidPhysicsBodyId && twist_b != kInvalidPhysicsBodyId, "CJ17b",
              "a clean pair of bodies for the twist load (the shared actors are welded by the other joints)");
        // 力矩臂 = B 质心 - 关节点 = (0, +2, 0)，重力 -Z ⇒ 绕 **-X** 的力矩，
        // 而扭转轴正是 +X ⇒ 扭转限位每步都在顶着。
        Vector3 twist_b_pos = Vector3::ZERO;
        Check(scene->GetRigidBodyPosition(twist_b, twist_b_pos) &&
                  std::fabs(twist_b_pos.y - 82.f) < 0.01f,
              "CJ17c", "the hanging body's centre of mass really is offset from the pivot (that offset IS the torque)");
        PhysicsJointDesc twist_desc;
        twist_desc.m_Type = PhysicsJointType::swing_twist;
        twist_desc.m_BodyA = twist_a;
        twist_desc.m_BodyB = twist_b;
        // Jolt 的 mPosition1 / mPosition2 是**世界**点，两端写同一个世界点。
        twist_desc.m_PivotA = Vector3(0.f, 80.f, 0.f);
        twist_desc.m_PivotB = Vector3(0.f, 80.f, 0.f);
        twist_desc.m_AxisA = Vector3(1.f, 0.f, 0.f);   // 扭转轴 = +X
        twist_desc.m_AxisB = Vector3(1.f, 0.f, 0.f);
        twist_desc.m_PlaneAxisA = Vector3(0.f, 1.f, 0.f);
        twist_desc.m_PlaneAxisB = Vector3(0.f, 1.f, 0.f);
        // ⚠️ 用**有限**的扭转限位，不要写成锁死（0 .. 0）。
        // 锁死走 TwistXLocked 分支 —— 那条分支确实把 B 焊住了（实测角速度被压到
        // 0.02 rad/s），但它的 total lambda 读出来**恒为 0**，于是"角冲量永远
        // 返回 0"这条变异体反而活下来。有限限位会先转过去**撞上**再被顶住，
        // 撞的那一步 lambda 是真的。
        twist_desc.m_TwistMinAngle = -0.05f;
        twist_desc.m_TwistMaxAngle = 0.05f;
        twist_desc.m_AutoDetectPoint = false;
        const uint32_t twist_id = scene->CreateJoint(twist_desc);
        Check(twist_id != s_InvalidJointId, "CJ17d", "a cone with a finite twist range can be created");
        // 每步逐步读：total lambda 每步清零，攒到最后再读只会读到 0。
        float twist_linear = 0.f;
        float twist_angular = 0.f;
        bool twist_impulse_ok = false;
        for (int twist_step = 0; twist_step < 12; ++twist_step)
        {
            StepScene(*scene, 1);
            float step_linear = 0.f;
            float step_angular = 0.f;
            if (scene->GetJointImpulse(twist_id, step_linear, step_angular))
            {
                twist_impulse_ok = true;
                twist_linear = std::max(twist_linear, step_linear);
                twist_angular = std::max(twist_angular, step_angular);
            }
        }
        Check(twist_impulse_ok, "CJ17e", "GetJointImpulse answers for a swing-twist constraint");
        Check(twist_linear > 0.f, "CJ17f", "the hanging cone carries a linear load");
        Check(twist_angular > 0.f, "CJ17g",
              "and the twist limit pushes back: a NON-ZERO ANGULAR impulse (breakForce needs it)");
        // 光有"非 0"还不够：`max(|twist|, |swingY|, |swingZ|)` 里少读任何一股，
        // 剩下两股照样能把它喂成非 0 ⇒ 行为上看不出来。所以再钉一次源头。
        const std::string scene_cpp = ReadFile("Source/Runtime/Function/Physics/PhysicsScene.cpp");
        Check(!scene_cpp.empty() && CountOccurrences(scene_cpp, "joint->GetTotalLambdaTwist()") >= 1 &&
                  CountOccurrences(scene_cpp, "joint->GetTotalLambdaSwingY()") >= 1 &&
                  CountOccurrences(scene_cpp, "joint->GetTotalLambdaSwingZ()") >= 1,
              "CJ17l", "the cone's angular impulse really reads all three lambda terms (twist + swingY + swingZ)");
        // 退化区间（0 .. 0）＝ 锁死。这一条**不能**靠冲量：锁死分支的 lambda 读
        // 出来是 0（见上面 CJ17d 的注释），只能看行为 —— B 还转不转。
        Check(scene->SetJointSwingTwistLimits(twist_id, 0.f, 0.f, 0.f, 0.f), "CJ17h",
              "the degenerate 0..0 twist range is accepted (it means locked, not unconfigured)");
        StepScene(*scene, 8);
        Vector3 locked_av = Vector3::ZERO;
        Check(scene->GetAngularVelocity(twist_b, locked_av) && std::fabs(locked_av.x) < 0.5f,
              "CJ17i", "and it really locks the twist: the body stops turning (a free cone spins up fast)");
        // ⚠️ 上面走的是 **setter**。`CreateJoint` 那条路上也有一个同样的守卫
        // （`min <= 0 && max >= 0`），它得单独钉：改成 `<` / `>` 的话 desc 里
        // **贴着 0 的那一侧**会被当成"没配"丢掉，Jolt 落回 settings 的默认值 ⇒
        // 建出来的关节区间和脚本写的不是一回事。
        //
        // ⚠️⚠️ 夹具**绝不能**用 `0 .. 0` 这一对。`SwingTwistConstraintSettings`
        // 的 `mTwistMinAngle` / `mTwistMaxAngle` 默认就是 **0 / 0**（不是 ±π），
        // 所以 `0 .. 0` 走"守卫放行"和"守卫拦下"两条路结果完全一样 —— 守卫整个
        // 删掉也看不出来（CJM23 就是这么活了一整轮）。真正能区分两种守卫的是
        // **只贴 0 一侧**的区间：`-0.4 .. 0` 与 `0 .. 0.4`。
        const uint32_t born_a = SpawnBox(*scene, Vector3(0.f, 90.f, 0.f), 0, false);
        const uint32_t born_b = SpawnBox(*scene, Vector3(0.f, 92.f, 0.f), 0, true);
        PhysicsJointDesc born_desc = twist_desc;
        born_desc.m_BodyA = born_a;
        born_desc.m_BodyB = born_b;
        born_desc.m_PivotA = Vector3(0.f, 90.f, 0.f);
        born_desc.m_PivotB = Vector3(0.f, 90.f, 0.f);
        born_desc.m_TwistMinAngle = -0.4f;
        born_desc.m_TwistMaxAngle = 0.f;
        const uint32_t born_id = scene->CreateJoint(born_desc);
        Check(born_id != s_InvalidJointId, "CJ17j",
              "a cone built from a desc whose twist range touches zero on one side (-0.4 .. 0) can be created");
        // 最直接的一刀：把区间读回来。守卫用 `<=` / `>=` 时是 -0.4 .. 0；写成
        // `<` / `>` 就掉回 Jolt 默认的 0 .. 0。
        float born_swing1 = 0.f;
        float born_swing2 = 0.f;
        float born_tmin = 0.f;
        float born_tmax = 0.f;
        Check(scene->GetJointSwingTwistLimits(born_id, born_swing1, born_swing2, born_tmin, born_tmax) &&
                  std::fabs(born_tmin - (-0.4f)) < 0.001f && std::fabs(born_tmax) < 0.001f,
              "CJ17k", "and the range really survives creation: reading it back gives -0.4 .. 0 (not the 0 .. 0 default)");
        // 另一侧的 `0 .. 0.4`，顺手把行为也钉上：重力绕扭转轴（±X，符号不必猜）
        // 拧，两条区间里**正好一条**放行、一条拦住。
        const uint32_t born2_a = SpawnBox(*scene, Vector3(0.f, 100.f, 0.f), 0, false);
        const uint32_t born2_b = SpawnBox(*scene, Vector3(0.f, 102.f, 0.f), 0, true);
        PhysicsJointDesc born2_desc = twist_desc;
        born2_desc.m_BodyA = born2_a;
        born2_desc.m_BodyB = born2_b;
        born2_desc.m_PivotA = Vector3(0.f, 100.f, 0.f);
        born2_desc.m_PivotB = Vector3(0.f, 100.f, 0.f);
        born2_desc.m_TwistMinAngle = 0.f;
        born2_desc.m_TwistMaxAngle = 0.4f;
        const uint32_t born2_id = scene->CreateJoint(born2_desc);
        Check(born2_id != s_InvalidJointId, "CJ17m", "and the mirror range (0 .. 0.4) can be created too");
        float born2_swing1 = 0.f;
        float born2_swing2 = 0.f;
        float born2_tmin = 0.f;
        float born2_tmax = 0.f;
        Check(scene->GetJointSwingTwistLimits(born2_id, born2_swing1, born2_swing2, born2_tmin, born2_tmax) &&
                  std::fabs(born2_tmin) < 0.001f && std::fabs(born2_tmax - 0.4f) < 0.001f,
              "CJ17n", "the mirror range survives creation as well (0 .. 0.4, not the 0 .. 0 default)");
        // 逐步取 max：撞上限位之后角速度会被压回去，只看最后一步可能读到 0。
        float spin_neg = 0.f;
        float spin_pos = 0.f;
        for (int spin_step = 0; spin_step < 12; ++spin_step)
        {
            StepScene(*scene, 1);
            Vector3 av_neg = Vector3::ZERO;
            Vector3 av_pos = Vector3::ZERO;
            if (scene->GetAngularVelocity(born_b, av_neg))
            {
                spin_neg = std::max(spin_neg, std::fabs(av_neg.x));
            }
            if (scene->GetAngularVelocity(born2_b, av_pos))
            {
                spin_pos = std::max(spin_pos, std::fabs(av_pos.x));
            }
        }
        Check(std::max(spin_neg, spin_pos) > 0.5f, "CJ17o",
              "behaviourally: one of the two one-sided ranges lets the body turn (the desc's range is honoured)");
        // 用**比值**而不是绝对值：被拦住那一侧每步仍会被重力推一下再被解掉，
        // 残留多少不好猜；但"转起来"和"被顶住"差一个数量级，4 倍是稳的。
        Check(std::min(spin_neg, spin_pos) * 4.f < std::max(spin_neg, spin_pos), "CJ17p",
              "and the other one blocks it: one side turns at least 4x more than the other (not 'the whole range became free')");

        // =====================================================================
        // 2c. ConfigurableJoint（六自由度）
        // =====================================================================
        // Jolt 的 `SixDOFConstraint` 与 Unity 的 `ConfigurableJoint` 轴模型一致：
        // axis = 参考系 X、secondaryAxis = Y、第三条取叉乘（Unity 文档同样这么说）。

        const uint32_t cfg_a = SpawnBox(*scene, Vector3(0.f, 110.f, 0.f), 0, false);
        const uint32_t cfg_b = SpawnBox(*scene, Vector3(0.f, 112.f, 0.f), 0, true);
        PhysicsJointDesc cfg_desc;
        cfg_desc.m_Type = PhysicsJointType::configurable;
        cfg_desc.m_BodyA = cfg_a;
        cfg_desc.m_BodyB = cfg_b;
        cfg_desc.m_PivotA = Vector3(0.f, 110.f, 0.f);
        cfg_desc.m_PivotB = Vector3(0.f, 110.f, 0.f);
        cfg_desc.m_AxisA = Vector3(1.f, 0.f, 0.f);
        cfg_desc.m_AxisB = Vector3(1.f, 0.f, 0.f);
        cfg_desc.m_SecondaryAxisA = Vector3(0.f, 1.f, 0.f);
        cfg_desc.m_SecondaryAxisB = Vector3(0.f, 1.f, 0.f);
        cfg_desc.m_AutoDetectPoint = false;
        const uint32_t cfg_id = scene->CreateJoint(cfg_desc);
        Check(cfg_id != s_InvalidJointId && scene->GetJointType(cfg_id, read_type) &&
                  read_type == PhysicsJointType::configurable,
              "CF0", "a configurable (six-DOF) joint can be created and reports its own type");

        // 默认六轴全 Free —— 与 Jolt 的 `SixDOFConstraintSettings` 初值（±FLT_MAX）
        // 一致，也和 Unity 里"刚 AddComponent 还没配"一样。
        bool cfg_all_free = true;
        for (int axis_index = 0; axis_index < kConfigurableAxisCount; ++axis_index)
        {
            ConfigurableJointMotion axis_motion = ConfigurableJointMotion::Locked;
            float axis_lo = -1.f;
            float axis_hi = -1.f;
            if (!scene->GetJointAxisMotion(cfg_id, static_cast<ConfigurableJointAxis>(axis_index),
                                           axis_motion, axis_lo, axis_hi) ||
                axis_motion != ConfigurableJointMotion::Free)
            {
                cfg_all_free = false;
            }
        }
        Check(cfg_all_free, "CF1",
              "all six axes start Free (Jolt's own default, and Unity's fresh ConfigurableJoint)");

        // 按类型拒绝 —— 对照组必须是一个**还活着**的关节（拆掉的那个会让
        // `JointAt` 直接返回 nullptr，断言于是因为"不存在"而不是"类型不对"通过）。
        Check(scene->GetJointType(fixed_id, read_type) && read_type == PhysicsJointType::fixed, "CF2",
              "the control joint for the type refusal is a LIVE fixed joint (a removed one proves nothing)");
        Check(!scene->SetJointAxisMotion(fixed_id, ConfigurableJointAxis::TranslationX,
                                         ConfigurableJointMotion::Locked, 0.f, 0.f),
              "CF2a", "a fixed joint refuses SetJointAxisMotion");
        Check(!scene->SetJointAxisDrive(fixed_id, ConfigurableJointAxis::TranslationX, JointDrive()),
              "CF2b", "and SetJointAxisDrive");
        Check(!scene->SetJointLinearLimitSpring(fixed_id, 1.f, 1.f), "CF2c", "and SetJointLinearLimitSpring");
        Vector3 cfg_dummy_vec = Vector3::ZERO;
        Quaternion cfg_dummy_rot = Quaternion::IDENTITY;
        Check(!scene->GetJointTarget(fixed_id, cfg_dummy_vec, cfg_dummy_vec, cfg_dummy_vec, cfg_dummy_rot),
              "CF2d", "and GetJointTarget");

        // --- 六个轴的模式与限位 ---------------------------------------------
        ConfigurableJointMotion read_motion = ConfigurableJointMotion::Free;
        float read_lo = -1.f;
        float read_hi = -1.f;
        Check(scene->SetJointAxisMotion(cfg_id, ConfigurableJointAxis::TranslationX,
                                        ConfigurableJointMotion::Locked, 0.f, 0.f),
              "CF3", "an axis can be switched to Locked at runtime (no rebuild needed)");
        Check(scene->GetJointAxisMotion(cfg_id, ConfigurableJointAxis::TranslationX,
                                        read_motion, read_lo, read_hi) &&
                  read_motion == ConfigurableJointMotion::Locked,
              "CF3a", "and reads back Locked");
        // ⚠️ Jolt 的平移 / 旋转限位是 **Vec3 整体** setter：改一个轴必须先读回
        // 另外两个。少了读改写，"改 X"会顺手把 Y / Z 也改掉 —— 而限位被改宽了
        // 只在物理表现上看得出来，不报错。
        Check(scene->GetJointAxisMotion(cfg_id, ConfigurableJointAxis::TranslationY,
                                        read_motion, read_lo, read_hi) &&
                  read_motion == ConfigurableJointMotion::Free,
              "CF3b", "and TranslationY is still Free (a whole-Vec3 write without read-modify-write would clobber it)");

        Check(scene->SetJointAxisMotion(cfg_id, ConfigurableJointAxis::TranslationZ,
                                        ConfigurableJointMotion::Limited, -0.5f, 0.5f) &&
                  scene->GetJointAxisMotion(cfg_id, ConfigurableJointAxis::TranslationZ,
                                            read_motion, read_lo, read_hi) &&
                  read_motion == ConfigurableJointMotion::Limited && Near(read_lo, -0.5f) &&
                  Near(read_hi, 0.5f),
              "CF4", "a Limited axis carries its range and reads it back");
        // 区间写反在 Jolt 里是 `Locked` 的写法（`IsFixedAxis` 判 min >= max），
        // 与脚本想要的"一个有区间的轴"是两回事 ⇒ 拒掉。
        Check(!scene->SetJointAxisMotion(cfg_id, ConfigurableJointAxis::TranslationZ,
                                         ConfigurableJointMotion::Limited, 0.5f, -0.5f),
              "CF4a", "an inverted range is refused (in Jolt that spelling means Locked)");
        // ⚠️ 上面走的是 **setter**。`CreateJoint` 那条路上也有一个同样的守卫，得
        // 单独钉：少了它，desc 里的反序区间会被当成 Locked 建出来 —— 而
        // "CreateJoint 返回了 id" 全程是绿的。
        const uint32_t inv_a = SpawnBox(*scene, Vector3(0.f, 170.f, 0.f), 0, false);
        const uint32_t inv_b = SpawnBox(*scene, Vector3(0.f, 172.f, 0.f), 0, true);
        PhysicsJointDesc inv_desc = cfg_desc;
        inv_desc.m_BodyA = inv_a;
        inv_desc.m_BodyB = inv_b;
        inv_desc.m_PivotA = Vector3(0.f, 170.f, 0.f);
        inv_desc.m_PivotB = Vector3(0.f, 170.f, 0.f);
        const int inv_z = static_cast<int>(ConfigurableJointAxis::TranslationZ);
        inv_desc.m_Motion[inv_z] = ConfigurableJointMotion::Limited;
        inv_desc.m_AxisLimitMin[inv_z] = 0.5f;
        inv_desc.m_AxisLimitMax[inv_z] = -0.5f;
        Check(scene->CreateJoint(inv_desc) == s_InvalidJointId, "CF4b",
              "CreateJoint refuses a desc with an inverted limit range (that spelling means Locked)");

        // --- 驱动 -------------------------------------------------------------
        JointDrive x_drive;
        x_drive.positionSpring = 100.f;
        x_drive.positionDamper = 10.f;
        x_drive.maximumForce = 500.f;
        JointDrive x_read;
        Check(scene->SetJointAxisDrive(cfg_id, ConfigurableJointAxis::TranslationX, x_drive) &&
                  scene->GetJointAxisDrive(cfg_id, ConfigurableJointAxis::TranslationX, x_read) &&
                  Near(x_read.positionSpring, 100.f) && Near(x_read.positionDamper, 10.f) &&
                  Near(x_read.maximumForce, 500.f),
              "CF5", "a translation drive round-trips (spring / damper / max force)");
        JointDrive rot_drive;
        rot_drive.positionSpring = 50.f;
        rot_drive.positionDamper = 5.f;
        rot_drive.maximumForce = 200.f;
        JointDrive rot_read;
        Check(scene->SetJointAxisDrive(cfg_id, ConfigurableJointAxis::RotationX, rot_drive) &&
                  scene->GetJointAxisDrive(cfg_id, ConfigurableJointAxis::RotationX, rot_read) &&
                  Near(rot_read.positionSpring, 50.f) && Near(rot_read.maximumForce, 200.f),
              "CF5a", "a rotation drive round-trips through the TORQUE limit (same field, a different cut)");
        // ⚠️ 从没驱动过的轴必须读回 spring = 0。Jolt 的 `MotorSettings` 默认是
        // `{FrequencyAndDamping, 2 Hz, 1}` —— 不清零的话这里会读到一个脚本从来
        // 没写过的 2.0，而"关节身上有一个看不见的弹簧"只在手感上暴露。
        JointDrive virgin_drive;
        Check(scene->GetJointAxisDrive(cfg_id, ConfigurableJointAxis::TranslationY, virgin_drive) &&
                  Near(virgin_drive.positionSpring, 0.f),
              "CF6", "an axis that was never driven reads back spring = 0 (CreateJoint must zero Jolt's 2 Hz default)");
        JointDrive bad_drive;
        bad_drive.positionSpring = 10.f;
        bad_drive.positionDamper = -1.f;
        Check(!scene->SetJointAxisDrive(cfg_id, ConfigurableJointAxis::TranslationY, bad_drive),
              "CF6a", "a negative damper is refused");

        // --- 目标量 -----------------------------------------------------------
        // ⚠️ `targetRotation` 那道门必须用**非单位**的约束参考系：B1 / B2 都是单位
        // 四元数时 `CS = B1⁻¹·q·B2` 退化成 `CS = q`，任何写错的反解公式读回来都还
        // 是 q ⇒ 断言恒真。所以这条关节的 axis 取 +Z、secondaryAxis 取 +Y
        //（参考系 X = +Z、Y = +Y、Z = -X，不是单位阵）。
        const uint32_t cfg2_a = SpawnBox(*scene, Vector3(0.f, 120.f, 0.f), 0, false);
        const uint32_t cfg2_b = SpawnBox(*scene, Vector3(0.f, 122.f, 0.f), 0, true);
        PhysicsJointDesc cfg2_desc = cfg_desc;
        cfg2_desc.m_BodyA = cfg2_a;
        cfg2_desc.m_BodyB = cfg2_b;
        cfg2_desc.m_PivotA = Vector3(0.f, 120.f, 0.f);
        cfg2_desc.m_PivotB = Vector3(0.f, 120.f, 0.f);
        cfg2_desc.m_AxisA = Vector3(0.f, 0.f, 1.f);
        cfg2_desc.m_AxisB = Vector3(0.f, 0.f, 1.f);
        const uint32_t cfg2_id = scene->CreateJoint(cfg2_desc);
        Check(cfg2_id != s_InvalidJointId, "CF7",
              "a configurable joint whose frame axes are not the identity can be created");
        // 绕 Z 转 30°（w = cos15°、z = sin15°）。
        const Quaternion target_rot(0.9659258f, 0.f, 0.f, 0.2588190f);
        Check(scene->SetJointTarget(cfg2_id, Vector3(0.f, 1.f, 0.f), Vector3(0.f, 0.f, 2.f),
                                    Vector3(1.f, 0.f, 0.f), target_rot),
              "CF7a", "the four target values can be written");
        Vector3 read_target_position = Vector3::ZERO;
        Vector3 read_target_velocity = Vector3::ZERO;
        Vector3 read_target_angular_velocity = Vector3::ZERO;
        Quaternion read_target_rotation = Quaternion::IDENTITY;
        Check(scene->GetJointTarget(cfg2_id, read_target_position, read_target_velocity,
                                    read_target_angular_velocity, read_target_rotation) &&
                  Near(read_target_position.y, 1.f) && Near(read_target_velocity.z, 2.f) &&
                  Near(read_target_angular_velocity.x, 1.f),
              "CF7b", "the three vector targets round-trip");
        // 这条是 BS / CS 换算那道门：写入走 `SetTargetOrientationBS`（body space），
        // Jolt 只存 constraint space 的那一份，读回来必须反解 `q = B1·CS·B2⁻¹`。
        Check(Near(read_target_rotation.w, target_rot.w) && Near(read_target_rotation.x, target_rot.x) &&
                  Near(read_target_rotation.y, target_rot.y) && Near(read_target_rotation.z, target_rot.z),
              "CF7c", "and targetRotation round-trips: the constraint-space value is converted back to body space");

        // --- 线性限位弹簧 -----------------------------------------------------
        Check(scene->SetJointLinearLimitSpring(cfg_id, 50.f, 2.f), "CF8",
              "the linear limit spring can be written");
        float read_lls_spring = -1.f;
        float read_lls_damper = -1.f;
        Check(scene->GetJointLinearLimitSpring(cfg_id, read_lls_spring, read_lls_damper) &&
                  Near(read_lls_spring, 50.f) && Near(read_lls_damper, 2.f),
              "CF8a", "and reads back (translation axes only — Jolt has no soft rotation limits)");
        Check(!scene->SetJointLinearLimitSpring(cfg_id, -1.f, 2.f), "CF8b",
              "a negative spring is refused");

        // --- 行为门：六个轴全 Locked = 焊死 -----------------------------------
        const uint32_t weld_a = SpawnBox(*scene, Vector3(0.f, 130.f, 0.f), 0, false);
        const uint32_t weld_b = SpawnBox(*scene, Vector3(0.f, 132.f, 0.f), 0, true);
        PhysicsJointDesc weld_desc = cfg_desc;
        weld_desc.m_BodyA = weld_a;
        weld_desc.m_BodyB = weld_b;
        weld_desc.m_PivotA = Vector3(0.f, 130.f, 0.f);
        weld_desc.m_PivotB = Vector3(0.f, 130.f, 0.f);
        for (int axis_index = 0; axis_index < kConfigurableAxisCount; ++axis_index)
        {
            weld_desc.m_Motion[axis_index] = ConfigurableJointMotion::Locked;
        }
        const uint32_t weld_id = scene->CreateJoint(weld_desc);
        Check(weld_id != s_InvalidJointId, "CF9", "a joint with all six axes Locked can be created");
        StepScene(*scene, 10);
        Vector3 weld_position = Vector3::ZERO;
        Check(scene->GetRigidBodyPosition(weld_b, weld_position) && std::isfinite(weld_position.z) &&
                  std::fabs(weld_position.z) < 0.05f && std::fabs(weld_position.y - 132.f) < 0.05f,
              "CF9a", "and it really welds: the hanging body does not fall (all six DOF locked)");

        // --- 行为门：Limited 的平移轴真的把 body 拦在限位上 --------------------
        // 三个旋转轴也锁掉，否则 body 会绕枢轴摆，COM 的位置就不只是平移了。
        const uint32_t lim_a = SpawnBox(*scene, Vector3(0.f, 140.f, 0.f), 0, false);
        const uint32_t lim_b = SpawnBox(*scene, Vector3(0.f, 142.f, 0.f), 0, true);
        PhysicsJointDesc lim_desc = cfg_desc;
        lim_desc.m_BodyA = lim_a;
        lim_desc.m_BodyB = lim_b;
        lim_desc.m_PivotA = Vector3(0.f, 140.f, 0.f);
        lim_desc.m_PivotB = Vector3(0.f, 140.f, 0.f);
        for (int axis_index = 0; axis_index < kConfigurableAxisCount; ++axis_index)
        {
            lim_desc.m_Motion[axis_index] = ConfigurableJointMotion::Locked;
        }
        const int lim_z = static_cast<int>(ConfigurableJointAxis::TranslationZ);
        lim_desc.m_Motion[lim_z] = ConfigurableJointMotion::Limited;
        lim_desc.m_AxisLimitMin[lim_z] = -0.5f;
        lim_desc.m_AxisLimitMax[lim_z] = 0.f;
        const uint32_t lim_id = scene->CreateJoint(lim_desc);
        Check(lim_id != s_InvalidJointId, "CF10", "a joint with one Limited translation axis can be created");
        StepScene(*scene, 60);
        Vector3 lim_position = Vector3::ZERO;
        Check(scene->GetRigidBodyPosition(lim_b, lim_position) && std::isfinite(lim_position.z) &&
                  lim_position.z < -0.35f && lim_position.z > -0.6f,
              "CF10a", "and the body stops AT the limit: it fell but only 0.5 m (a Free axis would fall forever)");
        Check(std::fabs(lim_position.x) < 0.05f && std::fabs(lim_position.y - 142.f) < 0.05f, "CF10b",
              "and the locked axes really held (no sideways drift)");

        // --- 行为门：驱动真的在驱动 -------------------------------------------
        // ⚠️ 只有**行为**能证明电机是开着的：`GetJointAxisDrive` 读的是弹簧参数，
        // 而参数不管电机开不开都会写下去（这是刻意的，见后端注释）。所以夹具要
        // 看 body 到底动没动。取 X 轴：重力在 -Z，X 上没有别的力，"到位"只可能
        // 来自电机。
        const uint32_t drv_a = SpawnBox(*scene, Vector3(0.f, 160.f, 0.f), 0, false);
        const uint32_t drv_b = SpawnBox(*scene, Vector3(0.f, 162.f, 0.f), 0, true);
        PhysicsJointDesc drv_desc = cfg_desc;
        drv_desc.m_BodyA = drv_a;
        drv_desc.m_BodyB = drv_b;
        drv_desc.m_PivotA = Vector3(0.f, 160.f, 0.f);
        drv_desc.m_PivotB = Vector3(0.f, 160.f, 0.f);
        for (int axis_index = 0; axis_index < kConfigurableAxisCount; ++axis_index)
        {
            drv_desc.m_Motion[axis_index] = ConfigurableJointMotion::Locked;
        }
        const int drv_x = static_cast<int>(ConfigurableJointAxis::TranslationX);
        drv_desc.m_Motion[drv_x] = ConfigurableJointMotion::Free;
        JointDrive move_drive;
        move_drive.positionSpring = 20000.f;   // 临界阻尼附近：ω = √(k/m) ≈ 4.5 rad/s
        move_drive.positionDamper = 9000.f;
        move_drive.maximumForce = 100000.f;
        drv_desc.m_AxisDrive[drv_x] = move_drive;
        drv_desc.m_TargetPosition = Vector3(0.8f, 0.f, 0.f);
        const uint32_t drv_id = scene->CreateJoint(drv_desc);
        Check(drv_id != s_InvalidJointId, "CF11", "a joint with one driven free axis can be created");
        StepScene(*scene, 90);
        Vector3 drv_position = Vector3::ZERO;
        Check(scene->GetRigidBodyPosition(drv_b, drv_position) && std::isfinite(drv_position.x) &&
                  drv_position.x > 0.2f,
              "CF11a", "and the drive really drives: the body moved toward targetPosition (a motor left Off would not budge)");

        // --- 参考系两条轴平行 = NaN -------------------------------------------
        // 与 swing-twist 那条 `PlaneAxisFor` 同一条路：Jolt 只叉乘一次，
        // cross(parallel, parallel) 是零向量，后面 `GetQuaternion()` 拿到退化矩阵
        // ⇒ 姿态 NaN，一步之后整个 broadphase 被拖垮。
        const uint32_t par_a = SpawnBox(*scene, Vector3(0.f, 150.f, 0.f), 0, false);
        const uint32_t par_b = SpawnBox(*scene, Vector3(0.f, 152.f, 0.f), 0, true);
        PhysicsJointDesc par_desc = cfg_desc;
        par_desc.m_BodyA = par_a;
        par_desc.m_BodyB = par_b;
        par_desc.m_PivotA = Vector3(0.f, 150.f, 0.f);
        par_desc.m_PivotB = Vector3(0.f, 150.f, 0.f);
        par_desc.m_SecondaryAxisA = Vector3(1.f, 0.f, 0.f);   // 与 axis（+X）平行
        par_desc.m_SecondaryAxisB = Vector3(1.f, 0.f, 0.f);
        const uint32_t par_id = scene->CreateJoint(par_desc);
        Check(par_id != s_InvalidJointId, "CF12",
              "a secondaryAxis parallel to axis does NOT poison the constraint");
        StepScene(*scene, 3);
        Vector3 par_position = Vector3::ZERO;
        Check(scene->GetRigidBodyPosition(par_b, par_position) && std::isfinite(par_position.x) &&
                  std::isfinite(par_position.y) && std::isfinite(par_position.z),
              "CF12a", "and a few steps later the body is still finite (a NaN frame poisons everything)");

        // --- 组件层 -----------------------------------------------------------
        ConfigurableJointComponent* cfg_joint = new ConfigurableJointComponent();
        om->AllocateAndAssignInstanceID(cfg_joint);
        actor_a.object->addComponent(cfg_joint);
        cfg_joint->PostLoadResource(actor_a.object);
        cfg_joint->SetConnectedBody(actor_b.body);
        Check(cfg_joint->HasJoint() &&
                  scene->GetJointType(cfg_joint->GetJointId(), read_type) &&
                  read_type == PhysicsJointType::configurable,
              "CF20", "a ConfigurableJointComponent builds a six-DOF constraint");
        Check(cfg_joint->GetXMotion() == ConfigurableJointMotion::Free &&
                  cfg_joint->GetYMotion() == ConfigurableJointMotion::Free &&
                  cfg_joint->GetAngularZMotion() == ConfigurableJointMotion::Free,
              "CF21", "the six motions default to Free");
        Check(Near(cfg_joint->GetSecondaryAxis().y, 1.f), "CF22",
              "secondaryAxis defaults to +Y (Unity's default)");
        Check(Near(cfg_joint->GetLinearLimit(), 0.f) && Near(cfg_joint->GetAngularYLimit(), 0.f),
              "CF22a", "and the limits default to 0");

        const uint32_t cfg_component_id = cfg_joint->GetJointId();
        Check(cfg_joint->SetXMotion(ConfigurableJointMotion::Locked) &&
                  cfg_joint->GetXMotion() == ConfigurableJointMotion::Locked,
              "CF23", "xMotion round-trips through the component");
        // ⚠️ 改模式**不**重建（后端有 setter），与改参考系那条相反。
        Check(cfg_joint->GetJointId() == cfg_component_id, "CF23a",
              "and changing a motion does NOT rebuild the joint (unlike the frame axes)");
        // 光读组件字段不够 —— 组件可以只是记住了一个数。直接问后端。
        Check(scene->GetJointAxisMotion(cfg_component_id, ConfigurableJointAxis::TranslationX,
                                        read_motion, read_lo, read_hi) &&
                  read_motion == ConfigurableJointMotion::Locked,
              "CF23b", "the backend really heard it (the component is not just remembering a field)");
        // 改参考系要重建（它是构造时烘进 `mConstraintToBody1/2` 的）。
        cfg_joint->SetSecondaryAxis(Vector3(0.f, 0.f, 1.f));
        Check(Near(cfg_joint->GetSecondaryAxis().z, 1.f) && cfg_joint->GetJointId() != cfg_component_id,
              "CF24", "changing secondaryAxis REBUILDS the joint (the frame is baked in)");

        // 限位：linearLimit 三个平移轴共用，只在 Limited 的轴上可见。
        Check(cfg_joint->SetZMotion(ConfigurableJointMotion::Limited), "CF25", "zMotion can be Limited");
        Check(!cfg_joint->SetLinearLimit(-1.f), "CF25a", "a negative linearLimit is refused");
        Check(cfg_joint->SetLinearLimit(0.25f) && Near(cfg_joint->GetLinearLimit(), 0.25f),
              "CF25b", "and a non-negative one round-trips");
        Check(scene->GetJointAxisMotion(cfg_joint->GetJointId(), ConfigurableJointAxis::TranslationZ,
                                        read_motion, read_lo, read_hi) &&
                  read_motion == ConfigurableJointMotion::Limited && Near(read_lo, -0.25f) &&
                  Near(read_hi, 0.25f),
              "CF25c", "the shared linear limit reaches the backend on the Limited axis");

        // 旋转限位：X 可以不对称（必须跨 0），Y / Z 只收半角。
        Check(!cfg_joint->SetAngularXLimits(0.2f, 0.5f), "CF26",
              "an angular X range that misses zero is refused");
        Check(cfg_joint->SetAngularXLimits(-0.2f, 0.5f) &&
                  Near(cfg_joint->GetLowAngularXLimit(), -0.2f) &&
                  Near(cfg_joint->GetHighAngularXLimit(), 0.5f),
              "CF26a", "and one that straddles zero round-trips");
        Check(cfg_joint->SetAngularXMotion(ConfigurableJointMotion::Limited) &&
                  scene->GetJointAxisMotion(cfg_joint->GetJointId(), ConfigurableJointAxis::RotationX,
                                            read_motion, read_lo, read_hi) &&
                  read_motion == ConfigurableJointMotion::Limited && Near(read_lo, -0.2f) &&
                  Near(read_hi, 0.5f),
              "CF26b", "the ASYMMETRIC X range reaches the backend (only X may be asymmetric)");
        Check(!cfg_joint->SetAngularYLimit(-0.1f), "CF27", "a negative angularYLimit is refused");
        Check(cfg_joint->SetAngularYLimit(0.3f) && cfg_joint->SetAngularZLimit(0.15f) &&
                  cfg_joint->SetAngularYMotion(ConfigurableJointMotion::Limited) &&
                  cfg_joint->SetAngularZMotion(ConfigurableJointMotion::Limited),
              "CF27a", "Y and Z take independent half angles");
        // ⚠️ 这条是 `ESwingType::Pyramid` 那道门：Cone 会把 Y / Z **强行**改成
        // 对称并且只留 max，于是 Z 会变成 0.3 而不是 0.15 —— 而 Unity 的
        // angularYLimit / angularZLimit 本来就是各自独立的。
        float y_hi = -1.f;
        float z_hi = -1.f;
        Check(scene->GetJointAxisMotion(cfg_joint->GetJointId(), ConfigurableJointAxis::RotationY,
                                        read_motion, read_lo, y_hi) &&
                  scene->GetJointAxisMotion(cfg_joint->GetJointId(), ConfigurableJointAxis::RotationZ,
                                            read_motion, read_lo, z_hi) &&
                  Near(y_hi, 0.3f) && Near(z_hi, 0.15f),
              "CF27b", "Y and Z keep DIFFERENT limits (Cone would have squashed them into one radius — hence Pyramid)");

        // 驱动
        JointDrive comp_drive;
        comp_drive.positionSpring = 80.f;
        comp_drive.positionDamper = 4.f;
        comp_drive.maximumForce = 300.f;
        Check(cfg_joint->SetXDrive(comp_drive) && Near(cfg_joint->GetXDrive().positionSpring, 80.f) &&
                  Near(cfg_joint->GetXDrive().maximumForce, 300.f),
              "CF28", "xDrive round-trips through the component");
        JointDrive comp_drive_read;
        Check(scene->GetJointAxisDrive(cfg_joint->GetJointId(), ConfigurableJointAxis::TranslationX,
                                       comp_drive_read) &&
                  Near(comp_drive_read.positionSpring, 80.f),
              "CF28a", "and reaches the backend");
        JointDrive yz_drive;
        yz_drive.positionSpring = 60.f;
        yz_drive.positionDamper = 3.f;
        yz_drive.maximumForce = 200.f;
        JointDrive y_read;
        JointDrive z_read;
        Check(cfg_joint->SetAngularYZDrive(yz_drive) &&
                  scene->GetJointAxisDrive(cfg_joint->GetJointId(), ConfigurableJointAxis::RotationY, y_read) &&
                  scene->GetJointAxisDrive(cfg_joint->GetJointId(), ConfigurableJointAxis::RotationZ, z_read) &&
                  Near(y_read.positionSpring, 60.f) && Near(z_read.positionSpring, 60.f),
              "CF29", "angularYZDrive drives BOTH Y and Z (one field, two axes)");
        JointDrive bad_component_drive;
        bad_component_drive.positionSpring = 10.f;
        bad_component_drive.maximumForce = -5.f;
        Check(!cfg_joint->SetXDrive(bad_component_drive), "CF29a",
              "a negative maximumForce is refused (and the res is left alone)");
        // 枚举是从 TS 的 number 来的，可以越界 —— 越界值要拒掉，而不是存下来之后
        // 让 getter 报一个没有任何 case 认它的值。
        Check(!cfg_joint->SetXMotion(static_cast<ConfigurableJointMotion>(99)) &&
                  cfg_joint->GetXMotion() == ConfigurableJointMotion::Locked,
              "CF29b", "an out-of-range motion is refused and the stored value is left alone");

        // 目标量与线性限位弹簧
        cfg_joint->SetTargetPosition(Vector3(0.f, 1.f, 0.f));
        Check(Near(cfg_joint->GetTargetPosition().y, 1.f), "CF30", "targetPosition round-trips");
        Check(scene->GetJointTarget(cfg_joint->GetJointId(), read_target_position, read_target_velocity,
                                    read_target_angular_velocity, read_target_rotation) &&
                  Near(read_target_position.y, 1.f),
              "CF30a", "and reaches the backend");
        SoftJointLimitSpring comp_spring;
        comp_spring.spring = 50.f;
        comp_spring.damper = 2.f;
        Check(cfg_joint->SetLinearLimitSpring(comp_spring) &&
                  Near(cfg_joint->GetLinearLimitSpring().spring, 50.f),
              "CF31", "linearLimitSpring round-trips through the component");
        Check(scene->GetJointLinearLimitSpring(cfg_joint->GetJointId(), read_lls_spring, read_lls_damper) &&
                  Near(read_lls_spring, 50.f) && Near(read_lls_damper, 2.f),
              "CF31a", "and reaches the backend");

        // =====================================================================
        // 3. 接线门（源码级）：证明脚本面真的被发布了、flush 真的被调了
        // =====================================================================

        const std::string dts = ReadFile("Source/Runtime/Scripting/ScriptVM/Test/Typings/zengine.d.ts");
        Check(!dts.empty(), "JN90", "the generated zengine.d.ts is readable");
        Check(CountOccurrences(dts, "declare class HingeJointComponent extends JointComponent") >= 1, "JN91",
              "HingeJointComponent is published to scripts and inherits the Joint facade");
        Check(CountOccurrences(dts, "declare class FixedJointComponent extends JointComponent") >= 1, "JN92",
              "and so is FixedJointComponent (it needs its own members to be published at all)");
        Check(CountOccurrences(dts, "declare class SpringJointComponent extends JointComponent") >= 1, "JN93",
              "and SpringJointComponent");
        Check(CountOccurrences(dts, "declare class SliderJointComponent extends JointComponent") >= 1, "JN94",
              "and SliderJointComponent");
        Check(CountOccurrences(dts, "declare type HingeJoint = HingeJointComponent;") >= 1, "JN95",
              "Unity's spelling (HingeJoint) is a type alias, so `let j: HingeJoint` compiles");
        Check(CountOccurrences(dts, "declare interface JointLimits {") >= 1 &&
                  CountOccurrences(dts, "declare interface JointMotor {") >= 1 &&
                  CountOccurrences(dts, "declare interface JointSpring {") >= 1,
              "JN96", "Unity's three joint value types are declared for scripts");

        // ⚠️ 上面那组读的是**签进仓库的那份** d.ts。它只在有人跑
        // `ScriptTsChainSmokeTest --dump-dts` 时才更新，所以"把 prelude 里的
        // 别名删掉"这类改动在那里是看不见的（变异体 JN31/JN32 就是这么活下来
        // 的）。发布面本身住在 ScriptBinding.cpp 的 prelude 里 —— 那条才是源头，
        // 这里直接在源头上查一遍。
        const std::string prelude = ReadFile("Source/Runtime/Scripting/Binding/ScriptBinding.cpp");
        Check(!prelude.empty(), "JN96b", "ScriptBinding.cpp is readable");
        Check(CountOccurrences(prelude, "declare type HingeJoint = HingeJointComponent;") >= 1 &&
                  CountOccurrences(prelude, "declare type FixedJoint = FixedJointComponent;") >= 1 &&
                  CountOccurrences(prelude, "declare type SpringJoint = SpringJointComponent;") >= 1 &&
                  CountOccurrences(prelude, "declare type SliderJoint = SliderJointComponent;") >= 1,
              "JN96c", "all four Unity spellings are emitted as aliases from the prelude");
        Check(CountOccurrences(prelude, "declare interface JointLimits {") >= 1 &&
                  CountOccurrences(prelude, "declare interface JointMotor {") >= 1 &&
                  CountOccurrences(prelude, "declare interface JointSpring {") >= 1,
              "JN96d", "and so are the three joint value types");

        // =====================================================================
        // 渲染器：ZCLASS 类"静默消失"这个坑
        // =====================================================================
        // ⚠️ ZCLASS 类若既无 ZFUNCTION 也无 ZSCRIPTPROPERTY，codegen 不发 ⇒
        // **不进 d.ts**。MeshRenderer / SkinMeshRenderer 之前就是这么消失的：
        // 引擎里有类、Inspector 能挂、C++ 能拿到，但脚本里连 `MeshRenderer`
        // 这个名字都不存在，`GetComponent(MeshRenderer)` 无从谈起 —— 而构建
        // 零警告、测试零失败。
        Check(CountOccurrences(dts, "declare class MeshRenderer extends BaseRenderer") >= 1, "MR1",
              "MeshRenderer reaches the d.ts (a ZCLASS with no ZFUNCTION is emitted as nothing at all)");
        Check(CountOccurrences(dts, "declare class SkinMeshRenderer extends BaseRenderer") >= 1, "MR2",
              "and so does SkinMeshRenderer (Unity spells it SkinnedMeshRenderer)");
        // 光查 d.ts 不够：它是**生成物**。源头在头文件里，所以再查一遍源头 ——
        // "把那个 ZFUNCTION 删掉"的改动在生成物上要等下一次 regen 才看得见。
        const std::string mesh_renderer_h =
            ReadFile("Source/Runtime/Function/Framework/Component/Mesh/MeshRenderer.h");
        Check(!mesh_renderer_h.empty() && CountOccurrences(mesh_renderer_h, "ZFUNCTION()") >= 1 &&
                  CountOccurrences(mesh_renderer_h, "bool HasMeshFilter() const;") >= 1,
              "MR3", "MeshRenderer.h really carries a ZFUNCTION (without one the class is never emitted)");
        const std::string skin_renderer_h =
            ReadFile("Source/Runtime/Function/Framework/Component/Mesh/SkinMeshRenderer.h");
        Check(!skin_renderer_h.empty() && CountOccurrences(skin_renderer_h, "ZFUNCTION()") >= 1 &&
                  CountOccurrences(skin_renderer_h, "bool HasAnimator() const;") >= 1,
              "MR4", "and so does SkinMeshRenderer.h");
        // Unity 拼作 `SkinnedMeshRenderer`（两个 n），引擎里是 `SkinMeshRenderer`
        // —— 所以还需要一条**类型别名**。别名是 prelude 里手写的，同样两头查。
        Check(CountOccurrences(dts, "declare type SkinnedMeshRenderer = SkinMeshRenderer;") >= 1, "MR5",
              "Unity's SkinnedMeshRenderer spelling reaches the d.ts as a type alias");
        Check(CountOccurrences(prelude, "declare type SkinnedMeshRenderer = SkinMeshRenderer;") >= 1, "MR6",
              "and the alias really is emitted from the prelude in ScriptBinding.cpp (the d.ts is a build artifact)");

        // --- ConfigurableJoint 的发布面 ---------------------------------------
        // ⚠️ d.ts 是**生成物**、不入 git，只查它会"还没 regen 就绿的"骗过去；所以
        // 每条都配一个**源码级**的孪生断言。
        Check(CountOccurrences(dts, "declare class ConfigurableJointComponent extends JointComponent") >= 1,
              "CF40", "ConfigurableJointComponent is published to scripts and inherits the Joint facade");
        Check(CountOccurrences(dts, "declare enum ConfigurableJointMotion") >= 1, "CF41",
              "and so is the ConfigurableJointMotion enum (without ZENUM the enum is silently skipped)");
        Check(CountOccurrences(dts, "declare interface JointDrive {") >= 1, "CF42",
              "and the JointDrive value type");
        Check(CountOccurrences(dts, "declare type ConfigurableJoint = ConfigurableJointComponent;") >= 1,
              "CF43", "Unity's ConfigurableJoint spelling is published as a type alias");
        Check(CountOccurrences(dts, "ConfigurableJointMotion;") >= 6, "CF44",
              "the six motion accessors really carry the enum type (not a plain number)");
        // 源码级孪生
        const std::string joint_value_h = ReadFile("Source/Runtime/Function/Physics/PhysicsJoint.h");
        Check(!joint_value_h.empty() && CountOccurrences(joint_value_h, "ZENUM()") >= 1 &&
                  CountOccurrences(joint_value_h, "enum class ConfigurableJointMotion") >= 1,
              "CF45", "PhysicsJoint.h really annotates the enum with ZENUM (the d.ts is generated from it)");
        Check(CountOccurrences(prelude, "declare interface JointDrive {") >= 1 &&
                  CountOccurrences(prelude, "declare type ConfigurableJoint = ConfigurableJointComponent;") >= 1,
              "CF46", "and ScriptBinding.cpp really emits the interface and the alias");
        const std::string binding_h_cfg = ReadFile("Source/Runtime/Scripting/Binding/ScriptBinding.h");
        Check(!binding_h_cfg.empty() && CountOccurrences(binding_h_cfg, "struct Converter<JointDrive>") >= 1,
              "CF47", "and ScriptBinding.h carries a marshaller for JointDrive");
        // 后端那些"必须这么写"的地方，各钉一条源码级断言 —— 它们都没有行为上的
        // 单一判据（都只在特定夹具下才分叉），只能查源头。
        const std::string scene_source = ReadFile("Source/Runtime/Function/Physics/PhysicsScene.cpp");
        Check(!scene_source.empty(), "CF48", "PhysicsScene.cpp is readable");
        Check(CountOccurrences(scene_source, "JPH::ESwingType::Pyramid") >= 1, "CF48a",
              "the swing type really is Pyramid (Cone would force Y and Z to share one radius)");
        Check(CountOccurrences(scene_source, "JPH::MotorSettings(JPH::ESpringMode::StiffnessAndDamping, 0.f, 0.f)") >= 1,
              "CF48b", "CreateJoint zeroes the motor settings (Jolt's 2 Hz default would read back as a phantom spring)");
        Check(CountOccurrences(scene_source, "JPH::EMotorState::PositionAndVelocity") >= 1, "CF48c",
              "an enabled drive uses PositionAndVelocity (that is what makes positionDamper mean Unity's damper)");
        Check(CountOccurrences(scene_source, "case PhysicsJointType::configurable:") >= 1 &&
                  CountOccurrences(scene_source, "JPH::SixDOFConstraintSettings settings;") >= 1,
              "CF48d", "CreateJoint has a configurable branch that builds a SixDOFConstraint");
        Check(CountOccurrences(scene_source, "min.SetComponent(component, new_min);") >= 1 &&
                  CountOccurrences(scene_source, "max.SetComponent(component, new_max);") >= 1,
              "CF48e", "the per-axis limit write is a read-modify-write (Jolt's setter takes the whole Vec3)");
        const std::string null_scene_cfg = ReadFile("Source/Runtime/Function/Physics/PhysicsSceneNull.cpp");
        Check(CountOccurrences(null_scene_cfg, "PhysicsScene::SetJointAxisMotion(uint32_t, ConfigurableJointAxis") >= 1 &&
                  CountOccurrences(null_scene_cfg, "PhysicsScene::SetJointAxisDrive(uint32_t, ConfigurableJointAxis") >= 1 &&
                  CountOccurrences(null_scene_cfg, "PhysicsScene::GetJointLinearLimitSpring(uint32_t, float&") >= 1,
              "CF49", "the no-physics build defines the new symbols too (a missing one is a link error there)");

        const std::string binding = ReadFile("Source/Runtime/Scripting/Binding/ScriptBinding.h");
        Check(!binding.empty(), "JN97", "ScriptBinding.h is readable");
        Check(CountOccurrences(binding, "struct Converter<JointLimits>") >= 1 &&
                  CountOccurrences(binding, "struct Converter<JointMotor>") >= 1 &&
                  CountOccurrences(binding, "struct Converter<JointSpring>") >= 1,
              "JN98", "and each of them has a marshaller (without one the method vanishes from the .d.ts)");

        const std::string world_source = ReadFile("Source/Runtime/Function/Framework/World/WorldManager.cpp");
        Check(!world_source.empty(), "JN99", "WorldManager.cpp is readable");
        const std::size_t at_rigid_flush = world_source.find("FlushPendingRigidBodies(new_level)");
        const std::size_t at_joint_flush = world_source.find("FlushPendingJoints(new_level)");
        Check(at_rigid_flush != std::string::npos && at_joint_flush != std::string::npos &&
                  at_joint_flush > at_rigid_flush,
              "JNA0", "deferred joints are flushed AFTER deferred bodies (a joint needs both bodies)");

        const std::string null_scene = ReadFile("Source/Runtime/Function/Physics/PhysicsSceneNull.cpp");
        Check(!null_scene.empty(), "JNA1", "PhysicsSceneNull.cpp is readable");
        // ⚠️ 记号必须带参数表。只写 `PhysicsScene::CreateStaticAnchorBody` 的话，
        // 一个把它改名成 `CreateStaticAnchorBodyRenamed` 的改动**照样命中**
        // （前缀匹配）—— 那条断言就恒真了。
        Check(CountOccurrences(null_scene, "PhysicsScene::CreateStaticAnchorBody(const Vector3&)") >= 1 &&
                  CountOccurrences(null_scene,
                                   "PhysicsScene::SetJointPositionMotor(uint32_t, bool, float, float, float, "
                                   "float)") >= 1 &&
                  CountOccurrences(null_scene, "PhysicsScene::GetJointImpulse(uint32_t, float&") >= 1,
              "JNA2", "the no-physics build defines the new symbols too (a missing one is EXC_BAD_ACCESS)");

        // --- CharacterJoint ---------------------------------------------------
        Check(CountOccurrences(dts, "declare class CharacterJointComponent extends JointComponent") >= 1,
              "CJW0", "CharacterJointComponent is published to scripts");
        Check(CountOccurrences(dts, "declare type CharacterJoint = CharacterJointComponent;") >= 1, "CJW1",
              "Unity's spelling (CharacterJoint) is a type alias");
        Check(CountOccurrences(dts, "declare interface SoftJointLimitSpring {") >= 1, "CJW2",
              "and Unity's SoftJointLimitSpring is declared (it has NO targetPosition)");
        Check(CountOccurrences(prelude, "declare type CharacterJoint = CharacterJointComponent;") >= 1 &&
                  CountOccurrences(prelude, "declare interface SoftJointLimitSpring {") >= 1,
              "CJW3", "both come out of the prelude itself (the d.ts on disk is a generated artifact)");
        Check(CountOccurrences(binding, "struct Converter<SoftJointLimitSpring>") >= 1, "CJW4",
              "and the value type has a marshaller (without one the accessors vanish from the .d.ts)");
        // ⚠️ 带参数表：`PhysicsScene::SetJointSwingTwistLimits` 会被
        // `...LimitsRenamed` 前缀命中 ⇒ "把符号改名"这种改动照样绿。
        Check(CountOccurrences(null_scene, "PhysicsScene::SetJointSwingTwistLimits(uint32_t, float, float, "
                                           "float, float)") >= 1 &&
                  CountOccurrences(null_scene, "PhysicsScene::GetJointSwingTwistLimits(uint32_t, float&, "
                                               "float&, float&, float&)") >= 1 &&
                  CountOccurrences(null_scene, "PhysicsScene::SetJointSwingTwistSpring(uint32_t, float, "
                                               "float, float, float)") >= 1 &&
                  CountOccurrences(null_scene, "PhysicsScene::GetJointSwingTwistSpring(uint32_t, float&, "
                                               "float&, float&, float&)") >= 1,
              "CJW5", "the no-physics build defines the cone symbols too");
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
        // +2 / +1 = `bounds` 那一档（`GetBounds` + `ClosestPointOnBounds` 两个
        // 方法、`bounds` 一个属性）。用**精确等号**而不是 >=：这个门存在的意义
        // 就是"以后谁偷偷往 Collider 上加东西，这里会红一下让人回头看一眼"，
        // 改成 >= 就永远绿了。
        Check(CountOccurrences(collider_header, "ZFUNCTION(") == 19, "AD11",
              "Collider exposes exactly 19 script methods");
        Check(CountOccurrences(collider_header, "ZSCRIPTPROPERTY(") == 7, "AD12",
              "and exactly 7 script properties");
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
        Check(CountOccurrences(math_header, "ZFUNCTION()") == 101, "AJ1",
              "ScriptMath declares exactly 101 script methods (a dropped ZFUNCTION is a silent API loss)");
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
        Check(CountOccurrences(math_header, "ZFUNCTION()") == 101, "AL1",
              "ScriptMath declares exactly 101 script methods");
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

    // --------------------------------------------------------------- QE ----
    // Unity 的 `Quaternion.Euler` / `q.eulerAngles`。
    //
    // 这一族以前是**故意空着**的（ScriptMath.h 里写着"反向要先定死旋转顺序，定错
    // 一样是静默算错"）。现在定的是引擎自己的 X -> Y -> Z（矩阵 Rz·Ry·Rx），
    // 因为它必须与 `Transform.eulerAngles` **同源**：两套欧拉角约定一旦漂移，
    // `Quaternion.Euler(v)` 与 `transform.eulerAngles = v` 会给出两个不同的朝向，
    // 而脚本里看不出来 —— 这正是"定错 = 静默算错"的那一类。
    //
    // ⚠️ 夹具一律**不对称**：(20, 30, 40) 而不是 (30, 30, 30)。对称夹具下
    // "把顺序写成 Y->X->Z"和正确答案在数值上碰巧相等，是最容易漏的一种。
    void TestQuaternionEuler()
    {
        const Vector3 unit_x = Vector3::UNIT_X;
        const Vector3 unit_y = Vector3::UNIT_Y;
        const Vector3 unit_z = Vector3::UNIT_Z;

        auto q_near = [](const Quaternion& a, const Quaternion& b, float eps = 1e-4f)
        {
            // 四元数取反表示同一个旋转，所以两个方向都要认。
            return (std::fabs(a.w - b.w) < eps && std::fabs(a.x - b.x) < eps &&
                    std::fabs(a.y - b.y) < eps && std::fabs(a.z - b.z) < eps) ||
                   (std::fabs(a.w + b.w) < eps && std::fabs(a.x + b.x) < eps &&
                    std::fabs(a.y + b.y) < eps && std::fabs(a.z + b.z) < eps);
        };
        auto v_near = [](const Vector3& a, const Vector3& b, float eps = 1e-3f)
        {
            return std::fabs(a.x - b.x) < eps && std::fabs(a.y - b.y) < eps &&
                   std::fabs(a.z - b.z) < eps;
        };
        auto q_len = [](const Quaternion& q)
        {
            return std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
        };
        auto axis_angle = [](float degrees, const Vector3& axis)
        {
            Quaternion q;
            q.FromAngleAxis(Radian(degrees * Math_fDeg2Rad), axis);
            return q;
        };

        // --- 退化与单轴 ------------------------------------------------
        Check(q_near(Quaternion::FromEulerAngles(Vector3(0.f, 0.f, 0.f)), Quaternion::IDENTITY),
              "QT0", "Euler(0,0,0) is the identity");
        // 单轴时必须与 AngleAxis 完全一致 —— 这是"x 绕 X、y 绕 Y、z 绕 Z"的判据。
        Check(q_near(Quaternion::FromEulerAngles(Vector3(37.f, 0.f, 0.f)),
                     axis_angle(37.f, unit_x)), "QT1", "x rotates about X");
        Check(q_near(Quaternion::FromEulerAngles(Vector3(0.f, 37.f, 0.f)),
                     axis_angle(37.f, unit_y)), "QT2", "y rotates about Y");
        Check(q_near(Quaternion::FromEulerAngles(Vector3(0.f, 0.f, 37.f)),
                     axis_angle(37.f, unit_z)), "QT3", "z rotates about Z");

        // --- ⚠️ 合成顺序：必须是 qz * qy * qx -------------------------------
        const Quaternion euler_20_30_40 =
            Quaternion::FromEulerAngles(Vector3(20.f, 30.f, 40.f));
        const Quaternion order_xyz =
            axis_angle(40.f, unit_z) * axis_angle(30.f, unit_y) * axis_angle(20.f, unit_x);
        Check(q_near(euler_20_30_40, order_xyz), "QT4",
              "the composition is qz*qy*qx (X first, then Y, then Z)");
        // 互补的一半：写成"照抄参数顺序的 qz... 反过来"必须**不**相等。只测上面
        // 那条的话，任何顺序都能被 q_near 的 ± 判断蒙混过去？不能 —— 但少了这条
        // 就分不出"顺序对了"和"顺序反了但碰巧对称"。
        const Quaternion order_zyx =
            axis_angle(20.f, unit_x) * axis_angle(30.f, unit_y) * axis_angle(40.f, unit_z);
        Check(!q_near(euler_20_30_40, order_zyx), "QT5",
              "and NOT the other order (the fixture is asymmetric on purpose)");
        // 顺序的**可观测**后果：交换两次旋转，结果不同。
        Check(!q_near(Quaternion::FromEulerAngles(Vector3(90.f, 90.f, 0.f)),
                      Quaternion::FromEulerAngles(Vector3(0.f, 90.f, 90.f))), "QT6",
              "Euler(90,90,0) != Euler(0,90,90)");

        // --- ⚠️ 90° 时基向量必须落到另一根基向量上（右手系）------------------
        // 绕 +X 转 +90°：Y -> Z。这条不是抄实现，是右手系的定义；写反成 Y -> -Z
        // 就是"欧拉角整体镜像"的那种静默错。
        Check(v_near((Quaternion::FromEulerAngles(Vector3(90.f, 0.f, 0.f)) * unit_y),
                     unit_z), "QT7", "90 about X takes +Y to +Z");
        Check(v_near((Quaternion::FromEulerAngles(Vector3(0.f, 0.f, 90.f)) * unit_x),
                     unit_y), "QT8", "90 about Z takes +X to +Y");
        Check(v_near((Quaternion::FromEulerAngles(Vector3(0.f, 90.f, 0.f)) * unit_z),
                     unit_x), "QT9", "90 about Y takes +Z to +X");

        // --- 往返：ToEulerAngles(FromEulerAngles(v)) ≈ v ---------------------
        // 两组不对称的值。只测一组的话，"把 x 和 z 换一下"这种实现在对称夹具下
        // 是绿的。
        const Vector3 samples[2] = {Vector3(10.f, 20.f, 30.f), Vector3(-35.f, 70.f, -15.f)};
        for (int i = 0; i < 2; ++i)
        {
            const Vector3 back = Quaternion::ToEulerAngles(
                Quaternion::FromEulerAngles(samples[i]));
            Check(v_near(back, samples[i], 1e-2f), ("QT10_" + std::to_string(i)).c_str(),
                  "the Euler round trip is the identity");
        }
        // 归一化：任意输入出来的都必须是单位四元数（Euler 里显式 normalise 了）。
        Check(std::fabs(q_len(Quaternion::FromEulerAngles(Vector3(123.f, -456.f, 789.f))) -
                        1.f) < 1e-4f, "QT11",
              "FromEulerAngles returns a unit quaternion for any input");

        // 万向锁：|y| == 90 时 x 与 z 无法分离，惯例是 x 取 0。
        const Vector3 locked = Quaternion::ToEulerAngles(
            Quaternion::FromEulerAngles(Vector3(30.f, 90.f, 45.f)));
        Check(std::fabs(std::fabs(locked.y) - 90.f) < 1e-2f && std::fabs(locked.x) < 1e-2f, "QT12",
              "gimbal lock puts everything on z and zeroes x (Unity's convention)");

        // --- 脚本面 ---------------------------------------------------------
        Check(q_near(Runtime::Quaternion::Euler(20.f, 30.f, 40.f), euler_20_30_40), "QT13",
              "Quaternion.Euler(x,y,z) is the same rotation");
        Check(q_near(Runtime::Quaternion::EulerVector(Vector3(20.f, 30.f, 40.f)),
                     euler_20_30_40), "QT14", "and so is EulerVector(v)");
        Check(v_near(Runtime::Quaternion::GetEulerAngles(euler_20_30_40),
                     Vector3(20.f, 30.f, 40.f), 1e-2f), "QT15",
              "GetEulerAngles reads it back");
        // Normalize：零四元数必须退回单位而不是 NaN（NaN 参与的比较恒为 false，
        // 最后表现为"物体莫名不转了"）。
        const Quaternion normalized_zero = Runtime::Quaternion::Normalize(Quaternion::ZERO);
        Check(q_near(normalized_zero, Quaternion::IDENTITY) &&
                  !std::isnan(normalized_zero.w), "QT16",
              "Normalize of a zero quaternion is the identity, not a NaN");
        Quaternion scaled = axis_angle(30.f, unit_y);
        scaled.w *= 5.f; scaled.x *= 5.f; scaled.y *= 5.f; scaled.z *= 5.f;
        Check(std::fabs(q_len(Runtime::Quaternion::Normalize(scaled)) - 1.f) < 1e-4f, "QT17",
              "Normalize really normalizes");
        // Mul：先转 b 再转 a。
        const Quaternion qa = axis_angle(40.f, unit_x);
        const Quaternion qb = axis_angle(25.f, unit_z);
        Check(v_near((Runtime::Quaternion::Mul(qa, qb) * unit_y),
                     (qa * (qb * unit_y))), "QT18",
              "Mul(a,b) applies b first, then a");
        // Nlerp：端点必须精确命中。
        Check(q_near(Runtime::Quaternion::Nlerp(qa, qb, 0.f), qa) &&
                  q_near(Runtime::Quaternion::Nlerp(qa, qb, 1.f), qb), "QT19",
              "Nlerp hits both endpoints");
        // ⚠️ Unclamped 的意义就在 t > 1：必须**超过** b，而不是被夹住。
        const float span = Runtime::Quaternion::Angle(qa, qb);
        Check(Runtime::Quaternion::Angle(qa, Runtime::Quaternion::SlerpUnclamped(qa, qb, 2.f)) >
                  span + 1.f, "QT20", "SlerpUnclamped extrapolates past b");
        Check(Runtime::Quaternion::Angle(qa, Runtime::Quaternion::LerpUnclamped(qa, qb, 2.f)) >
                  span + 1.f, "QT21", "and so does LerpUnclamped");
        // 反过来的一半：夹住的那两个不能超过。
        Check(Runtime::Quaternion::Angle(qa, Runtime::Quaternion::Slerp(qa, qb, 2.f)) <
                  span + 1.f, "QT22", "while Slerp clamps t");
        // 180° 的 Euler 与 identity 的夹角就是 180。
        Check(std::fabs(Runtime::Quaternion::Angle(
                  Quaternion::IDENTITY, Runtime::Quaternion::Euler(0.f, 180.f, 0.f)) - 180.f) <
                  1e-2f, "QT23", "Euler(0,180,0) is a half turn");
        // Lerp 同样要夹。⚠️ 没有这条的话，"只给 Slerp 加了 ClampT、Lerp 忘了"
        // 完全测不出来 —— 两条实现相邻，抄漏一半是最可能的错。
        Check(Runtime::Quaternion::Angle(qa, Runtime::Quaternion::Lerp(qa, qb, 2.f)) <
                  span + 1.f, "QT24", "and so does Lerp");

        // --- ⚠️ Unclamped 的另一半：不许翻到短弧 ----------------------------
        // `SLerp` / `NLerp` 的 `shortest_path` 打开时，dot < 0 会把 b 取反再插。
        // 取反是**同一个旋转**，所以"看朝向"分不出来 —— 只有看 dot 的符号能看见。
        const Quaternion qn_a = Quaternion::IDENTITY;
        const Quaternion qn_b = axis_angle(190.f, unit_y);
        // 夹具自检：这一对的 dot 必须真的是负的，否则下面两条是空转的。
        Check(Runtime::Quaternion::Dot(qn_a, qn_b) < 0.f, "QT25",
              "(the antiparallel fixture pair really does have a negative dot)");
        Check(Runtime::Quaternion::Dot(
                  Runtime::Quaternion::SlerpUnclamped(qn_a, qn_b, 1.f), qn_b) > 0.f, "QT26",
              "SlerpUnclamped lands on b at t=1, not on -b (shortest_path is off)");
        Check(Runtime::Quaternion::Dot(
                  Runtime::Quaternion::LerpUnclamped(qn_a, qn_b, 1.f), qn_b) > 0.f, "QT27",
              "and so does LerpUnclamped");

        // --- ⚠️ 已知盲区，钉在这里 -----------------------------------------
        // `FromEulerAngles` 末尾那句 `result.normalise()` 是**防御性**的：三个
        // 单位四元数相乘在数学上仍是单位四元数，float 下的漂移实测 < 1e-7，远
        // 小于 QT11 的 1e-4。也就是说**删掉它没有任何测试会红**（变异 QE02 存活）。
        //
        // 这个盲区不打算靠"再加一条断言"补（再紧的容差也只是把同一个结论换个
        // 说法），而是把结论本身固定下来：漂移必须**真的**小于 1e-6。哪天某个
        // 改动让中间量不再是单位（比如 FromAngleAxis 不再归一化），这条会先红，
        // 那时候 normalise 就从"防御"变成"必需"了。
        Check(std::fabs(q_len(Quaternion::FromEulerAngles(Vector3(20.f, 30.f, 40.f))) - 1.f) <
                  1e-6f, "QT28",
              "the composition is unit to 1e-6 even before the defensive normalise");
    }

    void TestQuaternionEulerWiring()
    {
        // ⚠️ 这条是本切片的头条：`Transform` 的欧拉角与 `Quaternion.Euler` 必须
        // 共用**同一个**实现。两边各写一遍的话，"设进去的欧拉角"和
        // "Quaternion.Euler 算出来的朝向"迟早会漂成两回事，而且完全静默。
        const std::string transform_source =
            ReadFile("Source/Runtime/Function/Framework/Component/Transform/Transform.cpp");
        Check(!transform_source.empty(), "QW0", "Transform.cpp is readable");
        if (!transform_source.empty())
        {
            const std::size_t at = transform_source.find("Quaternion EulerToQuaternion");
            Check(at != std::string::npos, "QW1", "Transform still has its euler helper");
            if (at != std::string::npos)
            {
                Check(BodyIsBounded(transform_source, at), "QW2", "its body slice is bounded");
                const std::string body = SliceFunctionBody(transform_source, at);
                Check(CountOccurrences(body, "Quaternion::FromEulerAngles") >= 1, "QW3",
                      "it delegates to Quaternion::FromEulerAngles (one convention, one owner)");
                // 反向门：实现不许再就地展开一遍（展开 = 第二套约定）。
                Check(body.find("UNIT_X") == std::string::npos, "QW4",
                      "and does not inline its own axis composition");
            }
            // ⚠️ **反方向**同样要同源。只盯 EulerToQuaternion 的话，
            // `EulerFromQuaternion` 悄悄退回 `GetYaw/GetPitch/GetRoll`（那三个是
            // Ogre 遗产、按 Y-up 定义）是**完全静默**的：读出来的数还是"某个
            // 欧拉角"，只是不再是你设进去的那个 —— 而且没有一条 QT 会红。
            const std::size_t at_rev = transform_source.find("Vector3 EulerFromQuaternion");
            Check(at_rev != std::string::npos, "QW10", "Transform's reverse euler helper is there");
            if (at_rev != std::string::npos)
            {
                Check(BodyIsBounded(transform_source, at_rev), "QW11", "its body slice is bounded");
                const std::string body_rev = SliceFunctionBody(transform_source, at_rev);
                Check(CountOccurrences(body_rev, "Quaternion::ToEulerAngles") >= 1, "QW12",
                      "and it delegates to Quaternion::ToEulerAngles (the same owner, backwards)");
                Check(body_rev.find("GetYaw") == std::string::npos, "QW13",
                      "and does not fall back to the Y-up GetYaw/GetPitch/GetRoll");
            }
        }
        const std::string script_source =
            ReadFile("Source/Runtime/Scripting/ScriptVector/ScriptVector.cpp");
        Check(!script_source.empty(), "QW5", "ScriptVector.cpp is readable");
        if (!script_source.empty())
        {
            const std::size_t at = script_source.find("::Quaternion Quaternion::Euler(");
            Check(at != std::string::npos, "QW6", "the script face has Euler");
            if (at != std::string::npos)
            {
                Check(BodyIsBounded(script_source, at), "QW7", "its body slice is bounded");
                const std::string body = SliceFunctionBody(script_source, at);
                Check(CountOccurrences(body, "FromEulerAngles") >= 1, "QW8",
                      "and it goes through the same owner (not a private copy)");
            }
        }
        const std::string facade_header =
            ReadFile("Source/Runtime/Scripting/ScriptVector/ScriptVector.h");
        Check(CountOccurrences(facade_header, "static ::Quaternion Euler(") >= 1 &&
                  CountOccurrences(facade_header, "static ::Quaternion EulerVector(") >= 1 &&
                  CountOccurrences(facade_header, "static ::Vector3 GetEulerAngles(") >= 1, "QW9",
              "the three new entries are declared on the facade");
    }

    // --------------------------------------------------------------- FV ----
    // Unity 的 `GeometryUtility.CalculateFrustumPlanes` / `TestPlanesAABB`
    // 与 `UnityEngine.Plane`。
    //
    // 这一组能测，是因为视锥是**纯几何**：拿一张自己拼的 `P * V` 就能验证六个
    // 平面的朝向、深度区间、以及盒子判据。它不依赖 RenderSystem（headless 下恒
    // 为 nullptr），也不依赖任何资产 —— 这正好是 `Renderer.isVisible` 那种
    // "得等真渲染一遍才知道"的判据的对立面。
    //
    // ⚠️ 夹具一律**不对称**：相机放在 `eye = (2, 0, 0)` 而不是原点。相机在原点的
    // 话，"view 矩阵没乘进去"和"乘进去了"对中心点给出同一个答案。
    void TestFrustumCulling()
    {
        const float znear = 0.1f;
        const float zfar = 100.f;

        // --- FV0：先钉死深度区间 ----------------------------------------
        // `Math::MakePerspectiveMatrix` 出来的是 **z/w ∈ [0,1]**（DirectX /
        // Vulkan 默认），不是 OpenGL 的 [-1,1]。这条断言就是"默认参数为什么是
        // kZeroToOne"的理由；将来引擎换约定，它会第一个红。
        {
            const Matrix4x4 proj = Math::MakePerspectiveMatrix(
                Radian(Math::DegreesToRadians(60.f)), 1.f, znear, zfar);
            const Vector4 at_near = proj * Vector4(0.f, 0.f, -znear, 1.f);
            const Vector4 at_far = proj * Vector4(0.f, 0.f, -zfar, 1.f);
            Check(std::fabs(at_near.z / at_near.w - 0.f) < 1e-4f, "FV0",
                  "the engine's perspective matrix maps the near plane to z/w = 0");
            Check(std::fabs(at_far.z / at_far.w - 1.f) < 1e-4f, "FV1",
                  "and the far plane to z/w = 1 (so DepthRange::kZeroToOne is the right default)");
        }

        // 相机在 (2,0,0)，朝 -Y（本引擎的 forward），上为 +Z。
        auto make_frustum = [&](const Vector3& eye, float fov_degrees, float aspect)
        {
            const Matrix4x4 view = Math::MakeLookAtMatrix(eye, eye + Vector3(0.f, -10.f, 0.f),
                                                          Vector3::UNIT_Z);
            const Matrix4x4 proj = Math::MakePerspectiveMatrix(
                Radian(Math::DegreesToRadians(fov_degrees)), aspect, znear, zfar);
            return Frustum::FromViewProjection(proj * view);
        };

        const Frustum f60 = make_frustum(Vector3(2.f, 0.f, 0.f), 60.f, 1.f);

        // --- 六个面各自的判据 -------------------------------------------
        Check(f60.ContainsPoint(Vector3(2.f, -5.f, 0.f)), "FV2", "a point dead ahead is inside");
        Check(!f60.ContainsPoint(Vector3(2.f, 5.f, 0.f)), "FV3", "a point behind the eye is outside");
        Check(!f60.ContainsPoint(Vector3(102.f, -5.f, 0.f)), "FV4", "and so is one far off to the side");
        // 近裁剪：0.05 在 znear=0.1 之前。
        Check(!f60.ContainsPoint(Vector3(2.f, -0.05f, 0.f)), "FV5", "a point in front of near is outside");
        Check(f60.ContainsPoint(Vector3(2.f, -0.11f, 0.f)), "FV6", "and just past near is inside");
        // 远裁剪。
        Check(f60.ContainsPoint(Vector3(2.f, -99.f, 0.f)), "FV7", "a point just inside far is inside");
        Check(!f60.ContainsPoint(Vector3(2.f, -101.f, 0.f)), "FV8", "and just past far is outside");

        // --- ⚠️ 不对称夹具：view 的平移真的算进去了 ------------------------
        // fov 30°、距离 5 时半宽只有 5*tan(15°) ≈ 1.34，所以"正前方"是 (2,-5,0)
        // 而 (0,-5,0) 已经出画。相机若在原点，(0,-5,0) 反而是正中心 —— 这条就是
        // 用来区分这两种实现的。
        const Frustum f30 = make_frustum(Vector3(2.f, 0.f, 0.f), 30.f, 1.f);
        Check(f30.ContainsPoint(Vector3(2.f, -5.f, 0.f)), "FV9", "the narrow frustum is centred on the eye");
        Check(!f30.ContainsPoint(Vector3(0.f, -5.f, 0.f)), "FV10",
              "and the world origin is off-screen (the eye offset is really applied)");

        // --- 盒子 ---------------------------------------------------------
        Check(f60.Intersects(Bounds::FromCenterSize(Vector3(2.f, -5.f, 0.f), Vector3(1.f, 1.f, 1.f))),
              "FV11", "a box in front intersects");
        Check(!f60.Intersects(Bounds::FromCenterSize(Vector3(2.f, 500.f, 0.f), Vector3(1.f, 1.f, 1.f))),
              "FV12", "a box behind the eye does not");
        // ⚠️ 保守性：一个把相机整个包进去的大盒子**算相交**（中心在相机后面也
        // 一样）。这是 TestPlanesAABB 的既定精度，写反成"必须全在锥内"会让
        // 大物体凭空消失。
        Check(f60.Intersects(Bounds::FromCenterSize(Vector3(2.f, 0.f, 0.f), Vector3(100.f, 100.f, 100.f))),
              "FV13", "a box enclosing the camera counts as intersecting (the test is conservative)");

        // --- 退化输入 ------------------------------------------------------
        Check(!f60.HasDegeneratePlane(), "FV14", "a real frustum has no degenerate plane");
        Check(Frustum::FromViewProjection(Matrix4x4::ZERO).HasDegeneratePlane(), "FV15",
              "a zero matrix does (and then everything counts as visible, not nothing)");

        // --- Plane 本身 ----------------------------------------------------
        const Plane ground = Plane(Vector3::UNIT_Z, Vector3::ZERO);
        Check(std::fabs(ground.GetDistanceToPoint(Vector3(0.f, 0.f, 3.f)) - 3.f) < 1e-5f, "FV16",
              "a plane's signed distance is dot(normal, p) + distance");
        Check(ground.GetSide(Vector3(0.f, 0.f, 0.0001f)) && !ground.GetSide(Vector3(0.f, 0.f, -0.0001f)),
              "FV17", "GetSide is true on the normal side (the boundary counts as inside)");
        Plane flipped = ground;
        flipped.Flip();
        Check(std::fabs(flipped.distance - 0.f) < 1e-5f && flipped.normal.z < 0.f, "FV18",
              "Flip negates both (the plane itself is unchanged)");

        // ⚠️ 符号判据：Translate 是 `distance -= dot(normal, t)`。写成 `+=` 的话
        // 平面会往反方向跑，而它仍然是一个自洽的平面 —— 只在实际裁剪时才现形。
        Plane moved = ground;
        moved.Translate(Vector3(0.f, 0.f, 2.f));
        Check(std::fabs(moved.GetDistanceToPoint(Vector3(0.f, 0.f, 2.f))) < 1e-5f, "FV19",
              "Translate moves the plane along the translation, not against it");

        float hit_t = 0.f;
        Check(ground.Raycast(Vector3(0.f, 0.f, 5.f), Vector3(0.f, 0.f, -1.f), hit_t) &&
                  std::fabs(hit_t - 5.f) < 1e-4f, "FV20", "Raycast hits the front face at t = 5");
        Check(!ground.Raycast(Vector3(0.f, 0.f, 5.f), Vector3(0.f, 0.f, 1.f), hit_t), "FV21",
              "and refuses to hit from behind");
        Check(!ground.Raycast(Vector3(0.f, 0.f, 5.f), Vector3(1.f, 0.f, 0.f), hit_t), "FV22",
              "or when the ray is parallel (no NaN, no phantom hit)");
    }

    void TestFrustumWiring()
    {
        const std::string frustum_source = ReadFile("Source/Runtime/Core/Math/Frustum.cpp");
        Check(!frustum_source.empty(), "FW0", "Frustum.cpp is readable");
        if (!frustum_source.empty())
        {
            // ⚠️ 头条：near 平面必须走 [0,1] 那条分支。写成 `m[w] + m[z]`（GL）
            // 的话"贴近相机的东西会被剔掉"，而 far 平面照样是对的 —— 所以只能
            // 靠源码门，不能靠"看起来没出问题"。
            const std::size_t at = frustum_source.find("Frustum Frustum::FromViewProjection");
            Check(at != std::string::npos, "FW1", "FromViewProjection is there");
            if (at != std::string::npos)
            {
                Check(BodyIsBounded(frustum_source, at), "FW2", "its body slice is bounded");
                const std::string body = SliceFunctionBody(frustum_source, at);
                Check(CountOccurrences(body, "kZeroToOne") >= 1, "FW3",
                      "the depth range is an explicit, named choice");
                Check(CountOccurrences(body, "m[z][0], m[z][1], m[z][2], m[z][3]") >= 1, "FW4",
                      "and the near plane is row2 alone (0 <= z), not row3 + row2");
            }
            // 盒子判据必须是"正顶点"，不是"测八个角"也不是"只测中心"。
            const std::size_t at_box = frustum_source.find("bool Frustum::Intersects");
            Check(at_box != std::string::npos, "FW5", "Intersects is there");
            if (at_box != std::string::npos)
            {
                const std::string body = SliceFunctionBody(frustum_source, at_box);
                Check(CountOccurrences(body, "positive_vertex") >= 2, "FW6",
                      "it builds the positive vertex (one corner per plane), not all eight");
            }
        }
        const std::string plane_header = ReadFile("Source/Runtime/Core/Math/Plane.h");
        Check(!plane_header.empty(), "FW7", "Plane.h is readable");
        if (!plane_header.empty())
        {
            // 符号：写反的 `Translate` 依然是一个自洽的平面（见 FV19）。
            Check(CountOccurrences(plane_header, "distance -= normal.dotProduct(translation)") >= 1,
                  "FW8", "Plane::Translate subtracts (sign is the whole ballgame)");
            Check(CountOccurrences(plane_header, "normal.dotProduct(point) + distance") >= 1, "FW9",
                  "and the plane equation is + distance, not - distance");
        }
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
        // `pixelRect` 现在是 **Rect**（不是早先那个把四个数塞进 Vector4 的
        // 退路）：字段名与 Unity 一致，`Rect` 那套 `width` / `height` /
        // `Contains` 都能直接用。
        Check(unbound.GetPixelRect() == ::Rect(0.f, 0.f, 0.f, 0.f), "AR3", "and an empty pixel rect");
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
        const ::Rect rect = cam.GetPixelRect();
        // ⚠️ 这里读的是 `width` / `height`，不是 `z` / `w`：Rect 那四个字段是
        // Unity 的拼写，把 z/w 的旧写法留着的话下面两条会静默地读到 0。
        Check(rect.width > 0.f && rect.height > 0.f, "AR22", "pixelRect has a positive size");
        // 一台占满渲染目标的相机，矩形就该是整块屏。宽高用反了的话这条会红。
        Check(Near(rect.width, width, 1.f) && Near(rect.height, height, 1.f), "AR23",
              "and for a full-target camera it is the whole screen");
        // Unity 的 `pixelRect` 是"左下原点"的 Rect（与屏幕坐标同一套原点），
        // 所以整块屏的那个矩形**包含**原点。
        Check(Runtime::Rect::ContainsPoint(rect, Vector2(0.f, 0.f)), "AR23b",
              "and it uses the same bottom-left origin as screen space (it contains (0,0))");

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

    // ---------------------------------------------------------------- VT ---
    // `Vector2` / `Vector4` / `Matrix4x4` 三个值类型的静态门面（Unity 拼写）。
    //
    // 与 `Vector3` / `Quaternion` 同一套做法（同名 class + interface 合并），只是
    // 晚做了一步。这里钉的是**各自平面/各自维度**的真值：
    //   * `Vector2.right` 是 +X，而 `Vector3.right` 是 -X —— 两边故意不同（屏幕
    //     平面 vs Z-up 世界），一条断言同时盯住两边，谁被"统一"了谁就红。
    //   * `Vector4` 的长度/normalize 是**四维**的，引擎值类型自带的
    //     `Vector4::normalise()` 只动 xyz（Ogre 平面语义）—— 直接转发会静默算错 w。
    //   * `Matrix4x4` 的 Perspective/Ortho 是 **OpenGL 约定**（z ∈ [-1,1]），
    //     与 Unity 一致；矩阵乘法的**顺序**（T*R*S，不是 S*R*T）必须验出来。
    void TestValueTypeFacades()
    {
        // ---- Vector2：常量 ----
        Check(Near(Runtime::Vector2::GetZero(), ::Vector2(0.0f, 0.0f)), "VT0",
              "Vector2.zero == (0,0)");
        Check(Near(Runtime::Vector2::GetOne(), ::Vector2(1.0f, 1.0f)), "VT1",
              "Vector2.one == (1,1)");
        Check(Near(Runtime::Vector2::GetUp(), ::Vector2(0.0f, 1.0f)), "VT2",
              "Vector2.up == (0,1) (screen plane)");
        Check(Near(Runtime::Vector2::GetDown(), ::Vector2(0.0f, -1.0f)), "VT3",
              "Vector2.down == (0,-1)");
        Check(Near(Runtime::Vector2::GetLeft(), ::Vector2(-1.0f, 0.0f)), "VT4",
              "Vector2.left == (-1,0)");
        // ⚠️ 与 Vector3.right **相反**：屏幕平面 x 朝右，世界的 local +X 是左。
        // 写成 -X 就与 Vector3 一致了，但那不是 Unity 的 Vector2.right。
        Check(Near(Runtime::Vector2::GetRight(), ::Vector2(1.0f, 0.0f)) &&
                  Near(Runtime::Vector3::GetRight(), ::Vector3(-1.0f, 0.0f, 0.0f)),
              "VT5", "Vector2.right is +X while Vector3.right is -X (different planes, on purpose)");
        Check(Runtime::Vector2::GetPositiveInfinity().x > 0.0f &&
                  std::isinf(Runtime::Vector2::GetPositiveInfinity().y),
              "VT6", "Vector2.positiveInfinity is +inf in both components");
        Check(std::isinf(Runtime::Vector2::GetNegativeInfinity().x) &&
                  Runtime::Vector2::GetNegativeInfinity().x < 0.0f,
              "VT7", "Vector2.negativeInfinity is -inf in both components");
        Check(Near(Runtime::Vector2::GetEpsilon(), 0.00001f, 1.0e-9f), "VT8",
              "Vector2.kEpsilon == 1e-5 (Unity's value)");

        // ---- Vector2：运算 ----
        const ::Vector2 a34(3.0f, 4.0f);
        Check(Near(Runtime::Vector2::Magnitude(a34), 5.0f), "VT9", "|(3,4)| == 5");
        Check(Near(Runtime::Vector2::SqrMagnitude(a34), 25.0f), "VT10", "|(3,4)|^2 == 25");
        // 零向量必须返回零向量，不是 NaN：NaN 一旦进 transform 就再也看不出来。
        Check(Near(Runtime::Vector2::Normalize(::Vector2(0.0f, 0.0f)), ::Vector2(0.0f, 0.0f)) &&
                  !std::isnan(Runtime::Vector2::Normalize(::Vector2(0.0f, 0.0f)).x),
              "VT11", "Normalize(0,0) == (0,0), not NaN");
        Check(Near(Runtime::Vector2::Normalize(a34), ::Vector2(0.6f, 0.8f)), "VT12",
              "Normalize(3,4) == (0.6,0.8)");
        Check(Near(Runtime::Vector2::Dot(::Vector2(1.0f, 2.0f), ::Vector2(3.0f, 4.0f)), 11.0f),
              "VT13", "Dot((1,2),(3,4)) == 11");
        Check(Near(Runtime::Vector2::Distance(::Vector2(1.0f, 1.0f), ::Vector2(4.0f, 5.0f)), 5.0f),
              "VT14", "Distance((1,1),(4,5)) == 5");
        Check(Near(Runtime::Vector2::Angle(::Vector2(1.0f, 0.0f), ::Vector2(0.0f, 1.0f)), 90.0f),
              "VT15", "Angle((1,0),(0,1)) == 90 (unsigned)");
        Check(Near(Runtime::Vector2::Angle(::Vector2(1.0f, 0.0f), ::Vector2(-1.0f, 0.0f)), 180.0f),
              "VT16", "Angle((1,0),(-1,0)) == 180");
        // 有符号：逆时针为正。反向断言同时钉住符号**和**大小（只钉大小的话 sign 写反
        // 全绿）。
        Check(Near(Runtime::Vector2::SignedAngle(::Vector2(1.0f, 0.0f), ::Vector2(0.0f, 1.0f)),
                   90.0f) &&
                  Near(Runtime::Vector2::SignedAngle(::Vector2(0.0f, 1.0f), ::Vector2(1.0f, 0.0f)),
                       -90.0f),
              "VT17", "SignedAngle is counter-clockwise positive (90 / -90)");
        // 夹 vs 不夹：t=2 时两者差一整个 b，是这个区别唯一能被看见的地方。
        Check(Near(Runtime::Vector2::Lerp(::Vector2(0.0f, 0.0f), ::Vector2(10.0f, 10.0f), 2.0f),
                   ::Vector2(10.0f, 10.0f)) &&
                  Near(Runtime::Vector2::LerpUnclamped(::Vector2(0.0f, 0.0f),
                                                       ::Vector2(10.0f, 10.0f), 2.0f),
                       ::Vector2(20.0f, 20.0f)),
              "VT18", "Vector2.Lerp clamps t at 1, LerpUnclamped extrapolates");
        Check(Near(Runtime::Vector2::Perpendicular(::Vector2(1.0f, 2.0f)), ::Vector2(-2.0f, 1.0f)),
              "VT19", "Perpendicular((1,2)) == (-2,1)");
        // 法线**故意不是单位向量**（(0,3)）：Unity 的 Reflect 要求法线已归一化，
        // 但这里的实现自己归一化一遍。忘了归一化一层的话，单位法线这条断言是绿的。
        Check(Near(Runtime::Vector2::Reflect(::Vector2(1.0f, -1.0f), ::Vector2(0.0f, 3.0f)),
                   ::Vector2(1.0f, 1.0f)),
              "VT20", "Reflect((1,-1),(0,3)) == (1,1) -- normal is normalized inside");
        Check(Near(Runtime::Vector2::Scale(::Vector2(2.0f, 3.0f), ::Vector2(4.0f, 5.0f)),
                   ::Vector2(8.0f, 15.0f)),
              "VT21", "Vector2.Scale is component-wise (Unity)");
        Check(Near(Runtime::Vector2::ScaleUniform(::Vector2(2.0f, 3.0f), 3.0f),
                   ::Vector2(6.0f, 9.0f)),
              "VT22", "ScaleUniform(2,3) == (6,9)");
        // 两个方向都要有：短于上限的原样返回，长于上限的才缩。只测"缩"的话，
        // 去掉提前返回的实现（无条件归一化到上限）是绿的。
        Check(Near(Runtime::Vector2::ClampMagnitude(a34, 2.5f), ::Vector2(1.5f, 2.0f)) &&
                  Near(Runtime::Vector2::ClampMagnitude(::Vector2(1.0f, 0.0f), 5.0f),
                       ::Vector2(1.0f, 0.0f)),
              "VT23", "ClampMagnitude((3,4),2.5) shrinks to (1.5,2) but leaves a short vector alone");
        // 够得到就直接落 b（不然会在终点附近永远抖），够不到才走 max_distance。
        Check(Near(Runtime::Vector2::MoveTowards(::Vector2(0.0f, 0.0f), ::Vector2(10.0f, 0.0f),
                                                 3.0f),
                   ::Vector2(3.0f, 0.0f)) &&
                  Near(Runtime::Vector2::MoveTowards(::Vector2(0.0f, 0.0f), ::Vector2(1.0f, 0.0f),
                                                     5.0f),
                       ::Vector2(1.0f, 0.0f)),
              "VT24", "MoveTowards steps by max_distance and snaps when close enough");
        Check(Near(Runtime::Vector2::Min(::Vector2(1.0f, 5.0f), ::Vector2(3.0f, 2.0f)),
                   ::Vector2(1.0f, 2.0f)) &&
                  Near(Runtime::Vector2::Max(::Vector2(1.0f, 5.0f), ::Vector2(3.0f, 2.0f)),
                       ::Vector2(3.0f, 5.0f)),
              "VT25", "Min/Max are component-wise (both components must differ)");

        // ---- Vector4：四维 ----
        Check(Near(Runtime::Vector4::GetZero(), ::Vector4(0.0f, 0.0f, 0.0f, 0.0f)), "VT26",
              "Vector4.zero == (0,0,0,0)");
        Check(Near(Runtime::Vector4::GetOne(), ::Vector4(1.0f, 1.0f, 1.0f, 1.0f)), "VT27",
              "Vector4.one == (1,1,1,1)");
        // (1,2,2,4)：三维 hypot 是 3，四维是 5 —— 用这个 3-4-5 把两种长度分开。
        const ::Vector4 v1224(1.0f, 2.0f, 2.0f, 4.0f);
        Check(Near(Runtime::Vector4::Magnitude(v1224), 5.0f), "VT28",
              "|(1,2,2,4)| == 5 -- FOUR dimensional (3-D hypot would say 3)");
        Check(Near(Runtime::Vector4::SqrMagnitude(v1224), 25.0f), "VT29", "|(1,2,2,4)|^2 == 25");
        Check(Near(Runtime::Vector4::Normalize(v1224), ::Vector4(0.2f, 0.4f, 0.4f, 0.8f)), "VT30",
              "Normalize((1,2,2,4)) divides by the 4-D length, w included");
        // 与引擎值类型自带的 `Vector4::normalise()`（只动 xyz）必须不同 —— 用 w 分得开：
        // 那个版本给出 w = 4/3 ≈ 1.333，Unity 的给 0.8。
        ::Vector4 ogre = v1224;
        ogre.normalise();
        Check(Near(ogre, ::Vector4(0.0f, 0.0f, 0.0f, 0.0f)) == false &&
                  !Near(ogre, Runtime::Vector4::Normalize(v1224)),
              "VT31", "Vector4.Normalize != Vector4::normalise() (the latter is Ogre's plane form)");
        Check(Near(Runtime::Vector4::Dot(::Vector4(1.0f, 2.0f, 3.0f, 4.0f),
                                         ::Vector4(5.0f, 6.0f, 7.0f, 8.0f)),
                   70.0f),
              "VT32", "Vector4.Dot is 4-D (70, not 1*5+2*6+3*7=38)");
        Check(Near(Runtime::Vector4::Distance(::Vector4(0.0f, 0.0f, 0.0f, 0.0f), v1224), 5.0f),
              "VT33", "Vector4.Distance is 4-D");
        Check(Near(Runtime::Vector4::Lerp(::Vector4(0.0f, 0.0f, 0.0f, 0.0f),
                                          ::Vector4(10.0f, 10.0f, 10.0f, 10.0f), 2.0f),
                   ::Vector4(10.0f, 10.0f, 10.0f, 10.0f)) &&
                  Near(Runtime::Vector4::LerpUnclamped(::Vector4(0.0f, 0.0f, 0.0f, 0.0f),
                                                       ::Vector4(10.0f, 10.0f, 10.0f, 10.0f), 2.0f),
                       ::Vector4(20.0f, 20.0f, 20.0f, 20.0f)),
              "VT34", "Vector4.Lerp clamps t at 1, LerpUnclamped extrapolates");
        Check(Near(Runtime::Vector4::Project(::Vector4(1.0f, 1.0f, 1.0f, 1.0f),
                                             ::Vector4(0.0f, 2.0f, 0.0f, 0.0f)),
                   ::Vector4(0.0f, 1.0f, 0.0f, 0.0f)),
              "VT35", "Project((1,1,1,1),(0,2,0,0)) == (0,1,0,0)");
        Check(Near(Runtime::Vector4::Project(::Vector4(1.0f, 1.0f, 1.0f, 1.0f),
                                             ::Vector4(0.0f, 0.0f, 0.0f, 0.0f)),
                   ::Vector4(0.0f, 0.0f, 0.0f, 0.0f)),
              "VT36", "Project onto the zero vector is zero, not NaN");
        Check(Near(Runtime::Vector4::Scale(::Vector4(1.0f, 2.0f, 3.0f, 4.0f),
                                           ::Vector4(2.0f, 3.0f, 4.0f, 5.0f)),
                   ::Vector4(2.0f, 6.0f, 12.0f, 20.0f)),
              "VT37", "Vector4.Scale is component-wise");
        Check(Near(Runtime::Vector4::Max(::Vector4(1.0f, 9.0f, 2.0f, 8.0f),
                                         ::Vector4(4.0f, 3.0f, 7.0f, 5.0f)),
                   ::Vector4(4.0f, 9.0f, 7.0f, 8.0f)) &&
                  Near(Runtime::Vector4::Min(::Vector4(1.0f, 9.0f, 2.0f, 8.0f),
                                             ::Vector4(4.0f, 3.0f, 7.0f, 5.0f)),
                       ::Vector4(1.0f, 3.0f, 2.0f, 5.0f)),
              "VT38", "Vector4 Min/Max are component-wise (every component differs)");

        // ---- Matrix4x4 ----
        const ::Matrix4x4 ident = Runtime::Matrix4x4::GetIdentity();
        const ::Matrix4x4 zero = Runtime::Matrix4x4::GetZero();
        Check(Runtime::Matrix4x4::IsIdentity(ident) && !Runtime::Matrix4x4::IsIdentity(zero),
              "VT39", "identity is identity, zero is not");
        Check(Near(Runtime::Matrix4x4::Determinant(ident), 1.0f) &&
                  Near(Runtime::Matrix4x4::Determinant(zero), 0.0f),
              "VT40", "det(I) == 1 and det(0) == 0");
        const ::Matrix4x4 scale234 = Runtime::Matrix4x4::Scale(::Vector3(2.0f, 3.0f, 4.0f));
        Check(Near(Runtime::Matrix4x4::Determinant(scale234), 24.0f), "VT41",
              "det(Scale(2,3,4)) == 24");
        // 奇异矩阵：返回零矩阵，不许把 NaN 传下去（NaN 会在几十帧后才在别处炸）。
        const ::Matrix4x4 singular_inv = Runtime::Matrix4x4::Inverse(zero);
        Check(Near(singular_inv, zero), "VT42",
              "Inverse of a singular matrix is the zero matrix (no NaN propagation)");
        // 真逆：M⁻¹·M == I。
        Check(Near(Runtime::Matrix4x4::Multiply(Runtime::Matrix4x4::Inverse(scale234), scale234),
                   ident),
              "VT43", "Inverse(M) * M == I");
        // 转置：平移列跑到平移行去了 —— 用 (1,2,3) 三个不同值，转置前后位置不同。
        const ::Matrix4x4 trans123 = Runtime::Matrix4x4::Translate(::Vector3(1.0f, 2.0f, 3.0f));
        Check(Near(trans123.m_Mat[0][3], 1.0f) && Near(trans123.m_Mat[1][3], 2.0f) &&
                  Near(trans123.m_Mat[2][3], 3.0f),
              "VT44", "Translate puts the offset in column 3 (column-vector convention)");
        const ::Matrix4x4 transposed = Runtime::Matrix4x4::Transpose(trans123);
        Check(Near(transposed.m_Mat[3][0], 1.0f) && Near(transposed.m_Mat[3][1], 2.0f) &&
                  Near(transposed.m_Mat[3][2], 3.0f) && Near(transposed.m_Mat[0][3], 0.0f),
              "VT45", "Transpose moves the offset to row 3");
        // 点 vs 方向：平移影响点，不影响方向。同一个矩阵、同一个输入，两条断言互相
        // 咬住 —— 只钉"点对"的话 MultiplyVector 退化成 MultiplyPoint 也全绿。
        Check(Near(Runtime::Matrix4x4::MultiplyPoint(trans123, ::Vector3(1.0f, 1.0f, 1.0f)),
                   ::Vector3(2.0f, 3.0f, 4.0f)) &&
                  Near(Runtime::Matrix4x4::MultiplyPoint3x4(trans123, ::Vector3(1.0f, 1.0f, 1.0f)),
                       ::Vector3(2.0f, 3.0f, 4.0f)) &&
                  Near(Runtime::Matrix4x4::MultiplyVector(trans123, ::Vector3(1.0f, 1.0f, 1.0f)),
                       ::Vector3(1.0f, 1.0f, 1.0f)),
              "VT46", "MultiplyPoint/Point3x4 add the translation, MultiplyVector does not");
        const ::Matrix4x4 rot_z90 = Runtime::Matrix4x4::Rotate(
            Runtime::Quaternion::AngleAxis(90.0f, ::Vector3(0.0f, 0.0f, 1.0f)));
        Check(Near(Runtime::Matrix4x4::MultiplyVector(rot_z90, ::Vector3(1.0f, 0.0f, 0.0f)),
                   ::Vector3(0.0f, 1.0f, 0.0f)),
              "VT47", "Rotate(90 deg about +Z) maps +X to +Y (right-handed)");
        // TRS == T * R * S（不是 S * R * T）：先用纯缩放+平移验顺序，再用旋转验。
        const ::Matrix4x4 trs = Runtime::Matrix4x4::TRS(::Vector3(1.0f, 2.0f, 3.0f),
                                                        Runtime::Quaternion::GetIdentity(),
                                                        ::Vector3(2.0f, 2.0f, 2.0f));
        Check(Near(Runtime::Matrix4x4::MultiplyPoint(trs, ::Vector3(1.0f, 0.0f, 0.0f)),
                   ::Vector3(3.0f, 2.0f, 3.0f)),
              "VT48", "TRS scales first, then translates (T*R*S, not S*R*T)");
        Check(Near(Runtime::Matrix4x4::Multiply(
                       Runtime::Matrix4x4::Multiply(trans123, rot_z90),
                       Runtime::Matrix4x4::Scale(::Vector3(2.0f, 2.0f, 2.0f))),
                   Runtime::Matrix4x4::TRS(::Vector3(1.0f, 2.0f, 3.0f),
                                           Runtime::Quaternion::AngleAxis(
                                               90.0f, ::Vector3(0.0f, 0.0f, 1.0f)),
                                           ::Vector3(2.0f, 2.0f, 2.0f))),
              "VT49", "TRS == Translate * Rotate * Scale (with a real rotation in it)");
        // 乘法顺序本身：T*S 与 S*T 不同 —— 只钉"乘出来是个矩阵"看不出来。
        Check(Near(Runtime::Matrix4x4::MultiplyPoint(
                       Runtime::Matrix4x4::Multiply(trans123, scale234), ::Vector3(1.0f, 1.0f, 1.0f)),
                   ::Vector3(3.0f, 5.0f, 7.0f)) &&
                  Near(Runtime::Matrix4x4::MultiplyPoint(
                           Runtime::Matrix4x4::Multiply(scale234, trans123),
                           ::Vector3(1.0f, 1.0f, 1.0f)),
                       ::Vector3(4.0f, 9.0f, 16.0f)),
              "VT50", "Multiply is ordered (T*S != S*T)");
        // Perspective：竖直 fov、度、OpenGL z ∈ [-1,1]。m11 = 1/tan(fov/2)，
        // m[3][2] = -1 就是"OpenGL 而不是 D3D"的那一条。
        const ::Matrix4x4 persp = Runtime::Matrix4x4::Perspective(90.0f, 1.0f, 0.1f, 100.0f);
        Check(Near(persp.m_Mat[1][1], 1.0f, 1.0e-4f) && Near(persp.m_Mat[0][0], 1.0f, 1.0e-4f),
              "VT51", "Perspective(90, aspect 1) has 1 on both diagonals (fov in DEGREES)");
        Check(Near(persp.m_Mat[3][2], -1.0f) && Near(persp.m_Mat[3][3], 0.0f),
              "VT52", "Perspective is OpenGL-style (m32 == -1, not D3D's 0..1 depth)");
        Check(Near(persp.m_Mat[2][2], 100.0f / (0.1f - 100.0f), 1.0e-3f), "VT53",
              "Perspective maps z into [-1,1] (f/(n-f))");
        // Ortho：左右不等宽时 m00/m03 都要跟着变，只验 m00 会漏掉偏移项。
        const ::Matrix4x4 ortho = Runtime::Matrix4x4::Ortho(-1.0f, 3.0f, -2.0f, 2.0f, 0.1f, 100.0f);
        Check(Near(ortho.m_Mat[0][0], 0.5f) && Near(ortho.m_Mat[0][3], -0.5f) &&
                  Near(ortho.m_Mat[1][1], 0.5f) && Near(ortho.m_Mat[3][3], 1.0f),
              "VT54", "Ortho(-1,3,-2,2) has the half-width and the centre offset right");
        // z 那一行也钉住：zNear/zFar 只影响 m22/m23，漏看这行的 `Ortho` 会把
        // 01 版（-1..0 深度）当成 -1..1 版用而看不出来。
        Check(Near(ortho.m_Mat[2][2], -2.0f / 99.9f, 1.0e-4f) &&
                  Near(ortho.m_Mat[2][3], -100.1f / 99.9f, 1.0e-3f),
              "VT54b", "Ortho maps z into [-1,1] (not the 0..1 variant)");
        // LookAt：相机在原点看向 -Y（本引擎的默认朝向），世界点 (0,-5,0) 应该落在
        // 视图空间的 -Z（OpenGL：相机看向 -Z）。
        Check(Near(Runtime::Matrix4x4::MultiplyPoint(
                       Runtime::Matrix4x4::LookAt(::Vector3(0.0f, 0.0f, 0.0f),
                                                  ::Vector3(0.0f, -1.0f, 0.0f),
                                                  ::Vector3(0.0f, 0.0f, 1.0f)),
                       ::Vector3(0.0f, -5.0f, 0.0f)),
                   ::Vector3(0.0f, 0.0f, -5.0f)),
              "VT55", "LookAt puts a point 5 units ahead at view-space z = -5");
        Check(Near(Runtime::Matrix4x4::GetColumn(trans123, 3), ::Vector4(1.0f, 2.0f, 3.0f, 1.0f)) &&
                  Near(Runtime::Matrix4x4::GetRow(trans123, 0), ::Vector4(1.0f, 0.0f, 0.0f, 1.0f)) &&
                  Near(Runtime::Matrix4x4::GetRow(trans123, 2), ::Vector4(0.0f, 0.0f, 1.0f, 3.0f)),
              "VT56", "GetColumn/GetRow return the right slice (transposed of each other)");
        Check(Near(Runtime::Matrix4x4::GetColumn(trans123, 9), ::Vector4(0.0f, 0.0f, 0.0f, 0.0f)) &&
                  Near(Runtime::Matrix4x4::GetRow(trans123, -1), ::Vector4(0.0f, 0.0f, 0.0f, 0.0f)),
              "VT57", "GetColumn/GetRow out of range return zero, not out-of-bounds garbage");

        // ---- Vector3.Scale 的 Unity 语义（本切片顺带改掉的那条）----
        Check(Near(Runtime::Vector3::Scale(::Vector3(2.0f, 3.0f, 4.0f),
                                           ::Vector3(5.0f, 7.0f, 11.0f)),
                   ::Vector3(10.0f, 21.0f, 44.0f)),
              "VT58", "Vector3.Scale is component-wise (Unity), not a uniform scale");
        Check(Near(Runtime::Vector3::ScaleUniform(::Vector3(2.0f, 3.0f, 4.0f), 3.0f),
                   ::Vector3(6.0f, 9.0f, 12.0f)),
              "VT59", "Vector3.ScaleUniform is the scalar one");
    }

    // ---------------------------------------------------------------- VW ---
    // 上面那组行为断言的**接线门**。
    //
    // 三个门面最容易出的问题不是算错，而是"根本没接上"：类写了、注册了、d.ts 里
    // 也有 —— 但脚本侧拿到的是 interface 而不是合并出来的 class，于是
    // `Vector2.zero` 编译不过。这类失败在没有 TS 用例的环境里完全静默，所以源码级
    // 与 d.ts 级各钉一层。
    void TestValueTypeFacadeWiring()
    {
        const std::string header = ReadFile("Source/Runtime/Scripting/ScriptVector/ScriptVector.h");
        const std::string source = ReadFile("Source/Runtime/Scripting/ScriptVector/ScriptVector.cpp");
        Check(!header.empty(), "VW0", "ScriptVector.h is readable");
        Check(!source.empty(), "VW1", "ScriptVector.cpp is readable");

        // 五个门面都在（ZCLASS 是 ZHeaderTool 的入口，没有它就没有任何绑定）。
        Check(CountOccurrences(header, "class Vector2 : public Object") >= 1 &&
                  CountOccurrences(header, "class Vector4 : public Object") >= 1 &&
                  CountOccurrences(header, "class Matrix4x4 : public Object") >= 1,
              "VW2", "Vector2 / Vector4 / Matrix4x4 all have Object-derived facades");
        // 值类型一律全称：命名空间里裸 `Matrix4x4` 指的是门面类，漏 `::` 编译器会
        // 立刻报错（不会静默），但 `m_Mat` 那种成员访问会**静默**拿到错的东西。
        Check(CountOccurrences(header, "static ::Vector2 GetZero()") >= 1 &&
                  CountOccurrences(header, "static ::Vector4 GetZero()") >= 1 &&
                  CountOccurrences(header, "static ::Matrix4x4 GetIdentity()") >= 1,
              "VW3", "the new facades return fully-qualified value types");
        Check(CountOccurrences(source, "::Matrix4x4::IDENTITY") >= 2, "VW4",
              "the matrix constants are read off ::Matrix4x4, not the facade class");
        // >= 2：Quaternion.identity 与 Matrix4x4.identity 是**同一串文本**。写成
        // >= 1 的话，谁删掉其中一个门禁仍然绿（变异 VTM34 就是这么存活的）。
        Check(CountOccurrences(header, "ZSCRIPTPROPERTY(Name = \"identity\", Getter = \"GetIdentity\")") >= 2,
              "VW5", "BOTH Quaternion.identity and Matrix4x4.identity are static script properties");

        // 轴向：Vector2.right = +X（与 Vector3.right = -X 故意不同）。
        Check(CountOccurrences(source, "::Vector2(1.0f, 0.0f)") >= 1, "VW6",
              "Vector2.right is +X (the screen plane, unlike Vector3.right)");
        // 四维，且**没有**转发引擎值类型的 `normalise()`（那个只动 xyz）。
        Check(CountOccurrences(source, "v.w * v.w") >= 2, "VW7",
              "Vector4 length is 4-D (w is squared too)");
        Check(CountOccurrences(source, "normalise()") == 0, "VW8",
              "Vector4.Normalize does NOT forward Vector4::normalise() (that one is 3-D)");
        // Lerp 夹 t：三个门面都要夹，共用一个 Clamp01 才不会漏夹一个。
        Check(CountOccurrences(source, "Clamp01(t)") >= 2, "VW9",
              "Vector2/Vector4 Lerp clamp t through the shared Clamp01");
        // 投影/视图矩阵转发引擎的 Math::Make*，不重写一份。
        Check(CountOccurrences(source, "Math::MakePerspectiveMatrix") >= 1 &&
                  CountOccurrences(source, "Math::MakeOrthographicProjectionMatrix") >= 1 &&
                  CountOccurrences(source, "Math::MakeLookAtMatrix") >= 1,
              "VW10", "the matrix builders forward Math::Make* (no second implementation)");
        // 度 → 弧度。忘了一行，Perspective 会把 90 度当 90 弧度，画面缩成一个点。
        Check(CountOccurrences(source, "Math::DegreesToRadians(fov)") >= 1, "VW11",
              "Perspective converts fov from degrees to radians");
        // 奇异矩阵守卫。
        Check(CountOccurrences(source, "1.0e-12f") >= 1, "VW12",
              "Inverse guards the singular case instead of emitting NaN");
        // Vector3.Scale 走的是逐分量那个（VectorMath::Mul），不是标量的 Scale。
        Check(CountOccurrences(source, "VectorMath::Mul(a, b)") >= 1, "VW13",
              "Vector3.Scale forwards the component-wise VectorMath::Mul");

        // d.ts：interface 与 class **都必须**在 —— 少了 class 就没有静态面，
        // 少了 interface 就没有值类型，两边缺一边 TS 侧都是"能编译但形状不对"。
        const std::string dts = ReadFile("Source/Runtime/Scripting/ScriptVM/Test/Typings/zengine.d.ts");
        Check(!dts.empty(), "VW14", "the generated zengine.d.ts is readable");
        // ⚠️ 一律带上 ` {`：`declare class Matrix4x4` 是 `declare class
        // Matrix4x4Facade` 的**子串**，光数前者会把"门面被改名了"当成还在
        // （变异 VTM35 第一次就是这么存活的）。
        Check(CountOccurrences(dts, "declare interface Vector2 {") >= 1 &&
                  CountOccurrences(dts, "declare class Vector2 {") >= 1 &&
                  CountOccurrences(dts, "declare interface Vector4 {") >= 1 &&
                  CountOccurrences(dts, "declare class Vector4 {") >= 1 &&
                  CountOccurrences(dts, "declare interface Matrix4x4 {") >= 1 &&
                  CountOccurrences(dts, "declare class Matrix4x4 {") >= 1,
              "VW15", "interface AND class are declared for all three (declaration merging)");
        // 静态面返回的是**合并出来的类型**：`identity: Matrix4x4` 而不是
        // `identity: any`，否则脚本里 `let m: Matrix4x4 = Matrix4x4.identity` 直接红。
        Check(CountOccurrences(dts, "static readonly identity: Matrix4x4;") >= 1 &&
                  CountOccurrences(dts, "static readonly zero: Vector2;") >= 1 &&
                  CountOccurrences(dts, "static readonly one: Vector4;") >= 1,
              "VW16", "the facades' static members are typed as the merged value type");
        Check(CountOccurrences(dts, "static TRS(pos: Vector3, q: Quaternion, s: Vector3): Matrix4x4;") >= 1 &&
                  CountOccurrences(dts, "static SignedAngle(from: Vector2, to: Vector2): number;") >= 1,
              "VW17", "Unity's own spellings reach the .d.ts (TRS, SignedAngle)");
    }

    // ---------------------------------------------------------------- GZ ---
    // Gizmos (Unity's `UnityEngine.Gizmos` static face).
    //
    // The backend is `DebugDrawGroup`, whose primitive counts are readable
    // without a GPU -- so this group asserts real geometry (positions and
    // colours), not just "something was added".

    void TestGizmos()
    {
        auto* manager = GET_SYSTEM(DebugDrawManager);
        Check(manager != nullptr, "GZ0", "DebugDrawManager is reachable");
        if (manager == nullptr)
        {
            return;
        }

        auto* group = manager->TryGetOrCreateDebugDrawGroup("gizmos");
        // 非 Vulkan 构建（__APPLE__ / !Z_HAS_VULKAN）下这个调用恒返回 nullptr：
        // DebugDrawManager.cpp 顶部是一整段空实现。所以"拿不到 group"在那一层
        // 是**正确行为**，不是缺陷 —— 两个平台各断言各的真值，谁都不是空测试。
#if defined(Z_HAS_VULKAN)
        Check(group != nullptr, "GZ1", "the gizmo group exists");
#else
        Check(group == nullptr, "GZ1", "no debug-draw backend: the group is absent by design");
#endif
        if (group == nullptr)
        {
            // 没有后端时整套 API 必须是 no-op 而不是崩 —— 走完这七行本身就是
            // 证明（崩了进程根本到不了下一行）。一个只在 macOS 上炸的
            // Gizmos.DrawLine 是最难查的那类 bug。
            Runtime::Gizmos::SetColor(::Color(1.0f, 0.0f, 0.0f, 1.0f));
            Runtime::Gizmos::SetMatrix(Matrix4x4::IDENTITY);
            Runtime::Gizmos::DrawLine(::Vector3(0.0f, 0.0f, 0.0f), ::Vector3(1.0f, 0.0f, 0.0f));
            Runtime::Gizmos::DrawRay(::Vector3(1.0f, 0.0f, 0.0f), ::Vector3(0.0f, 2.0f, 0.0f));
            Runtime::Gizmos::DrawWireSphere(::Vector3(0.0f, 0.0f, 0.0f), 1.0f);
            Runtime::Gizmos::DrawSphere(::Vector3(0.0f, 0.0f, 0.0f), 1.0f);
            Runtime::Gizmos::DrawWireCube(::Vector3(0.0f, 0.0f, 0.0f), ::Vector3(1.0f, 1.0f, 1.0f));
            Runtime::Gizmos::DrawCube(::Vector3(0.0f, 0.0f, 0.0f), ::Vector3(1.0f, 1.0f, 1.0f));
            Check(true, "GZ2", "every Gizmos entry survives a missing backend");
            return;
        }

        // Gizmos.color / Gizmos.matrix 是**进程级**状态，复位是必须的：漏了会把
        // 这一组跑歪，而且跑歪的是后面那些看起来不相干的断言。
        Runtime::Gizmos::SetColor(::Color(1.0f, 1.0f, 1.0f, 1.0f));
        Runtime::Gizmos::SetMatrix(Matrix4x4::IDENTITY);
        group->ClearData();
        Check(group->GetLineCount(false) == 0, "GZ2b", "a cleared group is empty");

        // --- DrawLine ----------------------------------------------------
        Runtime::Gizmos::DrawLine(::Vector3(0.0f, 0.0f, 0.0f), ::Vector3(1.0f, 0.0f, 0.0f));
        Check(group->GetLineCount(false) == 1, "GZ3", "DrawLine adds one depth-tested line");
        Check(group->GetLineCount(true) == 0, "GZ4",
              "and it is NOT in the always-on-top bucket (Unity's Gizmos are depth tested)");

        std::vector<DebugDrawVertex> verts;
        group->WriteLineData(verts, false);
        Check(verts.size() == 2, "GZ5", "a line is two vertices");
        if (verts.size() == 2)
        {
            Check(Near(verts[0].pos, ::Vector3(0.0f, 0.0f, 0.0f)) &&
                      Near(verts[1].pos, ::Vector3(1.0f, 0.0f, 0.0f)),
                  "GZ6", "the line runs from `from` to `to`");
            Check(Near(verts[0].color, ::Vector4(1.0f, 1.0f, 1.0f, 1.0f)), "GZ7",
                  "default Gizmos.color is white, like Unity");
        }

        // --- DrawRay: `direction` is a direction, not an endpoint ---------
        // 故意让 from 与 direction 完全不同（(1,0,0) 与 (0,2,0)）：如果传的是
        // 终点，verts[1] 会是 (0,2,0) 而不是 (1,2,0)。两者相等的话这条断言等于
        // 没写。
        group->ClearData();
        Runtime::Gizmos::DrawRay(::Vector3(1.0f, 0.0f, 0.0f), ::Vector3(0.0f, 2.0f, 0.0f));
        verts.clear();
        group->WriteLineData(verts, false);
        Check(verts.size() == 2, "GZ8", "DrawRay adds one line");
        if (verts.size() == 2)
        {
            Check(Near(verts[1].pos, ::Vector3(1.0f, 2.0f, 0.0f)), "GZ9",
                  "DrawRay's far point is from + direction");
        }

        // --- colour --------------------------------------------------------
        // 红色 (1,0,0,1)：通道互换在灰色样本下看不出来。
        group->ClearData();
        Runtime::Gizmos::SetColor(::Color(1.0f, 0.0f, 0.0f, 1.0f));
        Runtime::Gizmos::DrawLine(::Vector3(0.0f, 0.0f, 0.0f), ::Vector3(1.0f, 0.0f, 0.0f));
        verts.clear();
        group->WriteLineData(verts, false);
        Check(verts.size() == 2 && Near(verts[0].color, ::Vector4(1.0f, 0.0f, 0.0f, 1.0f)),
              "GZ10", "Gizmos.color reaches the vertex");
        Runtime::Gizmos::SetColor(::Color(1.0f, 1.0f, 1.0f, 1.0f));

        // --- matrix --------------------------------------------------------
        group->ClearData();
        Matrix4x4 translate = Matrix4x4::IDENTITY;
        translate.makeTrans(::Vector3(0.0f, 0.0f, 5.0f));
        Runtime::Gizmos::SetMatrix(translate);
        Runtime::Gizmos::DrawLine(::Vector3(1.0f, 0.0f, 0.0f), ::Vector3(2.0f, 0.0f, 0.0f));
        verts.clear();
        group->WriteLineData(verts, false);
        Check(verts.size() == 2 && Near(verts[0].pos.z, 5.0f) && Near(verts[1].pos.z, 5.0f),
              "GZ11", "Gizmos.matrix moves both endpoints");

        Runtime::Gizmos::SetMatrix(Matrix4x4::IDENTITY);
        group->ClearData();
        Runtime::Gizmos::DrawLine(::Vector3(1.0f, 0.0f, 0.0f), ::Vector3(2.0f, 0.0f, 0.0f));
        verts.clear();
        group->WriteLineData(verts, false);
        Check(verts.size() == 2 && Near(verts[0].pos.z, 0.0f), "GZ12",
              "restoring the identity matrix restores identity behaviour");

        // --- wire cube vs solid cube are different primitives --------------
        // 互补的一对：只写其中一条的话，"DrawCube 直接调 DrawWireCube" 全绿。
        group->ClearData();
        Runtime::Gizmos::DrawWireCube(::Vector3(0.0f, 0.0f, 0.0f), ::Vector3(2.0f, 2.0f, 2.0f));
        Check(group->GetLineCount(false) == 12, "GZ13",
              "DrawWireCube lands in the box bucket (12 wire edges)");
        Check(group->GetTriangleCount(false) == 0, "GZ14", "and adds no solid triangles");

        group->ClearData();
        Runtime::Gizmos::DrawCube(::Vector3(0.0f, 0.0f, 0.0f), ::Vector3(2.0f, 2.0f, 2.0f));
        Check(group->GetTriangleCount(false) == 12, "GZ15",
              "DrawCube is 12 solid triangles (6 faces x 2)");
        Check(group->GetLineCount(false) == 0, "GZ16", "and adds no wire lines");

        // --- spheres -------------------------------------------------------
        group->ClearData();
        Runtime::Gizmos::DrawWireSphere(::Vector3(0.0f, 0.0f, 0.0f), 1.5f);
        Check(group->GetUniformDynamicDataCount() == 1, "GZ17", "DrawWireSphere adds a sphere");
        Runtime::Gizmos::DrawSphere(::Vector3(0.0f, 0.0f, 0.0f), 1.5f);
        // 后端没有实心球体图元，所以 DrawSphere 目前就是 DrawWireSphere；这个
        // 数字是"它至少画了东西"，不是"它画成了实心"。
        Check(group->GetUniformDynamicDataCount() == 2, "GZ18", "DrawSphere adds one too");

        group->ClearData();
    }

    // ---------------------------------------------------------------- FR ---
    // `Gizmos.DrawFrustum`（Unity 的相机视锥线框）。
    //
    // 这一组最容易"看起来对"的错是**轴向**：照 Unity 抄成 +Z 张开，得到的仍然是
    // 12 条线、仍然是个台体、角点仍然两两对称 —— 所有计数型断言全绿。只有"远端
    // 落在 -Y 那一侧"这种**带符号**的断言才分得出来（FR7）。
    void TestGizmosFrustum()
    {
        auto* manager = GET_SYSTEM(DebugDrawManager);
        Check(manager != nullptr, "FR0", "DebugDrawManager is reachable");
        if (manager == nullptr)
        {
            return;
        }

        auto* group = manager->TryGetOrCreateDebugDrawGroup("gizmos");
#if defined(Z_HAS_VULKAN)
        Check(group != nullptr, "FR1", "the gizmo group exists");
#else
        Check(group == nullptr, "FR1", "no debug-draw backend: the group is absent by design");
#endif
        if (group == nullptr)
        {
            // 没有后端时必须是 no-op 而不是崩 —— 走到这一行本身就是证明。
            Runtime::Gizmos::SetColor(::Color(1.0f, 1.0f, 1.0f, 1.0f));
            Runtime::Gizmos::SetMatrix(Matrix4x4::IDENTITY);
            Runtime::Gizmos::DrawFrustum(::Vector3(0.0f, 0.0f, 0.0f), 90.0f, 4.0f, 2.0f, 1.0f);
            Check(true, "FR2", "DrawFrustum survives a missing backend");
            return;
        }

        Runtime::Gizmos::SetColor(::Color(1.0f, 1.0f, 1.0f, 1.0f));
        Runtime::Gizmos::SetMatrix(Matrix4x4::IDENTITY);

        // fov=90°, near=2, far=4, aspect=1 ⇒ tan(45°)=1：
        //   近平面半高 = 2、远平面半高 = 4（宽同）。
        //   张开方向 -Y：近平面 y = -2、远平面 y = -4。up = +Z、right = -X。
        group->ClearData();
        Runtime::Gizmos::DrawFrustum(::Vector3(0.0f, 0.0f, 0.0f), 90.0f, 4.0f, 2.0f, 1.0f);
        Check(group->GetLineCount(false) == 12, "FR3",
              "DrawFrustum is 12 wire lines (4 near + 4 far + 4 side)");
        Check(group->GetLineCount(true) == 0, "FR4", "and depth-tested, like every other gizmo");
        Check(group->GetTriangleCount(false) == 0, "FR5",
              "it is a WIRE frustum -- no solid triangles (DrawCube is the solid one)");

        std::vector<DebugDrawVertex> verts;
        group->WriteLineData(verts, false);
        Check(verts.size() == 24, "FR6", "12 lines are 24 vertices");
        if (verts.size() != 24)
        {
            group->ClearData();
            return;
        }

        // --- 轴向：远端在 **-Y**，不是 +Y ---------------------------------
        // 这是这一组唯一的真判据：张开方向写成 +Y，FR3/FR4/FR5/FR6 全部照绿。
        float min_y = verts[0].pos.y;
        float max_y = verts[0].pos.y;
        float max_abs_x = 0.0f;
        float max_abs_z = 0.0f;
        for (const auto& v : verts)
        {
            min_y = std::min(min_y, v.pos.y);
            max_y = std::max(max_y, v.pos.y);
            max_abs_x = std::max(max_abs_x, std::fabs(v.pos.x));
            max_abs_z = std::max(max_abs_z, std::fabs(v.pos.z));
        }
        Check(Near(max_y, -2.0f) && Near(min_y, -4.0f), "FR7",
              "the frustum opens along -Y (this engine's forward): near at y=-2, far at y=-4");
        Check(Near(max_abs_x, 4.0f) && Near(max_abs_z, 4.0f), "FR8",
              "far plane is 4 wide and 4 tall (fov 90 deg => half-height == distance)");

        // --- 台体而不是盒子：侧棱同时在 y 和 z 上变化 ----------------------
        // 盒子的每条边只沿一个轴走。只数图元数的话，`DrawFrustum` 退化成
        // `DrawWireCube` 是绿的（两个都是 12 条线）。
        bool has_diagonal_edge = false;
        for (size_t i = 0; i + 1 < verts.size(); i += 2)
        {
            const ::Vector3& a = verts[i].pos;
            const ::Vector3& b = verts[i + 1].pos;
            if (!Near(a.y, b.y) && !Near(a.z, b.z))
            {
                has_diagonal_edge = true;
            }
        }
        Check(has_diagonal_edge, "FR9",
              "some edge moves in y AND z -- it is a truncated pyramid, not a box");

        // --- aspect：只拉宽，不拉高 ---------------------------------------
        group->ClearData();
        Runtime::Gizmos::DrawFrustum(::Vector3(0.0f, 0.0f, 0.0f), 90.0f, 4.0f, 2.0f, 2.0f);
        verts.clear();
        group->WriteLineData(verts, false);
        float wide_x = 0.0f;
        float wide_z = 0.0f;
        for (const auto& v : verts)
        {
            wide_x = std::max(wide_x, std::fabs(v.pos.x));
            wide_z = std::max(wide_z, std::fabs(v.pos.z));
        }
        Check(Near(wide_x, 8.0f) && Near(wide_z, 4.0f), "FR10",
              "aspect 2 doubles the width only (8 x 4), it does not scale the height");

        // --- fov 是**度**：90 度 vs 90 弧度差 1.6 倍 ------------------------
        group->ClearData();
        // near=1 / far=2（⚠️ 不能取 far==near，那会被退化守卫挡掉，于是变成
        // "0 条线" 而不是 "角度单位错了" —— 一条自相矛盾的断言）。
        Runtime::Gizmos::DrawFrustum(::Vector3(0.0f, 0.0f, 0.0f), 60.0f, 2.0f, 1.0f, 1.0f);
        verts.clear();
        group->WriteLineData(verts, false);
        float fov60_z = 0.0f;
        for (const auto& v : verts)
        {
            fov60_z = std::max(fov60_z, std::fabs(v.pos.z));
        }
        // 远平面半高 = 2 * tan(30°) ≈ 1.1547（当成弧度算会得到 2*|tan(30 rad)| ≈ 12.9）。
        Check(Near(fov60_z, 1.1547005f, 1.0e-3f), "FR11",
              "fov is in DEGREES: 60 deg at distance 2 gives half-height 2*tan(30) = 1.155");

        // --- Gizmos.matrix / Gizmos.color 照常生效 -------------------------
        // ⚠️ 平移量选 +Z：视锥的 z 本来是 -4..+4，平移后变成 1..9。用"min z 变成
        // 1"来验，比"y 没变"强得多 —— 后者在"矩阵根本没被用上"的实现下也是绿的。
        group->ClearData();
        Matrix4x4 translate = Matrix4x4::IDENTITY;
        translate.makeTrans(::Vector3(0.0f, 0.0f, 5.0f));
        Runtime::Gizmos::SetMatrix(translate);
        Runtime::Gizmos::SetColor(::Color(1.0f, 0.0f, 0.0f, 1.0f));
        Runtime::Gizmos::DrawFrustum(::Vector3(0.0f, 0.0f, 0.0f), 90.0f, 4.0f, 2.0f, 1.0f);
        verts.clear();
        group->WriteLineData(verts, false);
        float shift_min_z = verts.empty() ? 0.0f : verts[0].pos.z;
        float shift_max_z = shift_min_z;
        for (const auto& v : verts)
        {
            shift_min_z = std::min(shift_min_z, v.pos.z);
            shift_max_z = std::max(shift_max_z, v.pos.z);
        }
        Check(verts.size() == 24 && Near(shift_min_z, 1.0f) && Near(shift_max_z, 9.0f), "FR12",
              "Gizmos.matrix moves the frustum (+Z by 5: z range -4..4 becomes 1..9)");
        Check(verts.size() == 24 && Near(verts[0].color, ::Vector4(1.0f, 0.0f, 0.0f, 1.0f)),
              "FR13", "Gizmos.color reaches the frustum's vertices");

        Runtime::Gizmos::SetMatrix(Matrix4x4::IDENTITY);
        Runtime::Gizmos::SetColor(::Color(1.0f, 1.0f, 1.0f, 1.0f));

        // --- 退化参数：一条线都不画，且绝不写 NaN ---------------------------
        // 四种退化各来一次。忘了守卫的话 `fov=180` 给出 tan(90°) = inf，一个 inf
        // 顶点会污染整条 draw call —— "某个 gizmo 没了"远好过"整屏乱掉"。
        struct
        {
            const char* what;
            float fov;
            float far_c;
            float near_c;
            float aspect;
        } degenerate[] = {
            {"fov == 0", 0.0f, 4.0f, 2.0f, 1.0f},
            {"fov == 180 (tan(90) diverges)", 180.0f, 4.0f, 2.0f, 1.0f},
            {"near <= 0", 90.0f, 4.0f, 0.0f, 1.0f},
            {"far <= near", 90.0f, 2.0f, 2.0f, 1.0f},
            {"aspect == 0", 90.0f, 4.0f, 2.0f, 0.0f},
        };
        for (const auto& d : degenerate)
        {
            group->ClearData();
            Runtime::Gizmos::DrawFrustum(::Vector3(0.0f, 0.0f, 0.0f), d.fov, d.far_c, d.near_c,
                                         d.aspect);
            verts.clear();
            group->WriteLineData(verts, false);
            bool any_non_finite = false;
            for (const auto& v : verts)
            {
                if (std::isnan(v.pos.x) || std::isnan(v.pos.y) || std::isnan(v.pos.z) ||
                    std::isinf(v.pos.x) || std::isinf(v.pos.y) || std::isinf(v.pos.z))
                {
                    any_non_finite = true;
                }
            }
            Check(group->GetLineCount(false) == 0 && !any_non_finite, "FR14",
                  "a degenerate frustum draws nothing (and no NaN/inf)");
            (void)d.what;
        }

        group->ClearData();
    }

    void TestGizmosWiring()
    {
        const std::string header =
            ReadFile("Source/Runtime/Scripting/ScriptGizmos/ScriptGizmos.h");
        Check(!header.empty(), "GW0", "ScriptGizmos.h is readable");
        Check(CountOccurrences(header, "static void DrawLine(") >= 1, "GW1", "DrawLine is declared");
        Check(CountOccurrences(header, "static void DrawRay(") >= 1, "GW2", "DrawRay is declared");
        Check(CountOccurrences(header, "DrawWireSphere") >= 1, "GW3", "DrawWireSphere is declared");
        Check(CountOccurrences(header, "DrawWireCube") >= 1, "GW4", "DrawWireCube is declared");

        const std::string source =
            ReadFile("Source/Runtime/Scripting/ScriptGizmos/ScriptGizmos.cpp");
        Check(!source.empty(), "GW5", "ScriptGizmos.cpp is readable");
        // 每个入口都得判空：非 Vulkan 构建下 TryGetOrCreateDebugDrawGroup 恒返
        // 回 nullptr，漏一个就是"只在 macOS 上炸"。
        Check(CountOccurrences(source, "group == nullptr") >= 5, "GW6",
              "every draw entry degrades to a no-op without a backend");
        // Gizmos.color / Gizmos.matrix 必须是**文件级**静态。写成函数内 static
        // 局部变量的话 getter 与 setter 各持一个对象 —— 编译能过、运行不崩，
        // 只是 `Gizmos.color = Color.red` 永远不生效。
        Check(CountOccurrences(source, "static ::Color s_Color") == 0, "GW7",
              "the colour is not a function-local static (getter/setter would diverge)");
        Check(CountOccurrences(source, "::Color s_Color") >= 1, "GW8",
              "it is a file-scope static shared by getter and setter");
        // 后端只写 m_Triangles：`AddQuad(_FillMode_solid)` 画不出任何东西。
        //
        // 计数器必须带 `group->`：源码注释里也会提到 AddQuad，不带前缀的 grep
        // 会被解释性文字命中，报一个并不存在的违规（和 ZFUNCTION() 那个坑一样）。
        Check(CountOccurrences(source, "group->AddQuad(") == 0, "GW9",
              "DrawCube does not call AddQuad (the backend never writes solid quads)");
        Check(CountOccurrences(source, "group->AddTriangle(") >= 2, "GW10",
              "it emits solid triangles instead");

        // ---- DrawFrustum ----
        Check(CountOccurrences(header, "static void DrawFrustum(") >= 1, "GW11",
              "DrawFrustum is declared");
        // 12 条线 = 近 4 + 远 4 + 侧棱 4。少一个循环就是 8 条，而 8 条看起来
        // "也像个框"，只有计数能分出来。
        Check(CountOccurrences(source, "add_edge(") >= 3, "GW12",
              "all three edge loops exist (near, far, and the four side edges)");
        // ⚠️ 轴向：张开方向必须是 -Y（语义前），不是 Unity 分量里的 +Z。写成
        // `UNIT_Z` 的话 FR7 会红，但 FR3/FR5/FR6 全绿 —— 所以这里也钉一道。
        Check(CountOccurrences(source, "::Vector3::NEGATIVE_UNIT_Y") >= 1, "GW13",
              "the frustum opens along -Y (this engine's forward), not Unity's +Z");
        Check(CountOccurrences(source, "::Vector3::UNIT_Z") >= 1, "GW14",
              "up is +Z, matching Vector3.up");
        // 退化守卫。没有它 `fov=180` 会把 inf 写进顶点缓冲。
        Check(CountOccurrences(source, "fov >= 180.0f") >= 1, "GW15",
              "a degenerate fov is rejected instead of producing inf");
        Check(CountOccurrences(source, "Math::DegreesToRadians(fov)") >= 1, "GW16",
              "fov is converted from degrees (Unity's unit)");

        // "声明了"不等于"脚本看得见"。发布那一半（绑定表里真的有 `Gizmos`、
        // 且是静态的）由 ScriptVMSmokeTest 的 L 组从 JS 里 `typeof Gizmos` 验
        // —— 不去读 build/Generated 下的产物：那个路径取决于构建目录名，换
        // 个 generator 就断，而门应该测引擎而不是测构建布局。
    }

    // ---------------------------------------------------------------- BC ---
    // Bounds + Color32: Unity's two remaining value types that had zero hits
    // in the whole Runtime tree before this slice.
    //
    // Both are *value* types (a POD that crosses the script boundary as a plain
    // object), so unlike Gizmos there is nothing to reach for through a system
    // registry -- the rules are pure arithmetic and are tested as such. The
    // wiring half below then proves the arithmetic is actually published.

    void TestBoundsAndColor32()
    {
        // --- ::Bounds: 默认盒 --------------------------------------------
        // Unity 的 `default(Bounds)` 是原点上的零尺寸点盒（不是"无效盒"），
        // 于是 `Contains(zero)` 为 true。两个事实一起断言，只写其中一个的话
        // "默认成 AxisAlignedBox 那种反的 min/max" 也能过。
        const ::Bounds zero_box;
        Check(Near(zero_box.center, ::Vector3(0.0f, 0.0f, 0.0f)) && Near(zero_box.extents, ::Vector3(0.0f, 0.0f, 0.0f)),
              "BC0", "default Bounds is a zero box at the origin (Unity's default(Bounds))");
        Check(zero_box.Contains(::Vector3(0.0f, 0.0f, 0.0f)), "BC1",
              "and it Contains the origin (Unity's default(Bounds).Contains(zero) is true)");
        Check(!zero_box.Contains(::Vector3(0.001f, 0.0f, 0.0f)), "BC2", "but nothing else");

        // --- size 是 extents 的两倍 ---------------------------------------
        // 夹具刻意用**非对称**的 (4, 6, 8)：三个分量全相等的话
        // "size 与 extents 搞混" 和 "顺序写错" 都看不出来。
        const ::Bounds box = ::Bounds::FromCenterSize(::Vector3(1.0f, 2.0f, 3.0f), ::Vector3(4.0f, 6.0f, 8.0f));
        Check(Near(box.extents, ::Vector3(2.0f, 3.0f, 4.0f)), "BC3", "extents is HALF of size");
        Check(Near(box.GetSize(), ::Vector3(4.0f, 6.0f, 8.0f)), "BC4", "size is 2 x extents");
        Check(Near(box.GetMin(), ::Vector3(-1.0f, -1.0f, -1.0f)), "BC5", "min is center - extents");
        Check(Near(box.GetMax(), ::Vector3(3.0f, 5.0f, 7.0f)), "BC6", "max is center + extents");

        // --- size 的 setter 保持中心不动 -----------------------------------
        ::Bounds resized = box;
        resized.SetSize(::Vector3(10.0f, 10.0f, 10.0f));
        Check(Near(resized.center, ::Vector3(1.0f, 2.0f, 3.0f)), "BC7", "setting size leaves the centre alone");
        Check(Near(resized.extents, ::Vector3(5.0f, 5.0f, 5.0f)), "BC8", "and sets extents to half of it");

        // --- SetMinMax 反过来是会挪中心的 ----------------------------------
        const ::Bounds from_corners = ::Bounds::FromMinMax(::Vector3(0.0f, 0.0f, 0.0f), ::Vector3(2.0f, 4.0f, 6.0f));
        Check(Near(from_corners.center, ::Vector3(1.0f, 2.0f, 3.0f)), "BC9", "SetMinMax derives the centre");
        Check(Near(from_corners.extents, ::Vector3(1.0f, 2.0f, 3.0f)), "BC10", "and half the diagonal");

        // --- Contains：边界算包含 ------------------------------------------
        Check(box.Contains(::Vector3(-1.0f, -1.0f, -1.0f)), "BC11", "a point exactly on min is contained");
        Check(box.Contains(::Vector3(3.0f, 5.0f, 7.0f)), "BC12", "so is one exactly on max");
        Check(box.Contains(::Vector3(1.0f, 2.0f, 3.0f)), "BC13", "the centre is contained");
        Check(!box.Contains(::Vector3(3.5f, 5.0f, 7.0f)), "BC14", "just outside on one axis is not");

        // --- Intersects：贴面算相交 ----------------------------------------
        // 贴面：box 的 max.x 是 3，所以对方的 min.x 必须正好是 3 —— 它的
        // extents.x 是 2，于是 center.x 是 5（写 7 就分开了，而"分开不相交"
        // 由下一条断言，两条一起才钉得住"贴面算相交"）。
        const ::Bounds touching = ::Bounds::FromCenterSize(::Vector3(5.0f, 2.0f, 3.0f), ::Vector3(4.0f, 6.0f, 8.0f));
        Check(box.Intersects(touching), "BC15", "boxes that merely touch do intersect (Unity counts it)");
        const ::Bounds apart = ::Bounds::FromCenterSize(::Vector3(20.0f, 2.0f, 3.0f), ::Vector3(1.0f, 1.0f, 1.0f));
        Check(!box.Intersects(apart), "BC16", "a separated box does not");
        const ::Bounds overlapping = ::Bounds::FromCenterSize(::Vector3(2.0f, 2.0f, 3.0f), ::Vector3(2.0f, 2.0f, 2.0f));
        Check(box.Intersects(overlapping), "BC17", "an overlapping one does");

        // --- ClosestPoint / SqrDistance ------------------------------------
        Check(Near(box.ClosestPoint(::Vector3(10.0f, 2.0f, 3.0f)), ::Vector3(3.0f, 2.0f, 3.0f)), "BC18",
              "ClosestPoint clamps onto the +X face");
        Check(Near(box.ClosestPoint(::Vector3(1.0f, 2.0f, 3.0f)), ::Vector3(1.0f, 2.0f, 3.0f)), "BC19",
              "a point inside is its own closest point");
        Check(Near(box.SqrDistance(::Vector3(1.0f, 2.0f, 3.0f)), 0.0f), "BC20", "SqrDistance is 0 inside");
        // (10,2,3) 到面 x=3 的距离是 7，平方是 49。用非对称的 7 而不是 1，
        // 免得"忘了平方"也过。
        Check(Near(box.SqrDistance(::Vector3(10.0f, 2.0f, 3.0f)), 49.0f), "BC21",
              "SqrDistance is the SQUARE (7 -> 49, not 7)");

        // --- Expand：只加一半 ----------------------------------------------
        // Unity 的 Expand 先 `amount *= .5f` 再加到 extents 上。这条最容易写
        // 错，而且写错之后 min/max/size 全都自洽 —— 只会让触发体积大一倍。
        ::Bounds expanded = box;
        expanded.Expand(2.0f);
        Check(Near(expanded.GetSize(), ::Vector3(6.0f, 8.0f, 10.0f)), "BC22",
              "Expand(2) grows size by 2 (not 4) -- Unity halves the amount first");
        ::Bounds expanded_vector = box;
        expanded_vector.Expand(::Vector3(2.0f, 0.0f, 4.0f));
        Check(Near(expanded_vector.GetSize(), ::Vector3(6.0f, 6.0f, 12.0f)), "BC23",
              "Expand(Vector3) halves per axis");

        // --- Encapsulate ---------------------------------------------------
        ::Bounds grown = box;
        grown.Encapsulate(::Vector3(0.0f, 0.0f, 20.0f));
        Check(Near(grown.GetMax().z, 20.0f), "BC24", "Encapsulate(point) stretches max");
        Check(Near(grown.GetMin(), ::Vector3(-1.0f, -1.0f, -1.0f)), "BC25", "and leaves min alone");
        Check(Near(grown.center, ::Vector3(1.0f, 2.0f, 9.5f)), "BC26", "so the centre moves");

        // 互补的一条：必须包对方的**两个角**而不是它的 center。只包 center 会
        // 漏掉对方比自己中心更远的那一半 —— 下面这个对方 x 跨 [-10, 10]，
        // 而它自己的 center.x 是 0（落在 box 的 [-1, 3] 之内）：只包 center
        // 的话 max.x 停在 3，包角才是 10。（零盒在这里分不出来：它的两个角
        // 就是 center。）
        ::Bounds grew_by_box = box;
        grew_by_box.Encapsulate(::Bounds::FromCenterSize(::Vector3(0.0f, 2.0f, 3.0f), ::Vector3(20.0f, 6.0f, 8.0f)));
        Check(Near(grew_by_box.GetMax().x, 10.0f), "BC27",
              "Encapsulate(Bounds) uses the far CORNER (the other box's centre is already inside)");
        Check(Near(grew_by_box.GetMin().x, -10.0f), "BC27b", "and the near corner too");

        // --- IntersectRay --------------------------------------------------
        float hit = -1.0f;
        Check(box.IntersectRay(::Vector3(0.0f, 0.0f, -5.0f), ::Vector3(0.0f, 0.0f, 1.0f), hit) && Near(hit, 4.0f),
              "BC28", "a ray from -Z hits the box's near face at t=4");
        Check(box.IntersectRay(::Vector3(1.0f, 2.0f, 3.0f), ::Vector3(0.0f, 0.0f, 1.0f), hit) && Near(hit, 0.0f),
              "BC29", "a ray starting inside hits at t=0 (Unity's answer, not a negative t)");
        Check(!box.IntersectRay(::Vector3(0.0f, 0.0f, -5.0f), ::Vector3(0.0f, 0.0f, -1.0f), hit), "BC30",
              "a ray pointing away misses");
        // 平行且在板外：这里如果先除了零，NaN 参与 min/max 比较会把结果带成
        // "命中"（NaN 的任何比较都是 false，于是 t_enter > t_exit 不成立）。
        Check(!box.IntersectRay(::Vector3(100.0f, 0.0f, -5.0f), ::Vector3(0.0f, 0.0f, 1.0f), hit), "BC31",
              "a ray parallel to a slab and outside it misses (no NaN false positive)");
        Check(box.IntersectRay(::Vector3(1.0f, 2.0f, -5.0f), ::Vector3(0.0f, 0.0f, 1.0f), hit) && Near(hit, 4.0f),
              "BC32", "a ray parallel to a slab but inside it still hits");

        // --- ::Color32 <-> ::Color ----------------------------------------
        const ::Color32 red = ::Color32(::Color(1.0f, 0.0f, 0.0f, 1.0f));
        Check(red.r == 255 && red.g == 0 && red.b == 0 && red.a == 255, "BC33", "Color(1,0,0,1) -> RGBA(255,0,0,255)");
        // 截断而不是四舍五入：0.5 * 255 = 127.5 -> 127（Unity 的 (byte)float）。
        Check(::Color32(::Color(0.5f, 0.5f, 0.5f, 0.5f)).r == 127, "BC34",
              "the conversion TRUNCATES (0.5 -> 127, not 128) -- Unity's (byte)float");
        Check(Near(static_cast<::Color>(::Color32(255, 0, 0, 255)).r, 1.0f), "BC35", "255 -> 1.0 exactly");
        // 来回一趟不保等，这是 Unity 的行为而不是 bug。
        const ::Color half {0.5f, 0.5f, 0.5f, 1.0f};
        Check(!Near(static_cast<::Color>(::Color32(half)).r, 0.5f), "BC36",
              "Color -> Color32 -> Color is NOT the identity (truncation, same as Unity)");
        Check(::Color32(::Color(2.0f, -1.0f, 0.0f, 1.0f)).r == 255 && ::Color32(::Color(2.0f, -1.0f, 0.0f, 1.0f)).g == 0,
              "BC37", "out-of-range channels clamp to 0..255");

        // --- Color32::Lerp -------------------------------------------------
        const ::Color32 black_bytes {0, 0, 0, 255};
        const ::Color32 white_bytes {255, 255, 255, 255};
        Check(::Color32::Lerp(black_bytes, white_bytes, 0.5f).r == 127, "BC38",
              "Lerp(0, 255, 0.5) truncates to 127");
        Check(::Color32::Lerp(black_bytes, white_bytes, 2.0f).r == 255, "BC39", "Lerp clamps t to 1");
        Check(::Color32::Lerp(black_bytes, white_bytes, -1.0f).r == 0, "BC40", "and to 0");
        // Unclamped 的 "unclamped" 只针对 t：字节值域仍然夹（否则 float->uint8
        // 是 UB，MSVC 给 254 而 clang 给 255）。
        Check(::Color32::LerpUnclamped(black_bytes, white_bytes, 2.0f).r == 255, "BC41",
              "LerpUnclamped does not clamp t, but still saturates at 255 (float->uint8 out of range is UB)");

        // ⚠️ BC39/BC40 **分辨不出**"Lerp 到底夹没夹 t"：端点是 0 和 255 时，
        // t 越界的外推值一定撞上字节值域的饱和，夹不夹都是同一个 255 / 0。
        // 要分出来得挑一对**外推之后仍落在 0..255 之内**的端点：
        //   t=2  夹住 -> 100（就是 b）  不夹 -> 200 + (100-200)*2 = 0
        //   t=-1 夹住 -> 200（就是 a）  不夹 -> 200 + (100-200)*(-1) = 300 -> 255
        // 于是 BC39b/BC39c 与 BC39d/BC39e 各是一对互补的断言。
        const ::Color32 two_hundred {200, 200, 200, 255};
        const ::Color32 one_hundred {100, 100, 100, 255};
        Check(::Color32::Lerp(two_hundred, one_hundred, 2.0f).r == 100, "BC39b",
              "Lerp clamps t: t=2 gives exactly b (100), not the extrapolated 0");
        Check(::Color32::LerpUnclamped(two_hundred, one_hundred, 2.0f).r == 0, "BC39c",
              "LerpUnclamped does not clamp t: the same call extrapolates to 0");
        Check(::Color32::Lerp(two_hundred, one_hundred, -1.0f).r == 200, "BC39d",
              "Lerp clamps t: t=-1 gives exactly a (200)");
        Check(::Color32::LerpUnclamped(two_hundred, one_hundred, -1.0f).r == 255, "BC39e",
              "LerpUnclamped extrapolates past a and saturates (200 -> 300 -> 255)");

        // --- 门面 ----------------------------------------------------------
        const ::Bounds via_facade = Runtime::Bounds::Create(::Vector3(1.0f, 2.0f, 3.0f), ::Vector3(4.0f, 6.0f, 8.0f));
        Check(Near(via_facade.extents, ::Vector3(2.0f, 3.0f, 4.0f)), "BC42",
              "Bounds.Create takes SIZE (not extents) -- the classic Unity mix-up");
        Check(Near(Runtime::Bounds::GetSize(via_facade), ::Vector3(4.0f, 6.0f, 8.0f)), "BC43", "Bounds.GetSize");
        Check(Near(Runtime::Bounds::GetMin(via_facade), ::Vector3(-1.0f, -1.0f, -1.0f)), "BC44", "Bounds.GetMin");
        Check(Near(Runtime::Bounds::GetMax(via_facade), ::Vector3(3.0f, 5.0f, 7.0f)), "BC45", "Bounds.GetMax");
        Check(Runtime::Bounds::Contains(via_facade, ::Vector3(1.0f, 2.0f, 3.0f)), "BC46", "Bounds.Contains");
        Check(Runtime::Bounds::Intersects(via_facade, overlapping), "BC47", "Bounds.Intersects");
        Check(Near(Runtime::Bounds::ClosestPoint(via_facade, ::Vector3(10.0f, 2.0f, 3.0f)),
                   ::Vector3(3.0f, 2.0f, 3.0f)),
              "BC48", "Bounds.ClosestPoint");
        Check(Near(Runtime::Bounds::SqrDistance(via_facade, ::Vector3(10.0f, 2.0f, 3.0f)), 49.0f), "BC49",
              "Bounds.SqrDistance");

        // 非破坏性：Encapsulate / Expand 返回新值，传入的那个不动。
        const ::Bounds before = via_facade;
        const ::Bounds after = Runtime::Bounds::Encapsulate(via_facade, ::Vector3(0.0f, 0.0f, 20.0f));
        Check(Near(before.center, ::Vector3(1.0f, 2.0f, 3.0f)) && Near(before.extents, ::Vector3(2.0f, 3.0f, 4.0f)),
              "BC50", "Encapsulate does not mutate the argument (it returns a new value)");
        Check(Near(after.GetMax().z, 20.0f), "BC51", "and the returned box really grew");
        Check(Near(Runtime::Bounds::Expand(via_facade, 2.0f).GetSize(), ::Vector3(6.0f, 8.0f, 10.0f)), "BC52",
              "Bounds.Expand grows size by amount");
        Check(Near(Runtime::Bounds::ExpandVector(via_facade, ::Vector3(2.0f, 0.0f, 4.0f)).GetSize(),
                   ::Vector3(6.0f, 6.0f, 12.0f)),
              "BC53", "Bounds.ExpandVector is per-axis");
        Check(Near(Runtime::Bounds::EncapsulateBounds(via_facade, ::Bounds::FromCenterSize(
                                                                      ::Vector3(0.0f, 2.0f, 3.0f),
                                                                      ::Vector3(20.0f, 6.0f, 8.0f)))
                       .GetMax()
                       .x,
                   10.0f),
              "BC54", "Bounds.EncapsulateBounds takes the far corner, not the centre");

        // IntersectRay：null 射线与真射线。
        Check(Runtime::Bounds::IntersectRay(via_facade, nullptr) < 0.0f, "BC55", "a null ray is a miss, not a crash");
        ::Ray ray;
        ray.SetOrigin(::Vector3(0.0f, 0.0f, -5.0f));
        ray.SetDirection(::Vector3(0.0f, 0.0f, 1.0f));
        Check(Near(Runtime::Bounds::IntersectRay(via_facade, &ray), 4.0f), "BC56", "Bounds.IntersectRay(Ray)");
        ray.SetDirection(::Vector3(0.0f, 0.0f, -1.0f));
        Check(Runtime::Bounds::IntersectRay(via_facade, &ray) < 0.0f, "BC57", "a miss is reported as a negative");
        Check(Runtime::Bounds::ToString(::Bounds()) == "Center: (0.0, 0.0, 0.0), Extents: (0.0, 0.0, 0.0)", "BC58",
              "Bounds.ToString matches Unity's format");

        // Color32 门面 + Color 侧的两个互转入口。
        Check(Runtime::Color32::FromColor(::Color(1.0f, 0.0f, 0.0f, 1.0f)) == ::Color32(255, 0, 0, 255), "BC59",
              "Color32.FromColor");
        Check(Near(Runtime::Color32::ToColor(::Color32(255, 0, 0, 255)).r, 1.0f), "BC60", "Color32.ToColor");
        Check(Runtime::Color32::ToString(::Color32(255, 0, 0, 255)) == "RGBA(255, 0, 0, 255)", "BC61",
              "Color32.ToString matches Unity's format");
        Check(Runtime::Color::ToColor32(::Color(0.0f, 1.0f, 0.0f, 1.0f)) == ::Color32(0, 255, 0, 255), "BC62",
              "Color.ToColor32");
        Check(Near(Runtime::Color::FromColor32(::Color32(0, 255, 0, 255)).g, 1.0f), "BC63", "Color.FromColor32");
    }

    void TestBoundsAndColor32Wiring()
    {
        // 值类型只在**声明了 interface 且 Converter 存在**的时候才真的能过边界。
        // 少任何一半都是"编译能过、脚本一跑就是 undefined"——所以两边都要 grep。
        const std::string binding_source =
            ReadFile("Source/Runtime/Scripting/Binding/ScriptBinding.cpp");
        Check(!binding_source.empty(), "BCW0", "ScriptBinding.cpp is readable");
        Check(CountOccurrences(binding_source, "declare interface Bounds") >= 1, "BCW1",
              "the .d.ts declares `interface Bounds`");
        Check(CountOccurrences(binding_source, "declare interface Color32") >= 1, "BCW2",
              "the .d.ts declares `interface Color32`");

        const std::string binding_header =
            ReadFile("Source/Runtime/Scripting/Binding/ScriptBinding.h");
        Check(!binding_header.empty(), "BCW3", "ScriptBinding.h is readable");
        Check(CountOccurrences(binding_header, "struct Converter<Bounds>") >= 1, "BCW4",
              "there is a Converter for Bounds");
        Check(CountOccurrences(binding_header, "struct Converter<Color32>") >= 1, "BCW5",
              "there is a Converter for Color32");

        const std::string bounds_source = ReadFile("Source/Runtime/Core/Math/Bounds.cpp");
        Check(!bounds_source.empty(), "BCW6", "Bounds.cpp is readable");
        // 平行的那一轴必须判"起点在板内"，否则除零出 NaN、NaN 参与比较会把结
        // 果带成假命中。
        Check(CountOccurrences(bounds_source, "1e-8f") >= 1, "BCW7",
              "IntersectRay guards the parallel axis instead of dividing by zero");

        const std::string color_source = ReadFile("Source/Runtime/Core/Color/Color.cpp");
        Check(!color_source.empty(), "BCW8", "Color.cpp is readable");
        // 截断而不是四舍五入：出现 `std::lround(` / `std::round(` 说明 Unity 的
        // (byte)float 语义被"修好"了，于是 0.5 会变成 128 而不是 127。
        //
        // ⚠️ 必须带左括号：源码注释里也要提 `std::lround` 这个名字，不带括号的
        // grep 会被解释性文字命中，报一个并不存在的违规（和 ZFUNCTION()、
        // group->AddQuad( 同一个坑）。
        Check(CountOccurrences(color_source, "lround(") == 0 && CountOccurrences(color_source, "std::round(") == 0,
              "BCW9", "the byte conversion truncates (no rounding helper slipped in)");

        const std::string facade_source =
            ReadFile("Source/Runtime/Scripting/ScriptBounds/ScriptBounds.cpp");
        Check(!facade_source.empty(), "BCW10", "ScriptBounds.cpp is readable");
        Check(CountOccurrences(facade_source, "ray == nullptr") >= 1, "BCW11",
              "Bounds.IntersectRay(null) degrades instead of dereferencing");
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

        /// What `EvalScriptPredicate` answers. Flipping this is how a test
        /// makes a WaitUntil / WaitWhile become ready.
        bool predicate_value = false;

        /// How many times the predicate has been polled. A `WaitUntil`
        /// implementation that snapshots the value at yield time instead of
        /// asking every frame polls exactly once -- which is invisible if you
        /// only assert "eventually resumed".
        int predicate_polls = 0;

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

        bool EvalScriptPredicate(uint64_t script_id, bool& out_value) override
        {
            if (script_id == 0)
            {
                return false;
            }
            ++predicate_polls;
            out_value = predicate_value;
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

        // --- WaitUntil / WaitWhile ----------------------------------------
        //
        // 调度器**本来**就支持这两个等待（`Coroutine.cpp` 的 TickUpdate 里轮询
        // std::function），所以这一段测的不是调度器，是"脚本侧那个谓词有没有
        // 真的被接到轮询上"这一层适配器。
        //
        // 关键的两条：谓词必须**每帧**被问（不是 yield 那一帧快照一次），且
        // WaitWhile 是 WaitUntil 的**反** —— 两个只测一个的话，"把 kWhile 当成
        // kUntil 转发"这种实现是绿的。
        {
            CoroutineProbe probe;
            ScriptYield until_yield {};
            until_yield.kind = ScriptYieldKind::kUntil;
            ScriptYield tail {};
            probe.body = {until_yield, tail};
            probe.predicate_value = false;

            probe.StartCoroutine("Until");
            Check(probe.steps == 1, "BE44", "WaitUntil runs the first segment synchronously");
            Check(probe.predicate_polls == 0, "BE45", "and is not polled before the first Update");

            AdvanceUpdate(1, 0.1f, 0.1f);
            Check(probe.steps == 1, "BE46", "a false predicate does not resume");
            Check(probe.predicate_polls == 1, "BE47", "but it was asked");
            AdvanceUpdate(1, 0.1f, 0.1f);
            Check(probe.steps == 1, "BE48", "and still does not on the next frame");
            // 互补的一半：只在 yield 那一帧问一次的实现 polls 恒为 1，于是
            // "等下去"这件事永远不成立 —— 而上面三条在那种实现下也全绿。
            Check(probe.predicate_polls == 2, "BE49",
                  "the predicate is re-polled every frame (not snapshotted once)");

            probe.predicate_value = true;
            AdvanceUpdate(1, 0.1f, 0.1f);
            Check(probe.steps == 2, "BE50", "and flipping it resumes the coroutine");
        }

        {
            CoroutineProbe probe;
            ScriptYield while_yield {};
            while_yield.kind = ScriptYieldKind::kWhile;
            ScriptYield tail {};
            probe.body = {while_yield, tail};
            probe.predicate_value = true;

            probe.StartCoroutine("While");
            AdvanceUpdate(1, 0.1f, 0.1f);
            Check(probe.steps == 1, "BE51", "WaitWhile blocks while the predicate is true");
            Check(probe.predicate_polls == 1, "BE52", "and polls it every frame");

            probe.predicate_value = false;
            AdvanceUpdate(1, 0.1f, 0.1f);
            Check(probe.steps == 2, "BE53", "and resumes when the predicate goes false");
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

        // --- WaitUntil / WaitWhile ----------------------------------------
        // 这两个带闭包，是家族里唯一"少一环就永远挂着、且不报错"的成员：
        // 强引用没建 → 谓词被回收；pump 没转发 → 调度器拿不到 WaitUntil；
        // 调度器不轮询 → 位永远停在第一段。每一环都要单独 grep。
        Check(CountOccurrences(env_source, "g.WaitUntil") >= 1,
              "BF33", "WaitUntil is installed as a global");
        Check(CountOccurrences(env_source, "g.WaitWhile") >= 1,
              "BF34", "and WaitWhile");
        // 标记必须被读走：GeneratorNext 少了分支的话，yield 出来的对象会被
        // 当成"下一帧继续"（静默的语义降级，不是错误）。
        Check(CountOccurrences(env_source, "\"until\"") >= 1,
              "BF35", "GeneratorNext reads the 'until' tag");
        Check(CountOccurrences(env_source, "\"while\"") >= 1,
              "BF36", "and the 'while' tag");
        // 声明在 .h、定义在 .cpp，所以两边各判一次（合起来才是"既有入口也有
        // 实现"）——只数 .cpp 会得到 1，写成 >= 2 会误报。
        Check(CountOccurrences(env_source, "ScriptEnv::CallPredicate") >= 1,
              "BF37", "the predicate poll is implemented");
        const std::string env_header = ReadFile("Source/Runtime/Scripting/Wrapper/ScriptEnv.h");
        Check(!env_header.empty(), "BF37b", "ScriptEnv.h is readable");
        Check(CountOccurrences(env_header, "CallPredicate") >= 1,
              "BF37c", "and exposed as an entry point");

        const std::string yield_header =
            ReadFile("Source/Runtime/Function/Framework/Coroutine/ScriptYield.h");
        Check(!yield_header.empty(), "BF38", "ScriptYield.h is readable");
        Check(CountOccurrences(yield_header, "kUntil") >= 1 && CountOccurrences(yield_header, "kWhile") >= 1,
              "BF39", "both kinds exist in the POD that crosses the boundary");

        // ⚠️ 变量名不能叫 `behaviour_source`：本函数里已经有一个同名的了。
        const std::string pump_source =
            ReadFile("Source/Runtime/Function/Framework/Component/Behaviour.cpp");
        Check(!pump_source.empty(), "BF40", "Behaviour.cpp is readable");
        // 转发必须分别指向 WaitUntil / WaitWhile：两个都写成 WaitUntil 的话
        // WaitWhile 永远不会恢复，而且没有任何日志。
        Check(CountOccurrences(pump_source, "WaitUntil {") >= 1,
              "BF41", "the pump forwards kUntil to WaitUntil");
        Check(CountOccurrences(pump_source, "WaitWhile {") >= 1,
              "BF42", "and kWhile to WaitWhile (not to WaitUntil again)");

        const std::string ts_component =
            ReadFile("Source/Runtime/Function/Framework/Component/Script/TypeScriptComponent.cpp");
        Check(CountOccurrences(ts_component, "EvalScriptPredicate") >= 1,
              "BF43", "the script component answers the predicate poll");
        // 谓词的强引用必须有释放点：`yield new WaitUntil(() => ...)` 在循环里
        // 每帧造一个新闭包，只放生成器不放谓词就是每帧一个 GC 看不见的泄漏。
        // 4 = 定义 1 + StepScriptCoroutine 两条分支各 1 + EndScriptCoroutine 1。
        // 泄漏在运行期**不可观测**（GC 看不见强引用），所以只能靠数调用点：
        // 少了 EndScriptCoroutine 那一次，协程结束时谓词就永远挂着。
        Check(CountOccurrences(ts_component, "SetCoroutinePredicate") >= 4,
              "BF44", "the predicate ref is replaced, released on end and cleared on teardown");
        // 5 = EndScriptCoroutine 的生成器 1 + SetCoroutinePredicate 的两条释放
        // 分支 2 + TearDown 的两趟循环 2。同样是纯泄漏守卫。
        Check(CountOccurrences(ts_component, "ReleaseCoroutineValue") >= 5,
              "BF48", "every value-ref release path is present (leak guard)");

        Check(CountOccurrences(binding_source, "declare class WaitUntil") >= 1,
              "BF45", "the .d.ts declares WaitUntil");
        Check(CountOccurrences(binding_source, "declare class WaitWhile") >= 1,
              "BF46", "and WaitWhile");
        Check(CountOccurrences(binding_source, "constructor(predicate: () => boolean);") >= 2,
              "BF47", "and both constructors take a predicate (the whole point)");

        // --- `yield return StartCoroutine(other)` --------------------------
        //
        // 这一类 yield 与前面六个的差别是：句柄**不带** `__zWait` 标记（Unity
        // 里就是直接 yield 那个句柄）。所以"读标记"这条通用路径认不出它，缺了
        // 按形状认的分支就退化成"下一帧继续"——一个**静默**的语义降级：外层
        // 不等内层了，测试里看起来只是快了一帧。
        Check(CountOccurrences(env_source, "__zId") >= 2,
              "BF49", "the Coroutine handle carries a numeric id (shim writes, GeneratorNext reads)");
        Check(CountOccurrences(env_source, "pesapi_get_property(m_Papis, env, yielded, \"__zId\")") >= 1,
              "BF50", "GeneratorNext recognises the handle by shape, not by __zWait");
        // 顺序门：`__zId` 必须在读 `__zWait` **之前**判。反过来的话句柄上
        // `__zWait` 是 undefined，落到"未知对象 ⇒ 下一帧继续"，嵌套等待整个
        // 消失，而且没有日志。位置比较是这里唯一能抓到它的判据。
        const std::size_t id_at = env_source.find("__zId\")");
        const std::size_t wait_at = env_source.find("pesapi_get_property(m_Papis, env, yielded, \"__zWait\")");
        Check(id_at != std::string::npos && wait_at != std::string::npos && id_at < wait_at,
              "BF51", "the __zId check comes before the __zWait tag read (order gate)");

        Check(CountOccurrences(yield_header, "kCoroutine") >= 1,
              "BF52", "ScriptYield carries the nested-wait kind");
        Check(CountOccurrences(yield_header, "coroutine_id") >= 1,
              "BF53", "and a slot for the id being waited on");
        // 槽位声明了不等于被读了：pump 里不写 `yielded.coroutine_id` 而写个常量
        // 的话，BF54 的 `WaitForCoroutine {` 照样命中。所以两边各判一次。
        Check(CountOccurrences(pump_source, "yielded.coroutine_id") >= 1,
              "BF53b", "and the pump reads that slot (not a hardcoded id)");
        Check(CountOccurrences(pump_source, "WaitForCoroutine {") >= 1,
              "BF54", "the pump forwards kCoroutine to WaitForCoroutine");
        // 名义类型门：`YieldInstruction` 带私有字段，只有真子类才能塞进
        // `Generator<YieldInstruction | null>`。不 extends 的话用户必须写
        // `as any`，等于把类型检查关掉 —— 而这行是唯一能抓到它的地方。
        Check(CountOccurrences(binding_source, "declare class Coroutine extends YieldInstruction") >= 1,
              "BF55", "the .d.ts makes Coroutine a YieldInstruction (so it can be yielded)");
    }

    // ---------------------------------------------------------------- LU ---
    // Unity 的 `LateUpdate` 阶段。

    /// 记录自己被哪一趟 Tick 打到了。
    ///
    /// ⚠️ 故意**不**写 `REGISTER_CLASS(LateTickProbe)`：本 exe 不编 .gen.cpp，
    /// 一个没注册过的类型 `GetTypeName()` 会拿 `Type::DefaultTypeIndex`
    /// (0x80000000) 去索引 `RuntimeTypeArray::types[1024]` —— 不是返回空串，
    /// 是越界读。不带 REGISTER_CLASS 时 `GetTypeVirtualInternal()` 走基类
    /// `Component` 的那份，于是 `GetTypeName()` == "Component"，而 Component
    /// 是 `RegisterRuntimeTypesForTest()` 显式注册过的，索引有效。
    ///
    /// 代价：白名单按类型名判，所以这个探针和 Component 共享同一条白名单项。
    class LateTickProbe : public Component
    {
    public:
        LateTickProbe(std::string* shared_log, char tag) : m_Log(shared_log), m_Tag(tag) {}

        int updates = 0;
        int late = 0;
        int fixed = 0;

        /// Unity 的 `Behaviour.enabled` 语义：Component 基类上没有 setter（Unity
        /// 也一样，`enabled` 挂在 Behaviour 上），这里自己记一个再重写判据，
        /// 免得为了让测试跑起来去改引擎的公开头。
        void SetProbeEnabled(bool on) { m_ProbeEnabled = on; }
        bool isEnabled() const override { return m_ProbeEnabled; }

        void Reset()
        {
            updates = 0;
            late = 0;
            fixed = 0;
        }

        void Tick(float) override
        {
            ++updates;
            Append('u');
        }
        void FixedTick(float) override
        {
            ++fixed;
            Append('f');
        }
        void LateTick(float) override
        {
            ++late;
            Append('l');
        }

    private:
        void Append(char phase)
        {
            if (m_Log != nullptr)
            {
                m_Log->push_back(m_Tag);
                m_Log->push_back(phase);
            }
        }

        std::string* m_Log;
        char m_Tag;
        bool m_ProbeEnabled {true};
    };

    void TestLateUpdate()
    {
        // 编辑态下 `shouldComponentTick` 认白名单，而探针报出来的类型名是
        // "Component"，所以白名单项就是它。测完必须撤掉：白名单是全局的，
        // 留着会静默地让后面每一组的 Component 都开始被 Tick。
        const bool had_component_whitelisted =
            g_editorTickComponentTypes.find("Component") != g_editorTickComponentTypes.end();
        g_editorTickComponentTypes.insert("Component");

        ObjectManager* om = GET_SYSTEM(ObjectManager);
        Check(om != nullptr, "LU0", "ObjectManager is reachable");
        if (om == nullptr)
        {
            if (!had_component_whitelisted) { g_editorTickComponentTypes.erase("Component"); }
            return;
        }

        Check(TypeManager::GetInstance().ClassNameToType("Component") != nullptr,
              "LU1", "Component is a registered type (the probe reports that name)");

        Level level;
        level.setLevelResUrl("Assets/Scenes/LateUpdate.level");
        level.LoadObjectGraph({}, false);
        Check(level.IsLoaded(), "LU2", "the synthetic level is loaded");

        GameObject* go = new GameObject(ObjectIDAllocator::Alloc());
        om->AllocateAndAssignInstanceID(go);
        Transform* transform = new Transform();
        om->AllocateAndAssignInstanceID(transform);
        go->addComponent(transform);
        transform->PostLoadResource(go);

        std::string log;
        LateTickProbe* a = new LateTickProbe(&log, 'a');
        LateTickProbe* b = new LateTickProbe(&log, 'b');
        // 必须在 AllocateAndAssignInstanceID 之前：它会把 m_InstanceID 归零。
        a->InitializeRuntimeTypeInfo();
        b->InitializeRuntimeTypeInfo();
        om->AllocateAndAssignInstanceID(a);
        om->AllocateAndAssignInstanceID(b);
        go->addComponent(a);
        go->addComponent(b);
        Check(a->GetTypeName() == std::string("Component"), "LU3",
              "the probe reports a registered type name (not an out-of-range index)");
        Check(level.RegisterRuntimeGObject(go), "LU4", "the object is in the level's runtime table");

        // --- 每帧一次，且在 Update 之后 ------------------------------------
        go->Tick(0.016f);
        go->LateTick(0.016f);
        Check(a->updates == 1 && a->late == 1, "LU5", "one Update and one LateUpdate per frame");
        // 关键的顺序断言：所有 Update 都跑完才轮到任何 LateUpdate。写在 Update
        // 里的相机跟随之所以抖，就是因为缺这一趟；而"在 Tick 的循环里顺手调
        // LateTick"这种实现会给出 aualbubl 而不是 aubualbl。
        Check(log == "aubualbl", "LU6", "every Update runs before any LateUpdate");

        // --- 与 FixedUpdate 的分工：按帧，不按步 ----------------------------
        log.clear();
        a->Reset();
        b->Reset();
        go->FixedTick(0.02f);
        go->FixedTick(0.02f);
        go->FixedTick(0.02f);
        go->Tick(0.016f);
        go->LateTick(0.016f);
        Check(a->fixed == 3, "LU7", "three fixed steps reach the component three times");
        Check(a->updates == 1, "LU8", "but Update still runs once");
        Check(a->late == 1, "LU9", "and LateUpdate runs once -- it is per frame, not per step");

        // --- 三个 Tick 的启用判据必须逐字一致 ------------------------------
        log.clear();
        a->Reset();
        b->Reset();
        a->SetProbeEnabled(false);
        go->Tick(0.016f);
        go->LateTick(0.016f);
        Check(a->updates == 0, "LU10", "a disabled component gets no Update");
        // 互补的另一半：只判了 Update 而漏了 LateUpdate 的实现会在这里给出
        // late == 1 —— 相机于是"对象还在、跟随没了"，且只在禁用窗口内出现。
        Check(a->late == 0, "LU11", "and no LateUpdate either (the two gates agree)");
        Check(b->updates == 1 && b->late == 1, "LU12", "while the enabled one still gets both");
        a->SetProbeEnabled(true);

        log.clear();
        a->Reset();
        b->Reset();
        go->SetActive(false);
        Check(!go->IsActiveInHierarchy(), "LU13", "SetActive(false) really deactivates it");
        go->Tick(0.016f);
        go->LateTick(0.016f);
        Check(a->updates == 0 && a->late == 0, "LU14", "an inactive object gets neither Update nor LateUpdate");
        go->SetActive(true);
        log.clear();
        a->Reset();
        b->Reset();
        go->Tick(0.016f);
        go->LateTick(0.016f);
        Check(a->updates == 1 && a->late == 1, "LU15", "and reactivating restores both");

        // --- 编辑态白名单同样管 LateUpdate ----------------------------------
        log.clear();
        a->Reset();
        b->Reset();
        g_editorTickComponentTypes.erase("Component");
        go->Tick(0.016f);
        go->LateTick(0.016f);
        Check(a->updates == 0 && a->late == 0, "LU16",
              "the edit-mode whitelist gates LateUpdate too (not just Update)");
        g_editorTickComponentTypes.insert("Component");

        // --- Level 那一层 ---------------------------------------------------
        log.clear();
        a->Reset();
        b->Reset();
        level.LateTick(0.016f);
        Check(a->late == 1, "LU17", "Level::LateTick reaches runtime-registered objects");
        Check(a->updates == 0, "LU18", "and does not re-run Update");

        // LateTick 不做销毁：销毁统一在帧末收口。这里要能查到"还在"。
        GameObject* doomed = new GameObject(ObjectIDAllocator::Alloc());
        om->AllocateAndAssignInstanceID(doomed);
        Transform* doomed_transform = new Transform();
        om->AllocateAndAssignInstanceID(doomed_transform);
        doomed->addComponent(doomed_transform);
        doomed_transform->PostLoadResource(doomed);
        Check(level.RegisterRuntimeGObject(doomed), "LU19", "a second runtime object is registered");
        level.QueueDestroy(doomed->GetID());
        level.LateTick(0.016f);
        Check(level.FindRuntimeGObject(doomed->GetID()) != nullptr, "LU20",
              "LateUpdate does not destroy (the queue survives it)");
        level.FlushPendingDestroys();
        Check(level.FindRuntimeGObject(doomed->GetID()) == nullptr, "LU21",
              "the frame-end flush does (so the check above was not vacuous)");

        level.UnregisterRuntimeGObject(go->GetID());
        delete a;
        delete b;
        delete go;
        if (!had_component_whitelisted) { g_editorTickComponentTypes.erase("Component"); }
    }

    // ---------------------------------------------------------------- LUW --
    void TestLateUpdateWiring()
    {
        const std::string app = ReadFile("Source/Runtime/Application/Application.cpp");
        Check(!app.empty(), "LUW0", "Application.cpp is readable");
        // 接线门：Level / WorldManager / Component 都实现了 LateTick，但只要
        // 主循环不调，整个阶段就不存在 —— 而且没有任何报错可循。
        Check(CountOccurrences(app, "WorldManager)->LateTick(delta_time)") >= 1,
              "LUW1", "the frame loop drives WorldManager::LateTick");
        // 顺序：必须在 Update 之后、WaitForLateUpdate 之前。
        const std::size_t at_update = app.find("WorldManager)->Tick(delta_time)");
        const std::size_t at_late = app.find("WorldManager)->LateTick(delta_time)");
        // ⚠️ 必须带 `coroutines->`：注释里也提到了 TickLateUpdate 这个名字，不带
        // 限定符的 find 会命中注释（比调用点更靠前），把这条判成"顺序反了"。
        const std::size_t at_late_coroutines = app.find("coroutines->TickLateUpdate");
        Check(at_update != std::string::npos && at_late != std::string::npos && at_update < at_late,
              "LUW2", "LateTick is called after the Update tick");
        Check(at_late_coroutines != std::string::npos && at_late < at_late_coroutines,
              "LUW3", "and before the LateUpdate coroutine phase");

        const std::string world_source = ReadFile("Source/Runtime/Function/Framework/World/WorldManager.cpp");
        Check(!world_source.empty(), "LUW4", "WorldManager.cpp is readable");
        Check(CountOccurrences(world_source, "void WorldManager::LateTick") >= 1,
              "LUW5", "WorldManager::LateTick is implemented");
        Check(CountOccurrences(world_source, "LateTickLoadedLevels") >= 2,
              "LUW6", "and world-partition levels are late-ticked too (declared + called)");
        // 转发必须是 LateTick 而不是 Tick：写成 Tick 的话一帧会跑两遍 Update，
        // 而且因为没有别的调用点，症状只有"逻辑莫名快一倍"。
        Check(CountOccurrences(world_source, "m_CurrentActiveLevel->LateTick(delta_time)") >= 1,
              "LUW6a", "the single-level path forwards to Level::LateTick (not Level::Tick)");
        Check(CountOccurrences(world_source, "level->LateTick(delta_time)") >= 1,
              "LUW6b", "and so does the world-partition path");

        const std::string level_source = ReadFile("Source/Runtime/Function/Framework/Level/Level.cpp");
        Check(!level_source.empty(), "LUW7", "Level.cpp is readable");
        // 两处遍历：主表 + 运行时表。写成 `LateTick(float` 会把实现的签名漏掉，
        // 写成 `LateTick(` 又会被 Level.h 的声明之类干扰 —— 这里两条一起判。
        Check(CountOccurrences(level_source, "->LateTick(delta_time)") >= 2,
              "LUW8", "Level::LateTick walks both the level table and the runtime table");
        Check(CountOccurrences(level_source, "void Level::LateTick") >= 1,
              "LUW8a", "Level::LateTick is implemented");
        // LateTick 里出现物理推进或销毁收口就说明它把 FixedTick / 帧末的活干了。
        Check(CountOccurrences(level_source, "physics_scene->Tick") == 1,
              "LUW9", "physics is stepped in exactly one place (FixedTick)");

        const std::string go_source = ReadFile("Source/Runtime/BaseClasses/GameObject.cpp");
        Check(!go_source.empty(), "LUW10", "GameObject.cpp is readable");
        // 三趟 Tick 的判据必须逐条相同。数 IsActiveInHierarchy / shouldComponentTick
        // 出现 3 次是这个不变式的机器可读形式。
        Check(CountOccurrences(go_source, "IsActiveInHierarchy()") >= 3,
              "LUW11", "all three tick passes gate on activeInHierarchy");
        Check(CountOccurrences(go_source, "shouldComponentTick(") >= 4,
              "LUW12", "and on the edit-mode whitelist (3 call sites + the definition)");

        const std::string ts_source =
            ReadFile("Source/Runtime/Function/Framework/Component/Script/TypeScriptComponent.cpp");
        Check(!ts_source.empty(), "LUW13", "TypeScriptComponent.cpp is readable");
        Check(CountOccurrences(ts_source, "OnLateUpdate") >= 1,
              "LUW14", "the script component forwards to the JS OnLateUpdate");

        const std::string binding_source =
            ReadFile("Source/Runtime/Scripting/Binding/ScriptBinding.cpp");
        Check(CountOccurrences(binding_source, "OnLateUpdate(delta_time: number): void;") >= 1,
              "LUW15", "the .d.ts declares OnLateUpdate on Behaviour");

        const std::string env_source = ReadFile("Source/Runtime/Scripting/Wrapper/ScriptEnv.cpp");
        Check(!env_source.empty(), "LUW16", "ScriptEnv.cpp is readable");
        Check(CountOccurrences(env_source, "g.Behaviour.prototype.OnLateUpdate") >= 1,
              "LUW17", "the JS Behaviour prototype has an OnLateUpdate default");
    }

    // ---------------------------------------------------------------- EN ---
    // Unity 的 `OnEnable` / `OnDisable`。
    //
    // 行为那一半在 ScriptComponentSmokeTest（要真的 JS 实例才能数回调）；这里
    // 只守接线：三个声明点少任何一个，用户写 `OnEnable()` 都要么编译不过、要么
    // 编译过了永远不触发。
    void TestEnabledCallbacksWiring()
    {
        const std::string ts_source =
            ReadFile("Source/Runtime/Function/Framework/Component/Script/TypeScriptComponent.cpp");
        Check(!ts_source.empty(), "EN0", "TypeScriptComponent.cpp is readable");
        Check(CountOccurrences(ts_source, "\"OnEnable\"") >= 1, "EN1",
              "the C++ side can dispatch OnEnable");
        Check(CountOccurrences(ts_source, "\"OnDisable\"") >= 1, "EN2",
              "and OnDisable");
        // 合取而不是"某个条件变了就发"：IsActiveInHierarchy 必须出现在同一个
        // 判定式里。写成只看 isEnabled() 的实现会在父对象停用时漏掉 OnDisable。
        Check(CountOccurrences(ts_source, "IsActiveInHierarchy()") >= 1, "EN3",
              "the gate is the conjunction with activeInHierarchy (Unity's isActiveAndEnabled)");
        Check(CountOccurrences(ts_source, "isEnabled()") >= 1, "EN4",
              "and with the component's own enabled flag");
        // 销毁前必须先 OnDisable：Unity 的顺序是 OnDisable -> OnDestroy。
        Check(CountOccurrences(ts_source, "OnDisable") >= 2, "EN5",
              "OnDisable fires both on transition and before OnDestroy");

        const std::string ts_header =
            ReadFile("Source/Runtime/Function/Framework/Component/Script/TypeScriptComponent.h");
        Check(!ts_header.empty(), "EN6", "TypeScriptComponent.h is readable");
        Check(CountOccurrences(ts_header, "void SetEnabled(bool enabled) override;") >= 1, "EN7",
              "the component hooks its own enabled setter");
        Check(CountOccurrences(ts_header, "void OnOwnerActiveChanged(bool active) override;") >= 1,
              "EN8", "and the host's activeInHierarchy change");

        const std::string binding_source =
            ReadFile("Source/Runtime/Scripting/Binding/ScriptBinding.cpp");
        Check(CountOccurrences(binding_source, "OnEnable(): void;") >= 1, "EN9",
              "the .d.ts declares OnEnable on Behaviour");
        Check(CountOccurrences(binding_source, "OnDisable(): void;") >= 1, "EN10",
              "and OnDisable");

        const std::string env_source = ReadFile("Source/Runtime/Scripting/Wrapper/ScriptEnv.cpp");
        Check(CountOccurrences(env_source, "g.Behaviour.prototype.OnEnable") >= 1, "EN11",
              "the JS Behaviour prototype has an OnEnable default");
        Check(CountOccurrences(env_source, "g.Behaviour.prototype.OnDisable") >= 1, "EN12",
              "and an OnDisable default (a missing one is a per-object TypeError)");
    }

    // ---------------------------------------------------------------- AM ---
    // Unity 的应用层消息（OnApplicationFocus / Pause / Quit、
    // OnTransformParentChanged、OnValidate）。

    /// 记录收到了哪些应用层消息。
    ///
    /// ⚠️ 与 LateTickProbe 同样**不**写 REGISTER_CLASS：本 exe 不编 .gen.cpp，
    /// 未注册类型的 `GetTypeName()` 会拿 `Type::DefaultTypeIndex` 越界索引
    /// RuntimeTypeArray。不带它时走基类 Component 的那份，名字是 "Component"。
    class AppEventProbe : public Component
    {
    public:
        AppEventProbe(std::string* shared_log, char tag) : m_Log(shared_log), m_Tag(tag) {}

        int focus_true = 0;
        int focus_false = 0;
        int pause_true = 0;
        int pause_false = 0;
        int quits = 0;
        int parent_changes = 0;
        int children_changes = 0;
        int gizmo_draws = 0;
        int gizmo_selected_draws = 0;
        int validates = 0;
        /// 只为 DG23/DG24 存在：证明"enabled"这个开关确实接在本探针上，于是
        /// "gizmo 不判 enabled"那条断言不是空转的。
        int ticks = 0;

        // Unity 的七条鼠标消息。七个分开记：合成一个"收到了消息"计数会把
        // "该来的是哪一条"这件事彻底抹掉 —— 而这一族消息的全部复杂度就在
        // "谁在什么时候来"。
        int mouse_enter = 0;
        int mouse_over = 0;
        int mouse_exit = 0;
        int mouse_down = 0;
        int mouse_drag = 0;
        int mouse_up = 0;
        int mouse_up_as_button = 0;

        // Unity 的两条可见性消息。分开记：合成一个"收到了消息"会把"进还是出"
        // 抹掉，而这一族的复杂度全在方向和时间点上。
        int became_visible = 0;
        int became_invisible = 0;

        /// Unity 的 `Behaviour.enabled`：基类没有 setter，这里自己记一个。
        void SetProbeEnabled(bool on) { m_ProbeEnabled = on; }
        bool isEnabled() const override { return m_ProbeEnabled; }

        void Reset()
        {
            focus_true = focus_false = 0;
            pause_true = pause_false = 0;
            quits = parent_changes = children_changes = validates = 0;
            gizmo_draws = gizmo_selected_draws = 0;
            ticks = 0;
            mouse_enter = mouse_over = mouse_exit = 0;
            mouse_down = mouse_drag = mouse_up = mouse_up_as_button = 0;
            became_visible = became_invisible = 0;
        }

        void OnBecameVisible() override { ++became_visible; }
        void OnBecameInvisible() override { ++became_invisible; }

        void OnApplicationFocus(bool has_focus) override
        {
            if (has_focus) { ++focus_true; } else { ++focus_false; }
            Append(has_focus ? 'F' : 'f');
        }
        void OnApplicationPause(bool paused) override
        {
            if (paused) { ++pause_true; } else { ++pause_false; }
            Append(paused ? 'P' : 'p');
        }
        void OnApplicationQuit() override { ++quits; Append('q'); }
        void Tick(float delta_time) override { (void)delta_time; ++ticks; }
        void OnTransformParentChanged() override { ++parent_changes; Append('r'); }
        // 与上面那个是同一件事的两半，但收的人不同（父级 vs 子级）—— 探针里
        // 分开记，正是为了能在同一个对象上同时看到"只来了一个"。
        void OnTransformChildrenChanged() override { ++children_changes; Append('c'); }
        // Unity 的两个 gizmo 钩子。用两个不同的字符（'g' / 's'）是为了让"选中时
        // 两个都发、且顺序固定"这件事在日志串里可读 —— 共用一个计数器的话，
        // "只发 Selected" 和"两个都发"看起来一样。
        void OnDrawGizmos() override { ++gizmo_draws; Append('g'); }
        void OnDrawGizmosSelected() override { ++gizmo_selected_draws; Append('s'); }

        // Unity 的七条鼠标消息。每个钩子一个字符，于是"谁在什么时候来"在日志串
        // 里是可读的 —— 共用一个计数器的话，"Down 发了两次"和"Down + Drag 各一次"
        // 长得一模一样。
        void OnMouseEnter() override { ++mouse_enter; Append('E'); }
        void OnMouseOver() override { ++mouse_over; Append('O'); }
        void OnMouseExit() override { ++mouse_exit; Append('X'); }
        void OnMouseDown() override { ++mouse_down; Append('D'); }
        void OnMouseDrag() override { ++mouse_drag; Append('G'); }
        void OnMouseUp() override { ++mouse_up; Append('U'); }
        void OnMouseUpAsButton() override { ++mouse_up_as_button; Append('B'); }
        void OnSerializedFieldsUpdated() override { ++validates; Append('v'); }

    private:
        void Append(char what)
        {
            if (m_Log != nullptr)
            {
                m_Log->push_back(m_Tag);
                m_Log->push_back(what);
            }
        }

        std::string* m_Log;
        char m_Tag;
        bool m_ProbeEnabled {true};
    };

    void TestApplicationMessages()
    {
        ObjectManager* om = GET_SYSTEM(ObjectManager);
        Check(om != nullptr, "AM0", "ObjectManager is reachable");
        if (om == nullptr) { return; }

        Level level;
        level.setLevelResUrl("Assets/Scenes/AppMessages.level");
        level.LoadObjectGraph({}, false);
        Check(level.IsLoaded(), "AM1", "the synthetic level is loaded");

        GameObject* go = new GameObject(ObjectIDAllocator::Alloc());
        om->AllocateAndAssignInstanceID(go);
        Transform* transform = new Transform();
        om->AllocateAndAssignInstanceID(transform);
        go->addComponent(transform);
        transform->PostLoadResource(go);

        std::string log;
        AppEventProbe* a = new AppEventProbe(&log, 'a');
        AppEventProbe* b = new AppEventProbe(&log, 'b');
        a->InitializeRuntimeTypeInfo();
        b->InitializeRuntimeTypeInfo();
        om->AllocateAndAssignInstanceID(a);
        om->AllocateAndAssignInstanceID(b);
        go->addComponent(a);
        go->addComponent(b);
        Check(level.RegisterRuntimeGObject(go), "AM2", "the object is in the level's runtime table");

        // --- 四种消息各走一遍 --------------------------------------------
        level.BroadcastAppEvent(AppEventKind::kFocus, true);
        Check(a->focus_true == 1 && a->focus_false == 0, "AM3", "OnApplicationFocus(true) reaches the component");
        log.clear();
        level.BroadcastAppEvent(AppEventKind::kFocus, false);
        Check(a->focus_false == 1, "AM4", "and so does OnApplicationFocus(false)");
        // 顺序探针必须在这里读：下面的 pause / quit 还会往同一个 log 里追加。
        // 留到 AM7 之后再比，字符串里会混进它们，断言就变成在验别的东西。
        Check(log == "afbf", "AM5", "the focus(false) broadcast reached both components, in order");
        level.BroadcastAppEvent(AppEventKind::kPause, true);
        Check(a->pause_true == 1, "AM6", "OnApplicationPause(true) reaches it");
        level.BroadcastAppEvent(AppEventKind::kQuit, false);
        Check(a->quits == 1, "AM7", "OnApplicationQuit reaches it");
        // 同一趟广播必须**每个组件**都收到：只发给第一个的实现（break 写成
        // continue 之外的东西）在只有一个组件的测试里完全看不出来。
        Check(b->focus_true == 1 && b->pause_true == 1 && b->quits == 1, "AM8",
              "every component on the object receives it, not just the first");

        // --- 与三趟 Tick 相反：**不判** enabled / activeInHierarchy --------
        //
        // 这是这一组最重要的断言。"应用要退出了"跟某个对象激不激活无关；
        // 按激活态过滤掉它，得到的是"被停用的对象存不上档"这种没法复现的 bug。
        a->Reset();
        b->Reset();
        a->SetProbeEnabled(false);
        level.BroadcastAppEvent(AppEventKind::kQuit, false);
        Check(a->quits == 1, "AM9", "a disabled component STILL gets OnApplicationQuit");

        a->Reset();
        b->Reset();
        go->SetActive(false);
        Check(!go->IsActiveInHierarchy(), "AM10", "SetActive(false) really deactivates it");
        level.BroadcastAppEvent(AppEventKind::kQuit, false);
        Check(a->quits == 1, "AM11", "and so does an inactive object (the whole point)");
        go->SetActive(true);
        a->SetProbeEnabled(true);

        // --- OnTransformParentChanged：只发给宿主自己，不发给子树 ----------
        GameObject* child = new GameObject(ObjectIDAllocator::Alloc());
        om->AllocateAndAssignInstanceID(child);
        Transform* child_transform = new Transform();
        om->AllocateAndAssignInstanceID(child_transform);
        child->addComponent(child_transform);
        child_transform->PostLoadResource(child);

        GameObject* other = new GameObject(ObjectIDAllocator::Alloc());
        om->AllocateAndAssignInstanceID(other);
        Transform* other_transform = new Transform();
        om->AllocateAndAssignInstanceID(other_transform);
        other->addComponent(other_transform);
        other_transform->PostLoadResource(other);

        AppEventProbe* c = new AppEventProbe(&log, 'c');
        c->InitializeRuntimeTypeInfo();
        om->AllocateAndAssignInstanceID(c);
        child->addComponent(c);
        Check(level.RegisterRuntimeGObject(child), "AM12", "the child is registered");
        Check(level.RegisterRuntimeGObject(other), "AM13", "and the would-be parent too");

        c->Reset();
        Check(child_transform->SetParent(other_transform, true), "AM14", "SetParent succeeded");
        Check(c->parent_changes == 1, "AM15", "the reparented object's components get OnTransformParentChanged");
        // 同一个父级再设一次不算"变了"：SetParent 提前 return，不该发。
        c->Reset();
        Check(child_transform->SetParent(other_transform, true), "AM16", "setting the same parent succeeds");
        Check(c->parent_changes == 0, "AM17", "and does NOT fire again (early return, no change)");
        // 互补的一半：真的换回去要再发一次。
        c->Reset();
        Check(child_transform->SetParent(nullptr, true), "AM18", "detaching succeeds");
        Check(c->parent_changes == 1, "AM19", "detaching to the scene root fires again");

        // --- Level 那一层：两张表都要走到 ----------------------------------
        a->Reset();
        b->Reset();
        level.BroadcastAppEvent(AppEventKind::kQuit, false);
        Check(a->quits == 1, "AM20", "Level::BroadcastAppEvent reaches runtime-registered objects");

        // --- pause 的推导（唯一"有判断"的一步，纯函数）--------------------
        Check(!Application::DeriveApplicationPaused(true, true), "AM21",
              "focused + runInBackground: not paused");
        // 失焦但 runInBackground=true：世界照跑，**只**发 Focus(false)，不暂停。
        // 把 Pause 当成 Focus 的别名的实现在这里就错了。
        Check(!Application::DeriveApplicationPaused(false, true), "AM22",
              "unfocused but runInBackground: NOT paused (Pause is not Focus's alias)");
        Check(Application::DeriveApplicationPaused(false, false), "AM23",
              "unfocused + !runInBackground: paused");
        Check(!Application::DeriveApplicationPaused(true, false), "AM24",
              "focused again: not paused");

        level.UnregisterRuntimeGObject(go->GetID());
        level.UnregisterRuntimeGObject(child->GetID());
        level.UnregisterRuntimeGObject(other->GetID());
        delete a;
        delete b;
        delete c;
        delete go;
        delete child;
        delete other;
    }

    // ---------------------------------------------------------------- CH ---
    // Unity 的 `OnTransformChildrenChanged()`：`OnTransformParentChanged()` 的
    // 互补半边。那一个发给"被搬的对象"，这一个发给"孩子的个数变了的对象"。

    void TestTransformChildrenChanged()
    {
        ObjectManager* om = GET_SYSTEM(ObjectManager);
        Check(om != nullptr, "CH0", "ObjectManager is reachable");
        if (om == nullptr) { return; }

        Level level;
        level.setLevelResUrl("Assets/Scenes/ChildrenChanged.level");
        level.LoadObjectGraph({}, false);
        Check(level.IsLoaded(), "CH1", "the synthetic level is loaded");

        std::string log;

        struct Node
        {
            GameObject* object {nullptr};
            Transform* transform {nullptr};
        };

        // 这一组要五个这样的对象，抄五遍就会在某一份里少一行（少注册 = 静默
        // 收不到任何消息，而且看起来跟"没实现"一模一样）。
        auto make_node = [&](Node& out) -> bool
        {
            out.object = new GameObject(ObjectIDAllocator::Alloc());
            om->AllocateAndAssignInstanceID(out.object);
            out.transform = new Transform();
            om->AllocateAndAssignInstanceID(out.transform);
            out.object->addComponent(out.transform);
            out.transform->PostLoadResource(out.object);
            return level.RegisterRuntimeGObject(out.object);
        };
        auto add_probe = [&](Node& node, char tag) -> AppEventProbe*
        {
            AppEventProbe* probe = new AppEventProbe(&log, tag);
            probe->InitializeRuntimeTypeInfo();
            om->AllocateAndAssignInstanceID(probe);
            node.object->addComponent(probe);
            return probe;
        };

        Node a, b, c, s, d;
        Check(make_node(a), "CH2", "node A is in the level's runtime table");
        Check(make_node(b), "CH3", "node B too");
        Check(make_node(c), "CH4", "node C (the one that gets reparented) too");
        Check(make_node(s), "CH5", "node S (a sibling, for the reorder case) too");

        // A 上挂**两个**探针：单探针的实现只发给第一个组件也照样过。
        AppEventProbe* pa1 = add_probe(a, 'A');
        AppEventProbe* pa2 = add_probe(a, 'a');
        AppEventProbe* pb = add_probe(b, 'B');
        AppEventProbe* pc = add_probe(c, 'C');

        // --- 挂上来：父级收，子级不收 --------------------------------------
        Check(c.transform->SetParent(a.transform, true), "CH6", "attaching C to A succeeds");
        Check(pa1->children_changes == 1, "CH7", "the new PARENT gets OnTransformChildrenChanged");
        Check(pa2->children_changes == 1, "CH8", "every component on it gets it, not just the first");
        Check(pc->children_changes == 0, "CH9",
              "the CHILD does NOT get it (it gets the other half instead)");
        Check(pc->parent_changes == 1, "CH10", "the child gets OnTransformParentChanged");
        Check(pb->children_changes == 0, "CH11", "an uninvolved object gets nothing");
        Check(a.transform->GetChildrenCount() == 1, "CH12", "A really has one child now");

        // --- 换父级：新旧两个父级各一次 ------------------------------------
        pa1->Reset(); pa2->Reset(); pb->Reset(); pc->Reset();
        log.clear();
        Check(c.transform->SetParent(b.transform, true), "CH13", "moving C from A to B succeeds");
        Check(pa1->children_changes == 1, "CH14", "the OLD parent is told it lost a child");
        Check(pb->children_changes == 1, "CH15", "the NEW parent is told it gained one");
        Check(pc->children_changes == 0, "CH16", "the child itself is still not told");
        Check(a.transform->GetChildrenCount() == 0 && b.transform->GetChildrenCount() == 1,
              "CH17", "the hierarchy really moved");
        // 顺序：子对象自己的那条先走，再旧父级，再新父级。四条各一次（A 上有
        // 两个探针，所以是 "Ac" + "ac"）—— 重复发或漏发都会让这个串对不上。
        Check(log == "CrAcacBc", "CH18", "one notification per affected component, in one walk");

        // --- 脱离：只有旧父级收到 ------------------------------------------
        pa1->Reset(); pa2->Reset(); pb->Reset(); pc->Reset();
        Check(c.transform->SetParent(nullptr, true), "CH19", "detaching to the scene root succeeds");
        Check(pb->children_changes == 1, "CH20", "the old parent is told");
        Check(pa1->children_changes == 0, "CH21", "the previous one is not (it already let go)");
        Check(b.transform->GetChildrenCount() == 0, "CH22", "B has no children now");

        // --- 重排**不发**：只认成员增减，不认顺序 --------------------------
        Check(s.transform->SetParent(b.transform, true), "CH23", "S is attached to B");
        Check(c.transform->SetParent(b.transform, true), "CH24", "and C too (B now has two)");
        Check(b.transform->GetChildrenCount() == 2, "CH25", "B really has two children");
        pa1->Reset(); pa2->Reset(); pb->Reset(); pc->Reset();
        Check(c.transform->GetSiblingIndex() == 1, "CH26", "C is the second child before the reorder");
        c.transform->SetSiblingIndex(0);
        Check(c.transform->GetSiblingIndex() == 0, "CH27", "SetSiblingIndex really reordered");
        Check(b.transform->GetChild(0) == c.transform, "CH28", "B's first child is C now");
        // ⚠️ 顺序探针必须在断言 0 之前：SetSiblingIndex 要是空操作，下面这条
        // 也是 0，门就白设了。
        Check(pb->children_changes == 0, "CH29",
              "reordering does NOT fire it (the list of children did not change)");
        // ⚠️ 互补的一半：真的少一个成员**要**发。断言 0 是最容易空转的一种门 ——
        // 一个"从来不发"的实现同样给出 0，而 CH7..CH20 验的是"挂上来/摘下去"
        // 这两个动作，正好绕过了"重排之后紧接着摘一个"这条路径。
        pa1->Reset(); pa2->Reset(); pb->Reset(); pc->Reset();
        Check(s.transform->SetParent(a.transform, true), "CH29a",
              "moving S away from B succeeds");
        Check(pb->children_changes == 1, "CH29b",
              "and losing a member DOES fire it (CH29's 0 is not 'never fires')");

        // --- 与三趟 Tick 相反：不判 enabled / activeInHierarchy ------------
        pa1->Reset(); pa2->Reset(); pb->Reset(); pc->Reset();
        pa1->SetProbeEnabled(false);
        Check(c.transform->SetParent(a.transform, true), "CH30", "moving C back to A succeeds");
        Check(pa1->children_changes == 1, "CH31", "a DISABLED component still gets it");
        pa1->SetProbeEnabled(true);

        pa1->Reset(); pa2->Reset(); pb->Reset(); pc->Reset();
        a.object->SetActive(false);
        Check(!a.object->IsActiveInHierarchy(), "CH32", "A is really inactive");
        Check(c.transform->SetParent(b.transform, true), "CH33", "moving C to B succeeds");
        Check(pa1->children_changes == 1, "CH34", "an INACTIVE object still gets it");
        a.object->SetActive(true);

        // --- 搬整棵子树：被搬的那个自己"孩子没变" --------------------------
        Check(make_node(d), "CH35", "node D is in the runtime table");
        AppEventProbe* pd = add_probe(d, 'D');
        Check(d.transform->SetParent(c.transform, true), "CH36", "D is attached under C");
        Check(pc->children_changes == 1, "CH37", "C hears about its own new child");

        pa1->Reset(); pa2->Reset(); pb->Reset(); pc->Reset(); pd->Reset();
        Check(c.transform->SetParent(a.transform, true), "CH38", "the C+D subtree moves to A");
        Check(pc->children_changes == 0, "CH39",
              "C does NOT hear: its own child list is unchanged (D is still there)");
        Check(pc->parent_changes == 1, "CH40", "C hears the other half instead (it was moved)");
        Check(pb->children_changes == 1, "CH41", "the old parent B hears (it lost C)");
        Check(pa1->children_changes == 1, "CH42", "the new parent A hears (it gained C)");
        Check(pd->children_changes == 0, "CH43", "the grandchild hears nothing at all");

        // --- DetachChildren 走的是 SetParent，所以也要发 --------------------
        pa1->Reset(); pa2->Reset(); pb->Reset(); pc->Reset(); pd->Reset();
        Check(c.transform->GetChildrenCount() == 1, "CH44", "C still has D before detaching");
        c.transform->DetachChildren();
        Check(c.transform->GetChildrenCount() == 0, "CH45", "DetachChildren emptied C");
        Check(pc->children_changes == 1, "CH46", "and fires it once (per removed child)");

        level.UnregisterRuntimeGObject(a.object->GetID());
        level.UnregisterRuntimeGObject(b.object->GetID());
        level.UnregisterRuntimeGObject(c.object->GetID());
        level.UnregisterRuntimeGObject(s.object->GetID());
        level.UnregisterRuntimeGObject(d.object->GetID());
        delete pa1;
        delete pa2;
        delete pb;
        delete pc;
        delete pd;
        delete a.object;
        delete b.object;
        delete c.object;
        delete s.object;
        delete d.object;
    }

    void TestTransformChildrenChangedWiring()
    {
        const std::string component_header =
            ReadFile("Source/Runtime/Function/Framework/Component/Component.h");
        Check(!component_header.empty(), "CW0", "Component.h is readable");
        Check(CountOccurrences(component_header, "kChildrenChanged") >= 1,
              "CW1", "AppEventKind has a kChildrenChanged entry");
        Check(CountOccurrences(component_header, "virtual void OnTransformChildrenChanged(") >= 1,
              "CW2", "OnTransformChildrenChanged is a virtual hook");
        // ⚠️ 两个回调是同一件事的两半，但**收的人不同**。这里守的是"两个都在"，
        // 而不是"至少有一个"：只声明一个的实现能过 >= 1 的那种门。
        Check(CountOccurrences(component_header, "virtual void OnTransformParentChanged(") >= 1,
              "CW3", "and OnTransformParentChanged is still declared (a pair, not one merged hook)");

        const std::string go_source = ReadFile("Source/Runtime/BaseClasses/GameObject.cpp");
        Check(!go_source.empty(), "CW4", "GameObject.cpp is readable");
        {
            const std::size_t at = go_source.find("void GameObject::BroadcastAppEvent");
            Check(at != std::string::npos, "CW5", "BroadcastAppEvent is implemented");
            Check(BodyIsBounded(go_source, at), "CW5b",
                  "the body slice is bounded (the scan is not degenerate)");
            const std::string body = SliceFunctionBody(go_source, at);
            Check(CountOccurrences(body, "AppEventKind::k") >= 5, "CW6",
                  "all five kinds are dispatched (the fifth is the new one)");
            // 加了 case 却调错函数是这一层最容易犯的错：五个 case 长得一模一样，
            // 少一个也照样编译、照样运行，只是那条消息永远没人收。
            Check(CountOccurrences(body, "OnTransformChildrenChanged()") >= 1, "CW7",
                  "and the new case calls the children hook");
        }

        const std::string transform_source =
            ReadFile("Source/Runtime/Function/Framework/Component/Transform/Transform.cpp");
        Check(!transform_source.empty(), "CW8", "Transform.cpp is readable");
        // ⚠️ 必须**两处**：旧父级一次、新父级一次。只写一处的话"挂上来"和"摘
        // 下去"各有一半的观察者永远收不到 —— 而两处文本完全一样，>= 1 抓不到。
        Check(CountOccurrences(transform_source, "AppEventKind::kChildrenChanged") >= 2,
              "CW9", "SetParent dispatches it to BOTH the old and the new parent");
        {
            const std::size_t at = transform_source.find("bool Transform::SetParent");
            Check(at != std::string::npos, "CW10", "SetParent is implemented");
            Check(BodyIsBounded(transform_source, at), "CW10b",
                  "the SetParent body slice is bounded (the scan is not degenerate)");
            const std::string body = SliceFunctionBody(transform_source, at);
            Check(body.find("AppEventKind::kChildrenChanged") != std::string::npos, "CW11",
                  "the dispatch lives INSIDE SetParent (not in a helper nobody calls)");
            // 两处判空缺一不可：脱离父级时 new_parent 就是 nullptr。
            Check(CountOccurrences(body, "old_father != nullptr") >= 1, "CW12",
                  "the old-parent dispatch is null-guarded");
            Check(CountOccurrences(body, "new_parent != nullptr") >= 1, "CW13",
                  "and so is the new-parent one (detaching passes nullptr)");
        }
        // 反向门：SetSiblingIndex **不许**发它。读成"顺序变了"的实现会让编辑器
        // 拖一次排序给脚本灌进几十次回调。
        {
            const std::size_t at = transform_source.find("void Transform::SetSiblingIndex");
            Check(at != std::string::npos, "CW14", "SetSiblingIndex is implemented");
            Check(BodyIsBounded(transform_source, at), "CW14b",
                  "the SetSiblingIndex body slice is bounded (the scan is not degenerate)");
            const std::string body = SliceFunctionBody(transform_source, at);
            Check(body.find("kChildrenChanged") == std::string::npos, "CW15",
                  "SetSiblingIndex does NOT fire it (reordering is not a membership change)");
        }
        // 门本身的门：**读文件的那一行**必须是归一化的那条。
        //
        // 上面 CW10b / CW14b 只在"文件恰好是 CRLF"时才红 —— 哪天 Transform.cpp
        // 被统一成 LF，它们就变成"恒真"，再也证明不了 ReadFile 做过归一化。
        // 所以再补一条不依赖任何文件行尾的接线门。
        //
        // ⚠️ 范围必须限定在 **ReadFile 的函数体内**：数整个文件里的
        // "NormalizeNewlines" 会被这条门自己的字面量命中（自引用），变异体把
        // 真正的调用删掉之后计数仍是 1 —— 一个看起来很合理、实际恒真的门。
        // （同一类坑之前在 VW8 / 这里各栽一次：断言串出现在自己的注释或字面量里。）
        {
            const std::string self = ReadFile(
                "Source/Runtime/Function/Framework/Test/UnityParitySmokeTest.cpp");
            Check(!self.empty(), "CW21", "this test can read its own source");
            const std::size_t at_read = self.find("std::string ReadFile(const std::string& rel_path)");
            Check(at_read != std::string::npos, "CW21b", "ReadFile is implemented");
            Check(BodyIsBounded(self, at_read), "CW21c",
                  "the ReadFile body slice is bounded");
            const std::string read_body = SliceFunctionBody(self, at_read);
            Check(CountOccurrences(read_body, "NormalizeNewlines") >= 1, "CW22",
                  "ReadFile normalizes line endings before anyone slices a function body");
        }

        const std::string ts_source =
            ReadFile("Source/Runtime/Function/Framework/Component/Script/TypeScriptComponent.cpp");
        Check(!ts_source.empty(), "CW16", "TypeScriptComponent.cpp is readable");
        Check(CountOccurrences(ts_source, "\"OnTransformChildrenChanged\"") >= 1, "CW17",
              "the script component forwards OnTransformChildrenChanged");
        // 名字只差一个词，写错成 Parent 是这一层最容易犯也最难发现的错。
        Check(CountOccurrences(ts_source, "\"OnTransformParentChanged\"") >= 1, "CW18",
              "and still forwards OnTransformParentChanged (they are not merged)");

        const std::string env_source = ReadFile("Source/Runtime/Scripting/Wrapper/ScriptEnv.cpp");
        Check(CountOccurrences(env_source, "g.Behaviour.prototype.OnTransformChildrenChanged") >= 1,
              "CW19", "the JS prototype has a no-op default for it");

        const std::string binding_source =
            ReadFile("Source/Runtime/Scripting/Binding/ScriptBinding.cpp");
        Check(CountOccurrences(binding_source, "OnTransformChildrenChanged(): void;") >= 1,
              "CW20", "the .d.ts declares OnTransformChildrenChanged");
    }

    // ---------------------------------------------------------------- ME ---
    // Unity 的七条鼠标消息（`OnMouseEnter/Over/Exit/Down/Drag/Up/UpAsButton`）。
    //
    // 这一组测的是 `MouseEventDispatcher::DispatchTo` —— 派发那一半。
    // `PickAt`（屏幕坐标 -> 对象）需要相机 + 物理，headless 下两者都没有，所以
    // 它只有"降级"那一条断言（ME40）；`Tick` = `DispatchTo(state, PickAt(...))`
    // 这层壳由 MW 的源码级门守。
    //
    // ⚠️ 每一帧的派发都依赖**跨帧状态**（悬停的是谁、按下的是谁），所以每个
    // 子用例开头都要 `MouseEventDispatcher::Reset()` —— 否则上一用例的悬停对象
    // 会漏进来，表现为"多了一条 Exit"，而且只在整组一起跑的时候出现。

    // --------------------------------------------------------------- RV ----
    // Unity 的 `Renderer.bounds` / `localBounds` / `isVisible` 与
    // `OnBecameVisible` / `OnBecameInvisible`。
    //
    // 这一组能测，靠的是把可见性判据压成**纯几何**：相机用场景里的
    // `CameraComponent`（不是 `RenderCamera`，后者 headless 下恒为 nullptr），
    // 包围盒用脚本设的 `localBounds`（不是从网格资产算的，headless 下算不出来）。
    // 于是"有没有相机" / "相机在哪" / "盒子在哪"全是可控输入。
    //
    // ⚠️ 夹具一律**不对称**：相机放在 `(2, 0, 0)` 而不是原点，盒子尺寸取
    // `(4, 2, 2)` 而不是立方体。对称夹具下"没用上 view 的平移"和"旋转把两个
    // 轴换了"这两类错都是绿的。
    void TestRendererVisibility()
    {
        ObjectManager* om = GET_SYSTEM(ObjectManager);
        if (om == nullptr)
        {
            om = new ObjectManager();
            SystemRegistry::GetInstance().RegisterSystem(om);
            om->Initialize();
        }
        Check(om != nullptr, "RV0", "ObjectManager is reachable");

        Level level;
        level.setLevelResUrl("Assets/Scenes/RendererVisibility.level");

        // --- 一台相机 ------------------------------------------------------
        GameObject* camera_object = new GameObject(ObjectIDAllocator::Alloc());
        om->AllocateAndAssignInstanceID(camera_object);
        Transform* camera_transform = new Transform();
        om->AllocateAndAssignInstanceID(camera_transform);
        camera_object->addComponent(camera_transform);
        camera_transform->PostLoadResource(camera_object);
        CameraComponent* camera = new CameraComponent();
        om->AllocateAndAssignInstanceID(camera);
        camera_object->addComponent(camera);
        camera->PostLoadResource(camera_object);
        level.RegisterRuntimeGObject(camera_object);

        camera_transform->SetPosition(Vector3(2.f, 0.f, 0.f));
        camera->SetNearClipPlane(0.1f);
        camera->SetFarClipPlane(100.f);
        // 默认朝向是 -Y（本引擎的 forward），默认 up 是 +Z，headless 下 Tick
        // 不会改它们。钉一下，免得以后改了初值而这条测试悄悄变成别的意思。
        Check(camera->getForward().y < -0.9f, "RV1", "(the test camera looks down -Y, as the default says)");

        // --- 一个渲染器 ----------------------------------------------------
        GameObject* object = new GameObject(ObjectIDAllocator::Alloc());
        om->AllocateAndAssignInstanceID(object);
        Transform* transform_component = new Transform();
        om->AllocateAndAssignInstanceID(transform_component);
        object->addComponent(transform_component);
        transform_component->PostLoadResource(object);
        BaseRenderer* renderer = new BaseRenderer();
        om->AllocateAndAssignInstanceID(renderer);
        object->addComponent(renderer);
        renderer->PostLoadResource(object);
        level.RegisterRuntimeGObject(object);

        std::string log;
        AppEventProbe* probe = new AppEventProbe(&log, 'V');
        probe->InitializeRuntimeTypeInfo();
        om->AllocateAndAssignInstanceID(probe);
        object->addComponent(probe);

        // 没有网格资产（headless）⇒ 默认的 localBounds 是零盒。这条是"默认回
        // 退成零盒而不是单位立方体"的判据。
        const Bounds default_local = renderer->GetLocalBounds();
        Check(default_local.extents.length() < 1e-5f && default_local.center.length() < 1e-5f,
              "RV2", "with no mesh asset the default localBounds is the zero box, not a unit cube");

        renderer->SetLocalBounds(Bounds::FromCenterSize(Vector3::ZERO, Vector3(2.f, 2.f, 2.f)));
        Check(renderer->GetLocalBounds().GetMax().x > 0.99f, "RV3", "SetLocalBounds takes over");

        // --- 包围盒：平移 + 缩放 ---------------------------------------------
        transform_component->SetPosition(Vector3(0.f, 0.f, 0.f));
        transform_component->SetLocalScale(Vector3(2.f, 2.f, 2.f));
        renderer->SetLocalBounds(Bounds::FromCenterSize(Vector3(1.f, 0.f, 0.f), Vector3(2.f, 2.f, 2.f)));
        {
            const Bounds world = renderer->GetBounds();
            Check(std::fabs(world.center.x - 2.f) < 1e-3f && std::fabs(world.GetSize().x - 4.f) < 1e-3f,
                  "RV4", "world bounds follows the transform (centre 1 -> 2, size 2 -> 4)");
        }

        // --- ⚠️ 包围盒：旋转把两个轴换过来 -----------------------------------
        // 尺寸 (4,2,2) 绕 Z 转 90°：+X 到 +Y（见 QT8），于是 extents 从 (2,1,1)
        // 变成 (1,2,1)，size 从 (4,2,2) 变成 (2,4,2)。用立方体测不出这个。
        transform_component->SetPosition(Vector3::ZERO);
        transform_component->SetLocalScale(Vector3::UNIT_SCALE);
        transform_component->SetLocalEulerAngles(Vector3(0.f, 0.f, 90.f));
        renderer->SetLocalBounds(Bounds::FromCenterSize(Vector3::ZERO, Vector3(4.f, 2.f, 2.f)));
        {
            const Bounds world = renderer->GetBounds();
            const Vector3 size = world.GetSize();
            Check(std::fabs(size.x - 2.f) < 1e-3f && std::fabs(size.y - 4.f) < 1e-3f &&
                      std::fabs(size.z - 2.f) < 1e-3f, "RV5",
                  "a 90-degree turn about Z swaps the box's x and y extents (asymmetric fixture)");
        }
        transform_component->SetLocalEulerAngles(Vector3::ZERO);

        // --- 可见性：只在**变化**那一帧发一次 ---------------------------------
        renderer->SetLocalBounds(Bounds::FromCenterSize(Vector3::ZERO, Vector3(2.f, 2.f, 2.f)));
        transform_component->SetPosition(Vector3(2.f, -10.f, 0.f));  // 相机正前方
        probe->Reset();
        BaseRenderer::UpdateVisibilityForAll();
        Check(renderer->IsVisible(), "RV6", "a renderer in front of the camera is visible");
        Check(probe->became_visible == 1 && probe->became_invisible == 0, "RV7",
              "and OnBecameVisible fires exactly once");

        probe->Reset();
        BaseRenderer::UpdateVisibilityForAll();
        Check(renderer->IsVisible() && probe->became_visible == 0 && probe->became_invisible == 0,
              "RV8", "the next frame sends nothing (it is a transition, not a per-frame event)");

        // 挪到相机背后。
        transform_component->SetPosition(Vector3(2.f, 10.f, 0.f));
        probe->Reset();
        BaseRenderer::UpdateVisibilityForAll();
        Check(!renderer->IsVisible(), "RV9", "a renderer behind the eye is not visible");
        Check(probe->became_invisible == 1 && probe->became_visible == 0, "RV10",
              "and OnBecameInvisible fires exactly once (direction is not lost)");

        // 超出 far。
        transform_component->SetPosition(Vector3(2.f, -500.f, 0.f));
        BaseRenderer::UpdateVisibilityForAll();
        Check(!renderer->IsVisible(), "RV11", "nor is one beyond the far clip plane");

        // --- ⚠️ near 平面：盒子**不是点**，两条断言互补 -----------------------
        // 半边长 1 的盒子中心离眼 0.05 ⇒ 它**跨过** near=0.1，保守判据算可见。
        // 这不是 bug（`TestPlanesAABB` 就是这个精度），但"跨过近裁剪面算可见"
        // 必须写成断言，否则后人会以为是漏了什么"顺手修好"。
        transform_component->SetPosition(Vector3(2.f, -0.05f, 0.f));
        BaseRenderer::UpdateVisibilityForAll();
        Check(renderer->IsVisible(), "RV12",
              "a box that STRADDLES the near plane counts as visible (bounds are not points)");
        // 互补的一半：把盒子缩到完全在 near 之前，它就真的不可见了。
        renderer->SetLocalBounds(Bounds::FromCenterSize(Vector3::ZERO, Vector3(0.01f, 0.01f, 0.01f)));
        BaseRenderer::UpdateVisibilityForAll();
        Check(!renderer->IsVisible(), "RV12b",
              "while a box entirely in front of the near plane is not");

        // --- ⚠️ 停用：渲染器停用 ⇒ 不渲染 ⇒ 不可见 ----------------------------
        transform_component->SetPosition(Vector3(2.f, -10.f, 0.f));
        BaseRenderer::UpdateVisibilityForAll();
        Check(renderer->IsVisible(), "RV13", "(back in front of the camera)");
        probe->Reset();
        renderer->SetEnabled(false);
        Check(!renderer->IsVisible() && probe->became_invisible == 1, "RV14",
              "disabling the renderer makes it invisible right away (no wait for the next Tick)");
        probe->Reset();
        renderer->SetEnabled(true);
        Check(renderer->IsVisible() && probe->became_visible == 1, "RV15",
              "and re-enabling brings it back");

        // --- ⚠️ 脚本被停用照样收得到（与 Unity 的 enabled 语义一致）----------
        probe->SetProbeEnabled(false);
        transform_component->SetPosition(Vector3(2.f, 10.f, 0.f));
        probe->Reset();
        BaseRenderer::UpdateVisibilityForAll();
        Check(probe->became_invisible == 1, "RV16",
              "a disabled SCRIPT still receives the message (enabled only blocks Update and friends)");
        probe->SetProbeEnabled(true);

        // --- 对象不在层级里 ⇒ 不可见 -----------------------------------------
        transform_component->SetPosition(Vector3(2.f, -10.f, 0.f));
        BaseRenderer::UpdateVisibilityForAll();
        probe->Reset();
        object->SetActive(false);
        BaseRenderer::UpdateVisibilityForAll();
        Check(!renderer->IsVisible(), "RV17", "a deactivated object is not visible");
        // ⚠️ 互补的一半：上一条只看 `IsVisible()`，而那个值在
        // `UpdateVisibilityAndNotify` 里就算出来了 —— `BroadcastVisibilityChange`
        // 里那条 `IsActiveInHierarchy()` 判据被删掉时 RV17 照样是绿的。必须证明
        // 消息**没有**发出去。
        Check(probe->became_invisible == 0 && probe->became_visible == 0, "RV17b",
              "and it is not even told about it (the broadcast gates on activeInHierarchy)");
        probe->Reset();
        object->SetActive(true);
        // ⚠️ 不先扫一帧就断言：`OnOwnerActiveChanged` 里那次刷新是"层级变了但
        // Transform 没脏"这唯一一条能走到可见性的路（Tick 那时已经不会来了）。
        Check(renderer->IsVisible() && probe->became_visible == 1, "RV18",
              "reactivating brings it back without waiting for the next scan");
        probe->Reset();
        BaseRenderer::UpdateVisibilityForAll();
        Check(renderer->IsVisible() && probe->became_visible == 0, "RV18b",
              "and the next scan is then a no-op (the two paths agree)");

        // --- ⚠️ 停用的相机不算数 ---------------------------------------------
        // 少了这条的话，"相机停用了"会表现成"物体还在画"（可见性停在 true）。
        camera->SetEnabled(false);
        BaseRenderer::UpdateVisibilityForAll();
        Check(!renderer->IsVisible(), "RV19", "a disabled camera does not make anything visible");
        camera->SetEnabled(true);
        BaseRenderer::UpdateVisibilityForAll();
        Check(renderer->IsVisible(), "RV20", "and enabling it back does");

        // 收尾：把这台相机停用，别让它影响后面几组测试（可见性是全局扫描）。
        camera->SetEnabled(false);
    }

    void TestRendererVisibilityWiring()
    {
        const std::string app_source = ReadFile("Source/Runtime/Application/Application.cpp");
        Check(!app_source.empty(), "RW0", "Application.cpp is readable");
        if (!app_source.empty())
        {
            // ⚠️ 头条：接线门。可见性**必须**每帧被扫一次，而
            // `BaseRenderer::Tick` 在 transform 不脏时直接 return —— 挂在 Tick
            // 上就只剩"物体动了才更新"，相机转开永远发现不了。
            Check(CountOccurrences(app_source, "BaseRenderer::UpdateVisibilityForAll()") >= 1, "RW1",
                  "the main loop drives visibility every frame (it cannot live in Tick)");
        }
        const std::string renderer_header =
            ReadFile("Source/Runtime/Function/Framework/Component/Mesh/BaseRenderer.h");
        Check(!renderer_header.empty(), "RW2", "BaseRenderer.h is readable");
        if (!renderer_header.empty())
        {
            Check(CountOccurrences(renderer_header, "Name=\"bounds\"") >= 1 &&
                      CountOccurrences(renderer_header, "Name=\"localBounds\"") >= 1 &&
                      CountOccurrences(renderer_header, "Name=\"isVisible\"") >= 1, "RW3",
                  "the three Unity spellings are published");
        }
        const std::string game_object_source = ReadFile("Source/Runtime/BaseClasses/GameObject.cpp");
        Check(!game_object_source.empty(), "RW4", "GameObject.cpp is readable");
        if (!game_object_source.empty())
        {
            const std::size_t at = game_object_source.find("void GameObject::BroadcastVisibilityChange");
            Check(at != std::string::npos, "RW5", "the visibility broadcast is there");
            if (at != std::string::npos)
            {
                Check(BodyIsBounded(game_object_source, at), "RW6", "its body slice is bounded");
                const std::string body = SliceFunctionBody(game_object_source, at);
                // ⚠️ 判据必须是"只判层级激活"，与鼠标 / gizmo 那两组一致。
                Check(CountOccurrences(body, "IsActiveInHierarchy") >= 1, "RW7",
                      "it gates on activeInHierarchy");
                Check(CountOccurrences(body, "isEnabled()") == 0, "RW8",
                      "and NOT on enabled (Unity's enabled does not block visibility messages)");
            }
        }
        // --- 发布面 ----------------------------------------------------------
        //
        // 这两个名字只有"发布"这一步把它们交给 TS。Behaviour 是**合成**脚本类
        // （不在 C++ 反射里），所以漏掉 `.d.ts` 那行的表现不是"调用失败"，而是
        // tsc 在**用户工程**里报"属性不存在" —— 错误离原因十万八千里。
        const std::string binding_source =
            ReadFile("Source/Runtime/Scripting/Binding/ScriptBinding.cpp");
        Check(!binding_source.empty(), "RW9", "ScriptBinding.cpp is readable");
        if (!binding_source.empty())
        {
            Check(CountOccurrences(binding_source, "\"    OnBecameVisible(): void;\"") >= 1 &&
                      CountOccurrences(binding_source, "\"    OnBecameInvisible(): void;\"") >= 1,
                  "RW10", "both visibility hooks are declared on Behaviour's .d.ts");
        }
        const std::string env_source = ReadFile("Source/Runtime/Scripting/Wrapper/ScriptEnv.cpp");
        Check(!env_source.empty(), "RW11", "ScriptEnv.cpp is readable");
        if (!env_source.empty())
        {
            // 缺了空默认，每个**没写**这两个钩子的类在进出视锥时都要走一次
            // 属性查找失败 —— 而写了钩子的类一切正常，于是只有项目里"那个没
            // 写钩子的"才炸。
            Check(CountOccurrences(env_source, "g.Behaviour.prototype.OnBecameVisible") >= 1 &&
                      CountOccurrences(env_source, "g.Behaviour.prototype.OnBecameInvisible") >= 1,
                  "RW12", "both have a no-op default on the Behaviour prototype");
        }
    }

    // ---- Unity 的 AnimationCurve（AC 组：算术 / ACW 组：接线）--------------
    //
    // 这一组的价值集中在三处"写错了也不报错"：
    //   * 切线有没有乘 `dt`（AC7）—— 等距采样下两种实现给出同一个数；
    //   * `SmoothTangents` 是加权平均还是算术平均（AC12）—— 时间间距相等时
    //     两者相同；
    //   * `ClampForever` 用的是哪一条切线（AC15/AC16）—— 首末斜率相同的曲线
    //     看不出方向反没反。
    void TestAnimationCurve()
    {
        using ::AnimationCurve;
        using ::Keyframe;
        using ::WrapMode;

        // --- 退化情形：不该是 NaN ------------------------------------------
        AnimationCurve empty;
        Check(empty.GetKeyCount() == 0, "AC0", "a fresh curve has no keys");
        Check(empty.Evaluate(0.f) == 0.f && empty.Evaluate(5.f) == 0.f, "AC1",
              "an empty curve evaluates to 0");
        Check(empty.GetDuration() == 0.f, "AC2",
              "its duration is 0 (not NaN -- a division that never happens)");
        AnimationCurve single;
        single.AddKey(Keyframe(1.f, 7.f));
        Check(Near(single.Evaluate(-100.f), 7.f) && Near(single.Evaluate(100.f), 7.f), "AC3",
              "a one-key curve is that value at every time");
        // 单帧 + Loop：`GetDuration()` 为 0 时取模会除零。除零产生的 NaN 参与
        // 后面的比较恒 false，于是静默走到兜底 —— 这里断言它连着包模式一起是
        // 有定义的。
        single.preWrapMode = WrapMode::Loop;
        single.postWrapMode = WrapMode::Loop;
        Check(Near(single.Evaluate(100.f), 7.f), "AC4",
              "and wrapping a zero-duration curve does not produce NaN");

        // --- 三个静态工厂 ---------------------------------------------------
        const AnimationCurve linear = Runtime::AnimationCurve::Linear(0.f, 0.f, 10.f, 20.f);
        Check(linear.GetKeyCount() == 2, "AC5", "Linear() makes two keys");
        Check(Near(Runtime::AnimationCurve::Evaluate(linear, 5.f), 10.f), "AC6",
              "Linear(0,0,10,20) at the midpoint is 10");
        // ⚠️ 四分之一点：Hermite 在端点切线为 0 时才退化成直线。填成 slope 的话
        // 端点仍然经过 (0,0) 和 (10,20)，只有中间能看出来。
        Check(Near(Runtime::AnimationCurve::Evaluate(linear, 2.5f), 5.f), "AC7",
              "and at a quarter it is 5 (the Hermite really degenerates to a line)");

        // ⚠️ AC8 是"切线要乘 dt"那条判据。dt = 2 时正确的 m0 = outTangent * dt
        // = 4，漏了那个 `* dt` 就是 2。两者只在**非等距**的段上分得开。
        AnimationCurve scaled;
        scaled.AddKey(Keyframe(0.f, 0.f, 0.f, 2.f));
        scaled.AddKey(Keyframe(2.f, 4.f, 0.f, 0.f));
        Check(Near(scaled.Evaluate(1.f), 2.5f), "AC8",
              "the out tangent is scaled by dt before it enters the Hermite basis");

        const AnimationCurve ease = Runtime::AnimationCurve::EaseInOut(0.f, 0.f, 1.f, 1.f);
        Check(Near(ease.Evaluate(0.5f), 0.5f), "AC9", "EaseInOut at the midpoint is 0.5");
        // 0.25 处 smoothstep 是 0.15625，线性是 0.25 —— 只测中点分不出这两者。
        Check(Near(ease.Evaluate(0.25f), 0.15625f), "AC10",
              "and at a quarter it is 0.15625, not 0.25 (it is not a line)");
        Check(Near(ease.Evaluate(0.75f), 0.84375f), "AC11",
              "and at three quarters it is 0.84375 (symmetric, not clipped)");

        const AnimationCurve flat = Runtime::AnimationCurve::Constant(0.f, 10.f, 5.f);
        Check(Near(flat.Evaluate(0.f), 5.f) && Near(flat.Evaluate(3.7f), 5.f) &&
                  Near(flat.Evaluate(10.f), 5.f), "AC12",
              "Constant(0,10,5) is 5 everywhere in range");

        // --- SmoothTangents：加权，不是算术平均 ------------------------------
        AnimationCurve smooth;
        smooth.AddKey(Keyframe(0.f, 0.f));
        smooth.AddKey(Keyframe(1.f, 1.f));
        smooth.AddKey(Keyframe(4.f, 2.f));
        smooth.SmoothTangents(1);
        // 左段斜率 1（dt=1），右段斜率 1/3（dt=3）。加权（权重取**对面**那段的
        // 时长）得 (1*3 + 1/3*1)/4 = 0.8333；算术平均得 0.6667。
        Check(Near(smooth.keys[1].inTangent, 0.833333f) &&
                  Near(smooth.keys[1].outTangent, 0.833333f), "AC13",
              "SmoothTangents weights each slope by the OTHER segment's length");

        // --- 编辑：排序 / 去重 / 删 / 移 -------------------------------------
        AnimationCurve sorted;
        sorted.AddKey(Keyframe(2.f, 20.f));
        sorted.AddKey(Keyframe(1.f, 10.f));
        sorted.AddKey(Keyframe(0.f, 0.f));
        Check(sorted.GetKeyCount() == 3 && sorted.keys[0].time == 0.f && sorted.keys[2].time == 2.f,
              "AC14", "AddKey keeps the key array sorted by time");
        Check(Near(sorted.Evaluate(1.f), 10.f), "AC15",
              "and the curve built out of order still evaluates correctly");
        sorted.AddKey(Keyframe(1.f, 99.f));
        Check(sorted.GetKeyCount() == 3 && Near(sorted.Evaluate(1.f), 99.f), "AC16",
              "adding a key at an existing time replaces it (dt would be 0 otherwise)");

        AnimationCurve edited = Runtime::AnimationCurve::RemoveKey(sorted, 0);
        Check(edited.GetKeyCount() == 2 && edited.keys[0].time == 1.f, "AC17", "RemoveKey drops it");
        // MoveKey 挪到时间轴的另一头 ⇒ 必须重新排序。
        edited = Runtime::AnimationCurve::AddKeyFrame(edited, Keyframe(5.f, 50.f));
        // 把 index 0（t=1）挪到 t=9：它现在该排在**最后**，不是留在原位。
        edited = Runtime::AnimationCurve::MoveKey(edited, 0, Keyframe(9.f, 90.f));
        Check(edited.GetKeyCount() == 3 && edited.keys[0].time == 2.f &&
                  edited.keys[1].time == 5.f && edited.keys[2].time == 9.f, "AC18",
              "MoveKey re-sorts (the moved key can land anywhere)");
        AnimationCurve cleared = Runtime::AnimationCurve::SetPreWrapMode(edited, WrapMode::PingPong);
        cleared = Runtime::AnimationCurve::ClearKeys(cleared);
        Check(cleared.GetKeyCount() == 0, "AC19", "ClearKeys leaves zero keys");
        // wrap 模式属于曲线，不属于帧列表 —— 把它一起清掉是 Unity 没有的行为。
        Check(Runtime::AnimationCurve::GetPreWrapMode(cleared) == WrapMode::PingPong, "AC19b",
              "and it does not touch the wrap modes (they belong to the curve, not the keys)");
        Check(Runtime::AnimationCurve::GetKeyCount(edited) == 3, "AC20",
              "and the original is untouched (these are value semantics)");

        // --- 访问器 ----------------------------------------------------------
        Check(Runtime::AnimationCurve::GetDuration(linear) == 10.f, "AC21", "duration is end - start");
        // ⚠️ 互补的一半：上一条的曲线**从 0 开始**，于是"跨度"和"末帧时间"是同一个
        // 数 —— `GetDuration` 返回末帧时间的实现在它上面是绿的（变异 ACM15 就是这么
        // 活的）。必须有一条起点不是 0 的。
        Check(Runtime::AnimationCurve::GetDuration(
                  Runtime::AnimationCurve::Linear(1.f, 0.f, 3.f, 6.f)) == 2.f, "AC21b",
              "and it is the SPAN, not the end time (a curve that does not start at 0)");
        Check(Runtime::AnimationCurve::GetKey(linear, 1).value == 20.f, "AC22", "GetKey reads a key");
        const Keyframe out_of_range = Runtime::AnimationCurve::GetKey(linear, 99);
        Check(out_of_range.time == 0.f && out_of_range.value == 0.f, "AC23",
              "and an out-of-range index gives a zero key (not a crash)");
        Check(Runtime::AnimationCurve::GetKeys(linear).size() == 2, "AC24", "GetKeys returns all of them");

        // --- 环绕模式 --------------------------------------------------------
        // 默认：到端点后**保持**端点值。
        Check(Near(linear.Evaluate(-1.f), 0.f) && Near(linear.Evaluate(20.f), 20.f), "AC25",
              "the default wrap holds the end value");

        // ⚠️ ClampForever 用哪一条切线是反直觉的：范围**之前**用首帧的 **out**
        // 切线（把首帧右侧的斜率往左延长），范围**之后**用末帧的 **in** 切线。
        // 用反了的话这里会变成 0 和 1（也就是 Once 的值）—— 所以必须配一条
        // **首末斜率不同**的曲线，对称的曲线分不出这两者。
        AnimationCurve tailed;
        tailed.AddKey(Keyframe(0.f, 0.f, 0.f, 1.f));   // out = 1
        tailed.AddKey(Keyframe(1.f, 1.f, 3.f, 0.f));   // in  = 3
        tailed.preWrapMode = WrapMode::ClampForever;
        tailed.postWrapMode = WrapMode::ClampForever;
        Check(Near(tailed.Evaluate(-1.f), -1.f), "AC26",
              "before the first key it follows the FIRST key's OUT tangent (0 + -1 * 1)");
        Check(Near(tailed.Evaluate(2.f), 4.f), "AC27",
              "after the last key it follows the LAST key's IN tangent (1 + 1 * 3)");

        // Loop / PingPong。用**不对称**的曲线：0→3→0 的三角形，Loop 与 PingPong
        // 在同一个时间上给出的数不同（线性 0→1 的曲线上两者相同，测不出来）。
        // ⚠️ 夹具必须**不对称**：0→3→0 这种回原值的曲线关于中点镜像对称，
        // Loop 与 PingPong 在它上面给出同一个数（曾在这儿白测过一次）。末值取 1
        // 之后两条段的中点分别是 1.5 和 2.0 —— 这才分得开。
        AnimationCurve triangle;
        triangle.AddKey(Keyframe(0.f, 0.f));
        triangle.AddKey(Keyframe(1.f, 3.f));
        triangle.AddKey(Keyframe(2.f, 1.f));
        Check(Near(triangle.Evaluate(0.5f), 1.5f) && Near(triangle.Evaluate(1.5f), 2.0f), "AC28",
              "(the fixture itself is asymmetric: 0.5 -> 1.5 but 1.5 -> 2.0)");
        AnimationCurve looping = Runtime::AnimationCurve::SetPreWrapMode(triangle, WrapMode::Loop);
        looping = Runtime::AnimationCurve::SetPostWrapMode(looping, WrapMode::Loop);
        Check(Near(looping.Evaluate(2.5f), 1.5f), "AC29",
              "Loop restarts from the beginning (2.5 -> 0.5 -> 1.5)");
        AnimationCurve pong = Runtime::AnimationCurve::SetPreWrapMode(triangle, WrapMode::PingPong);
        pong = Runtime::AnimationCurve::SetPostWrapMode(pong, WrapMode::PingPong);
        Check(Near(pong.Evaluate(2.5f), 2.0f), "AC30",
              "PingPong mirrors instead (2.5 -> 1.5 -> 2.0, the 1.5 above would be a Loop)");
        Check(Near(pong.Evaluate(-0.5f), 1.5f), "AC31",
              "and PingPong mirrors before the start too (-0.5 -> 0.5 -> 1.5)");

        Check(Runtime::AnimationCurve::GetPreWrapMode(pong) == WrapMode::PingPong &&
                  Runtime::AnimationCurve::GetPostWrapMode(pong) == WrapMode::PingPong, "AC32",
              "the wrap modes round-trip through the facade");

        // ⚠️ 非有限输入：NaN 参与 `<=` / `>=` 恒 false，不判掉会一路走到"没找到
        // 段"的兜底 —— 那和"真的没有段"是分不开的。
        Check(triangle.Evaluate(std::numeric_limits<float>::quiet_NaN()) == 0.f, "AC33",
              "a NaN time gives 0 (it cannot be left to fall through the range checks)");

        // --- 脚本侧的形状：SetKeys 会排序 -------------------------------------
        std::vector<Keyframe> unsorted;
        unsorted.push_back(Keyframe(3.f, 30.f));
        unsorted.push_back(Keyframe(1.f, 10.f));
        const AnimationCurve from_script = Runtime::AnimationCurve::SetKeys(empty, unsorted);
        Check(from_script.keys[0].time == 1.f && from_script.keys[1].time == 3.f, "AC34",
              "SetKeys sorts what the script hands over (Evaluate depends on the order)");
    }

    void TestAnimationCurveWiring()
    {
        const std::string header =
            ReadFile("Source/Runtime/Scripting/ScriptAnimationCurve/ScriptAnimationCurve.h");
        Check(!header.empty(), "ACW0", "ScriptAnimationCurve.h is readable");
        if (!header.empty())
        {
            // 三个静态工厂 + 两个 wrap 访问器：缺任何一个脚本都只能自己拼。
            Check(CountOccurrences(header, "static ::AnimationCurve Linear(") >= 1 &&
                      CountOccurrences(header, "static ::AnimationCurve EaseInOut(") >= 1 &&
                      CountOccurrences(header, "static ::AnimationCurve Constant(") >= 1, "ACW1",
                  "the three Unity factory methods are on the facade");
            Check(CountOccurrences(header, "SetPreWrapMode") >= 1 &&
                      CountOccurrences(header, "SetPostWrapMode") >= 1, "ACW2",
                  "both wrap modes are settable");
        }
        const std::string value_header = ReadFile("Source/Runtime/Core/Math/AnimationCurve.h");
        Check(!value_header.empty(), "ACW3", "the value type is readable");
        if (!value_header.empty())
        {
            // ⚠️ WrapMode 必须是 ZENUM：不是的话脚本侧只有一个裸 int，`WrapMode.Loop`
            // 这样的写法在 .d.ts 里根本不存在，错误落在用户工程。
            Check(CountOccurrences(value_header, "ZENUM()") >= 1, "ACW4",
                  "WrapMode is published as a script enum");
            // 五个数值与 Unity 对齐（脚本里 `WrapMode.Loop` 就是 2）。
            Check(CountOccurrences(value_header, "Once         = 1") >= 1 &&
                      CountOccurrences(value_header, "Loop         = 2") >= 1 &&
                      CountOccurrences(value_header, "PingPong     = 4") >= 1 &&
                      CountOccurrences(value_header, "ClampForever = 8") >= 1, "ACW5",
                  "and its four values match Unity's numbering");
        }
        const std::string binding_header =
            ReadFile("Source/Runtime/Scripting/Binding/ScriptBinding.h");
        Check(!binding_header.empty(), "ACW6", "ScriptBinding.h is readable");
        if (!binding_header.empty())
        {
            Check(CountOccurrences(binding_header, "struct Converter<Keyframe>") >= 1 &&
                      CountOccurrences(binding_header, "struct Converter<AnimationCurve>") >= 1,
                  "ACW7", "both value types have a marshaller");
        }
        const std::string binding_source =
            ReadFile("Source/Runtime/Scripting/Binding/ScriptBinding.cpp");
        Check(!binding_source.empty(), "ACW8", "ScriptBinding.cpp is readable");
        if (!binding_source.empty())
        {
            Check(CountOccurrences(binding_source, "declare interface Keyframe {") >= 1 &&
                      CountOccurrences(binding_source, "declare interface AnimationCurve {") >= 1,
                  "ACW9", "both are declared in the .d.ts");
        }
    }

    // =========================================================================
    // Vector2Int / Vector3Int
    // =========================================================================
    //
    // ⚠️ 拿到"整数向量"最容易做错的两处，各配一组**互补**夹具：
    //
    //   1. `FloorToInt` 与"截断"在负数上不同。只用正数（1.7 → 1）测不出来，
    //      必须有一条 `-0.5`：`floor` 给 -1，截断给 0。
    //   2. `RoundToInt` 的平局规则。Unity 与 `Mathf.RoundToInt` 同为"平局取偶"
    //      （banker's rounding），`std::round` / `lround` 是"平局远离零"。
    //      只有 0.5 / 1.5 / 2.5 这类 `.5` 分得开。
    void TestIntVectors()
    {
        using ::Vector2Int;
        using ::Vector3Int;

        // --- 值类型本身 ------------------------------------------------------
        Vector2Int a(3, -4);
        Check(a.GetSqrMagnitude() == 25, "IV0", "sqrMagnitude is an int (3^2 + 4^2 = 25)");
        // ⚠️ magnitude 是 **float**：开方不可能是整数，Unity 的
        // `Vector2Int.magnitude` 也是 float。返回 int 的实现在 3-4-5 这种整勾股
        // 数上正好是绿的，必须用非整勾股数才能分出来。
        Check(Near(a.GetMagnitude(), 5.f), "IV1", "magnitude is a float");
        Vector2Int a2(1, 1);
        Check(Near(a2.GetMagnitude(), 1.4142135f), "IV1b",
              "and it is not truncated to an int (1,1) -> 1.41421, not 1");
        Check(Vector2Int(3, -4) == a && Vector2Int(3, 4) != a, "IV2", "equality is per component");
        Check((a + Vector2Int(1, 1)) == Vector2Int(4, -3), "IV3", "addition is per component");
        Check((a - Vector2Int(1, 1)) == Vector2Int(2, -5), "IV4", "and so is subtraction");
        Check((a * 2) == Vector2Int(6, -8), "IV5", "scalar multiply");
        // ⚠️ 分量乘（`Scale`）与标量乘是两个不同的运算符，写成同一个实现的话下面
        // 这条会拿到 (6*2, -8*2) 那种结果。
        Check((Vector2Int(2, 3) * Vector2Int(4, 5)) == Vector2Int(8, 15), "IV6",
              "component-wise multiply is not the scalar one");
        Vector2Int scaled(2, 3);
        scaled.Scale(Vector2Int(4, 5));
        Check(scaled == Vector2Int(8, 15), "IV7", "Scale() is the same operator, in place");
        Vector2Int settable;
        settable.Set(7, 8);
        Check(settable == Vector2Int(7, 8), "IV8", "Set() overwrites both components");

        Vector3Int b(1, -2, 3);
        Check(b.GetSqrMagnitude() == 14, "IV9", "3D sqrMagnitude");
        Check((b + Vector3Int(1, 1, 1)) == Vector3Int(2, -1, 4), "IV10", "3D addition");
        Check((b * 3) == Vector3Int(3, -6, 9), "IV11", "3D scalar multiply");
        Check((Vector3Int(1, 2, 3) * Vector3Int(4, 5, 6)) == Vector3Int(4, 10, 18), "IV12",
              "3D component-wise multiply");

        // --- 门面：构造 / 转换 ------------------------------------------------
        Check(Runtime::Vector2Int::Create(5, 6) == Vector2Int(5, 6), "IV13", "Create()");
        Check(Runtime::Vector3Int::Create(5, 6, 7) == Vector3Int(5, 6, 7), "IV14", "3D Create()");

        // ⚠️ **floor**，不是截断。正数上两者相同，负数上差 1。
        Check(Runtime::Vector2Int::FloorToInt(Vector2(1.7f, 1.2f)) == Vector2Int(1, 1), "IV15",
              "FloorToInt truncates toward -inf on positives (same as a cast here)");
        Check(Runtime::Vector2Int::FloorToInt(Vector2(-0.5f, -1.2f)) == Vector2Int(-1, -2), "IV16",
              "and it is really FLOOR on negatives: -0.5 -> -1, not 0");
        Check(Runtime::Vector2Int::CeilToInt(Vector2(-0.5f, 1.2f)) == Vector2Int(0, 2), "IV17",
              "CeilToInt rounds toward +inf");
        // ⚠️ 平局取偶（`std::rint`），不是 `std::round` 的"远离零"。
        // 0.5 -> 0（偶）、1.5 -> 2（偶）、2.5 -> 2（偶）；`std::round` 会给
        // 1 / 2 / 3。三个值一起测：只测 0.5 的话"永远向下"的实现也过。
        Check(Runtime::Vector2Int::RoundToInt(Vector2(0.5f, 1.5f)) == Vector2Int(0, 2), "IV18",
              "RoundToInt breaks ties to even (0.5 -> 0, 1.5 -> 2)");
        Check(Runtime::Vector2Int::RoundToInt(Vector2(2.5f, 3.5f)) == Vector2Int(2, 4), "IV18b",
              "and it stays banker's at 2.5 -> 2 (round-away-from-zero would give 3)");
        Check(Runtime::Vector2Int::ToVector2(Vector2Int(3, -4)) == Vector2(3.f, -4.f), "IV19",
              "ToVector2 widens to float");
        Check(Runtime::Vector3Int::ToVector3(Vector3Int(3, -4, 5)) == Vector3(3.f, -4.f, 5.f),
              "IV20", "ToVector3 widens to float");

        // --- 常量（Unity 的 Y-up 命名）----------------------------------------
        Check(Runtime::Vector2Int::GetZero() == Vector2Int(0, 0), "IV21", "zero");
        Check(Runtime::Vector2Int::GetOne() == Vector2Int(1, 1), "IV22", "one");
        Check(Runtime::Vector2Int::GetUp() == Vector2Int(0, 1) &&
                  Runtime::Vector2Int::GetDown() == Vector2Int(0, -1) &&
                  Runtime::Vector2Int::GetLeft() == Vector2Int(-1, 0) &&
                  Runtime::Vector2Int::GetRight() == Vector2Int(1, 0), "IV23",
              "the four 2D directions are Unity's Y-up literals");
        Check(Runtime::Vector3Int::GetZero() == Vector3Int(0, 0, 0) &&
                  Runtime::Vector3Int::GetOne() == Vector3Int(1, 1, 1), "IV24", "3D zero / one");
        Check(Runtime::Vector3Int::GetUp() == Vector3Int(0, 1, 0) &&
                  Runtime::Vector3Int::GetDown() == Vector3Int(0, -1, 0) &&
                  Runtime::Vector3Int::GetLeft() == Vector3Int(-1, 0, 0) &&
                  Runtime::Vector3Int::GetRight() == Vector3Int(1, 0, 0) &&
                  Runtime::Vector3Int::GetForward() == Vector3Int(0, 0, 1) &&
                  Runtime::Vector3Int::GetBack() == Vector3Int(0, 0, -1), "IV25",
              "the six 3D directions are Unity's, NOT ZEngine's Z-up/forward=-Y convention");
        // ⚠️ `up` 与 `forward` 必须**不同**：把它们都填成 (0,1,0) 的实现在只查
        // up 的夹具上是绿的。
        Check(Runtime::Vector3Int::GetUp() != Runtime::Vector3Int::GetForward(), "IV25b",
              "up and forward are distinct axes");

        // --- 门面：算术 / 查询 ------------------------------------------------
        Check(Runtime::Vector2Int::Add(Vector2Int(1, 2), Vector2Int(3, 4)) == Vector2Int(4, 6),
              "IV26", "Add");
        Check(Runtime::Vector2Int::Sub(Vector2Int(3, 4), Vector2Int(1, 2)) == Vector2Int(2, 2),
              "IV27", "Sub");
        Check(Runtime::Vector2Int::Scale(Vector2Int(2, 3), Vector2Int(4, 5)) == Vector2Int(8, 15),
              "IV28", "Scale is component-wise");
        Check(Runtime::Vector2Int::MulScalar(Vector2Int(2, -3), 3) == Vector2Int(6, -9), "IV29",
              "MulScalar");
        Check(Runtime::Vector2Int::Min(Vector2Int(5, -1), Vector2Int(2, 3)) == Vector2Int(2, -1),
              "IV30", "Min is per component (it mixes both inputs)");
        Check(Runtime::Vector2Int::Max(Vector2Int(5, -1), Vector2Int(2, 3)) == Vector2Int(5, 3),
              "IV31", "Max is per component (it mixes both inputs)");
        Check(Runtime::Vector2Int::Clamp(Vector2Int(9, -9), Vector2Int(0, 0), Vector2Int(5, 5)) ==
                  Vector2Int(5, 0), "IV32",
              "Clamp bounds both components independently");
        // ⚠️ Distance 是 **float**（3-4-5 是整勾股数，返回 int 的实现在上面是绿
        // 的）。用 (1,1)-(0,0) → √2。
        Check(Near(Runtime::Vector2Int::Distance(Vector2Int(0, 0), Vector2Int(1, 1)), 1.4142135f),
              "IV33", "Distance is a float");
        Check(Near(Runtime::Vector2Int::Distance(Vector2Int(0, 0), Vector2Int(3, 4)), 5.f), "IV33b",
              "and the 3-4-5 case agrees with it");
        Check(Runtime::Vector2Int::GetSqrMagnitude(Vector2Int(3, 4)) == 25, "IV34",
              "sqrMagnitude on the facade");
        Check(Runtime::Vector2Int::Equals(Vector2Int(1, 2), Vector2Int(1, 2)) &&
                  !Runtime::Vector2Int::Equals(Vector2Int(1, 2), Vector2Int(1, 3)), "IV35",
              "Equals compares component-wise");
        Check(Runtime::Vector2Int::ToString(Vector2Int(3, -4)) == "(-4, 3)" ||
                  Runtime::Vector2Int::ToString(Vector2Int(3, -4)) == "(3, -4)", "IV36",
              "ToString is defined (not empty)");
        // 上面那条是 or，等于没断言。真正要的是"两个分量都出现"。
        const std::string s2 = Runtime::Vector2Int::ToString(Vector2Int(3, -4)).c_str();
        Check(s2.find("3") != std::string::npos && s2.find("-4") != std::string::npos, "IV36b",
              "and it contains both components");

        Check(Runtime::Vector3Int::Add(Vector3Int(1, 2, 3), Vector3Int(4, 5, 6)) ==
                  Vector3Int(5, 7, 9), "IV37", "3D Add");
        Check(Runtime::Vector3Int::Sub(Vector3Int(4, 5, 6), Vector3Int(1, 2, 3)) ==
                  Vector3Int(3, 3, 3), "IV38", "3D Sub");
        Check(Runtime::Vector3Int::Scale(Vector3Int(1, 2, 3), Vector3Int(4, 5, 6)) ==
                  Vector3Int(4, 10, 18), "IV39", "3D Scale");
        Check(Runtime::Vector3Int::MulScalar(Vector3Int(1, -2, 3), 2) == Vector3Int(2, -4, 6),
              "IV40", "3D MulScalar");
        Check(Runtime::Vector3Int::Min(Vector3Int(5, -1, 2), Vector3Int(2, 3, 0)) ==
                  Vector3Int(2, -1, 0), "IV41", "3D Min mixes components");
        Check(Runtime::Vector3Int::Max(Vector3Int(5, -1, 2), Vector3Int(2, 3, 0)) ==
                  Vector3Int(5, 3, 2), "IV42", "3D Max mixes components");
        Check(Runtime::Vector3Int::Clamp(Vector3Int(9, -9, 3), Vector3Int(0, 0, 0),
                                         Vector3Int(5, 5, 5)) == Vector3Int(5, 0, 3), "IV43",
              "3D Clamp leaves an in-range component alone");
        Check(Near(Runtime::Vector3Int::Distance(Vector3Int(0, 0, 0), Vector3Int(1, 1, 1)),
                  1.7320508f), "IV44", "3D Distance is a float too (sqrt 3)");
        Check(Runtime::Vector3Int::GetSqrMagnitude(Vector3Int(1, 2, 3)) == 14, "IV45",
              "3D sqrMagnitude");
    }

    // =========================================================================
    // Rect / RectInt
    // =========================================================================
    //
    // ⚠️ 这个类型的三处"反直觉"各配一组互补夹具，否则全是静默错误：
    //
    //   1. `Contains` 是**半开** `[min, max)`，与 `Bounds.Contains`（闭区间）相反。
    //      只测内部点分不出来 —— 必须测**正好落在右/上边**的点。
    //   2. 负 width / height 合法，且 `min`/`max` 归一化。只测正 width 的矩形，
    //      把 `xMin` 实现成 `return x;` 也全绿。
    //   3. `size`/`position` 是**原样**返回 width/height 与 x/y（可以为负），
    //      而 `min`/`max`/`center` 才是几何量。把 size 写成"归一化后的正尺寸"
    //      在正 width 上也是绿的。
    void TestRects()
    {
        using ::Rect;
        using ::RectInt;

        // --- 值类型：边与归一化 ------------------------------------------------
        const Rect normal(0.f, 0.f, 10.f, 20.f);
        Check(normal.GetXMin() == 0.f && normal.GetXMax() == 10.f && normal.GetYMin() == 0.f &&
                  normal.GetYMax() == 20.f, "RC0", "a positive rect's edges are x .. x+width");
        // ⚠️ 负 width：两条边**交换**，`xMin` 不是 `x`。
        const Rect flipped(10.f, 20.f, -10.f, -20.f);
        Check(flipped.GetXMin() == 0.f && flipped.GetXMax() == 10.f, "RC1",
              "a negative-width rect's xMin is NOT x (it is 0, not 10)");
        Check(flipped.GetYMin() == 0.f && flipped.GetYMax() == 20.f, "RC2",
              "and the same on Y");
        Check(Runtime::Rect::GetMin(flipped) == Vector2(0.f, 0.f) &&
                  Runtime::Rect::GetMax(flipped) == Vector2(10.f, 20.f), "RC3",
              "min/max are the normalised corners");
        // ⚠️ 互补的一半：`size` / `position` **不**归一化。
        Check(Runtime::Rect::GetSize(flipped) == Vector2(-10.f, -20.f), "RC4",
              "but size is literally {width, height} -- still negative");
        Check(Runtime::Rect::GetPosition(flipped) == Vector2(10.f, 20.f), "RC5",
              "and position is literally {x, y} -- not min");
        Check(Runtime::Rect::GetCenter(flipped) == Vector2(5.f, 10.f), "RC6",
              "while center is geometric (x + width/2)");

        // --- Contains：半开区间 -------------------------------------------------
        Check(Runtime::Rect::ContainsPoint(normal, Vector2(5.f, 5.f)), "RC7",
              "an interior point is contained");
        Check(Runtime::Rect::ContainsPoint(normal, Vector2(0.f, 0.f)), "RC8",
              "the min corner is contained (>= on the low side)");
        // ⚠️ 这一条就是"半开"的判据：`Bounds.Contains` 在同样位置返回 true。
        Check(!Runtime::Rect::ContainsPoint(normal, Vector2(10.f, 20.f)), "RC9",
              "but the max corner is NOT (the range is [min, max), unlike Bounds)");
        Check(!Runtime::Rect::ContainsPoint(normal, Vector2(10.f, 5.f)) &&
                  !Runtime::Rect::ContainsPoint(normal, Vector2(5.f, 20.f)), "RC10",
              "and each edge on its own is out too");
        Check(!Runtime::Rect::ContainsPoint(normal, Vector2(-0.001f, 5.f)), "RC11",
              "a hair outside the low edge is out");
        Check(Runtime::Rect::ContainsXY(normal, 5.f, 5.f) &&
                  !Runtime::Rect::ContainsXY(normal, 10.f, 5.f), "RC12",
              "ContainsXY agrees with Contains");
        // 负 width 的矩形照样能 Contains（用的是归一化的两条边）。
        Check(Runtime::Rect::ContainsPoint(flipped, Vector2(5.f, 5.f)), "RC13",
              "a negative-width rect contains points too (it uses the normalised edges)");

        // --- Overlaps：严格不等 -------------------------------------------------
        Check(Runtime::Rect::Overlaps(Rect(0.f, 0.f, 10.f, 10.f), Rect(5.f, 5.f, 10.f, 10.f)),
              "RC14", "overlapping rects overlap");
        // ⚠️ 只共享一条边**不算**相交（与 `Bounds.Intersects` 相反）。
        Check(!Runtime::Rect::Overlaps(Rect(0.f, 0.f, 10.f, 10.f), Rect(10.f, 0.f, 10.f, 10.f)),
              "RC15", "but rects sharing only an edge do NOT (strict, unlike Bounds.Intersects)");
        Check(!Runtime::Rect::Overlaps(Rect(0.f, 0.f, 10.f, 10.f), Rect(20.f, 0.f, 5.f, 5.f)),
              "RC16", "and disjoint rects do not");
        Check(Runtime::Rect::Overlaps(Rect(0.f, 0.f, 10.f, 10.f), Rect(-5.f, -5.f, 100.f, 100.f)),
              "RC17", "a containing rect overlaps");
        // 负 width 的矩形也要能相交：Overlaps 内部用 min/max，不是 x/width。
        Check(Runtime::Rect::Overlaps(Rect(10.f, 0.f, -10.f, 10.f), Rect(5.f, 5.f, 10.f, 10.f)),
              "RC18", "and a negative-width rect overlaps through its normalised edges");

        // --- 非破坏 setter ------------------------------------------------------
        const Rect moved = Runtime::Rect::WithXMin(normal, 2.f);
        Check(moved.x == 2.f && moved.width == 8.f, "RC19",
              "setting xMin moves the left edge and shrinks width (right edge stays)");
        const Rect grown = Runtime::Rect::WithXMax(normal, 20.f);
        Check(grown.x == 0.f && grown.width == 20.f, "RC20",
              "setting xMax keeps the left edge and grows width");
        Check(normal.width == 10.f, "RC21", "and the original is untouched (value semantics)");
        const Rect recentered = Runtime::Rect::WithCenter(normal, Vector2(0.f, 0.f));
        Check(recentered.x == -5.f && recentered.y == -10.f && recentered.width == 10.f, "RC22",
              "setting center translates without resizing");
        const Rect resized = Runtime::Rect::WithSize(normal, Vector2(1.f, 2.f));
        Check(resized.x == 0.f && resized.width == 1.f && resized.height == 2.f, "RC23",
              "setting size keeps x/y");
        const Rect repositioned = Runtime::Rect::WithPosition(normal, Vector2(3.f, 4.f));
        Check(repositioned.x == 3.f && repositioned.width == 10.f, "RC24",
              "setting position moves without resizing");
        const Rect relaid = Runtime::Rect::WithLayout(normal, 1.f, 2.f, 3.f, 4.f);
        Check(relaid.x == 1.f && relaid.y == 2.f && relaid.width == 3.f && relaid.height == 4.f,
              "RC25", "Set() replaces all four fields");
        // min/max setter：挪一个角，对角不动。
        const Rect re_min = Runtime::Rect::WithMin(normal, Vector2(2.f, 3.f));
        Check(re_min.GetXMin() == 2.f && re_min.GetXMax() == 10.f && re_min.GetYMin() == 3.f &&
                  re_min.GetYMax() == 20.f, "RC26", "setting min moves only the low corner");
        const Rect re_max = Runtime::Rect::WithMax(normal, Vector2(30.f, 40.f));
        Check(re_max.GetXMin() == 0.f && re_max.GetXMax() == 30.f && re_max.GetYMin() == 0.f &&
                  re_max.GetYMax() == 40.f, "RC27", "setting max moves only the high corner");

        // --- OrderMinMax / 归一化坐标 --------------------------------------------
        const Rect ordered = Runtime::Rect::OrderMinMax(flipped);
        Check(ordered.x == 0.f && ordered.width == 10.f && ordered.y == 0.f &&
                  ordered.height == 20.f, "RC28",
              "OrderMinMax flips negative extents (x becomes min)");
        Check(Runtime::Rect::OrderMinMax(normal) == normal, "RC29",
              "and leaves an already-ordered rect alone");
        const Vector2 uv = Runtime::Rect::PointToNormalized(normal, Vector2(2.5f, 5.f));
        Check(Near(uv.x, 0.25f) && Near(uv.y, 0.25f), "RC30",
              "PointToNormalized maps to 0..1 (a quarter in, a quarter out)");
        // ⚠️ 不能只测中点：缩放写反（乘而不是除）在中点 (0.5,0.5) 上也是 0.5。
        const Vector2 uv2 = Runtime::Rect::PointToNormalized(normal, Vector2(7.5f, 15.f));
        Check(Near(uv2.x, 0.75f) && Near(uv2.y, 0.75f), "RC30b",
              "and three quarters in is 0.75 (a mirrored implementation would give 0.25)");
        Check(Runtime::Rect::NormalizedToPoint(normal, Vector2(0.25f, 0.5f)) ==
                  Vector2(2.5f, 10.f), "RC31", "NormalizedToPoint is its inverse");
        Check(Runtime::Rect::MinMaxRect(1.f, 2.f, 3.f, 4.f).width == 2.f &&
                  Runtime::Rect::MinMaxRect(1.f, 2.f, 3.f, 4.f).x == 1.f, "RC32",
              "MinMaxRect takes two opposite corners");
        // 顺序反过来也要对（内部做 Min/Max）——“两条边”与"x + 尺寸"的分界线。
        const Rect reversed = Runtime::Rect::MinMaxRect(3.f, 4.f, 1.f, 2.f);
        Check(reversed.GetXMin() == 1.f && reversed.GetXMax() == 3.f && reversed.GetYMin() == 2.f &&
                  reversed.GetYMax() == 4.f, "RC33",
              "and it normalises when the corners are given in reverse");

        // --- RectInt ------------------------------------------------------------
        const RectInt cell(2, 3, 4, 5);
        Check(Runtime::RectInt::GetXMin(cell) == 2 && Runtime::RectInt::GetXMax(cell) == 6, "RC34",
              "RectInt edges");
        Check(Runtime::RectInt::GetMin(cell) == ::Vector2Int(2, 3) &&
                  Runtime::RectInt::GetMax(cell) == ::Vector2Int(6, 8), "RC35",
              "RectInt min/max are Vector2Int (not Vector2)");
        Check(Runtime::RectInt::GetSize(cell) == ::Vector2Int(4, 5), "RC36",
              "RectInt size is Vector2Int");
        Check(Runtime::RectInt::GetPosition(cell) == ::Vector2Int(2, 3), "RC37",
              "RectInt position is Vector2Int");
        // ⚠️ `center` 是 **Vector2（float）**，与 `size`（Vector2Int）不同 —— 奇数
        // 尺寸的矩形中心落在半格上。
        Check(Runtime::RectInt::GetCenter(RectInt(0, 0, 3, 5)) == Vector2(1.5f, 2.5f), "RC38",
              "RectInt center is a float Vector2 (an odd size lands on a half cell)");
        Check(Runtime::RectInt::ContainsPoint(cell, ::Vector2Int(3, 4)), "RC39",
              "RectInt contains an interior cell");
        Check(!Runtime::RectInt::ContainsPoint(cell, ::Vector2Int(6, 4)), "RC40",
              "and excludes the max edge ([min, max) here too)");
        Check(Runtime::RectInt::ContainsXY(cell, 2, 3) && !Runtime::RectInt::ContainsXY(cell, 6, 8),
              "RC41", "RectInt.ContainsXY agrees");
        Check(Runtime::RectInt::Overlaps(RectInt(0, 0, 5, 5), RectInt(4, 4, 5, 5)), "RC42",
              "RectInt overlaps");
        Check(!Runtime::RectInt::Overlaps(RectInt(0, 0, 5, 5), RectInt(5, 0, 5, 5)), "RC43",
              "and sharing an edge is not an overlap (strict)");

        // --- RectInt.ClampToBounds（只有 RectInt 有）------------------------------
        const RectInt bounds(0, 0, 10, 10);
        const RectInt inside = Runtime::RectInt::ClampToBounds(RectInt(2, 2, 3, 3), bounds);
        Check(inside.x == 2 && inside.y == 2 && inside.width == 3 && inside.height == 3, "RC44",
              "a rect already inside the bounds is untouched");
        const RectInt left = Runtime::RectInt::ClampToBounds(RectInt(-5, 2, 3, 3), bounds);
        Check(left.x == 0 && left.y == 2 && left.width == 3 && left.height == 3, "RC45",
              "one hanging off the low side is pushed in, keeping its size");
        const RectInt right = Runtime::RectInt::ClampToBounds(RectInt(9, 2, 3, 3), bounds);
        Check(right.x == 7 && right.width == 3, "RC46",
              "one hanging off the high side comes back to xMax - width (7, not 9)");
        // ⚠️ 比 bounds 还大：顶到 bounds 边上，尺寸被裁。只测小的看不出这条。
        const RectInt huge = Runtime::RectInt::ClampToBounds(RectInt(-5, -5, 100, 100), bounds);
        Check(huge.x == 0 && huge.y == 0 && huge.width == 10 && huge.height == 10, "RC47",
              "one bigger than the bounds is clipped to the bounds");
        // 原始值不被改（非破坏）。
        Check(Runtime::RectInt::GetXMin(RectInt(9, 2, 3, 3)) == 9, "RC48",
              "ClampToBounds does not mutate its argument");
        // 负尺寸的 RectInt 也要能夹取：内部走的是 min/max 而不是裸 x/width。
        // ⚠️ 夹具必须**伸到 bounds 外面**（x=15 → 边在 12..15），否则"不夹取"
        // 的实现也在 bounds 内，测不出来。用裸 width(-3) 的实现会得到
        // xMin=10、xMax=13 —— 仍有一半在 bounds 外。
        const RectInt neg = Runtime::RectInt::ClampToBounds(RectInt(15, 2, -3, 3), bounds);
        Check(neg.GetXMin() == 7 && neg.GetXMax() == 10 && neg.width == 3, "RC49",
              "and a negative-width RectInt is clamped through its normalised edges");

        const RectInt remaxed = Runtime::RectInt::WithMinMax(RectInt(0, 0, 1, 1),
                                                             ::Vector2Int(5, 6),
                                                             ::Vector2Int(7, 8));
        Check(remaxed.GetXMin() == 5 && remaxed.GetXMax() == 7, "RC50",
              "RectInt.SetMinMax takes two opposite corners");
        Check(Runtime::RectInt::MinMaxRect(7, 8, 5, 6).GetXMin() == 5, "RC51",
              "and MinMaxRect normalises reversed corners too");
        const std::string rs = Runtime::RectInt::ToString(cell).c_str();
        Check(rs.find("2") != std::string::npos && rs.find("5") != std::string::npos, "RC52",
              "RectInt.ToString is defined and mentions its fields");
        const std::string fs = Runtime::Rect::ToString(normal).c_str();
        Check(!fs.empty(), "RC53", "Rect.ToString is defined");
    }

    void TestRectWiring()
    {
        const std::string value_header = ReadFile("Source/Runtime/Core/Math/Rect.h");
        Check(!value_header.empty(), "RCW0", "Rect.h is readable");
        if (value_header.empty()) { return; }
        // ⚠️ 两个 using 是"Rect / RectInt 存在"的源头：没有它们，`::Rect` 只是
        // `RectT<float>` 的一个模板实例化，脚本侧名字对不上。
        Check(CountOccurrences(value_header, "using Rect = RectT<float>;") >= 1, "RCW1",
              "Rect is RectT<float>");
        Check(CountOccurrences(value_header, "using RectInt = RectT<int>;") >= 1, "RCW2",
              "RectInt is RectT<int>");
        // 归一化那两个 getter 是"负 width 也正确"的唯一来源。
        Check(CountOccurrences(value_header, "GetXMin") >= 1 &&
                  CountOccurrences(value_header, "GetXMax") >= 1 &&
                  CountOccurrences(value_header, "GetYMin") >= 1 &&
                  CountOccurrences(value_header, "GetYMax") >= 1, "RCW3",
              "all four normalised edge getters exist");
        // ⚠️ 半开区间：`Contains` 里必须是 `>=` + `<`。只 grep 名字看不出符号方向。
        Check(CountOccurrences(value_header, "point_x >= GetXMin() && point_x < GetXMax()") >= 1,
              "RCW4", "Contains is half-open: >= on the low side, < on the high side");
        Check(CountOccurrences(value_header, "other.GetXMax() > GetXMin()") >= 1, "RCW5",
              "Overlaps uses strict comparisons");

        const std::string int_header = ReadFile("Source/Runtime/Core/Math/VectorInt.h");
        Check(!int_header.empty(), "RCW6", "VectorInt.h is readable");
        if (!int_header.empty())
        {
            Check(CountOccurrences(int_header, "struct Vector2Int") >= 1 &&
                      CountOccurrences(int_header, "struct Vector3Int") >= 1, "RCW7",
                  "both integer vector value types exist");
            // ⚠️ magnitude 必须是 float：返回 int 的实现在 3-4-5 上是绿的。
            Check(CountOccurrences(int_header, "float GetMagnitude() const") >= 2, "RCW8",
                  "magnitude is a float on both of them (not int)");
        }

        const std::string binding_header =
            ReadFile("Source/Runtime/Scripting/Binding/ScriptBinding.h");
        Check(!binding_header.empty(), "RCW9", "ScriptBinding.h is readable");
        if (!binding_header.empty())
        {
            Check(CountOccurrences(binding_header, "struct Converter<Vector2Int>") >= 1 &&
                      CountOccurrences(binding_header, "struct Converter<Vector3Int>") >= 1 &&
                      CountOccurrences(binding_header, "struct Converter<Rect>") >= 1 &&
                      CountOccurrences(binding_header, "struct Converter<RectInt>") >= 1, "RCW10",
                  "all four value types have a marshaller");
        }

        const std::string binding_source =
            ReadFile("Source/Runtime/Scripting/Binding/ScriptBinding.cpp");
        Check(!binding_source.empty(), "RCW11", "ScriptBinding.cpp is readable");
        if (!binding_source.empty())
        {
            Check(CountOccurrences(binding_source, "declare interface Vector2Int {") >= 1 &&
                      CountOccurrences(binding_source, "declare interface Vector3Int {") >= 1 &&
                      CountOccurrences(binding_source, "declare interface Rect {") >= 1 &&
                      CountOccurrences(binding_source, "declare interface RectInt {") >= 1,
                  "RCW12", "all four are declared in the .d.ts");
        }

        const std::string rect_facade = ReadFile("Source/Runtime/Scripting/ScriptRect/ScriptRect.h");
        Check(!rect_facade.empty(), "RCW13", "ScriptRect.h is readable");
        if (!rect_facade.empty())
        {
            // ⚠️ `ClampToBounds` 只有 RectInt 有 —— 两边都导出的实现在"照搬 Unity"
            // 这条上是错的，但没有任何一条行为测试能发现。这里按**所在类**数。
            Check(CountOccurrences(rect_facade, "static ::RectInt ClampToBounds(") >= 1, "RCW14",
                  "RectInt exports ClampToBounds");
            Check(CountOccurrences(rect_facade, "static ::Rect ClampToBounds(") == 0, "RCW15",
                  "and Rect does NOT (Unity has no Rect.ClampToBounds)");
            Check(CountOccurrences(rect_facade, "PointToNormalized") >= 1, "RCW16",
                  "Rect exports PointToNormalized");
        }
    }

    // =========================================================================
    // Plane / GeometryUtility
    // =========================================================================
    //
    // 这两个类型在 C++ 里早就有了（`Runtime/Core/Math/Plane.h` 与 `Frustum.h`
    // 都写得很完整），但脚本侧**一个都没有**：`Converter<Plane>`、
    // `declare interface Plane`、门面全是零命中。于是脚本做不了"这个盒子在不在
    // 画面里"—— 只能把 `Physics.Raycast` 当万能钥匙用。
    //
    // ⚠️ 三处判据各配一组**互补**夹具，否则全是静默错误：
    //   1. `distance` 的符号（"点在正面"是哪一侧）。只测"点在平面上"（距离 0）
    //      分不出 `+distance` 与 `-distance`。
    //   2. `Flip` 必须同时翻转 normal **和** distance —— 只翻 normal 的话平面
    //      变成另一个平面，而它仍然自洽（法线是单位向量、点仍在面上求 0）。
    //   3. `Raycast` 只打正面。只测"从正面射过去"分不出"背面也算命中"。
    void TestPlaneAndGeometryUtility()
    {
        using ::Bounds;
        using ::Plane;

        // --- 构造 ------------------------------------------------------------
        const Plane ground_from_position = Runtime::Plane::FromNormalAndPosition(
            Vector3(0.f, 0.f, 1.f), Vector3(0.f, 0.f, 3.f));
        // ⚠️ 这一条就是 distance 符号的判据：平面在 z = 3，原点是"下面"，所以
        // `distance` 是 **-3**（`dot(n, p) + d = 0` ⇒ `0 + d = -3`）。写成 +3 的
        // 话 GetDistanceToPoint 会给 +3，方向整个反过来，而 `GetSide` 仍然返回
        // 一个"看着合理"的 bool。
        Check(Near(ground_from_position.distance, -3.f), "PL0",
              "a plane through (0,0,3) with normal +Z has distance -3 (not +3)");
        Check(Near(ground_from_position.GetDistanceToPoint(Vector3(0.f, 0.f, 3.f)), 0.f), "PL1",
              "a point on the plane is at distance 0");
        Check(Near(ground_from_position.GetDistanceToPoint(Vector3(0.f, 0.f, 5.f)), 2.f), "PL2",
              "and a point above it is at +2 (the normal side is positive)");
        Check(Near(ground_from_position.GetDistanceToPoint(Vector3(0.f, 0.f, 1.f)), -2.f), "PL3",
              "and below it is -2");
        // FromNormalAndPosition 归一化法线：给一个长度 2 的法线，距离仍然是真距离。
        const Plane unnormalised = Runtime::Plane::FromNormalAndPosition(
            Vector3(0.f, 0.f, 2.f), Vector3(0.f, 0.f, 3.f));
        Check(Near(unnormalised.normal.z, 1.f), "PL4", "FromNormalAndPosition normalises the normal");
        Check(Near(unnormalised.GetDistanceToPoint(Vector3(0.f, 0.f, 5.f)), 2.f), "PL5",
              "so the distance is a real distance, not twice it");
        // ⚠️ 互补的一半：`Create` **不**归一化（照 Unity 的构造函数）。
        const Plane raw = Runtime::Plane::Create(Vector3(0.f, 0.f, 2.f), -6.f);
        Check(Near(raw.normal.z, 2.f), "PL6", "Create does NOT normalise (same as Unity's ctor)");
        Check(Near(raw.GetDistanceToPoint(Vector3(0.f, 0.f, 5.f)), 4.f), "PL7",
              "so its 'distance' is scaled by the normal's length");
        Check(Near(Runtime::Plane::Normalized(raw).GetDistanceToPoint(Vector3(0.f, 0.f, 5.f)), 2.f),
              "PL8", "and Normalize brings it back to a real distance");

        // 三点定面：绕序按右手定则给出法线。
        const Plane from_points = Runtime::Plane::FromPoints(
            Vector3(0.f, 0.f, 0.f), Vector3(1.f, 0.f, 0.f), Vector3(0.f, 1.f, 0.f));
        Check(Near(from_points.normal.z, 1.f), "PL9", "three CCW points give the +Z normal");
        // 反向绕序 ⇒ 法线朝 -Z。只测一个绕序分不出"叉乘的两边写反了"。
        const Plane reversed = Runtime::Plane::FromPoints(
            Vector3(0.f, 0.f, 0.f), Vector3(0.f, 1.f, 0.f), Vector3(1.f, 0.f, 0.f));
        Check(Near(reversed.normal.z, -1.f), "PL10", "and the reversed winding gives -Z");
        // 共线 ⇒ 退化成零平面（不产生 NaN）。
        const Plane collinear = Runtime::Plane::FromPoints(
            Vector3(0.f, 0.f, 0.f), Vector3(1.f, 0.f, 0.f), Vector3(2.f, 0.f, 0.f));
        Check(collinear.normal == Vector3::ZERO && collinear.distance == 0.f, "PL11",
              "three collinear points degenerate to the zero plane (no NaN)");
        Check(Runtime::Plane::Normalized(collinear).normal == Vector3::ZERO, "PL12",
              "and Normalize on a zero plane is a no-op (it would divide by zero)");

        // --- 查询 ------------------------------------------------------------
        const Plane vertical(Vector3(1.f, 0.f, 0.f), -5.f);   // x = 5，正面是 +x
        Check(Runtime::Plane::GetSide(vertical, Vector3(6.f, 0.f, 0.f)), "PL13",
              "GetSide is true on the normal side");
        Check(!Runtime::Plane::GetSide(vertical, Vector3(4.f, 0.f, 0.f)), "PL14",
              "and false behind");
        // ⚠️ 边界算正面（`>= 0`），与 Unity 的 GetSide 一致。
        Check(Runtime::Plane::GetSide(vertical, Vector3(5.f, 0.f, 0.f)), "PL15",
              "and a point exactly ON the plane counts as the front side (>= 0)");
        Check(Runtime::Plane::SameSide(vertical, Vector3(6.f, 0.f, 0.f), Vector3(9.f, 0.f, 0.f)),
              "PL16", "SameSide is true for two points on one side");
        Check(!Runtime::Plane::SameSide(vertical, Vector3(6.f, 0.f, 0.f), Vector3(1.f, 0.f, 0.f)),
              "PL17", "and false for one on each side");
        // ⚠️ 边界：与 `GetSide` 的 `>=` 一致，边界点算**正面**那一侧 —— 所以它与
        // 正面上的点同侧，与背面上的点不同侧。Unity 的 SameSide 一样是"由
        // GetDistanceToPoint 的符号两段拼出来"，两层规则必须同一套 `>=`。
        Check(Runtime::Plane::SameSide(vertical, Vector3(5.f, 0.f, 0.f), Vector3(9.f, 0.f, 0.f)) &&
                  !Runtime::Plane::SameSide(vertical, Vector3(5.f, 0.f, 0.f), Vector3(1.f, 0.f, 0.f)),
              "PL18", "a point exactly ON the plane counts as the front side (SameSide follows GetSide's >=)");
        Check(Runtime::Plane::GetClosestPointOnPlane(vertical, Vector3(9.f, 3.f, 4.f)) ==
                  Vector3(5.f, 3.f, 4.f), "PL19", "ClosestPointOnPlane drops the normal component");

        // --- Raycast：只打正面，t 是射线参数 ----------------------------------
        // ⚠️ `vertical` 的正面是 **x > 5**（法线 +x）。所以"打正面"的射线是
        // **从 x = 9 往 -x 方向**射：起点在正面、朝着法线的反方向走。
        Check(Near(Runtime::Plane::Raycast(vertical, Vector3(9.f, 0.f, 0.f),
                                           Vector3(-1.f, 0.f, 0.f)), 4.f), "PL20",
              "a ray fired at the front face from x=9 towards -x hits at t = 4");
        // ⚠️ 互补的一半：从**背面**（x = 0）朝 +x 射过去**不**命中（-1）。
        // 只测"从正面射"分不出"背面也算命中"。
        Check(Runtime::Plane::Raycast(vertical, Vector3(0.f, 0.f, 0.f),
                                      Vector3(1.f, 0.f, 0.f)) == -1.f, "PL21",
              "a ray fired from BEHIND the plane does NOT hit (it only hits the front face)");
        Check(Runtime::Plane::Raycast(vertical, Vector3(9.f, 0.f, 0.f),
                                      Vector3(1.f, 0.f, 0.f)) == -1.f, "PL22",
              "and neither does one fired away from the front face");
        Check(Runtime::Plane::Raycast(vertical, Vector3(0.f, 1.f, 0.f),
                                      Vector3(0.f, 1.f, 0.f)) == -1.f, "PL23",
              "a parallel ray misses (no NaN, and 'grazing' is not a hit)");
        // ⚠️ t 不是世界距离：方向长 2 时 t 是原来的一半（4 → 2）。
        Check(Near(Runtime::Plane::Raycast(vertical, Vector3(9.f, 0.f, 0.f),
                                           Vector3(-2.f, 0.f, 0.f)), 2.f), "PL24",
              "t is the ray parameter, not a world distance (a length-2 direction halves it)");
        // 起点在平面上 ⇒ t = 0（不是"不命中"）。
        Check(Runtime::Plane::Raycast(vertical, Vector3(5.f, 0.f, 0.f),
                                      Vector3(-1.f, 0.f, 0.f)) == 0.f, "PL25",
              "starting exactly on the plane gives t = 0, not a miss");

        // --- 非破坏的"改" ------------------------------------------------------
        // ⚠️ Flip 必须翻 normal **和** distance：只翻 normal 的话平面跑到 x = -5，
        // 但 `GetDistanceToPoint(0)` 仍是一个看着合理的数。用原点到平面的距离判。
        const Plane flipped = Runtime::Plane::Flipped(vertical);
        Check(Near(flipped.normal.x, -1.f) && Near(flipped.distance, 5.f), "PL26",
              "Flip negates the normal AND the distance (the plane itself is unchanged)");
        Check(Near(flipped.GetDistanceToPoint(Vector3(5.f, 0.f, 0.f)), 0.f), "PL27",
              "so the flipped plane still passes through the same points");
        Check(Near(vertical.GetDistanceToPoint(Vector3(0.f, 0.f, 0.f)), -5.f), "PL28",
              "and the original is untouched (value semantics)");
        // ⚠️ Translate 的符号：`distance -= dot(normal, translation)`。
        // 沿法线正方向推 2 ⇒ 平面从 x = 5 到 x = 7。
        const Plane moved = Runtime::Plane::Translated(vertical, Vector3(2.f, 0.f, 0.f));
        Check(Near(moved.distance, -7.f), "PL29",
              "translating along +normal by 2 moves the plane to x = 7 (distance -= dot)");
        Check(Near(moved.GetDistanceToPoint(Vector3(7.f, 0.f, 0.f)), 0.f), "PL30",
              "and the moved plane really passes through x = 7");
        // 沿法线方向平移不改变"点相对平面的有符号距离"？会改变 —— 这正是平移的定义。
        Check(Near(moved.GetDistanceToPoint(Vector3(0.f, 0.f, 0.f)), -7.f), "PL31",
              "so the origin is now 7 behind it");
        // 沿**垂直**于法线的方向平移不改变平面。
        Check(Near(Runtime::Plane::Translated(vertical, Vector3(0.f, 5.f, 5.f)).distance, -5.f),
              "PL32", "translating perpendicular to the normal does not move the plane");

        // --- GeometryUtility.TestPlanesAABB ------------------------------------
        // 一个"半空间"：x >= 5 那一侧。
        std::vector<Plane> half_space;
        half_space.push_back(vertical);
        const Bounds inside_bounds(Vector3(8.f, 0.f, 0.f), Vector3(1.f, 1.f, 1.f));
        const Bounds outside_bounds(Vector3(0.f, 0.f, 0.f), Vector3(1.f, 1.f, 1.f));
        Check(Runtime::GeometryUtility::TestPlanesAABB(half_space, inside_bounds), "PL33",
              "a box on the front side survives the half-space");
        Check(!Runtime::GeometryUtility::TestPlanesAABB(half_space, outside_bounds), "PL34",
              "a box entirely behind it does not");
        // 跨过平面的盒子**算**通过（正顶点在正面）—— 这是"保守、宁多画"的那一半。
        const Bounds straddling(Vector3(5.f, 0.f, 0.f), Vector3(2.f, 1.f, 1.f));
        Check(Runtime::GeometryUtility::TestPlanesAABB(half_space, straddling), "PL35",
              "a box straddling the plane still passes (conservative: it may over-draw)");
        // 空平面数组：没有任何平面能把它判到外面去 ⇒ true。
        Check(Runtime::GeometryUtility::TestPlanesAABB(std::vector<Plane>(), inside_bounds), "PL36",
              "an empty plane list rejects nothing (no plane can push it out)");

        // 六个面围出来的"视锥"：用一个手搭的轴对齐盒子当锥（法线朝内）。
        std::vector<Plane> box_planes;
        box_planes.push_back(Plane(Vector3(1.f, 0.f, 0.f), 10.f));    // x >= -10
        box_planes.push_back(Plane(Vector3(-1.f, 0.f, 0.f), 10.f));   // x <= 10
        box_planes.push_back(Plane(Vector3(0.f, 1.f, 0.f), 10.f));    // y >= -10
        box_planes.push_back(Plane(Vector3(0.f, -1.f, 0.f), 10.f));   // y <= 10
        box_planes.push_back(Plane(Vector3(0.f, 0.f, 1.f), 10.f));    // z >= -10
        box_planes.push_back(Plane(Vector3(0.f, 0.f, -1.f), 10.f));   // z <= 10
        Check(Runtime::GeometryUtility::TestPlanesAABB(box_planes,
                                                       Bounds(Vector3::ZERO, Vector3(2.f, 2.f, 2.f))),
              "PL37", "a box at the centre of a 6-plane box frustum passes");
        Check(!Runtime::GeometryUtility::TestPlanesAABB(box_planes,
                                                        Bounds(Vector3(50.f, 0.f, 0.f),
                                                               Vector3(2.f, 2.f, 2.f))),
              "PL38", "one far outside on X fails");
        // ⚠️ 六个面里**只要一个**判出去就 false —— 只测"整体在外面"的话，
        // "只看第一个平面"的实现照样全绿。
        Check(!Runtime::GeometryUtility::TestPlanesAABB(box_planes,
                                                        Bounds(Vector3(0.f, 50.f, 0.f),
                                                               Vector3(2.f, 2.f, 2.f))),
              "PL39", "and so does one far outside on Y (every plane is checked, not just the first)");
        Check(!Runtime::GeometryUtility::TestPlanesAABB(box_planes,
                                                        Bounds(Vector3(0.f, 0.f, 50.f),
                                                               Vector3(2.f, 2.f, 2.f))),
              "PL40", "and on Z");

        // --- GeometryUtility.CalculateFrustumPlanesVP -----------------------------
        //
        // 手搭一张**正交**投影矩阵：把 `[-10, 10]^3` 映射到裁剪空间
        // `x, y ∈ [-1, 1]`、`z ∈ [0, 1]`。这样六个平面就是"盒子的六个面"，可以
        // **逐条**对上手写的值 —— 用透视矩阵的话只能对上"符号"，对错都过。
        //
        // ⚠️ 约定是**列向量**（`clip = M * world`，与 `MakePerspectiveMatrix` /
        // `Frustum::FromViewProjection` 同一套）：第 i 行点乘 world 给出 clip 的第
        // i 个分量，平移项在 `m[i][3]`。所以 z 的平移是 `m[2][3]`，**不是** `m[3][2]`
        // —— 后者会污染 w 行，六个平面就全都不是盒子的面了（`w + x` 里混进 z 项）。
        Matrix4x4 ortho = Matrix4x4::IDENTITY;
        ortho.m_Mat[0][0] = 0.1f;    // x: [-10,10] -> [-1,1]
        ortho.m_Mat[1][1] = 0.1f;    // y
        ortho.m_Mat[2][2] = 0.05f;   // z: [-10,10] -> [0,1]
        ortho.m_Mat[2][3] = 0.5f;    // z 的平移项（z = 0 落在 near 上）
        // w 行保持 (0,0,0,1)：正交投影的 w 恒为 1。

        const std::vector<Plane> ortho_planes =
            Runtime::GeometryUtility::CalculateFrustumPlanesVP(ortho);
        Check(ortho_planes.size() == 6, "PL41",
              "CalculateFrustumPlanesVP returns six planes (same as Unity)");
        if (ortho_planes.size() == 6)
        {
            // ⚠️ 六个平面必须与上面**手搭**的那六个逐个相同（含顺序）：
            // 左/右/下/上/近/远。这条同时钉住 `distance` 的符号与法线的朝向 ——
            // 只比较 `TestPlanesAABB` 的 bool 的话，六个面全翻过来也全绿。
            bool same = true;
            for (int i = 0; i < 6; ++i)
            {
                if (!Near(ortho_planes[static_cast<std::size_t>(i)].normal.x,
                          box_planes[static_cast<std::size_t>(i)].normal.x) ||
                    !Near(ortho_planes[static_cast<std::size_t>(i)].normal.y,
                          box_planes[static_cast<std::size_t>(i)].normal.y) ||
                    !Near(ortho_planes[static_cast<std::size_t>(i)].normal.z,
                          box_planes[static_cast<std::size_t>(i)].normal.z) ||
                    !Near(ortho_planes[static_cast<std::size_t>(i)].distance,
                          box_planes[static_cast<std::size_t>(i)].distance))
                {
                    same = false;
                }
            }
            Check(same, "PL42",
                  "and they are exactly the six faces of that box, in Unity's order");
            // 与 `Frustum::Intersects` 同一套判据：门面不该自己另写一份。
            const ::Bounds probe_a(Vector3::ZERO, Vector3(2.f, 2.f, 2.f));
            const ::Bounds probe_b(Vector3(50.f, 0.f, 0.f), Vector3(2.f, 2.f, 2.f));
            Check(Runtime::GeometryUtility::TestPlanesAABB(ortho_planes, probe_a) ==
                      Frustum::FromViewProjection(ortho).Intersects(probe_a), "PL43",
                  "TestPlanesAABB agrees with Frustum::Intersects (one implementation, two doors)");
            Check(Runtime::GeometryUtility::TestPlanesAABB(ortho_planes, probe_b) ==
                      Frustum::FromViewProjection(ortho).Intersects(probe_b), "PL44",
                  "and it agrees on the outside case too (not just when both are true)");
        }

        // --- 拿不到相机 ⇒ 空数组，不是 6 个退化平面 -----------------------------
        Check(Runtime::GeometryUtility::CalculateFrustumPlanes(nullptr).empty(), "PL45",
              "a null camera gives an EMPTY list (six degenerate planes would make "
              "TestPlanesAABB silently always true)");
        Camera unbound_camera;
        Check(Runtime::GeometryUtility::CalculateFrustumPlanes(&unbound_camera).empty(), "PL46",
              "and so does an unbound Camera");
    }

    void TestPlaneWiring()
    {
        const std::string binding_header =
            ReadFile("Source/Runtime/Scripting/Binding/ScriptBinding.h");
        Check(!binding_header.empty(), "PLW0", "ScriptBinding.h is readable");
        if (binding_header.empty()) { return; }
        Check(CountOccurrences(binding_header, "struct Converter<Plane>") >= 1, "PLW1",
              "Plane has a marshaller (without it the name never reaches scripts)");

        const std::string binding_source =
            ReadFile("Source/Runtime/Scripting/Binding/ScriptBinding.cpp");
        Check(!binding_source.empty(), "PLW2", "ScriptBinding.cpp is readable");
        if (!binding_source.empty())
        {
            Check(CountOccurrences(binding_source, "declare interface Plane {") >= 1, "PLW3",
                  "Plane is declared in the .d.ts");
        }

        const std::string facade = ReadFile("Source/Runtime/Scripting/ScriptPlane/ScriptPlane.h");
        Check(!facade.empty(), "PLW4", "ScriptPlane.h is readable");
        if (facade.empty()) { return; }
        // ⚠️ Plane 的 `distance` 符号是"整个视锥内外反不反"的唯一来源，但只有
        // 源码门能看见"它还写着那行算术"。行为测试（PL0）看的是结果，这里看的是
        // 它还挂在 `Plane.h` 里而不是被复制到门面里。
        Check(CountOccurrences(facade, "GetDistanceToPoint(plane, point)") >= 1 ||
                  CountOccurrences(facade, "GetDistanceToPoint") >= 1, "PLW5",
              "the plane facade exposes GetDistanceToPoint");
        // 三个 Unity 静态成员：视锥那两个 + Raycast。
        Check(CountOccurrences(facade, "static std::vector<::Plane> CalculateFrustumPlanes(") >= 1,
              "PLW6", "GeometryUtility.CalculateFrustumPlanes takes a Camera (Unity's shape)");
        Check(CountOccurrences(facade, "static bool TestPlanesAABB(") >= 1, "PLW7",
              "GeometryUtility.TestPlanesAABB exists");
    }

    // =========================================================================
    // SmoothDamp（Mathf / Vector3）
    // =========================================================================
    //
    // 在此之前 `SmoothDamp` 在整个 `engine/` 里**零命中**。它缺席的原因是"TS
    // 没有 out/ref 参数"（`ref currentVelocity` 传不出去），而绕过这个限制的正
    // 确做法不是删掉这个 API，是"新值 + 新速度一起返回"。
    //
    // ⚠️ 这个算法的错**全都是静默的** —— 输出仍然是平滑的、仍然收敛，只是曲线
    // 不是 Unity 的那条。所以判据不是"能不能用"，是"数字对不对"：
    //   * `exp` 必须是 Unity 的多项式近似，不是 `std::exp`（SD0/SD1 用黄金数）；
    //   * 过冲守卫（SD7/SD8）：少了它会在速度大时冲过目标，而它仍然收敛；
    //   * `maxSpeed` 夹的是 `change`，上限 `maxSpeed * smoothTime`（不是
    //     `* deltaTime`）—— 写错了限速会随帧率变（SD5/SD6）；
    //   * `SmoothDampAngle` 必须先走最短弧（SD11），否则 350° → 10° 绕一大圈。
    void TestSmoothDamp()
    {
        using ::SmoothDampResult;
        using ::Vector3SmoothDampResult;

        const float dt = 1.f / 60.f;

        // --- 黄金数：与 Unity 的实现逐位对齐 ---------------------------------
        // current=0, target=1, vel=0, smoothTime=0.5, dt=1/60
        //   omega = 4, x = 1/15, exp = 1/(1 + x + 0.48x² + 0.235x³) = 0.935568
        //   temp = -1/15, velocity = 0.249485, output = 0.002061
        const SmoothDampResult first = Mathf::SmoothDamp(0.f, 1.f, 0.f, 0.5f, 0.f, dt);
        Check(Near(first.value, 0.002061f, 1e-6f), "SD0",
              "SmoothDamp's first step matches Unity's polynomial (0.002061, not exp(-x))");
        Check(Near(first.velocity, 0.249485f, 1e-6f), "SD1",
              "and so does the velocity it hands back (0.249485)");
        // ⚠️ 互补的一半：把多项式换成 `std::exp(-x)` 的话 exp = 0.935507 而不是
        // 0.935568。差 6e-5 看起来微不足道，但在 value 上就是 0.002126 而不是
        // 0.002061（3%），在 velocity 上是 0.249469 而不是 0.249485（1.6e-5）。
        // 两条曲线**都平滑**、**都收敛** —— 只有 1e-6 的黄金数能看见。这是
        // 为什么容差取 1e-6 而不是 1e-4：1e-4 会让 std::exp 静默通过。

        // --- 速度是跨帧状态：必须接住 -----------------------------------------
        // 第二帧接着用第一帧的速度 ⇒ 比"每帧从静止重来"走得远得多。
        const SmoothDampResult second =
            Mathf::SmoothDamp(first.value, 1.f, first.velocity, 0.5f, 0.f, dt);
        const SmoothDampResult from_rest =
            Mathf::SmoothDamp(first.value, 1.f, 0.f, 0.5f, 0.f, dt);
        Check(second.value > from_rest.value, "SD2",
              "carrying the velocity over goes further than restarting from rest");
        Check(Near(second.velocity, 0.466820f, 1e-6f), "SD3",
              "the second step's velocity is the carried one (0.466820)");

        // --- 收敛 -------------------------------------------------------------
        float value = 0.f;
        float velocity = 0.f;
        for (int i = 0; i < 300; ++i)
        {
            const SmoothDampResult r = Mathf::SmoothDamp(value, 1.f, velocity, 0.5f, 0.f, dt);
            value = r.value;
            velocity = r.velocity;
        }
        Check(Near(value, 1.f, 1e-4f), "SD4",
              "after 5 seconds it is on the target (smoothTime 0.5 -> 300 frames at 60Hz)");
        Check(std::isfinite(velocity), "SD4b", "and the velocity is finite there (no drift to NaN)");

        // --- maxSpeed ----------------------------------------------------------
        // max_change = maxSpeed * smoothTime = 0.5（**不是** maxSpeed * dt）。
        const SmoothDampResult limited = Mathf::SmoothDamp(0.f, 1000.f, 0.f, 0.5f, 1.f, dt);
        const SmoothDampResult unlimited = Mathf::SmoothDamp(0.f, 1000.f, 0.f, 0.5f, 0.f, dt);
        Check(Near(limited.value, 0.001031f, 1e-6f), "SD5",
              "maxSpeed clamps the step to 0.5 (maxSpeed * smoothTime), giving 0.001031");
        // ⚠️ 互补的一半：不限速时同一帧走到 2.061 —— 差三个数量级。只断言
        // "limited < unlimited" 的话，任何限速都过；这里两边都是黄金数。
        Check(Near(unlimited.value, 2.061f, 1e-3f), "SD6",
              "and without it the same frame reaches 2.061 (three orders of magnitude apart)");
        // ⚠️ 写成 `maxSpeed * deltaTime` 的话上限是 1/60 而不是 0.5，value 会是
        // 3.4e-5 —— 而且**随帧率变**，这是最坏的一种错：30Hz 与 60Hz 的限速不同，
        // 而每一帧看都"确实在限速"。
        Check(!Near(limited.value, Mathf::SmoothDamp(0.f, 1000.f, 0.f, 0.5f, 1.f, 1.f / 30.f).value,
                    1e-6f),
              "SD6b", "the clamp does not track the frame rate (dt is not in the limit)");

        // --- 过冲守卫 -----------------------------------------------------------
        // 初速很大、朝着目标冲：不钉住的话这一步会冲到 1.56。
        const SmoothDampResult overshoot = Mathf::SmoothDamp(0.f, 1.f, 100.f, 0.5f, 0.f, dt);
        Check(overshoot.value == 1.f, "SD7",
              "a big velocity towards the target is pinned to the target (no overshoot)");
        Check(overshoot.velocity == 0.f, "SD8",
              "and the velocity is zeroed there (Unity divides (output - target) by dt)");
        // 反向（初速背离目标）不该被钉住。
        const SmoothDampResult away = Mathf::SmoothDamp(0.f, 1.f, -100.f, 0.5f, 0.f, dt);
        Check(away.value < 0.f, "SD9",
              "but a velocity AWAY from the target is not pinned (it still moves backwards first)");

        // --- 退化输入 -----------------------------------------------------------
        // dt <= 0 就是"用引擎当前帧的 delta"（哨兵，因为 TS 没有可选参数）。
        // 引擎自己暂停（delta 为 0）时，不动、速度原样带过去 —— Unity 在这个
        // 帧上会得到 NaN，而 NaN 会静默污染后面的每一帧。
        TimeState::Get().BeginFrame(0.f, 0.f, true);
        Check(Time::GetDeltaTime() == 0.f, "SD10a",
              "the engine really reports a 0 delta for the paused-frame case");
        const SmoothDampResult no_dt = Mathf::SmoothDamp(3.f, 10.f, 7.f, 0.5f, 0.f, 0.f);
        Check(no_dt.value == 3.f && no_dt.velocity == 7.f, "SD10",
              "a paused frame holds position and keeps the velocity (NaN would poison every "
              "later frame)");
        // ⚠️ current == target 是"过冲守卫"唯一会在 dt==0 上触发的角落：它算出
        // `(output - target) / 0`，也就是 NaN。少了 Step 里那个 dt<=0 的早退，
        // 速度会变成 NaN —— 而**值**仍然是对的（5），所以只断言 value 的测试
        // 看不见。变异 SDM06 就是这么漏的。
        const SmoothDampResult at_target = Mathf::SmoothDamp(5.f, 5.f, 0.f, 0.5f, 0.f, 0.f);
        Check(at_target.value == 5.f && at_target.velocity == 0.f, "SD10b",
              "sitting on the target during a paused frame stays there with a ZERO velocity "
              "(the overshoot guard would divide by dt and give NaN)");

        // ⚠️ dt <= 0 这个哨兵的**另一半**：它必须真的取到引擎的 delta。只测
        // "引擎 delta 也是 0 时不动"证明不了这一点（两种实现都给出"不动"）。
        TimeState::Get().BeginFrame(dt, dt, true);
        const float engine_dt = Time::GetDeltaTime();
        Check(engine_dt > 0.f, "SD10c", "the engine reports a non-zero delta for the live frame");
        const SmoothDampResult sentinel = Mathf::SmoothDamp(0.f, 1.f, 0.f, 0.5f, 0.f, 0.f);
        const SmoothDampResult explicit_dt =
            Mathf::SmoothDamp(0.f, 1.f, 0.f, 0.5f, 0.f, engine_dt);
        Check(Near(sentinel.value, explicit_dt.value, 1e-9f), "SD10d",
              "deltaTime <= 0 really falls back to the engine's delta (not to a zero-length step)");
        // Vector3 版有自己的一份哨兵（Mathf 那份救不了它）。
        const Vector3SmoothDampResult v3_sentinel = Runtime::Vector3::SmoothDamp(
            Vector3::ZERO, Vector3(1.f, 1.f, 1.f), Vector3::ZERO, 0.5f, 0.f, 0.f);
        Check(Near(v3_sentinel.value.x, explicit_dt.value, 1e-9f), "SD10e",
              "and Vector3.SmoothDamp has its own copy of that sentinel");
        // smoothTime <= 0：Unity 内部取 Max(0.0001, smoothTime) ⇒ 几乎瞬时到达。
        const SmoothDampResult no_time = Mathf::SmoothDamp(0.f, 1.f, 0.f, 0.f, 0.f, dt);
        Check(Near(no_time.value, 1.f, 1e-3f), "SD11",
              "smoothTime <= 0 is clamped to 0.0001 (no division by zero, and it snaps at once)");

        // --- SmoothDampAngle：必须先走最短弧 -------------------------------------
        // 350° → 10°：最短弧是 **+20**（跨过 0），不是 -340。
        const SmoothDampResult angle = Mathf::SmoothDampAngle(350.f, 10.f, 0.f, 0.5f, 0.f, dt);
        // ⚠️ 判据是"值往**上**走"（350 → 350.04），而不是往下（349.3）。少了
        // DeltaAngle 那一步实现照样平滑、照样收敛到 10°，只是绕了一大圈。
        Check(angle.value > 350.f, "SD12",
              "350 -> 10 takes the SHORT arc: the value increases past 360, it does not go back");
        Check(Near(angle.value, 350.041f, 1e-3f), "SD13", "and it lands on 350.041 (Unity's number)");
        // 不跨 0 的情形：与标量版完全一致（那一步 DeltaAngle 是恒等）。
        Check(Near(Mathf::SmoothDampAngle(10.f, 50.f, 0.f, 0.5f, 0.f, dt).value,
                   Mathf::SmoothDamp(10.f, 50.f, 0.f, 0.5f, 0.f, dt).value, 1e-6f),
              "SD14", "and when the arc does not wrap it is identical to the scalar version");

        // --- Vector3.SmoothDamp：分量各自跑标量版 ---------------------------------
        const Vector3SmoothDampResult v3 = Runtime::Vector3::SmoothDamp(
            Vector3(0.f, 0.f, 0.f), Vector3(100.f, 100.f, 0.f), Vector3::ZERO, 0.5f, 1.f, dt);
        const SmoothDampResult scalar = Mathf::SmoothDamp(0.f, 100.f, 0.f, 0.5f, 1.f, dt);
        // ⚠️ 这一条同时是"分量式"与"两份实现同源"的判据：整向量限速的实现（把
        // delta 归一化再夹长度）给不出 100 和 100 各自等于标量版的数。
        Check(Near(v3.value.x, scalar.value, 1e-6f) && Near(v3.value.y, scalar.value, 1e-6f),
              "SD15", "Vector3.SmoothDamp runs the SCALAR version per component (Unity does too)");
        Check(Near(v3.velocity.x, scalar.velocity, 1e-6f) &&
                  Near(v3.velocity.y, scalar.velocity, 1e-6f), "SD16",
              "and its velocity is per component as well");
        // 已经在目标上的那个分量不动。
        Check(v3.value.z == 0.f && v3.velocity.z == 0.f, "SD17",
              "a component already on its target stays there");
        // ⚠️ 分量式限速的后果（Unity 就是这样）：对角方向的**合成**速度可以超过
        // maxSpeed。照 Unity 做，不"修"。
        Check(Near(Vector2(v3.value.x, v3.value.y).length(),
                   scalar.value * std::sqrt(2.f), 1e-6f),
              "SD18", "so a diagonal move covers sqrt(2) times one component (Unity's behaviour)");
        // 三个分量各不相同 ⇒ 各自收敛。
        Vector3 v_value(0.f, 10.f, -5.f);
        Vector3 v_vel = Vector3::ZERO;
        for (int i = 0; i < 300; ++i)
        {
            const Vector3SmoothDampResult r = Runtime::Vector3::SmoothDamp(
                v_value, Vector3(3.f, -2.f, 4.f), v_vel, 0.5f, 0.f, dt);
            v_value = r.value;
            v_vel = r.velocity;
        }
        Check(Near(v_value.x, 3.f, 1e-4f) && Near(v_value.y, -2.f, 1e-4f) &&
                  Near(v_value.z, 4.f, 1e-4f), "SD19",
              "all three components converge independently");
    }

    void TestSmoothDampWiring()
    {
        const std::string math_header =
            ReadFile("Source/Runtime/Scripting/ScriptMath/ScriptMath.h");
        Check(!math_header.empty(), "SDW0", "ScriptMath.h is readable");
        if (math_header.empty()) { return; }
        // ⚠️ Mathf 与 Vector3 必须**共用**一份实现：两份迟早漂移，而漂移后的两条
        // 曲线都仍然平滑 —— 只有源码门能看见它们是不是同一个函数。
        Check(CountOccurrences(math_header, "SmoothDampDetail::Step") >= 1, "SDW1",
              "Mathf.SmoothDamp goes through the shared implementation");

        const std::string vector_source =
            ReadFile("Source/Runtime/Scripting/ScriptVector/ScriptVector.cpp");
        Check(!vector_source.empty(), "SDW2", "ScriptVector.cpp is readable");
        if (!vector_source.empty())
        {
            Check(CountOccurrences(vector_source, "SmoothDampDetail::Step") >= 3, "SDW3",
                  "Vector3.SmoothDamp calls the same shared implementation (once per component)");
        }

        const std::string header = ReadFile("Source/Runtime/Core/Math/SmoothDamp.h");
        Check(!header.empty(), "SDW4", "SmoothDamp.h is readable");
        if (!header.empty())
        {
            // ⚠️ `exp` 必须是 Unity 的多项式，不是 std::exp。只 grep 名字看不出。
            Check(CountOccurrences(header, "0.48f * x * x + 0.235f * x * x * x") >= 1, "SDW5",
                  "the damping term is Unity's polynomial, not std::exp(-x)");
            // 过冲守卫：少了它仍然收敛，看不出来。
            Check(CountOccurrences(header, "original_to - current > 0.f") >= 1, "SDW6",
                  "the overshoot guard is still there");
            // maxSpeed 的上限是 maxSpeed * smoothTime，不是 * deltaTime。
            Check(CountOccurrences(header, "max_speed * smooth_time") >= 1, "SDW7",
                  "the speed limit is maxSpeed * smoothTime (it must not track the frame rate)");
        }

        const std::string binding_source =
            ReadFile("Source/Runtime/Scripting/Binding/ScriptBinding.cpp");
        Check(!binding_source.empty(), "SDW8", "ScriptBinding.cpp is readable");
        if (!binding_source.empty())
        {
            Check(CountOccurrences(binding_source, "declare interface SmoothDampResult {") >= 1 &&
                      CountOccurrences(binding_source,
                                       "declare interface Vector3SmoothDampResult {") >= 1,
                  "SDW9", "both result structs are declared in the .d.ts");
        }
    }

    void TestMouseEvents()
    {
        ObjectManager* om = GET_SYSTEM(ObjectManager);
        Check(om != nullptr, "ME0", "ObjectManager is reachable");
        if (om == nullptr) { return; }

        Level level;
        level.setLevelResUrl("Assets/Scenes/MouseEvents.level");
        level.LoadObjectGraph({}, false);
        Check(level.IsLoaded(), "ME1", "the synthetic level is loaded");

        std::string log;

        struct Node
        {
            GameObject* object {nullptr};
            Transform* transform {nullptr};
        };
        auto make_node = [&](Node& out) -> bool
        {
            out.object = new GameObject(ObjectIDAllocator::Alloc());
            om->AllocateAndAssignInstanceID(out.object);
            out.transform = new Transform();
            om->AllocateAndAssignInstanceID(out.transform);
            out.object->addComponent(out.transform);
            out.transform->PostLoadResource(out.object);
            return level.RegisterRuntimeGObject(out.object);
        };
        auto add_probe = [&](Node& node, char tag) -> AppEventProbe*
        {
            AppEventProbe* probe = new AppEventProbe(&log, tag);
            probe->InitializeRuntimeTypeInfo();
            om->AllocateAndAssignInstanceID(probe);
            node.object->addComponent(probe);
            return probe;
        };

        Node a, b;
        Check(make_node(a), "ME2", "node A is in the level's runtime table");
        Check(make_node(b), "ME3", "node B too");
        // A 上挂**两个**探针：只发给第一个组件的实现照样过单探针的夹具。
        AppEventProbe* pa1 = add_probe(a, 'A');
        AppEventProbe* pa2 = add_probe(a, 'a');
        AppEventProbe* pb = add_probe(b, 'B');

        MouseEventDispatcher::Reset();

        // --- 悬停：Enter 一次，Over 每帧 ----------------------------------
        MouseFrameState idle;
        Check(MouseEventDispatcher::DispatchTo(idle, a.object) == a.object, "ME4",
              "DispatchTo returns the object under the mouse");
        Check(pa1->mouse_enter == 1 && pa1->mouse_over == 1, "ME5",
              "the first frame gives Enter + Over");
        Check(pa2->mouse_enter == 1 && pa2->mouse_over == 1, "ME6",
              "every component on it gets it, not just the first");
        Check(pb->mouse_enter == 0 && pb->mouse_over == 0, "ME7", "the other object gets nothing");

        pa1->Reset(); pa2->Reset(); pb->Reset();
        log.clear();
        MouseEventDispatcher::DispatchTo(idle, a.object);
        Check(pa1->mouse_enter == 0 && pa1->mouse_over == 1, "ME8",
              "a second frame on the same object is Over only (Enter is an edge)");

        // --- 移到另一个对象：旧的收 Exit，新的收 Enter ----------------------
        pa1->Reset(); pa2->Reset(); pb->Reset();
        log.clear();
        MouseEventDispatcher::DispatchTo(idle, b.object);
        Check(pa1->mouse_exit == 1, "ME9", "the OLD object gets OnMouseExit");
        Check(pb->mouse_enter == 1 && pb->mouse_over == 1, "ME10", "the new one gets Enter + Over");
        // 顺序探针：Exit 必须**先于** Enter。倒过来的话脚本里"先清旧的高亮再画
        // 新的"这种写法会闪一下。
        Check(log.find("AX") != std::string::npos && log.find("BE") != std::string::npos &&
                  log.find("AX") < log.find("BE"),
              "ME11", "Exit on the old one comes before Enter on the new one");

        // --- 移开：Exit 一次 ------------------------------------------------
        pa1->Reset(); pa2->Reset(); pb->Reset();
        MouseEventDispatcher::DispatchTo(idle, nullptr);
        Check(pb->mouse_exit == 1, "ME12", "leaving to nothing still fires Exit");
        Check(MouseEventDispatcher::GetHovered() == nullptr, "ME13", "and nothing is hovered");

        // --- 按下：Down 是边沿，Drag 每帧 ----------------------------------
        pa1->Reset(); pa2->Reset(); pb->Reset();
        log.clear();
        MouseFrameState press;
        press.pressed = true;
        press.held = true;
        MouseEventDispatcher::DispatchTo(press, a.object);
        Check(pa1->mouse_enter == 1 && pa1->mouse_down == 1, "ME14",
              "the pressed frame gives Enter (edge) + Over + Down");
        // ⚠️ 按下那一帧**也**发 Drag：Unity 对 OnMouseDrag 的说法是 "every frame
        // while the mouse is down"，而按下那一帧鼠标确实是按下的。为此加一个
        // `!state.pressed` 的特例等于自造一条文档里没有的规则。
        Check(pa1->mouse_drag == 1, "ME15", "and Drag too (Unity: every frame while down)");
        Check(MouseEventDispatcher::GetPressed() == a.object, "ME16", "the object is remembered");
        // ⚠️ 串是**按消息**分组的（Enter 走完两个组件，才轮到 Over），不是按组件
        // 分组（一个组件收完七条再换下一个）。这个顺序是 `BroadcastMouseEvent` 的
        // 结构决定的，写死在断言里正是为了让它不可随意改变 —— 脚本里"先清旧的再
        // 画新的"这类写法依赖它。
        Check(log == "AEaEAOaOADaDAGaG", "ME17",
              "one pass: Enter, Over, Down, Drag -- grouped by message, in that order");

        pa1->Reset(); pa2->Reset(); pb->Reset();
        MouseFrameState hold;
        hold.held = true;
        MouseEventDispatcher::DispatchTo(hold, a.object);
        Check(pa1->mouse_down == 0 && pa1->mouse_drag == 1 && pa1->mouse_over == 1, "ME18",
              "holding: Drag every frame, Down only once");

        // --- ⚠️ Drag 发给**按下时**那个对象，不是当前悬停的那个 --------------
        pa1->Reset(); pa2->Reset(); pb->Reset();
        MouseEventDispatcher::DispatchTo(hold, b.object);
        Check(pa1->mouse_drag == 1, "ME19",
              "Drag still goes to the PRESSED object after the mouse moved away");
        Check(pb->mouse_over == 1 && pb->mouse_drag == 0, "ME20",
              "the newly hovered one only gets Over");
        Check(pa1->mouse_over == 0, "ME21", "the pressed one no longer gets Over either");

        // --- 松手在别处：Up 有，UpAsButton 没有 ------------------------------
        pa1->Reset(); pa2->Reset(); pb->Reset();
        MouseFrameState release;
        release.released = true;
        MouseEventDispatcher::DispatchTo(release, b.object);
        Check(pa1->mouse_up == 1, "ME22", "Up goes to the pressed object");
        Check(pa1->mouse_up_as_button == 0, "ME23",
              "but NOT UpAsButton (the mouse is no longer over it)");
        Check(MouseEventDispatcher::GetPressed() == nullptr, "ME24", "the press is forgotten");

        // --- 互补的一半：在同一个对象上松手，UpAsButton 要来 -----------------
        MouseEventDispatcher::Reset();
        pa1->Reset(); pa2->Reset(); pb->Reset();
        MouseEventDispatcher::DispatchTo(press, a.object);
        pa1->Reset(); pa2->Reset(); pb->Reset();
        log.clear();
        MouseEventDispatcher::DispatchTo(release, a.object);
        Check(pa1->mouse_up == 1 && pa1->mouse_up_as_button == 1, "ME25",
              "releasing over the SAME object gives Up and then UpAsButton");
        Check(log.find("AU") != std::string::npos && log.find("AB") != std::string::npos &&
                  log.find("AU") < log.find("AB"),
              "ME26", "Up comes before UpAsButton");

        // --- ⚠️ 与 gizmo 同一条规则：不判 enabled ---------------------------
        MouseEventDispatcher::Reset();
        pa1->SetProbeEnabled(false);
        pa1->Reset(); pa2->Reset(); pb->Reset();
        MouseEventDispatcher::DispatchTo(idle, a.object);
        Check(pa1->mouse_enter == 1, "ME27",
              "a DISABLED component still gets it (Unity's enabled only blocks "
              "Start/Awake/Update/FixedUpdate/OnGUI)");
        // 互补的一半：同一个探针在同一状态下 Tick 必须被跳过。
        const bool had_whitelisted =
            g_editorTickComponentTypes.find("Component") != g_editorTickComponentTypes.end();
        if (!had_whitelisted) { g_editorTickComponentTypes.insert("Component"); }
        pa1->ticks = 0;
        level.Tick(0.016f);
        Check(pa1->ticks == 0, "ME28", "…while its Tick really is skipped");
        if (!had_whitelisted) { g_editorTickComponentTypes.erase("Component"); }
        pa1->SetProbeEnabled(true);
        pa1->Reset(); pa2->Reset(); pb->Reset();

        // --- 判 activeInHierarchy：停用的对象一个都不收 ----------------------
        MouseEventDispatcher::Reset();
        a.object->SetActive(false);
        Check(!a.object->IsActiveInHierarchy(), "ME29", "A is really inactive");
        MouseEventDispatcher::DispatchTo(press, a.object);
        Check(pa1->mouse_enter == 0 && pa1->mouse_down == 0 && pa1->mouse_over == 0, "ME30",
              "an inactive object gets no mouse events at all");
        // ⚠️ 派发器**不**判激活：判据活在下层的 `BroadcastMouseEvent` 里。真实
        // 路径下这个状态不可达（停用对象的碰撞体不在物理世界里，`PickAt` 打不到
        // 它），所以这里记的是"两层各管一件事"这个划分，而不是一条行为规则。
        Check(MouseEventDispatcher::GetHovered() == a.object, "ME31",
              "the dispatcher still records it (the active gate lives in the broadcast)");
        a.object->SetActive(true);

        // --- ⚠️ 销毁时的 Forget：这是本切片唯一一处真的会崩的地方 ------------
        MouseEventDispatcher::Reset();
        MouseEventDispatcher::DispatchTo(idle, a.object);
        Check(MouseEventDispatcher::GetHovered() == a.object, "ME32", "A is hovered");
        {
            // 脚本里"点一下就消失"是最常见的写法：销毁时鼠标正停在上面。不摘掉
            // 的话下一次派发的 OnMouseExit 就是一次 use-after-free。
            Node doomed;
            Check(make_node(doomed), "ME33", "a doomed node is registered");
            MouseEventDispatcher::DispatchTo(idle, doomed.object);
            Check(MouseEventDispatcher::GetHovered() == doomed.object, "ME34",
                  "and it is the one hovered now");
            level.UnregisterRuntimeGObject(doomed.object->GetID());
            delete doomed.object;
            Check(MouseEventDispatcher::GetHovered() == nullptr, "ME35",
                  "destroying the hovered object clears the hover (no use-after-free)");
        }
        // 按下状态同样要摘。
        MouseEventDispatcher::Reset();
        {
            Node doomed;
            Check(make_node(doomed), "ME36", "another doomed node is registered");
            MouseEventDispatcher::DispatchTo(press, doomed.object);
            Check(MouseEventDispatcher::GetPressed() == doomed.object, "ME37", "it is pressed");
            level.UnregisterRuntimeGObject(doomed.object->GetID());
            delete doomed.object;
            Check(MouseEventDispatcher::GetPressed() == nullptr, "ME38",
                  "destroying the pressed object clears the press too");
        }

        // --- PickAt 的降级：没有相机就没有"鼠标下面" ------------------------
        MouseEventDispatcher::Reset();
        Check(GET_SYSTEM(RenderSystem) == nullptr, "ME39", "(the fixture has no RenderSystem)");
        Check(MouseEventDispatcher::PickAt(Vector2(10.0f, 10.0f)) == nullptr, "ME40",
              "no camera -> PickAt is null, not a crash");
        Check(MouseEventDispatcher::Tick(idle) == nullptr, "ME41",
              "and a whole tick with no camera dispatches nothing");

        // --- ⚠️ Reset 必须**两个**都清 --------------------------------------
        // 只清 hovered 不清 pressed 的话，切关卡之后第一次 Drag / Up 打到的还是
        // 上一张关卡里已经销毁的对象 —— 而这个状态在真实路径上要"恰好切关卡时
        // 正按着鼠标"才出现，所以只能在这里显式钉住。
        MouseEventDispatcher::DispatchTo(press, a.object);
        Check(MouseEventDispatcher::GetHovered() == a.object &&
                  MouseEventDispatcher::GetPressed() == a.object, "ME42",
              "A is both hovered and pressed");
        MouseEventDispatcher::Reset();
        Check(MouseEventDispatcher::GetHovered() == nullptr, "ME43", "Reset clears the hover");
        Check(MouseEventDispatcher::GetPressed() == nullptr, "ME44",
              "and the press (forgetting this is a use-after-free on the next Drag)");

        MouseEventDispatcher::Reset();
        level.UnregisterRuntimeGObject(a.object->GetID());
        level.UnregisterRuntimeGObject(b.object->GetID());
        delete pa1;
        delete pa2;
        delete pb;
        delete a.object;
        delete b.object;
    }

    void TestMouseEventsWiring()
    {
        const std::string component_header =
            ReadFile("Source/Runtime/Function/Framework/Component/Component.h");
        Check(!component_header.empty(), "MW0", "Component.h is readable");
        // 七条都要在：少一条就是"那条消息永远收不到"。
        const char* kHooks[] = {"virtual void OnMouseEnter(", "virtual void OnMouseOver(",
                                "virtual void OnMouseExit(",  "virtual void OnMouseDown(",
                                "virtual void OnMouseDrag(",  "virtual void OnMouseUp(",
                                "virtual void OnMouseUpAsButton("};
        for (int i = 0; i < 7; ++i)
        {
            Check(CountOccurrences(component_header, kHooks[i]) >= 1,
                  ("MW1_" + std::to_string(i)).c_str(), kHooks[i]);
        }
        Check(CountOccurrences(component_header, "enum class MouseEventKind") >= 1, "MW2",
              "the seven are one enum (one walk, not seven)");
        // ⚠️ 子串陷阱：`OnMouseUp(` **包含** `OnMouseUp...`? 不 —— 上面七条都带
        // 左括号，所以 `OnMouseUp(` 不会命中 `OnMouseUpAsButton(`。反过来
        // `OnMouseUpAsButton(` 也不会被 `OnMouseUp(` 满足。两条都数，是互相的门。
        Check(CountOccurrences(component_header, "virtual void OnMouseUp(") >= 1, "MW3",
              "OnMouseUp is declared (and is not just a prefix of UpAsButton)");

        const std::string go_source = ReadFile("Source/Runtime/BaseClasses/GameObject.cpp");
        Check(!go_source.empty(), "MW4", "GameObject.cpp is readable");
        const std::size_t at_broadcast = go_source.find("void GameObject::BroadcastMouseEvent");
        Check(at_broadcast != std::string::npos, "MW5", "BroadcastMouseEvent is implemented");
        if (at_broadcast != std::string::npos)
        {
            Check(BodyIsBounded(go_source, at_broadcast), "MW5b",
                  "the BroadcastMouseEvent body slice is bounded (the scan is not degenerate)");
            const std::string body = SliceFunctionBody(go_source, at_broadcast);
            Check(CountOccurrences(body, "IsActiveInHierarchy") >= 1, "MW6",
                  "it gates on activeInHierarchy");
            // ⚠️ 反向门，与 DW6 同款：函数体里**不许**出现 enabled 判据，注释里也
            // 不许出现那个词（否则门恒真）。
            Check(body.find("isEnabled") == std::string::npos, "MW7",
                  "it does NOT gate on isEnabled (Unity: disabled scripts still get OnMouse*)");
            Check(body.find("shouldComponentTick") == std::string::npos, "MW8",
                  "nor on the edit-mode whitelist");
            Check(CountOccurrences(body, "MouseEventKind::k") >= 7, "MW9",
                  "all seven kinds are dispatched");
            // 七个 case 长得一模一样，加错 case 调错函数是最容易犯的错。
            Check(CountOccurrences(body, "->OnMouse") >= 7, "MW10",
                  "and each calls its own hook (not a copy-pasted neighbour)");
        }

        // 派发器。
        const std::string dispatcher_source =
            ReadFile("Source/Runtime/Function/Framework/Component/MouseEventDispatcher.cpp");
        Check(!dispatcher_source.empty(), "MW11", "MouseEventDispatcher.cpp is readable");
        {
            const std::size_t at = dispatcher_source.find("MouseEventDispatcher::DispatchTo");
            Check(at != std::string::npos, "MW12", "the dispatch half is a step of its own");
            if (at != std::string::npos)
            {
                Check(BodyIsBounded(dispatcher_source, at), "MW12b",
                      "the DispatchTo body slice is bounded");
                const std::string body = SliceFunctionBody(dispatcher_source, at);
                // 拖拽 / 松手发给**按下时**那个对象，不是当前悬停的那个。
                Check(CountOccurrences(body, "s_Pressed->BroadcastMouseEvent") >= 3, "MW13",
                      "Drag / Up / UpAsButton go to the PRESSED object");
                Check(CountOccurrences(body, "s_Hovered->BroadcastMouseEvent") >= 3, "MW14",
                      "Enter / Over / Down go to the hovered one");
                Check(CountOccurrences(body, "s_Pressed == s_Hovered") >= 1, "MW15",
                      "UpAsButton only fires when the release is on the same object");
            }
            Check(CountOccurrences(dispatcher_source, "MouseEventDispatcher::Forget") >= 1,
                  "MW16", "the dispatcher can forget a destroyed object");
        }
        // ⚠️ Forget 必须**真的被调**：写在头文件里没人调的话，"销毁时清状态"就是
        // 一句注释。这条是本切片唯一一处"不修就真的崩"的接线。
        Check(CountOccurrences(go_source, "MouseEventDispatcher::Forget(this)") >= 1, "MW17",
              "GameObject's destructor forgets itself (the hover pointer is raw)");

        // 主循环：实现了但没接进主循环 = 没实现。
        const std::string app_source =
            ReadFile("Source/Runtime/Application/Application.cpp");
        Check(!app_source.empty(), "MW18", "Application.cpp is readable");
        {
            const std::size_t at_tick = app_source.find("void Application::LogicalTick");
            Check(at_tick != std::string::npos, "MW19", "LogicalTick is implemented");
            if (at_tick != std::string::npos)
            {
                Check(BodyIsBounded(app_source, at_tick), "MW19b",
                      "the LogicalTick body slice is bounded");
                const std::string body = SliceFunctionBody(app_source, at_tick);
                Check(CountOccurrences(body, "MouseEventDispatcher::Tick(") >= 1, "MW20",
                      "the logical tick drives the mouse frame");
                // 三个 bool 是独立的：pressed / held / released 各来一处。少一个
                // 就是"Drag 永不触发"或"Down 每帧都触发"。
                Check(CountOccurrences(body, "mouse_state.pressed") >= 1, "MW21",
                      "the pressed edge is filled in");
                Check(CountOccurrences(body, "mouse_state.held") >= 1, "MW22",
                      "and the held state");
                Check(CountOccurrences(body, "mouse_state.released") >= 1, "MW23",
                      "and the released edge");
                // 相位：鼠标帧在**输入采样之后、世界 tick 之前**。挪到输入之前
                // 的话拾取用的是上一帧的输入（首帧恒为空）；挪到世界 tick 之后
                // 的话脚本的 OnUpdate 要等到下一帧才看得到这一下点击。
                const std::size_t at_input = body.find("GET_SYSTEM(InputSystem)->Tick();");
                const std::size_t at_mouse = body.find("MouseEventDispatcher::Tick(");
                const std::size_t at_world = body.find("GET_SYSTEM(WorldManager)->Tick(");
                Check(at_input != std::string::npos && at_mouse != std::string::npos &&
                          at_world != std::string::npos, "MW32",
                      "the three anchors are all in the sliced body");
                Check(at_input < at_mouse && at_mouse < at_world, "MW33",
                      "input -> mouse -> world, in that order");
            }
        }

        // 脚本面。
        const std::string ts_source =
            ReadFile("Source/Runtime/Function/Framework/Component/Script/TypeScriptComponent.cpp");
        Check(!ts_source.empty(), "MW24", "TypeScriptComponent.cpp is readable");
        const char* kNames[] = {"\"OnMouseEnter\"", "\"OnMouseOver\"", "\"OnMouseExit\"",
                                "\"OnMouseDown\"", "\"OnMouseDrag\"", "\"OnMouseUp\""};
        for (int i = 0; i < 6; ++i)
        {
            Check(CountOccurrences(ts_source, kNames[i]) >= 1,
                  ("MW25_" + std::to_string(i)).c_str(), kNames[i]);
        }
        // ⚠️ `"OnMouseUp"` 是 `"OnMouseUpAsButton"` 的**前缀**：只数裸串的话删掉
        // UpAsButton 那条照样绿。带上结束引号才分得开。
        Check(CountOccurrences(ts_source, "InvokeMethod(\"OnMouseUp\")") >= 1, "MW26",
              "Up is forwarded (and is not just a prefix of UpAsButton)");
        Check(CountOccurrences(ts_source, "InvokeMethod(\"OnMouseUpAsButton\")") >= 1, "MW27",
              "and UpAsButton is forwarded too");

        const std::string env_source = ReadFile("Source/Runtime/Scripting/Wrapper/ScriptEnv.cpp");
        // 同上：带 " = function" 才不会被更长的那个名字满足。
        Check(CountOccurrences(env_source, "g.Behaviour.prototype.OnMouseUp = function") >= 1,
              "MW28", "the JS prototype has an OnMouseUp default");
        Check(CountOccurrences(env_source, "g.Behaviour.prototype.OnMouseUpAsButton = function") >= 1,
              "MW29", "and an OnMouseUpAsButton default");

        const std::string binding_source =
            ReadFile("Source/Runtime/Scripting/Binding/ScriptBinding.cpp");
        Check(CountOccurrences(binding_source, "OnMouseDown(): void;") >= 1, "MW30",
              "the .d.ts declares OnMouseDown on Behaviour");
        Check(CountOccurrences(binding_source, "OnMouseUpAsButton(): void;") >= 1, "MW31",
              "and OnMouseUpAsButton");
    }

    // ---------------------------------------------------------------- PU ---
    // "引擎在调，脚本却看不见"这一类洞的**通用**门。
    //
    // 这一条是为一个真实的洞写的：`TypeScriptComponent::FixedTick` 每个固定步
    // 都在调 `OnFixedUpdate`，但 `ScriptBinding.cpp` 的 d.ts 行与 `ScriptEnv.cpp`
    // 的原型兜底两边都缺着。后果是 TS 里写 `OnFixedUpdate(...)` **过不了 tsc**，
    // 而任何 C++ 测试都发现不了 —— `InvokeInstanceMethodNumber` 对"方法不存在"
    // 是静默无事发生的。
    //
    // 所以这里的做法不是再补一条"OnFixedUpdate 在不在"，而是把
    // `TypeScriptComponent.cpp` 里**所有**被调用到脚本实例上的名字抽出来，逐条
    // 要求它同时出现在 d.ts 与 JS 原型上。以后再加任何钩子，忘了发两边都会红。
    void TestScriptMethodPublishing()
    {
        const std::string ts_source = ReadFile(
            "Source/Runtime/Function/Framework/Component/Script/TypeScriptComponent.cpp");
        const std::string binding_source =
            ReadFile("Source/Runtime/Scripting/Binding/ScriptBinding.cpp");
        const std::string env_source =
            ReadFile("Source/Runtime/Scripting/Wrapper/ScriptEnv.cpp");
        Check(!ts_source.empty(), "PU0", "TypeScriptComponent.cpp is readable");
        Check(!binding_source.empty(), "PU1", "ScriptBinding.cpp is readable");
        Check(!env_source.empty(), "PU2", "ScriptEnv.cpp is readable");
        if (ts_source.empty() || binding_source.empty() || env_source.empty()) { return; }

        // 抽 `InvokeMethod("X")` 与 `InvokeInstanceMethod*(ref, "X"` 里的名字。
        // 两个锚点都扫：`InvokeInstanceMethodNumber` 那一族名字在 `(` 之前，
        // 只认 `InvokeMethod("` 会整族漏掉。
        // ⚠️ 抽到的必须是**合法标识符**：注释里提到 `InvokeInstanceMethod` 时，
        // "下一个引号"可能是后面某条日志文案（"…: ScriptingManager not
        // initialised"）或某个 `#include "…"`。不过滤的话就会凭空多出两条
        // 永远满足不了的断言 —— 看起来像"有人忘了发布"，其实是扫描器自己
        // 捞错了东西。
        auto is_identifier = [](const std::string& s)
        {
            if (s.empty() || !(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_'))
            {
                return false;
            }
            for (const char c : s)
            {
                if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_'))
                {
                    return false;
                }
            }
            return true;
        };
        auto collect = [&is_identifier](const std::string& src, const char* anchor)
        {
            std::vector<std::string> names;
            std::size_t pos = 0;
            while ((pos = src.find(anchor, pos)) != std::string::npos)
            {
                const std::size_t open = src.find('"', pos);
                if (open == std::string::npos) { break; }
                const std::size_t close = src.find('"', open + 1);
                if (close == std::string::npos) { break; }
                const std::string name = src.substr(open + 1, close - open - 1);
                if (is_identifier(name) &&
                    std::find(names.begin(), names.end(), name) == names.end())
                {
                    names.push_back(name);
                }
                pos = close + 1;
            }
            return names;
        };
        std::vector<std::string> names = collect(ts_source, "InvokeMethod(\"");
        for (const auto& extra : collect(ts_source, "InvokeInstanceMethod"))
        {
            if (std::find(names.begin(), names.end(), extra) == names.end())
            {
                names.push_back(extra);
            }
        }

        // ⚠️ 扫描本身也要有门：抽不到名字时下面每一条都是"没有可判的"，全绿而
        // 什么都没验 —— 锚点被改坏 / 文件搬走都会是这个样子。
        Check(names.size() >= 18, "PU3",
              ("the scan found the hooks (" + std::to_string(names.size()) + ")").c_str());

        for (const auto& name : names)
        {
            // 四条缩进 + 左括号：既是"声明在 Behaviour 里"，也避开了
            // `OnMouseUp` 是 `OnMouseUpAsButton` 前缀那个坑。
            Check(CountOccurrences(binding_source, ("    " + name + "(").c_str()) >= 1,
                  ("PU4_" + name).c_str(), (name + " is declared in the .d.ts").c_str());
            Check(CountOccurrences(env_source,
                                   ("g.Behaviour.prototype." + name + " = function").c_str()) >= 1,
                  ("PU5_" + name).c_str(),
                  (name + " has a no-op default on the JS prototype").c_str());
        }
    }

    // ---------------------------------------------------------------- FU ---
    // Unity 的 `FixedUpdate` 那条链路的接线门（行为那一半在
    // ScriptComponentSmokeTest 的 T 段 —— 那里能真的跑到 JS）。
    void TestFixedUpdateWiring()
    {
        const std::string go_source = ReadFile("Source/Runtime/BaseClasses/GameObject.cpp");
        Check(!go_source.empty(), "FU0", "GameObject.cpp is readable");
        if (!go_source.empty())
        {
            // ⚠️ 两条循环必须**同构**：判定不一致就会出现"FixedUpdate 跑了但
            // Update 没跑"（或反之），组件状态在两套节奏之间静默错位。
            const std::size_t at_tick = go_source.find("void GameObject::Tick");
            const std::size_t at_fixed = go_source.find("void GameObject::FixedTick");
            Check(at_tick != std::string::npos && at_fixed != std::string::npos, "FU1",
                  "both Tick and FixedTick are implemented");
            if (at_tick != std::string::npos && at_fixed != std::string::npos)
            {
                Check(BodyIsBounded(go_source, at_tick) &&
                          BodyIsBounded(go_source, at_fixed), "FU2",
                      "both body slices are bounded");
                const std::string tick_body = SliceFunctionBody(go_source, at_tick);
                const std::string fixed_body = SliceFunctionBody(go_source, at_fixed);
                const char* kGates[] = {"IsActiveInHierarchy", "isEnabled",
                                        "shouldComponentTick"};
                for (int i = 0; i < 3; ++i)
                {
                    Check(CountOccurrences(tick_body, kGates[i]) >= 1,
                          ("FU3_" + std::to_string(i)).c_str(),
                          (std::string("Tick gates on ") + kGates[i]).c_str());
                    Check(CountOccurrences(fixed_body, kGates[i]) >= 1,
                          ("FU4_" + std::to_string(i)).c_str(),
                          (std::string("FixedTick gates on ") + kGates[i]).c_str());
                }
                Check(CountOccurrences(fixed_body, "FixedTick(fixed_delta_time)") >= 1, "FU5",
                      "FixedTick forwards the fixed step (not the frame delta)");
            }
        }

        const std::string ts_source = ReadFile(
            "Source/Runtime/Function/Framework/Component/Script/TypeScriptComponent.cpp");
        Check(!ts_source.empty(), "FU6", "TypeScriptComponent.cpp is readable");
        if (!ts_source.empty())
        {
            const std::size_t at = ts_source.find("void TypeScriptComponent::FixedTick");
            Check(at != std::string::npos, "FU7", "the script component has a FixedTick");
            if (at != std::string::npos)
            {
                Check(BodyIsBounded(ts_source, at), "FU8", "its body slice is bounded");
                const std::string body = SliceFunctionBody(ts_source, at);
                // ⚠️ 复制粘贴最容易错的一处：三个 Tick 长得一模一样，把 OnUpdate
                // 抄进 FixedTick 之后"物理推了但每帧一次"，且计数照样 +1。
                Check(CountOccurrences(body, "\"OnFixedUpdate\"") >= 1, "FU9",
                      "it forwards OnFixedUpdate");
                Check(body.find("\"OnUpdate\"") == std::string::npos, "FU10",
                      "and not OnUpdate (the copy-paste slip)");
            }
        }
    }

    // ---------------------------------------------------------------- DG ---
    // Unity 的 `OnDrawGizmos()` / `OnDrawGizmosSelected()`。
    //
    // 这一组真正值钱的是**判据**：只判 activeInHierarchy，**不判 enabled**。
    // 这与三趟 Tick（三个判据全上）和 `BroadcastAppEvent`（一个都不判）是
    // 第三种组合，而 Unity 是明文规定了它的（MonoBehaviour 参考页："The
    // checkbox for disabling a MonoBehavior will only prevent Start, Awake,
    // Update, FixedUpdate, and OnGUI from executing"）。
    //
    // ⚠️ 因此"停用的脚本照样画 gizmo"这条断言必须有互补的一半：同一个探针在
    // 同一状态下 **Tick 必须被跳过**。只断言前者的话，"enabled 这个开关压根
    // 没接上"的实现也过 —— 门就白设了。

    void TestDrawGizmos()
    {
        ObjectManager* om = GET_SYSTEM(ObjectManager);
        Check(om != nullptr, "DG0", "ObjectManager is reachable");
        if (om == nullptr) { return; }

        Level level;
        level.setLevelResUrl("Assets/Scenes/DrawGizmos.level");
        level.LoadObjectGraph({}, false);
        Check(level.IsLoaded(), "DG1", "the synthetic level is loaded");

        std::string log;

        struct Node
        {
            GameObject* object {nullptr};
            Transform* transform {nullptr};
        };
        auto make_node = [&](Node& out) -> bool
        {
            out.object = new GameObject(ObjectIDAllocator::Alloc());
            om->AllocateAndAssignInstanceID(out.object);
            out.transform = new Transform();
            om->AllocateAndAssignInstanceID(out.transform);
            out.object->addComponent(out.transform);
            out.transform->PostLoadResource(out.object);
            return level.RegisterRuntimeGObject(out.object);
        };
        auto add_probe = [&](Node& node, char tag) -> AppEventProbe*
        {
            AppEventProbe* probe = new AppEventProbe(&log, tag);
            probe->InitializeRuntimeTypeInfo();
            om->AllocateAndAssignInstanceID(probe);
            node.object->addComponent(probe);
            return probe;
        };

        Node a, b, c;
        Check(make_node(a), "DG2", "node A is in the level's runtime table");
        Check(make_node(b), "DG3", "node B too");
        Check(make_node(c), "DG4", "node C too");

        // B 上挂**两个**探针：只发给第一个组件的实现也照样过单探针的夹具。
        AppEventProbe* pa = add_probe(a, 'A');
        AppEventProbe* pb1 = add_probe(b, 'B');
        AppEventProbe* pb2 = add_probe(b, 'b');
        AppEventProbe* pc = add_probe(c, 'C');

        // --- 没有选中项：只有 OnDrawGizmos --------------------------------
        level.DrawGizmos({});
        Check(pa->gizmo_draws == 1, "DG5", "every component gets OnDrawGizmos once");
        Check(pb1->gizmo_draws == 1 && pb2->gizmo_draws == 1, "DG6",
              "every component on the SAME object gets it, not just the first");
        Check(pc->gizmo_draws == 1, "DG7", "and every object in the level");
        Check(pa->gizmo_selected_draws == 0, "DG8", "nothing selected: no OnDrawGizmosSelected");
        Check(pb1->gizmo_selected_draws == 0 && pb2->gizmo_selected_draws == 0, "DG9",
              "not even on B");

        // --- 选中 B：两个都来，且顺序固定（先常驻，再仅选中）--------------
        pa->Reset(); pb1->Reset(); pb2->Reset(); pc->Reset();
        log.clear();
        level.DrawGizmos({b.object});
        Check(pb1->gizmo_draws == 1, "DG10", "the selected object still gets the always-on one");
        Check(pb2->gizmo_draws == 1, "DG11", "on both its components");
        Check(pb1->gizmo_selected_draws == 1 && pb2->gizmo_selected_draws == 1, "DG12",
              "and the selected-only one on top of it (it is additive, not either/or)");
        Check(pa->gizmo_draws == 1 && pa->gizmo_selected_draws == 0, "DG13",
              "an unselected object gets only the always-on one");
        Check(pc->gizmo_draws == 1 && pc->gizmo_selected_draws == 0, "DG14", "same for C");
        // 顺序探针：同一个组件的两次派发在日志里是**相邻**的（Append 写两字符），
        // 所以 "BgBs" 这个子串只存在于"先 g 再 s"的实现里。
        Check(log.find("BgBs") != std::string::npos, "DG15",
              "OnDrawGizmos comes BEFORE OnDrawGizmosSelected on the same component");
        Check(log.find("BsBg") == std::string::npos, "DG16",
              "and never the other way round");

        // --- 多选：两个都被选中时两个都收 ----------------------------------
        pa->Reset(); pb1->Reset(); pb2->Reset(); pc->Reset();
        level.DrawGizmos({a.object, c.object});
        Check(pa->gizmo_selected_draws == 1, "DG17", "multi-select: A is selected");
        Check(pc->gizmo_selected_draws == 1, "DG18", "and C too");
        Check(pb1->gizmo_selected_draws == 0, "DG19", "B is not, and gets nothing extra");

        // --- 选中一个不在这张关卡里的对象：静默忽略 ------------------------
        pa->Reset(); pb1->Reset(); pb2->Reset(); pc->Reset();
        GameObject* stranger = new GameObject(ObjectIDAllocator::Alloc());
        level.DrawGizmos({stranger});
        delete stranger;
        Check(pa->gizmo_selected_draws == 0 && pc->gizmo_selected_draws == 0, "DG20",
              "a selected object from another level is simply not found (no crash, no callback)");
        Check(pa->gizmo_draws == 1 && pc->gizmo_draws == 1, "DG21",
              "and the always-on pass is unaffected by it");

        // --- ⚠️ 核心：不判 enabled ----------------------------------------
        pa->Reset();
        pa->SetProbeEnabled(false);
        level.DrawGizmos({});
        Check(pa->gizmo_draws == 1, "DG22",
              "a DISABLED component still gets OnDrawGizmos (Unity says so explicitly)");
        // 互补的一半：证明上面那条不是"enabled 开关压根没接上"。同一状态下
        // Tick 必须**不**来。
        //
        // ⚠️ 白名单：`Level::Tick` → `GameObject::Tick` 走 `shouldComponentTick`，
        // 而本探针没有 REGISTER_CLASS，`GetTypeName()` 是 "Component"。不先白名
        // 单一次的话 DG24 恒为 0，于是 DG23 变成空转门（"从来不 Tick"也给 0）。
        const bool had_component_whitelisted =
            g_editorTickComponentTypes.find("Component") != g_editorTickComponentTypes.end();
        if (!had_component_whitelisted) { g_editorTickComponentTypes.insert("Component"); }

        pa->ticks = 0;
        level.Tick(0.016f);
        Check(pa->ticks == 0, "DG23",
              "…while its Tick really is skipped (so DG22 is not a dead 'enabled' flag)");
        pa->SetProbeEnabled(true);
        pa->ticks = 0;
        level.Tick(0.016f);
        Check(pa->ticks == 1, "DG24", "and comes back when it is enabled again");
        // 门本身的门：证明 DG24 是靠白名单跑起来的，而不是"别的原因"（比如
        // g_isPlaying 恰好为真）。撤掉白名单再 Tick 一次，必须变回 0 —— 否则
        // DG23 那条"停用就不 Tick"也可能只是白名单没中的副产品。
        pa->ticks = 0;
        g_editorTickComponentTypes.erase("Component");
        level.Tick(0.016f);
        Check(pa->ticks == 0, "DG24b", "(that tick really came through the editor whitelist)");
        if (had_component_whitelisted) { g_editorTickComponentTypes.insert("Component"); }

        pa->Reset();

        // --- 判 activeInHierarchy：停用的对象一个都不发 --------------------
        a.object->SetActive(false);
        Check(!a.object->IsActiveInHierarchy(), "DG25", "A is really inactive");
        level.DrawGizmos({a.object});
        Check(pa->gizmo_draws == 0, "DG26", "an inactive object draws no gizmos at all");
        Check(pa->gizmo_selected_draws == 0, "DG27",
              "…and being selected does NOT override that gate");
        a.object->SetActive(true);

        // --- 父级停用 ⇒ 子级也收不到（是 activeInHierarchy，不是 activeSelf）--
        Check(c.transform->SetParent(a.transform, true), "DG28", "C is parented to A");
        Check(c.object->IsActiveInHierarchy(), "DG29", "C is active while A is");
        a.object->SetActive(false);
        Check(!c.object->IsActiveInHierarchy(), "DG30",
              "C is inactive in hierarchy too (its own activeSelf never changed)");
        pc->Reset();
        level.DrawGizmos({c.object});
        Check(pc->gizmo_draws == 0, "DG31",
              "a child of an inactive parent draws nothing (hierarchy, not self)");
        a.object->SetActive(true);

        // --- 没加载的关卡：不能崩，也不能发 --------------------------------
        {
            Level unloaded;
            unloaded.setLevelResUrl("Assets/Scenes/NeverLoaded.level");
            Check(!unloaded.IsLoaded(), "DG32", "the second level really is unloaded");
            // ⚠️ 光有"没加载"还不够：这张关卡里得**真的有对象**。否则"派发被
            // m_IsLoaded 拦下"和"压根没东西可派发"是同一件事，DG33 就成了空转门
            // （DGM11 实测存活过一次）。
            Node ghost;
            ghost.object = new GameObject(ObjectIDAllocator::Alloc());
            om->AllocateAndAssignInstanceID(ghost.object);
            ghost.transform = new Transform();
            om->AllocateAndAssignInstanceID(ghost.transform);
            ghost.object->addComponent(ghost.transform);
            ghost.transform->PostLoadResource(ghost.object);
            Check(unloaded.RegisterRuntimeGObject(ghost.object), "DG32b",
                  "a node really is in the unloaded level's runtime table");
            AppEventProbe* pg = add_probe(ghost, 'G');
            Check(pg->gizmo_draws == 0, "DG32c", "and it has not been drawn yet");
            unloaded.DrawGizmos({ghost.object});
            Check(pg->gizmo_draws == 0, "DG33",
                  "an unloaded level dispatches nothing (m_Gobjects holds the previous level's debris)");
            unloaded.UnregisterRuntimeGObject(ghost.object->GetID());
            delete pg;
            delete ghost.object;
        }

        // --- 一帧一次，没有内部状态：调两次就是两次 ------------------------
        pa->Reset();
        level.DrawGizmos({});
        level.DrawGizmos({});
        Check(pa->gizmo_draws == 2, "DG34", "one dispatch per call (gizmos are redrawn every frame)");

        // --- Application 那一层：`SetGizmoSelection` 与 ID -> 指针的解析 ----
        //
        // ⚠️ 这一段测的是"选中项怎么从 ID 变成指针"，不是"整帧走一遍"：后者
        // headless 下没有活动关卡（`m_CurrentActiveLevel` 只能由真实加载流程
        // 设置），所以 `LogicalTick -> DrawGizmosFrame` 那条路由由 DW24/DW25
        // 从源码级守。这里守的是那条链上唯一有判断、且能真的观测的一步。
        {
            Application::SetGizmoSelection({a.object->GetID(), c.object->GetID()});
            Check(Application::GetGizmoSelection().size() == 2, "DG36",
                  "the gizmo selection round-trips through the application");
            // 解析：这两张表与 Level::DrawGizmos 走的两张必须一一对应，否则
            // "选得中却画不出来" 或 "画得出来却选不中"。
            const std::vector<GameObject*> resolved =
                Application::ResolveGizmoSelection(&level);
            Check(resolved.size() == 2, "DG37",
                  "both selected IDs resolve against the level (runtime object table)");
            Check(resolved.size() == 2 && resolved[0] == a.object && resolved[1] == c.object,
                  "DG38", "and they resolve to the RIGHT objects");
            // 一个不在这张关卡里的 ID：静默缺席，不崩也不塞 nullptr。
            Application::SetGizmoSelection({a.object->GetID(), 0xDEADBEEFu});
            Check(Application::ResolveGizmoSelection(&level).size() == 1, "DG39",
                  "an unknown ID is simply absent (no crash, no null entry)");
            Check(Application::ResolveGizmoSelection(nullptr).empty(), "DG40",
                  "and a null level degrades to an empty selection");
            Application::SetGizmoSelection({});
            Check(Application::GetGizmoSelection().empty(), "DG41",
                  "the selection can be cleared (an editor click on empty space)");
        }

        // --- WorldManager 那一层：没有活动关卡时不能崩 ----------------------
        // 真正的"路由到已加载关卡"headless 测不到（要真实关卡文件），所以这里
        // 只守降级路径；`WorldManager::DrawGizmos` 本身存在与否由 DW15 守。
        if (WorldManager* world = GET_SYSTEM(WorldManager))
        {
            if (world->getCurrentActiveLevel() == nullptr)
            {
                pa->Reset();
                world->DrawGizmos({a.object});
                Check(pa->gizmo_draws == 0, "DG35",
                      "a world with no active level dispatches nothing (and does not crash)");
            }
        }

        level.UnregisterRuntimeGObject(a.object->GetID());
        level.UnregisterRuntimeGObject(b.object->GetID());
        level.UnregisterRuntimeGObject(c.object->GetID());
        delete pa;
        delete pb1;
        delete pb2;
        delete pc;
        delete a.object;
        delete b.object;
        delete c.object;
    }

    void TestDrawGizmosWiring()
    {
        const std::string component_header =
            ReadFile("Source/Runtime/Function/Framework/Component/Component.h");
        Check(!component_header.empty(), "DW0", "Component.h is readable");
        Check(CountOccurrences(component_header, "virtual void OnDrawGizmos(") >= 1,
              "DW1", "OnDrawGizmos is a virtual hook");
        // ⚠️ 子串陷阱：`OnDrawGizmosSelected(` 包含 `OnDrawGizmos` 但**不**包含
        // `OnDrawGizmos(`（多了一个左括号），所以 DW1 不会被它单独满足 ——
        // 反过来 DW2 才是 Selected 那条。
        Check(CountOccurrences(component_header, "virtual void OnDrawGizmosSelected(") >= 1,
              "DW2", "OnDrawGizmosSelected is a separate hook (not a parameter)");

        const std::string go_source = ReadFile("Source/Runtime/BaseClasses/GameObject.cpp");
        Check(!go_source.empty(), "DW3", "GameObject.cpp is readable");
        const std::size_t at_broadcast = go_source.find("void GameObject::BroadcastDrawGizmos");
        Check(at_broadcast != std::string::npos, "DW4", "BroadcastDrawGizmos is implemented");
        if (at_broadcast != std::string::npos)
        {
            Check(BodyIsBounded(go_source, at_broadcast), "DW4b",
                  "the BroadcastDrawGizmos body slice is bounded (the scan is not degenerate)");
            const std::string body = SliceFunctionBody(go_source, at_broadcast);
            // 正向门：必须判 activeInHierarchy。
            Check(CountOccurrences(body, "IsActiveInHierarchy") >= 1, "DW5",
                  "it gates on activeInHierarchy (an inactive object draws nothing)");
            // ⚠️ 反向门：**不许**判 enabled。这是本切片最容易被"顺手修掉"的一行，
            // 而 DG22 是运行时断言、这里是源码级断言 —— 两道门都要有：运行时那道
            // 证明行为，源码这道证明"函数体里没有那行"。
            Check(body.find("isEnabled") == std::string::npos, "DW6",
                  "it does NOT gate on isEnabled (Unity: disabled scripts still draw gizmos)");
            Check(body.find("shouldComponentTick") == std::string::npos, "DW7",
                  "nor on the edit-mode whitelist (gizmos are an editor-frame concern)");
            // 两个都发、且 Selected 是**额外**的（在 if 里，不在 else 里）。
            Check(CountOccurrences(body, "OnDrawGizmos();") >= 1, "DW8",
                  "the always-on hook is dispatched");
            Check(CountOccurrences(body, "OnDrawGizmosSelected();") >= 1, "DW9",
                  "and the selected-only one, additively");
        }

        const std::string level_source =
            ReadFile("Source/Runtime/Function/Framework/Level/Level.cpp");
        Check(!level_source.empty(), "DW10", "Level.cpp is readable");
        const std::size_t at_level = level_source.find("void Level::DrawGizmos");
        Check(at_level != std::string::npos, "DW11", "Level walks its objects for gizmos");
        if (at_level != std::string::npos)
        {
            Check(BodyIsBounded(level_source, at_level), "DW11b",
                  "the Level::DrawGizmos body slice is bounded (the scan is not degenerate)");
            const std::string body = SliceFunctionBody(level_source, at_level);
            // 两张表都要走：主表 + 运行时表（Instantiate 产物）。漏第二张的话，
            // 脚本刚创建出来的对象在编辑器里永远画不出辅助线。
            Check(CountOccurrences(body, "m_Gobjects") >= 1, "DW12",
                  "it walks the loaded object table");
            Check(CountOccurrences(body, "m_RuntimeGObjects") >= 1, "DW13",
                  "and the runtime (Instantiate) table too");
            // ⚠️ 两条 BroadcastDrawGizmos 调用文本**完全一样**，所以 >= 1 会在
            // 删掉任意一条时照过。数 2 才是"两张表都派发"。
            Check(CountOccurrences(body, "BroadcastDrawGizmos(") >= 2, "DW14",
                  "both tables are dispatched (not just one)");
        }

        const std::string world_source =
            ReadFile("Source/Runtime/Function/Framework/World/WorldManager.cpp");
        Check(CountOccurrences(world_source, "void WorldManager::DrawGizmos") >= 1,
              "DW15", "WorldManager routes gizmos to the loaded level(s)");

        const std::string ts_source =
            ReadFile("Source/Runtime/Function/Framework/Component/Script/TypeScriptComponent.cpp");
        Check(!ts_source.empty(), "DW16", "TypeScriptComponent.cpp is readable");
        Check(CountOccurrences(ts_source, "\"OnDrawGizmos\"") >= 1, "DW17",
              "the script component forwards OnDrawGizmos");
        // 名字只差一个词：写成 `"OnDrawGizmos"` 的话选中对象的常驻辅助线照来，
        // 仅选中的那次永远不来 —— 没有任何报错，只有"多出来的那部分没画"。
        Check(CountOccurrences(ts_source, "\"OnDrawGizmosSelected\"") >= 1, "DW18",
              "and OnDrawGizmosSelected (they are not merged)");

        const std::string ts_header =
            ReadFile("Source/Runtime/Function/Framework/Component/Script/TypeScriptComponent.h");
        Check(CountOccurrences(ts_header, "void OnDrawGizmos() override;") >= 1, "DW19",
              "the header declares both overrides");

        const std::string env_source = ReadFile("Source/Runtime/Scripting/Wrapper/ScriptEnv.cpp");
        // ⚠️ 带 " = function" 而不是裸标识符：`…OnDrawGizmosSelected = function`
        // **包含** `…OnDrawGizmos` 这个前缀，只数后者的话删掉 OnDrawGizmos 那
        // 一行照样绿（DGM16 实测存活过一次）。
        Check(CountOccurrences(env_source, "g.Behaviour.prototype.OnDrawGizmos = function") >= 1,
              "DW20", "the JS prototype has an OnDrawGizmos default");
        Check(CountOccurrences(env_source, "g.Behaviour.prototype.OnDrawGizmosSelected = function") >= 1,
              "DW21", "and an OnDrawGizmosSelected default");

        const std::string binding_source =
            ReadFile("Source/Runtime/Scripting/Binding/ScriptBinding.cpp");
        Check(CountOccurrences(binding_source, "OnDrawGizmos(): void;") >= 1, "DW22",
              "the .d.ts declares OnDrawGizmos on Behaviour");
        Check(CountOccurrences(binding_source, "OnDrawGizmosSelected(): void;") >= 1, "DW23",
              "and OnDrawGizmosSelected");

        // ---- 主循环那一层：实现了但没接进主循环 = 没实现 --------------------
        //
        // `Level::DrawGizmos` 本身的行为由 DG 组从运行时验，但"谁每帧调它"
        // 在 headless 下测不到（`m_CurrentActiveLevel` 只能由真实加载流程设置）。
        // 所以这一层用源码级门守 —— 少一条，整个特性就是一段没人调用的代码，
        // 而且**静默**（没有任何报错，只是辅助线永远不出现）。
        const std::string app_source =
            ReadFile("Source/Runtime/Application/Application.cpp");
        Check(!app_source.empty(), "DW24", "Application.cpp is readable");
        {
            const std::size_t at_tick = app_source.find("void Application::LogicalTick");
            Check(at_tick != std::string::npos, "DW25", "LogicalTick is implemented");
            if (at_tick != std::string::npos)
            {
                Check(BodyIsBounded(app_source, at_tick), "DW25b",
                      "the LogicalTick body slice is bounded (the scan is not degenerate)");
                const std::string body = SliceFunctionBody(app_source, at_tick);
                Check(CountOccurrences(body, "DrawGizmosFrame();") >= 1, "DW26",
                      "the logical tick drives the gizmo frame");
                // 必须在 LateTick **之后**：gizmo 画的是这一帧的最终位姿。
                Check(body.find("->LateTick(") < body.find("DrawGizmosFrame();"), "DW27",
                      "and after LateTick (gizmos show the frame's final pose)");
            }

            const std::size_t at_frame = app_source.find("void Application::DrawGizmosFrame");
            Check(at_frame != std::string::npos, "DW28", "DrawGizmosFrame is implemented");
            if (at_frame != std::string::npos)
            {
                Check(BodyIsBounded(app_source, at_frame), "DW28b",
                      "the DrawGizmosFrame body slice is bounded (the scan is not degenerate)");
                const std::string body = SliceFunctionBody(app_source, at_frame);
                Check(CountOccurrences(body, "->DrawGizmos(") >= 1, "DW29",
                      "it dispatches through WorldManager");
                // Player 里整趟跳过：这一趟要走遍全关卡。
                //
                // ⚠️ 数的是 **`if (!g_isEditor)` 这条语句**，不是 "g_isEditor"
                // 这个字符串：本函数的注释里也要提到这个开关（说明为什么这么
                // 判），只数裸标识符的话注释就足以让门恒真 —— 于是"把判据删掉"
                // 的改动测不出来（DGM21 实测存活过一次）。
                Check(CountOccurrences(body, "if (!g_isEditor)") >= 1, "DW30",
                      "and the player skips the whole pass (gizmos are editor-only)");
            }

            // ID -> 指针的解析必须查**两张**表（关卡对象表 + Instantiate 表）。
            const std::size_t at_resolve = app_source.find("Application::ResolveGizmoSelection");
            Check(at_resolve != std::string::npos, "DW31", "the ID resolution is a step of its own");
            if (at_resolve != std::string::npos)
            {
                Check(BodyIsBounded(app_source, at_resolve), "DW31b",
                      "the ResolveGizmoSelection body slice is bounded");
                const std::string body = SliceFunctionBody(app_source, at_resolve);
                Check(CountOccurrences(body, "GetGObjectByID") >= 1, "DW32",
                      "it looks in the level object table");
                Check(CountOccurrences(body, "FindRuntimeGObject") >= 1, "DW33",
                      "and in the Instantiate table too");
            }
        }
    }

    void TestApplicationMessagesWiring()
    {
        const std::string component_header =
            ReadFile("Source/Runtime/Function/Framework/Component/Component.h");
        Check(!component_header.empty(), "AW0", "Component.h is readable");
        Check(CountOccurrences(component_header, "enum class AppEventKind") >= 1,
              "AW1", "the four app messages are one enum (one walk, not four)");
        Check(CountOccurrences(component_header, "virtual void OnApplicationFocus(") >= 1,
              "AW2", "OnApplicationFocus is a virtual hook");
        Check(CountOccurrences(component_header, "virtual void OnApplicationPause(") >= 1,
              "AW3", "and OnApplicationPause");
        Check(CountOccurrences(component_header, "virtual void OnApplicationQuit(") >= 1,
              "AW4", "and OnApplicationQuit");
        Check(CountOccurrences(component_header, "virtual void OnTransformParentChanged(") >= 1,
              "AW5", "and OnTransformParentChanged");

        const std::string go_source = ReadFile("Source/Runtime/BaseClasses/GameObject.cpp");
        Check(CountOccurrences(go_source, "void GameObject::BroadcastAppEvent") >= 1,
              "AW6", "GameObject walks its components for app events");
        // ⚠️ 反向门：`BroadcastAppEvent` 里**不许**出现 Tick 那套判据。加回
        // IsActiveInHierarchy / shouldComponentTick 就等于把"被停用的对象也
        // 要收到退出通知"删掉 —— 而 AM9 / AM11 是运行时断言，这里守的是
        // "函数体里没有那两行"这件事本身（两道门，不是冗余）。
        const std::size_t at_broadcast = go_source.find("void GameObject::BroadcastAppEvent");
        const std::size_t at_tick = go_source.find("void GameObject::Tick");
        Check(at_broadcast != std::string::npos && at_tick != std::string::npos && at_broadcast > at_tick,
              "AW7", "BroadcastAppEvent is a separate walk (not folded into Tick)");
        {
            // ⚠️ "门本身的门"，同 CW5b / CW10b / CW14b：AW8 / AW9 是**反向门**，
            // 切函数体退化成"半个文件"时它们变成永假（永远找得到那两行），看
            // 起来像"有人把判据加回来了"，其实只是行尾变了。
            Check(BodyIsBounded(go_source, at_broadcast), "AW7b",
                  "the BroadcastAppEvent body slice is bounded (the scan is not degenerate)");
            const std::string body = SliceFunctionBody(go_source, at_broadcast);
            Check(body.find("IsActiveInHierarchy") == std::string::npos, "AW8",
                  "it does NOT gate on activeInHierarchy (unlike the three ticks)");
            Check(body.find("shouldComponentTick") == std::string::npos, "AW9",
                  "nor on the edit-mode whitelist");
            // 五个 case 都要有：少一个就是"某条消息永远收不到"。
            Check(CountOccurrences(body, "AppEventKind::k") >= 5, "AW10",
                  "all five kinds are dispatched");
        }

        const std::string world_source =
            ReadFile("Source/Runtime/Function/Framework/World/WorldManager.cpp");
        Check(CountOccurrences(world_source, "void WorldManager::BroadcastAppEvent") >= 1,
              "AW11", "WorldManager forwards app events");
        // ⚠️ 与 LateTick 相反：这里**不许**判 m_IsWorldLoaded。世界还没加载完
        // 就退出是 OnApplicationQuit 最常见的一次。
        {
            const std::size_t at = world_source.find("void WorldManager::BroadcastAppEvent");
            Check(at != std::string::npos, "AW12", "WorldManager::BroadcastAppEvent is implemented");
            // 同 AW7b：AW13 是反向门，切片退化时它变永假。
            Check(BodyIsBounded(world_source, at), "AW12b",
                  "the WorldManager body slice is bounded (the scan is not degenerate)");
            const std::string body = SliceFunctionBody(world_source, at);
            Check(body.find("m_IsWorldLoaded") == std::string::npos, "AW13",
                  "it does NOT early-return on m_IsWorldLoaded (quitting mid-load must still fire)");
        }

        const std::string level_source = ReadFile("Source/Runtime/Function/Framework/Level/Level.cpp");
        Check(CountOccurrences(level_source, "void Level::BroadcastAppEvent") >= 1,
              "AW14", "Level forwards app events");
        Check(CountOccurrences(level_source, "m_RuntimeGObjects") >= 1,
              "AW15", "and the level source knows about the runtime object table");

        // ---- 三个触发点：少任何一个，这条消息在真机上一次都不会发 --------
        const std::string app_source = ReadFile("Source/Runtime/Application/Application.cpp");
        Check(!app_source.empty(), "AW16", "Application.cpp is readable");
        Check(CountOccurrences(app_source, "AppEventKind::kQuit") >= 1,
              "AW17", "Quit() broadcasts OnApplicationQuit");
        Check(CountOccurrences(app_source, "AppEventKind::kFocus") >= 1,
              "AW18", "NotifyApplicationFocus broadcasts OnApplicationFocus");
        Check(CountOccurrences(app_source, "AppEventKind::kPause") >= 1,
              "AW19", "and the derived OnApplicationPause");
        // 顺序门：退出通知必须在 RequestClose **之前**。窗口系统一开始就关，
        // WorldManager 可能已经随系统反初始化被摘掉，那时候派发就是静默丢失。
        const std::size_t at_quit_broadcast = app_source.find("AppEventKind::kQuit");
        const std::size_t at_request_close = app_source.find("window_system->RequestClose()");
        Check(at_quit_broadcast != std::string::npos && at_request_close != std::string::npos &&
                  at_quit_broadcast < at_request_close,
              "AW20", "the quit broadcast happens before the window is told to close");

        const std::string window_source = ReadFile("Source/Runtime/Function/Render/WindowSystem.cpp");
        Check(!window_source.empty(), "AW21", "WindowSystem.cpp is readable");
        // 焦点是窗口系统的信号，而 Application 是 Core 阶段（比它早）—— 注册
        // 只能放在窗口系统这一侧，放在 Application::Initialize 里拿到的是
        // nullptr，注册就静默丢了。
        Check(CountOccurrences(window_source, "RegisterOnWindowFocusFunc") >= 1,
              "AW22", "the window system registers the focus forwarder");
        Check(CountOccurrences(window_source, "Application::NotifyApplicationFocus") >= 1,
              "AW23", "and forwards to Application (not straight to the world)");

        const std::string transform_source =
            ReadFile("Source/Runtime/Function/Framework/Component/Transform/Transform.cpp");
        Check(!transform_source.empty(), "AW24", "Transform.cpp is readable");
        Check(CountOccurrences(transform_source, "AppEventKind::kParentChanged") >= 1,
              "AW25", "SetParent broadcasts OnTransformParentChanged");

        const std::string ts_source =
            ReadFile("Source/Runtime/Function/Framework/Component/Script/TypeScriptComponent.cpp");
        Check(CountOccurrences(ts_source, "\"OnApplicationFocus\"") >= 1, "AW26",
              "the script component forwards OnApplicationFocus");
        Check(CountOccurrences(ts_source, "\"OnApplicationPause\"") >= 1, "AW27",
              "and OnApplicationPause");
        Check(CountOccurrences(ts_source, "\"OnApplicationQuit\"") >= 1, "AW28",
              "and OnApplicationQuit");
        Check(CountOccurrences(ts_source, "\"OnTransformParentChanged\"") >= 1, "AW29",
              "and OnTransformParentChanged");
        Check(CountOccurrences(ts_source, "\"OnValidate\"") >= 1, "AW30",
              "and OnValidate (from the editor's serialized-field hook)");
        // ⚠️ bool 参数必须走 Bool 版：塞成 1.0/0.0 的话 `hasFocus === true` 是假，
        // 而 `if (hasFocus)` 却是真 —— 只在严格比较时才炸。
        Check(CountOccurrences(ts_source, "InvokeMethodBool") >= 3, "AW31",
              "the bool-carrying hooks use the bool overload (definition + 2 call sites)");

        const std::string env_header = ReadFile("Source/Runtime/Scripting/Wrapper/ScriptEnv.h");
        Check(CountOccurrences(env_header, "InvokeInstanceMethodBool") >= 1, "AW32",
              "ScriptEnv exposes a bool-arg invoke");
        const std::string env_source = ReadFile("Source/Runtime/Scripting/Wrapper/ScriptEnv.cpp");
        Check(CountOccurrences(env_source, "pesapi_create_boolean") >= 1, "AW33",
              "and really creates a JS boolean (not a 1.0/0.0 number)");

        const std::string binding_source =
            ReadFile("Source/Runtime/Scripting/Binding/ScriptBinding.cpp");
        Check(CountOccurrences(binding_source, "OnApplicationFocus(has_focus: boolean): void;") >= 1,
              "AW34", "the .d.ts declares OnApplicationFocus");
        Check(CountOccurrences(binding_source, "OnApplicationPause(pause_status: boolean): void;") >= 1,
              "AW35", "and OnApplicationPause");
        Check(CountOccurrences(binding_source, "OnTransformParentChanged(): void;") >= 1,
              "AW36", "and OnTransformParentChanged");
        // 五个原型默认值一个都不能少：派发是无条件的，缺了兜底就是每个没写
        // 这些钩子的类每次事件都走一次属性查找失败。
        Check(CountOccurrences(env_source, "g.Behaviour.prototype.OnApplication") >= 2,
              "AW37", "the JS prototype has no-op defaults for the focus/pause hooks");
        Check(CountOccurrences(env_source, "g.Behaviour.prototype.OnApplicationQuit") >= 1,
              "AW38", "and for OnApplicationQuit");
        Check(CountOccurrences(env_source, "g.Behaviour.prototype.OnTransformParentChanged") >= 1,
              "AW39", "and for OnTransformParentChanged");
    }

    // ---------------------------------------------------------------- B ----
    void TestLayerMask()
    {
        // --- 名字 <-> 层号 -----------------------------------------------------
        Check(Runtime::LayerMask::NameToLayer("Default") == 0, "LM0",
              "LayerMask.NameToLayer(\"Default\") is 0");
        Check(Runtime::LayerMask::NameToLayer("UI") == 5, "LM1",
              "LayerMask.NameToLayer(\"UI\") is 5 (Unity's built-in slot)");
        // ⚠️ 找不到给 **-1**，不是 0。给 0 的话"拼错的层名"会静默变成"打 Default
        // 层" —— 而 Default 层上东西最多，射线照样有结果，完全看不出来。
        Check(Runtime::LayerMask::NameToLayer("NoSuchLayer") == -1, "LM2",
              "an unknown name is -1, NOT 0 (0 is Default -- the busiest layer)");
        Check(Runtime::LayerMask::NameToLayer("") == -1, "LM3", "and so is the empty string");

        // --- 层号 -> 名字 -------------------------------------------------------
        Check(Runtime::LayerMask::LayerToName(0) == "Default", "LM4", "LayerToName(0) is Default");
        // Unity 的第 3 层**没有名字**（退休的那层），6/7 保留 —— 空串是对的，
        // 给它起名会招人来用。
        Check(Runtime::LayerMask::LayerToName(3) == "", "LM5",
              "layer 3 has no name in Unity either (empty is correct, not a bug)");
        Check(Runtime::LayerMask::LayerToName(-1) == "" && Runtime::LayerMask::LayerToName(32) == "",
              "LM6", "out-of-range layer numbers give an empty string instead of throwing");

        // --- GetMask ------------------------------------------------------------
        Check(Runtime::LayerMask::GetMask({}) == 0, "LM7", "GetMask of nothing is 0 (hit nothing)");
        Check(Runtime::LayerMask::GetMask({"Default", "UI"}) == 33, "LM8",
              "GetMask(['Default','UI']) is (1<<0)|(1<<5) = 33");
        // ⚠️ 未知层名**跳过**，不是"整张掩码作废"（返回 -1 = 所有层）。返回 -1
        // 的失败方向是"射线打到所有东西"，比"少打一层"难查得多。见 ScriptLayerMask.h。
        Check(Runtime::LayerMask::GetMask({"NoSuchLayer", "UI"}) == 32, "LM9",
              "an unknown name is SKIPPED (32, not -1 -- -1 would mean every layer)");

        // --- 第 31 位是负数 ------------------------------------------------------
        // `1 << 31` 在 JS 里是 -2147483648。掩码是"负数"这件事本身没错（位模式
        // 对），但它会让人写出 `mask > 0` 这类判断 —— 所以判某层在不在要用
        // MaskContainsLayer，不要自己 `&`。
        const int32_t top = static_cast<int32_t>(1u << 31);
        Check(top < 0, "LM10", "layer 31's mask is negative in a 32-bit signed int (that is fine)");
        Check(Runtime::LayerMask::MaskContainsLayer(top, 31), "LM11",
              "MaskContainsLayer sees layer 31 in it anyway");
        Check(!Runtime::LayerMask::MaskContainsLayer(top, 30), "LM12", "and does not see layer 30");
        Check(Runtime::LayerMask::MaskContainsLayer(33, 0) &&
                  Runtime::LayerMask::MaskContainsLayer(33, 5) &&
                  !Runtime::LayerMask::MaskContainsLayer(33, 3),
              "LM13", "MaskContainsLayer reads the bits a GetMask call just wrote");
        Check(!Runtime::LayerMask::MaskContainsLayer(33, -1) &&
                  !Runtime::LayerMask::MaskContainsLayer(33, 32), "LM14",
              "and an out-of-range layer is simply not in it");
    }

    void TestLayerMaskWiring()
    {
        const std::string header =
            ReadFile("Source/Runtime/Scripting/ScriptLayerMask/ScriptLayerMask.h");
        Check(!header.empty(), "LMW0", "ScriptLayerMask.h is readable");
        if (header.empty()) { return; }
        // ⚠️ 门面必须**转发** `TagLayerManager`，不能自己存一份层表：两份层表
        // 迟早不一致，而不一致的表现是"射线打不到东西" —— 没有任何报错。
        Check(CountOccurrences(header, "TagLayerManager") >= 1, "LMW1",
              "the facade is documented against TagLayerManager (one owner of the table)");
        Check(CountOccurrences(header, "ZFUNCTION()") == 4, "LMW2",
              "LayerMask declares exactly 4 script methods (a dropped ZFUNCTION is a silent API loss)");

        const std::string source =
            ReadFile("Source/Runtime/Scripting/ScriptLayerMask/ScriptLayerMask.cpp");
        Check(!source.empty(), "LMW3", "ScriptLayerMask.cpp is readable");
        if (!source.empty())
        {
            // 三个查表的走 `GET_SYSTEM`；`MaskContainsLayer` 不需要 —— 它只做
            // 一次位测试，用的是 `TagLayerManager` 的静态 helper（那份位运算才是
            // 要和物理查询对齐的东西，它必须仍然在 TagLayerManager 里）。
            Check(CountOccurrences(source, "GET_SYSTEM(TagLayerManager)") >= 3, "LMW4",
                  "the three table lookups go through the shared TagLayerManager, not a local copy");
        }
    }

    void TestVector3Statics()
    {
        const ::Vector3 ex(1.f, 0.f, 0.f);
        const ::Vector3 ey(0.f, 1.f, 0.f);

        // --- Slerp：走弧，长度不塌 ----------------------------------------------
        const ::Vector3 sl = Runtime::Vector3::Slerp(ex, ey, 0.5f);
        const ::Vector3 lerp = Runtime::Vector3::Lerp(ex, ey, 0.5f);
        // ⚠️ 判据是**长度**：Slerp 全程 1，Lerp 在中点塌到 0.7071。只看方向的
        // 话两者在同一条 45° 射线上，分不开。
        Check(Near(sl.length(), 1.f, 1e-4f), "VX0",
              "Vector3.Slerp keeps the magnitude at 1 through the middle of the arc");
        Check(Near(lerp.length(), 0.70710678f, 1e-4f), "VX1",
              "while Lerp dips to 0.7071 there -- that is what makes them different");
        Check(Near(sl.x, 0.70710678f, 1e-4f) && Near(sl.y, 0.70710678f, 1e-4f), "VX2",
              "and Slerp's direction is the 45 degree one");
        // t 夹取：与 Unity 同（Vector3.Lerp 夹，LerpUnclamped 不夹）。
        Check(Near(Runtime::Vector3::Slerp(ex, ey, 2.f).y, 1.f, 1e-4f) &&
                  Near(Runtime::Vector3::Slerp(ex, ey, -1.f).x, 1.f, 1e-4f), "VX3",
              "Slerp clamps t to [0,1] like Unity");

        // --- RotateTowards -------------------------------------------------------
        // ⚠️ 第一个角度参数是**弧度**，不是度（Unity 自己的不一致，参数名就叫
        // maxRadiansDelta）。写成度的话单步会小 57 倍 —— 表现为"转得特别慢"，
        // 而它确实在转，没有任何报错。
        const float quarter = 0.7853982f;  // pi / 4
        const ::Vector3 rot =
            Runtime::Vector3::RotateTowards(ex, ey, quarter, 0.f);
        Check(Near(rot.x, 0.70710678f, 1e-4f) && Near(rot.y, 0.70710678f, 1e-4f), "VX4",
              "RotateTowards turns by at most maxRadiansDelta (RADIANS, not degrees)");
        Check(Near(Runtime::Vector3::RotateTowards(ex, ey, 0.f, 0.f).x, 1.f, 1e-4f), "VX5",
              "and a zero angular budget leaves the direction alone");
        Check(Near(Runtime::Vector3::RotateTowards(ex, ey, 10.f, 0.f).y, 1.f, 1e-4f), "VX6",
              "while a budget bigger than the angle reaches the target");
        // ⚠️ 长度是**单独**按 maxMagnitudeDelta 走的：用 MoveTowards(current, target)
        // 那种"方向和长度一起走"的实现，maxMagnitudeDelta 为 0 时等于一步没动。
        const ::Vector3 both = Runtime::Vector3::RotateTowards(
            ::Vector3(2.f, 0.f, 0.f), ::Vector3(0.f, 4.f, 0.f), 10.f, 1.f);
        Check(Near(both.y, 3.f, 1e-4f) && Near(both.x, 0.f, 1e-4f), "VX7",
              "the magnitude walks separately: 2 -> 3 while the direction reaches +Y");

        // --- ProjectOnPlane -------------------------------------------------------
        Check(Near(Runtime::Vector3::ProjectOnPlane(::Vector3(1.f, 2.f, 3.f),
                                                   ::Vector3(0.f, 0.f, 1.f)).z,
                   0.f, 1e-5f), "VX8",
              "ProjectOnPlane removes the along-normal part");
        // ⚠️ 法线**不**必是单位向量：忘了除 (n·n) 的话这一条就不成立，而单位
        // 法线的那条照样对 —— 只测单位法线等于没测。
        const ::Vector3 scaled = Runtime::Vector3::ProjectOnPlane(::Vector3(1.f, 2.f, 3.f),
                                                                 ::Vector3(0.f, 0.f, 2.f));
        Check(Near(scaled.x, 1.f, 1e-5f) && Near(scaled.y, 2.f, 1e-5f) &&
                  Near(scaled.z, 0.f, 1e-5f), "VX9",
              "and the normal does not have to be unit length");

        // --- SignedAngle ----------------------------------------------------------
        // ⚠️ 互补的一对：同一对向量换顺序必须换**符号**，而不只是换大小。
        // 只测 "== 90" 的话，少了符号的实现（就是 Angle）照样过。
        Check(Near(Runtime::Vector3::SignedAngle(ex, ey, ::Vector3(0.f, 0.f, 1.f)), 90.f, 1e-3f),
              "VX10", "SignedAngle(+X -> +Y about +Z) is +90");
        Check(Near(Runtime::Vector3::SignedAngle(ey, ex, ::Vector3(0.f, 0.f, 1.f)), -90.f, 1e-3f),
              "VX11", "and the same pair the other way round is -90 (the sign is the point)");
        // 绕 **-Z** 看符号反过来。
        Check(Near(Runtime::Vector3::SignedAngle(ex, ey, ::Vector3(0.f, 0.f, -1.f)), -90.f,
                   1e-3f), "VX12", "and flipping the axis flips the sign");
    }

    void TestVector3StaticsWiring()
    {
        const std::string header = ReadFile("Source/Runtime/Scripting/ScriptVector/ScriptVector.h");
        Check(!header.empty(), "VXW0", "ScriptVector.h is readable");
        if (header.empty()) { return; }
        Check(CountOccurrences(header, "ZFUNCTION()") == 114, "VXW1",
              "ScriptVector declares exactly 114 script methods (a dropped ZFUNCTION is a silent "
              "API loss)");
        // ⚠️ 这四个名字在 Unity 里是 `Vector3.xxx`，在 ZEngine 里原本只有
        // `VectorMath.xxx`。搬过来的代码每个调用点都得改 —— 而"方法不存在"在
        // 运行时是静默无事发生，只有源码门看得见。
        Check(CountOccurrences(header, "static ::Vector3 Slerp(") >= 1 &&
                  CountOccurrences(header, "static ::Vector3 RotateTowards(") >= 1 &&
                  CountOccurrences(header, "static ::Vector3 ProjectOnPlane(") >= 1 &&
                  CountOccurrences(header, "static float SignedAngle(") >= 1, "VXW2",
              "Vector3 carries Unity's Slerp / RotateTowards / ProjectOnPlane / SignedAngle");

        const std::string source = ReadFile("Source/Runtime/Scripting/ScriptVector/ScriptVector.cpp");
        Check(!source.empty(), "VXW3", "ScriptVector.cpp is readable");
        if (!source.empty())
        {
            // 四个都**转发** VectorMath，不在这里复制一份算法。
            Check(CountOccurrences(source, "VectorMath::Slerp") >= 1 &&
                      CountOccurrences(source, "VectorMath::RotateTowards") >= 1 &&
                      CountOccurrences(source, "VectorMath::ProjectOnPlane") >= 1 &&
                      CountOccurrences(source, "VectorMath::SignedAngle") >= 1, "VXW4",
                  "and each one forwards to VectorMath (no second copy of the algorithm)");
        }
    }

    void TestMathfTail()
    {
        // --- PerlinNoise1D ------------------------------------------------------
        // ⚠️ 整数点上**恒为 0.5**：梯度噪声在格点处的定义值就是 0。这不是"随到
        // 了中点"，而是结构性的 —— 用它当判据能抓住"插值写错"的一大类实现。
        Check(Near(Mathf::PerlinNoise1D(0.f), 0.5f, 1e-6f) &&
                  Near(Mathf::PerlinNoise1D(1.f), 0.5f, 1e-6f) &&
                  Near(Mathf::PerlinNoise1D(7.f), 0.5f, 1e-6f), "MF0",
              "PerlinNoise1D is exactly 0.5 at every integer (gradient noise is 0 on the lattice)");
        // 黄金数（与本机实现逐位对齐；换掉哈希或 fade 就变）。
        Check(Near(Mathf::PerlinNoise1D(3.3f), 0.134768f, 1e-5f), "MF1",
              "PerlinNoise1D(3.3) is 0.134768");
        Check(Near(Mathf::PerlinNoise1D(12.25f), 0.801758f, 1e-5f), "MF2",
              "PerlinNoise1D(12.25) is 0.801758");
        // 确定性：同一个 x 两次调用必须同一个值（否则每帧抖）。
        Check(Mathf::PerlinNoise1D(3.3f) == Mathf::PerlinNoise1D(3.3f), "MF3",
              "it is deterministic (the same x gives the same number twice)");
        // 值域与连续性。
        bool in_range = true;
        bool continuous = true;
        float previous = Mathf::PerlinNoise1D(0.f);
        for (int i = 1; i < 2000; ++i)
        {
            const float x = static_cast<float>(i) * 0.01f;
            const float v = Mathf::PerlinNoise1D(x);
            if (!(v >= 0.f && v <= 1.f)) { in_range = false; }
            // 0.01 的步长上不该有跳变（fade 是 C2 连续的）。
            if (std::fabs(v - previous) > 0.1f) { continuous = false; }
            previous = v;
        }
        Check(in_range, "MF4", "it stays inside [0,1] over 20 units");
        Check(continuous, "MF5", "and it is continuous (no jumps bigger than 0.1 per 0.01 step)");
    }

    /// 把 parent 的子节点顺序读成 "ABC" 这种串。
    ///
    /// 判据用**整串**而不是"被挪那个的下标"：MoveAfterSibling 的 off-by-one
    /// 只在"跨过目标"时才触发，被挪的那个自己往往是对的、另外两个的相对顺序
    /// 反了。只看挪动者会完全漏掉（B 挪到 A 后面，B 的下标确实变了）。
    static std::string ChildOrder3(Transform* parent, Transform* a, Transform* b, Transform* c)
    {
        std::string out;
        const int count = parent->GetChildrenCount();
        for (int i = 0; i < count; ++i)
        {
            Transform* child = parent->GetChild(i);
            out += (child == a) ? 'A' : (child == b) ? 'B' : (child == c) ? 'C' : '?';
        }
        return out;
    }

    // ---------------------------------------------------------------- TE ---
    /// Unity Transform 上还缺的那一组：`SetPositionAndRotation` /
    /// `SetLocalPositionAndRotation` / `hasChanged` / `hierarchyCapacity` /
    /// `hierarchyCount` / `MoveAfterSibling`。
    ///
    /// 夹具（**故意不对称**：root 有位移、A 带一个子节点、子节点顺序 ABC）：
    ///
    ///     root(10,0,0)
    ///      ├── A ── G
    ///      ├── B
    ///      └── C
    ///
    /// 层级节点数 5，A 自己的子树 2 —— 这两个数不一样，是分开
    /// "hierarchyCount 指整份层级" 和 "指自己子树" 的唯一判据。
    void TestTransformExtras()
    {
        ObjectManager* om = GET_SYSTEM(ObjectManager);
        if (om == nullptr)
        {
            om = new ObjectManager();
            SystemRegistry::GetInstance().RegisterSystem(om);
            om->Initialize();
        }
        Check(om != nullptr, "TE0", "ObjectManager is reachable");
        if (om == nullptr)
        {
            return;
        }

        GameObject* o_root = nullptr;
        GameObject* o_a = nullptr;
        GameObject* o_b = nullptr;
        GameObject* o_c = nullptr;
        GameObject* o_g = nullptr;
        GameObject* o_d = nullptr;
        Transform* root = MakeTransform(*om, o_root);
        Transform* a = MakeTransform(*om, o_a);
        Transform* b = MakeTransform(*om, o_b);
        Transform* c = MakeTransform(*om, o_c);
        Transform* g = MakeTransform(*om, o_g);
        Check(root != nullptr && a != nullptr && b != nullptr && c != nullptr && g != nullptr,
              "TE1", "five transforms exist");
        if (root == nullptr || a == nullptr || b == nullptr || c == nullptr || g == nullptr)
        {
            return;
        }

        a->SetParent(root, false);
        b->SetParent(root, false);
        c->SetParent(root, false);
        g->SetParent(a, false);
        root->SetLocalPosition(Vector3(10.f, 0.f, 0.f));
        root->SetLocalRotation(Quaternion::IDENTITY);

        // --- SetPositionAndRotation / SetLocalPositionAndRotation -----------
        // 90° 绕 Z（w=cos45, z=sin45）。用**朝向**当判据而不是比较四元数分量：
        // identity 的 forward 是 -Y，转 90° 后变成 +X，是"位置对了但朝向没设"
        // 这种漏设一眼能分开的判据。
        const Quaternion qz90(0.7071068f, 0.f, 0.f, 0.7071068f);
        a->SetPositionAndRotation(Vector3(1.f, 2.f, 3.f), qz90);
        Check(Near(a->GetPosition(), Vector3(1.f, 2.f, 3.f)), "TE2",
              "SetPositionAndRotation sets the WORLD position");
        // root 在 (10,0,0)，所以世界 (1,2,3) 的 local 是 (-9,2,3)。**不是** (1,2,3)
        // —— 这一条把"写成了 SetLocalPositionAndRotation"直接钉死。
        Check(Near(a->GetLocalPosition(), Vector3(-9.f, 2.f, 3.f)), "TE3",
              "and the local position is offset by the parent (so it really was world space)");
        Check(Near(a->GetForward(), Vector3(1.f, 0.f, 0.f)), "TE4",
              "and the rotation was applied too (forward -Y -> +X), not just the position");

        a->SetLocalPositionAndRotation(Vector3(4.f, 5.f, 6.f), Quaternion::IDENTITY);
        Check(Near(a->GetLocalPosition(), Vector3(4.f, 5.f, 6.f)), "TE5",
              "SetLocalPositionAndRotation sets the LOCAL position");
        Check(Near(a->GetPosition(), Vector3(14.f, 5.f, 6.f)), "TE6",
              "and the world position follows the parent (so it really was local space)");
        Check(Near(a->GetForward(), Vector3(0.f, -1.f, 0.f)), "TE7",
              "and the local rotation was reset to identity (forward back to -Y)");

        // --- 矩阵属性 -------------------------------------------------------
        // ZSCRIPTPROPERTY 挂的是已有的 Get* 方法，所以 C++ 侧只能验"两个矩阵互为
        // 逆"这件事本身（属性拼写由 TS 探针 TEW / ScriptComponentSmokeTest 覆盖）。
        const Matrix4x4 l2w = a->GetLocalToWorldMatrix();
        const Matrix4x4 w2l = a->GetWorldToLocalMatrix();
        Check(Near(l2w * w2l, Matrix4x4::IDENTITY), "TE8", "localToWorld * worldToLocal is the identity");
        Check(Near(w2l * l2w, Matrix4x4::IDENTITY), "TE9", "and it holds both ways round");
        // 局部原点变换过去就是世界位置：矩阵没跟上 TRS 的话这条先炸。
        Check(Near(l2w * Vector3::ZERO, a->GetPosition()), "TE10",
              "localToWorld maps the local origin onto GetPosition()");

        // --- hasChanged ------------------------------------------------------
        // 加载刚完是 false —— 加载是基准线，不是一次改动。少这一条的话脚本一拿到
        // 新 transform 就看到 true，"轮询 + 清零"的第一次必然误判成"被改过"。
        // ⚠️ 用**全新的** transform 判，不能用 a：a 上面刚被设过好几次。
        GameObject* o_fresh = nullptr;
        Transform* fresh = MakeTransform(*om, o_fresh);
        Check(fresh != nullptr && fresh->GetHasChanged() == false, "TE11b",
              "a freshly loaded transform starts with hasChanged == false");
        a->SetHasChanged(false);
        Check(a->GetHasChanged() == false, "TE11", "hasChanged can be cleared");
        a->SetLocalPosition(Vector3(0.f, 0.f, 0.f));
        Check(a->GetHasChanged(), "TE12", "and any local change sets it");
        // 读取**不**清零：轮询式用法如果读一次就清，第二次读必然是 false。
        Check(a->GetHasChanged(), "TE13", "reading hasChanged does NOT clear it");
        a->SetHasChanged(false);
        Check(a->GetHasChanged() == false, "TE14", "and writing false clears it again");

        // 只标记自己：子节点动不影响父节点（Unity 同）。
        root->SetHasChanged(false);
        g->SetLocalPosition(Vector3(0.f, 1.f, 0.f));
        Check(g->GetHasChanged(), "TE15", "a child's change flags the child");
        Check(root->GetHasChanged() == false, "TE16", "but NOT the parent (hasChanged is per-transform)");
        // 与 Component::IsDirty() 是两回事：后者每帧被消费，不设 false 也能变回
        // false。这里的要求是"只有 SetHasChanged(false) 能清"。
        Check(a->GetHasChanged() == false && a->IsDirty() == true, "TE17",
              "hasChanged is a flag of its own, not Component::IsDirty()");

        // --- hierarchyCount / hierarchyCapacity ------------------------------
        Check(a->GetHierarchyCount() == 5, "TE18",
              "hierarchyCount is the WHOLE hierarchy (5 nodes), not the subtree (2)");
        Check(root->GetHierarchyCount() == 5, "TE19", "and the root reads the same number");
        Check(g->GetHierarchyCount() == 5, "TE20", "and so does a grandchild");
        Check(root->GetHierarchyCapacity() >= root->GetHierarchyCount(), "TE21",
              "capacity is never smaller than the count");

        // 扩容：要 64，拿到的必须 >= 64（只增不减那一条由 TE24 覆盖）。
        root->SetHierarchyCapacity(64);
        Check(root->GetHierarchyCapacity() >= 64, "TE22", "SetHierarchyCapacity grows to at least what was asked");
        Check(root->GetHierarchyCount() == 5, "TE23", "and growing does not change the node count");

        // 扩容之后还**能继续加节点**：这才是"扩容没把层级搞坏"的判据。只断言
        // 容量数字的话，深拷贝漏搬一两个数组（parent_indices / next_indices）
        // 完全看不出来。
        //
        // ⚠️ 这一段必须排在 MoveAfterSibling **之后**：ChildOrder3 的判据是整串
        // 顺序，多挂一个 D 进去后面每一条都得多写一个 '?'，而 '?' 一多，"跑到
        // 最后一位"这种断言就认不出来了（D 掺在中间也还是 '?'）。
        GameObject* o_d_ref = o_d;
        Transform* d = MakeTransform(*om, o_d_ref);
        Check(d != nullptr, "TE24", "a sixth transform exists");
        if (d != nullptr)
        {
            d->SetParent(root, false);
            Check(root->GetHierarchyCount() == 6, "TE25",
                  "the hierarchy still counts correctly after a capacity grow");
            Check(root->GetChildrenCount() == 4, "TE26", "and the new child really got attached");

            // 只增不减：要 1 不能把容量压到 6 以下。
            root->SetHierarchyCapacity(1);
            Check(root->GetHierarchyCapacity() >= root->GetHierarchyCount(), "TE27",
                  "a smaller capacity than the count is ignored (never shrinks)");
            Check(root->GetHierarchyCount() == 6, "TE28", "and nothing was dropped");
            root->SetHierarchyCapacity(-7);
            Check(root->GetHierarchyCount() == 6, "TE29", "a negative capacity is ignored, not a crash");
        }
    }

    /// `MoveAfterSibling` 单独一段：它需要一个"正好三个儿子"的父节点，所以不能
    /// 和上面扩容那一段共用夹具（那段会挂上第四个）。
    void TestTransformMoveAfterSibling()
    {
        ObjectManager* om = GET_SYSTEM(ObjectManager);
        if (om == nullptr)
        {
            om = new ObjectManager();
            SystemRegistry::GetInstance().RegisterSystem(om);
            om->Initialize();
        }
        Check(om != nullptr, "MA0", "ObjectManager is reachable");
        if (om == nullptr)
        {
            return;
        }

        GameObject* o_root = nullptr;
        GameObject* o_a = nullptr;
        GameObject* o_b = nullptr;
        GameObject* o_c = nullptr;
        GameObject* o_g = nullptr;
        Transform* root = MakeTransform(*om, o_root);
        Transform* a = MakeTransform(*om, o_a);
        Transform* b = MakeTransform(*om, o_b);
        Transform* c = MakeTransform(*om, o_c);
        Transform* g = MakeTransform(*om, o_g);
        Check(root != nullptr && a != nullptr && b != nullptr && c != nullptr && g != nullptr,
              "MA1", "five transforms exist");
        if (root == nullptr || a == nullptr || b == nullptr || c == nullptr || g == nullptr)
        {
            return;
        }
        a->SetParent(root, false);
        b->SetParent(root, false);
        c->SetParent(root, false);
        g->SetParent(a, false);

        // --- MoveAfterSibling -------------------------------------------------
        Check(ChildOrder3(root, a, b, c) == "ABC", "MA2", "the fixture starts in the order ABC");
        // ⚠️ "排到自己后面"必须在**自己不是最后一个**时判：A 在末尾时，移除自己
        // 之后目标下标会被夹回末尾，顺序碰巧不变 —— 少了那道闸也看不出来。
        a->MoveAfterSibling(a);
        Check(ChildOrder3(root, a, b, c) == "ABC", "MA2b",
              "MoveAfterSibling(self) is a no-op even when self is NOT last");
        a->MoveAfterSibling(b);
        Check(ChildOrder3(root, a, b, c) == "BAC", "MA3", "MoveAfterSibling(b) moves A after B");
        Check(a->GetSiblingIndex() == 1 && b->GetSiblingIndex() == 0 && c->GetSiblingIndex() == 2, "MA4",
              "and GetSiblingIndex agrees with the new order");

        // 往前挪（跨过目标）：self 在目标之后时，移除自己**不会**让目标左移，
        // 目标下标要 +1。写错成统一 +1 的话这一步是空操作。
        b->MoveAfterSibling(a);
        Check(ChildOrder3(root, a, b, c) == "ABC", "MA5",
              "moving forwards across the target works (the off-by-one case)");

        a->MoveAfterSibling(c);
        Check(ChildOrder3(root, a, b, c) == "BCA", "MA6", "A can move to the very end");
        c->MoveAfterSibling(b);
        Check(ChildOrder3(root, a, b, c) == "BCA", "MA7", "already-after is a stable no-op, not a swap");

        // 三种静默空操作。
        const std::string before = ChildOrder3(root, a, b, c);
        a->MoveAfterSibling(nullptr);
        Check(ChildOrder3(root, a, b, c) == before, "MA8", "MoveAfterSibling(null) is a no-op");
        a->MoveAfterSibling(a);
        Check(ChildOrder3(root, a, b, c) == before, "MA9", "MoveAfterSibling(self) is a no-op");
        root->MoveAfterSibling(a);
        Check(ChildOrder3(root, a, b, c) == before, "MA11", "a root has no siblings to move among");
        // 跨父级：G 挂在 A 下、B 挂在 root 下。真搬要用 SetParent（它要决定
        // worldPositionStays），这里猜不出来，所以什么都不做。
        g->MoveAfterSibling(b);
        Check(g->GetParent() == a, "MA12", "MoveAfterSibling across parents does NOT re-parent");

        // 改顺序也要置脏：只改 m_Children 而忘了 MarkTransformDirty，表现是
        // "UI 上的顺序变了、渲染顺序没变"，而且只在有层级同步时才看得见。
        a->SetHasChanged(false);
        a->MoveAfterSibling(b);
        Check(a->GetHasChanged(), "MA13", "reordering marks the transform as changed");
    }

    // ---------------------------------------------------------------- CX ---
    /// `Color` 补齐面 + `ColorUtility`：grayscale / maxColorComponent / linear /
    /// gamma / ToString / ToHtmlString* / TryParseHtmlString /
    /// CorrelatedColorTemperatureToRGB。
    ///
    /// 这一片每个判据都挑"替代实现也满足"之外的那个点：
    ///   * `grayscale` 的权重不是 1/3（纯绿 0.587、纯蓝 0.114）；
    ///   * HTML 串走**四舍五入**（0.5 -> 0x80），`Color32` 走**截断**（0.5 -> 127）；
    ///   * 缩写形式每位 `*17` 不是 `*16`（"#FFF" 必须是纯白 FF 而不是 F0）；
    ///   * 解析失败返回**全 0**（含 alpha），不是保留调用方原值。
    void TestColorExtras()
    {
        // ---- grayscale ----------------------------------------------------
        // ⚠️ 三个纯色的亮度各不相同 —— 这是"感知亮度"与"平均值"唯一的分水岭。
        // 用 (r+g+b)/3 顶替的话三条都是 1/3，只有这条抓得住。
        Check(Near(Runtime::Color::GetGrayscale(MakeColor(1.f, 0.f, 0.f, 1.f)), 0.299f), "CX0",
              "grayscale(red) is 0.299 (Rec.601), not 1/3");
        Check(Near(Runtime::Color::GetGrayscale(MakeColor(0.f, 1.f, 0.f, 1.f)), 0.587f), "CX1",
              "grayscale(green) is 0.587 -- brighter than red");
        Check(Near(Runtime::Color::GetGrayscale(MakeColor(0.f, 0.f, 1.f, 1.f)), 0.114f), "CX2",
              "grayscale(blue) is 0.114 -- much darker than green");
        Check(Near(Runtime::Color::GetGrayscale(MakeColor(1.f, 1.f, 1.f, 1.f)), 1.f) &&
                  Near(Runtime::Color::GetGrayscale(MakeColor(0.f, 0.f, 0.f, 1.f)), 0.f),
              "CX3", "grayscale(white) is 1 and grayscale(black) is 0");
        // alpha 不参与亮度（Unity 同）：把 alpha 也算进去的话，"按亮度选前景色"
        // 会在半透明背景上选错。
        Check(Near(Runtime::Color::GetGrayscale(MakeColor(1.f, 0.f, 0.f, 0.f)),
                   Runtime::Color::GetGrayscale(MakeColor(1.f, 0.f, 0.f, 1.f))),
              "CX4", "alpha does not affect grayscale");

        // ---- maxColorComponent -------------------------------------------
        Check(Near(Runtime::Color::GetMaxColorComponent(MakeColor(0.2f, 0.9f, 0.4f, 1.f)), 0.9f), "CX5",
              "maxColorComponent picks the largest of rgb");
        // 同样不含 alpha：alpha 是 1、rgb 全是 0.1 时答案是 0.1 而不是 1。
        Check(Near(Runtime::Color::GetMaxColorComponent(MakeColor(0.1f, 0.1f, 0.1f, 1.f)), 0.1f), "CX6",
              "maxColorComponent ignores alpha");

        // ---- linear / gamma ------------------------------------------------
        // 中灰 0.5 的线性值是 0.214 —— 两端（0 和 1）是不动点，所以"纯黑纯白怎么
        // 测都对"是这个函数的经典盲区，中间值才是判据。
        Check(Near(Runtime::Color::ToLinear(MakeColor(0.5f, 0.5f, 0.5f, 1.f)).r, 0.2140f, 1e-3f), "CX7",
              "ToLinear(0.5) is 0.2140, not 0.5 (sRGB is not linear)");
        Check(Near(Runtime::Color::ToLinear(MakeColor(0.f, 0.f, 0.f, 1.f)).r, 0.f) &&
                  Near(Runtime::Color::ToLinear(MakeColor(1.f, 1.f, 1.f, 1.f)).r, 1.f),
              "CX8", "and the endpoints are fixed points");
        // 往返：gamma -> linear -> gamma 得回原值。
        const ::Color mid = MakeColor(0.5f, 0.25f, 0.75f, 0.3f);
        Check(SameColor(Runtime::Color::ToGamma(Runtime::Color::ToLinear(mid)), mid), "CX9",
              "ToGamma(ToLinear(c)) round-trips");
        // alpha 不参与转换：把 alpha 也转了的话，"同一个颜色不同透明度"会变出
        // 两种不同的 RGB 观感。
        Check(Near(Runtime::Color::ToLinear(mid).a, 0.3f) && Near(Runtime::Color::ToGamma(mid).a, 0.3f),
              "CX10", "neither linear nor gamma touches alpha");

        // ---- ToString ------------------------------------------------------
        Check(Runtime::Color::ToString(MakeColor(1.f, 0.f, 0.f, 1.f)) == "RGBA(1.000, 0.000, 0.000, 1.000)",
              "CX11", "Color.ToString is 'RGBA(1.000, 0.000, 0.000, 1.000)'");
        // ⚠️ 三位小数，不是 0..255 的整数 —— 与 Color32.ToString 的分水岭。
        Check(Runtime::Color::ToString(MakeColor(0.5f, 0.5f, 0.5f, 0.5f)) == "RGBA(0.500, 0.500, 0.500, 0.500)",
              "CX12", "and it prints 0..1 floats, not 0..255 bytes");

        // ---- ColorUtility.ToHtmlString* ------------------------------------
        Check(Runtime::ColorUtility::ToHtmlStringRGB(MakeColor(1.f, 0.f, 0.f, 1.f)) == "FF0000", "CX13",
              "ToHtmlStringRGB(red) is 'FF0000' (no '#', no alpha)");
        // RGB 版**丢掉** alpha：把 alpha 拼进去的话，同一个颜色在两条路上会得到
        // 两个串，配色表就对不上了。
        Check(Runtime::ColorUtility::ToHtmlStringRGB(MakeColor(1.f, 0.f, 0.f, 0.25f)) == "FF0000", "CX14",
              "the RGB form drops alpha");
        Check(Runtime::ColorUtility::ToHtmlStringRGBA(MakeColor(1.f, 0.f, 0.f, 0.25f)) == "FF000040", "CX15",
              "the RGBA form keeps alpha (0.25 -> 40)");
        // ⚠️ 四舍五入 vs 截断：0.5 * 255 = 127.5，这里是 **128 = 0x80**。
        // `Color32(Color)` 是截断（127 = 0x7F）—— 复用它的实现会差一位。
        Check(Runtime::ColorUtility::ToHtmlStringRGB(MakeColor(0.5f, 0.5f, 0.5f, 1.f)) == "808080", "CX16",
              "the HTML form ROUNDS (0.5 -> 0x80), unlike Color32 which truncates (0.5 -> 127)");
        // 越界先夹再转，不夹的话 snprintf 会打出一串负数。
        Check(Runtime::ColorUtility::ToHtmlStringRGB(MakeColor(2.f, -1.f, 0.f, 1.f)) == "FF0000", "CX17",
              "out-of-range channels are clamped before formatting");

        // ---- ColorUtility.TryParseHtmlString --------------------------------
        ColorParseResult parsed = Runtime::ColorUtility::TryParseHtmlString("#FF0000");
        Check(parsed.valid, "CX18", "'#FF0000' parses");
        Check(SameColor(parsed.color, MakeColor(1.f, 0.f, 0.f, 1.f)), "CX19", "and it is opaque red");
        // '#' 可有可无。
        Check(Runtime::ColorUtility::TryParseHtmlString("FF0000").valid, "CX20", "the leading '#' is optional");
        // 大小写不敏感。
        Check(SameColor(Runtime::ColorUtility::TryParseHtmlString("#ff8800").color,
                        Runtime::ColorUtility::TryParseHtmlString("#FF8800").color),
              "CX21", "hex digits are case-insensitive");
        // 8 位带 alpha。
        parsed = Runtime::ColorUtility::TryParseHtmlString("#FF000040");
        Check(parsed.valid && SameColor(parsed.color, MakeColor(1.f, 0.f, 0.f, 0.2509f)), "CX22",
              "an 8-digit string carries alpha");
        // ⚠️ 缩写形式每位 `*17` 不是 `*16`：'#FFF' 必须是**纯白** FF 而不是 F0。
        // 差在最低位，肉眼看不出，但和 Unity 对不上。
        Check(SameColor(Runtime::ColorUtility::TryParseHtmlString("#FFF").color,
                        MakeColor(1.f, 1.f, 1.f, 1.f)),
              "CX23", "the 3-digit form expands each nibble with *17 ('#FFF' is pure white)");
        Check(SameColor(Runtime::ColorUtility::TryParseHtmlString("#F0A").color,
                        Runtime::ColorUtility::TryParseHtmlString("#FF00AA").color),
              "CX24", "'#F0A' equals '#FF00AA'");
        parsed = Runtime::ColorUtility::TryParseHtmlString("#F0A8");
        Check(parsed.valid && SameColor(parsed.color, Runtime::ColorUtility::TryParseHtmlString("#FF00AA88").color),
              "CX25", "and the 4-digit form does the same for alpha");
        // 6 位时 alpha 补 1（不是补 0 —— 补 0 的话解析出来的颜色全是透明的）。
        Check(Near(Runtime::ColorUtility::TryParseHtmlString("#123456").color.a, 1.f), "CX26",
              "a 6-digit string defaults alpha to 1, not 0");

        // 失败：valid == false 且 color 是**全 0**（含 alpha）。保留调用方原值的
        // 话，配置表里一个拼错的色值会变成"用了上一个颜色"，比"看不见"难查得多。
        // ⚠️ 别把 "FF00" 放进这张表 —— 它是**合法**的 4 位缩写（RGBA），Unity
        // 也认。要造非法的 4 位就用含非十六进制字符的那种。
        const char* bad[] = {"", "#", "#GGGGGG", "GG00", "#12345", "#FF0000F",
                             "1234567", "  FF0000", "0x123456", "#FF00Z0"};
        bool all_rejected = true;
        bool all_zeroed = true;
        for (const char* text : bad)
        {
            const ColorParseResult r = Runtime::ColorUtility::TryParseHtmlString(text);
            if (r.valid) { all_rejected = false; }
            if (!(r.color.r == 0.f && r.color.g == 0.f && r.color.b == 0.f && r.color.a == 0.f))
            {
                all_zeroed = false;
            }
        }
        Check(all_rejected, "CX27", "malformed strings are rejected (length, non-hex, stray space)");
        Check(all_zeroed, "CX28", "and a rejected parse yields all-zero colour, not a leftover value");

        // 往返：串 -> 颜色 -> 串 得回同一个串（大小写归一到大写）。
        Check(Runtime::ColorUtility::ToHtmlStringRGBA(
                  Runtime::ColorUtility::TryParseHtmlString("#1A2B3C4D").color) == "1A2B3C4D",
              "CX29", "parse then format round-trips");

        // ---- CorrelatedColorTemperatureToRGB --------------------------------
        // ⚠️ 见 ScriptColor.h 那条说明：这是**近似**，Unity 没公布曲线，所以这里
        // 判的是"物理上说得通的定性 + 单调性"，不是逐位一致的数值。
        const ::Color warm = Runtime::Color::CorrelatedColorTemperatureToRGB(1500.f);
        const ::Color neutral = Runtime::Color::CorrelatedColorTemperatureToRGB(6500.f);
        const ::Color cold = Runtime::Color::CorrelatedColorTemperatureToRGB(10000.f);
        Check(Near(neutral.r, 1.f, 0.01f) && neutral.g > 0.95f && neutral.b > 0.95f, "CX30",
              "6500 K is very close to white (slightly warm)");
        // 暖：红拉满、蓝为 0、绿居中。
        Check(Near(warm.r, 1.f) && Near(warm.b, 0.f) && warm.g > 0.2f && warm.g < 0.8f, "CX31",
              "1500 K is orange (red saturated, no blue)");
        // 冷：蓝拉满、红被压下来。
        Check(Near(cold.b, 1.f) && cold.r < 0.95f && cold.r > 0.5f, "CX32",
              "10000 K is cold blue (blue saturated, red pulled down)");
        // 单调：色温升高 -> 蓝增加、红减少。
        Check(warm.b < neutral.b && neutral.b < cold.b && cold.r < warm.r, "CX33",
              "blue rises and red falls as the temperature rises");
        Check(Near(warm.a, 1.f) && Near(neutral.a, 1.f) && Near(cold.a, 1.f), "CX34",
              "alpha is always 1");
        // 极端值不出 [0,1]：t 很小时 log(t) 是负的、幂函数在某段又会冲过 255，
        // 两个方向都要夹。
        //
        // ⚠️ 判据**不能用几个挑好的色温**：上夹真正被踩到只在 t 刚过 66 那一段
        // （6650 K 附近算出来是 256.9），而 6500 K 走的是"红恒为 255"那一支、
        // 10000 K 已经衰减到 201 —— 两个都测不到上夹。所以这里**扫一遍**，
        // 而不是挑点（变异 COM14 第一版就是这么漏的）。
        bool in_range = true;
        for (float kelvin = 500.f; kelvin <= 40000.f; kelvin += 50.f)
        {
            const ::Color c = Runtime::Color::CorrelatedColorTemperatureToRGB(kelvin);
            if (!(c.r >= 0.f && c.r <= 1.f && c.g >= 0.f && c.g <= 1.f && c.b >= 0.f && c.b <= 1.f))
            {
                in_range = false;
            }
        }
        Check(in_range, "CX35", "the whole 500K..40000K range stays inside [0,1] (both clamps are live)");
    }

    // ---------------------------------------------------------------- GR ---
    /// Unity 的 `Gradient`：颜色随时间变化。
    ///
    /// 判据挑的是"两条键序列"这个设计真正带来的差别：
    ///   * alpha **只**由 alphaKeys 决定，颜色键里自带的 a 不参与（GR4）；
    ///   * `GradientMode.Fixed` **只**作用于颜色键，alpha 键永远插值（GR7）；
    ///   * 键未排序也能用 —— 内部排序不是死代码（GR5）。
    // =========================================================================
    // Unity 的 Input 那一层（slice 51）
    //
    // 分两段：
    //   EN* —— 直接驱动 EnhancedInputSystem（不需要 WindowSystem：OnChar /
    //         OnTouch / EndFrame 都不碰窗口），覆盖文本输入编码、连击判定、
    //         抬起那一帧仍可枚举这些真正的规则。
    //   IN* —— 走门面。这个进程没有 InputSystem，所以这一段断言的是**安全默认
    //         值**（headless 不崩、不返回假数据），以及 AdvanceAxis 那条纯函数
    //         规则（抽成公开静态方法就是为了能在这里直接喂数）。
    // =========================================================================
    /// 把一局 EnhancedInputSystem 的状态清干净，让下一段测试不带上一段的手指。
    /// 用"把所有手指抬起 + 滚两帧"来做，比加一个测试专用的 Reset() 好 —— 后者
    /// 会变成一条只有测试走的路，实现改了它也未必跟着改。
    static void enhanced_input_reset(EnhancedInputSystem& input)
    {
        using TouchAction = WindowSystem::TouchAction;
        const int kUp     = static_cast<int>(TouchAction::Up);
        for (int i = 0; i < input.GetFrameTouchCount(); ++i)
        {
            Touch touch;
            if (input.GetTouchAt(i, touch) && touch.fingerId >= 0)
            {
                input.OnTouch(touch.fingerId, kUp, touch.position.x, touch.position.y);
            }
        }
        input.EndFrame();
        input.EndFrame();
    }

    void TestInputParity()
    {
        // ---- 文本输入 ----
        EnhancedInputSystem input;

        input.OnChar('a');
        input.OnChar('b');
        input.EndFrame();
        Check(input.GetInputString() == "ab", "EN0", "OnChar accumulates into inputString");

        // 中文：U+4E2D -> E4 B8 AD
        input.OnChar(0x4E2Du);
        input.EndFrame();
        Check(input.GetInputString() == "\xE4\xB8\xAD", "EN1",
              "a CJK codepoint is encoded as 3 UTF-8 bytes");

        // ⚠️ 孤立代理对是坏数据：替它编一个 UTF-8 序列会让输入框显示成乱码，
        // 丢掉比显示 U+FFFD 好查。
        input.OnChar(0xD800u);
        input.EndFrame();
        Check(input.GetInputString().empty(), "EN2",
              "a lone surrogate is dropped, not encoded into a broken sequence");

        // 控制字符不进 inputString（回车走 GetKeyDown，不变成一个字符）。
        input.OnChar(static_cast<unsigned int>('\n'));
        input.OnChar(0x7Fu);
        input.EndFrame();
        Check(input.GetInputString().empty(), "EN3",
              "control characters never reach inputString");

        // emoji：U+1F600 -> F0 9F 98 80
        input.OnChar(0x1F600u);
        input.EndFrame();
        Check(input.GetInputString() == "\xF0\x9F\x98\x80", "EN4",
              "an astral codepoint is encoded as 4 UTF-8 bytes");

        // ⚠️ 再测一个 **跨过第 18 位** 的 4 字节码位（U+10FFFF -> F4 8F BF BF）。
        // 只测 U+1F600 是抓不出位移错的：0x1F600 >> 18 和 >> 17 都是 0，两种
        // 实现编出来的首字节一模一样（0xF0）。首字节里真正带位的是 >= U+40000
        // 的那一段，所以边界这一侧必须单独有一条。
        input.OnChar(0x10FFFFu);
        input.EndFrame();
        Check(input.GetInputString() == "\xF4\x8F\xBF\xBF", "EN43",
              "a 4-byte codepoint above the 18-bit boundary encodes its high bits");

        // 读数滚动之后收集器必须清空，否则每个字符会被读到两帧。
        input.EndFrame();
        Check(input.GetInputString().empty(), "EN5",
              "inputString is a one-frame reading (the accumulator is cleared)");

        // 2 字节：U+00E9 (é) -> C3 A9。四档长度各测一次 —— 位错一格在 3 字节上
        // 看不出来（会变成另一个汉字），在 2 字节上才是明显的乱码。
        input.OnChar(0x00E9u);
        input.EndFrame();
        Check(input.GetInputString() == "\xC3\xA9", "EN42",
              "a 2-byte codepoint is encoded as 2 UTF-8 bytes");

        // 夹紧：轴上界之外的原始输入不能传出 1（Unity 的轴恒在 [-1,1]）。
        Check(Input::AdvanceAxis(0.0f, 2.0f, 1.0f) == 1.0f, "IN42",
              "an out-of-range raw axis clamps to 1");

        // ---- 任意键 ----
        Check(!input.IsAnyKeyDown(), "EN6", "no keys held at start");
        Check(!input.WasAnyKeyPressedThisFrame(), "EN7", "and no press edge");
        input.OnKey(static_cast<int>(KeyCodes::Key::W), 0, KeyCodes::PRESS, 0);
        Check(input.IsAnyKeyDown(), "EN8", "anyKey follows a held key");
        input.EndFrame();
        Check(input.WasAnyKeyPressedThisFrame(), "EN9", "anyKeyDown sees the press edge");
        input.OnKey(static_cast<int>(KeyCodes::Key::W), 0, KeyCodes::RELEASE, 0);
        Check(!input.IsAnyKeyDown(), "EN10", "and clears once it is released");

        // ---- 触摸：相位与生命周期 ----
        using TouchAction = WindowSystem::TouchAction;
        const int kDown   = static_cast<int>(TouchAction::Down);
        const int kMove   = static_cast<int>(TouchAction::Move);
        const int kUp     = static_cast<int>(TouchAction::Up);
        const int kCancel = static_cast<int>(TouchAction::Cancel);

        enhanced_input_reset(input);

        input.OnTouch(7, kDown, 10.0f, 20.0f);
        Check(input.GetFrameTouchCount() == 1, "EN11", "a fresh touch is enumerable");
        Touch began;
        Check(input.GetTouchAt(0, began), "EN12", "and readable");
        Check(began.fingerId == 7, "EN13", "the finger id is the host's pointer id");
        Check(began.phase == TouchPhase::Began, "EN14", "and the phase is Began");
        Check(began.tapCount == 1, "EN15", "the first press is tap 1");
        Check(Near(began.position.x, 10.0f) && Near(began.position.y, 20.0f), "EN16",
              "position is the reported point");
        Check(Near(began.deltaPosition.x, 0.0f), "EN17",
              "and Began has no delta (there is no previous point yet)");
        // 不支持压力的设备上 Unity 规定是 1.0，不是 0。
        Check(Near(began.pressure, 1.0f) && Near(began.maximumPossiblePressure, 1.0f), "EN18",
              "pressure is Unity's 1.0 on devices that cannot report it");
        Check(began.type == TouchType::Direct, "EN19", "and the type is Direct");

        input.OnTouch(7, kMove, 12.0f, 25.0f);
        Touch moved;
        input.GetTouchAt(0, moved);
        Check(moved.phase == TouchPhase::Moved, "EN20", "a real move is phase Moved");
        Check(Near(moved.deltaPosition.x, 2.0f) && Near(moved.deltaPosition.y, 5.0f), "EN21",
              "deltaPosition is this frame's movement");

        // ⚠️ 收帧之后还必须是 Moved。"这一帧真的动过"和"上一帧动过"是两个状态：
        // EndFrame 只把**本帧没动**的 Moved 收成 Stationary，动过的要留着，
        // 否则拖动脚本在同一帧的收尾处会读到 Stationary 而当成本帧没有输入。
        input.EndFrame();
        Touch moved_after;
        input.GetTouchAt(0, moved_after);
        Check(moved_after.phase == TouchPhase::Moved, "EN44",
              "a touch that moved this frame is still Moved after EndFrame()");

        // 原地"移动"（宿主每帧都推 move 事件是常态）=> Stationary
        input.EndFrame();
        input.OnTouch(7, kMove, 12.0f, 25.0f);
        input.EndFrame();
        Touch stationary;
        input.GetTouchAt(0, stationary);
        Check(stationary.phase == TouchPhase::Stationary, "EN22",
              "a move event that did not move collapses to Stationary");
        Check(Near(stationary.deltaPosition.x, 0.0f), "EN23", "and its delta is zero");

        // ---- 抬起那一帧仍要能枚举到 ----
        input.OnTouch(7, kUp, 12.0f, 25.0f);
        Check(input.GetFrameTouchCount() == 1, "EN24",
              "an ended touch is still enumerable for that one frame");
        Touch ended;
        input.GetTouchAt(0, ended);
        Check(ended.phase == TouchPhase::Ended, "EN25", "with phase Ended");
        Check(input.GetTouchCount() == 0, "EN26",
              "but GetTouchCount() (the internal stick/camera path) already dropped it");
        input.EndFrame();
        Check(input.GetFrameTouchCount() == 0, "EN27",
              "and it is gone the frame after");

        // Canceled 与 Ended 必须是两个不同的相位（系统打断 / 来电 与 正常松手
        // 在玩法上处理不同）。
        input.OnTouch(8, kDown, 1.0f, 1.0f);
        input.EndFrame();
        input.OnTouch(8, kCancel, 1.0f, 1.0f);
        Touch canceled;
        input.GetTouchAt(0, canceled);
        Check(canceled.phase == TouchPhase::Canceled, "EN28", "a cancel reports Canceled");

        // ---- 连击 ----
        enhanced_input_reset(input);
        input.OnTouch(9, kDown, 50.0f, 50.0f);
        input.OnTouch(9, kUp, 50.0f, 50.0f);
        input.EndFrame();
        input.OnTouch(9, kDown, 52.0f, 51.0f);
        Touch second_tap;
        input.GetTouchAt(0, second_tap);
        Check(second_tap.tapCount == 2, "EN29",
              "a second press inside the window and slop is tap 2");

        // 落点太远：那是"在屏幕另一头点了第二下"，不是双击。
        enhanced_input_reset(input);
        input.OnTouch(11, kDown, 10.0f, 10.0f);
        input.OnTouch(11, kUp, 10.0f, 10.0f);
        input.EndFrame();
        input.OnTouch(11, kDown, 400.0f, 400.0f);
        Touch far_tap;
        input.GetTouchAt(0, far_tap);
        Check(far_tap.tapCount == 1, "EN30", "a press beyond the slop is tap 1, not 2");

        // 超窗：等过双击窗口再按，连击数必须从头开始。
        enhanced_input_reset(input);
        input.OnTouch(12, kDown, 10.0f, 10.0f);
        input.OnTouch(12, kUp, 10.0f, 10.0f);
        input.EndFrame();
        std::this_thread::sleep_for(std::chrono::milliseconds(350));
        input.OnTouch(12, kDown, 10.0f, 10.0f);
        Touch slow_tap;
        input.GetTouchAt(0, slow_tap);
        Check(slow_tap.tapCount == 1, "EN31", "a press past the double-tap window is tap 1");

        // ---- 枚举顺序：还按着的在前（按 id 升序），本帧结束的在后 ----
        enhanced_input_reset(input);
        input.OnTouch(3, kDown, 1.0f, 1.0f);
        input.OnTouch(1, kDown, 2.0f, 2.0f);
        input.OnTouch(5, kDown, 3.0f, 3.0f);
        input.EndFrame();
        input.OnTouch(5, kUp, 3.0f, 3.0f);
        Check(input.GetFrameTouchCount() == 3, "EN32", "two live plus one just-ended");
        Touch first;
        Touch second;
        Touch third;
        input.GetTouchAt(0, first);
        input.GetTouchAt(1, second);
        input.GetTouchAt(2, third);
        Check(first.fingerId == 1 && second.fingerId == 3, "EN33",
              "live touches come first, ordered by id (the map order is not stable)");
        Check(first.phase == TouchPhase::Began && second.phase == TouchPhase::Began, "EN34",
              "and they are still live");
        Check(third.phase == TouchPhase::Ended && third.fingerId == 5, "EN35",
              "the ended one is appended after them");
        Check(!input.GetTouchAt(3, third) && !input.GetTouchAt(-1, third), "EN36",
              "GetTouchAt out of range returns false");

        // ---- ResetInputAxes ----
        enhanced_input_reset(input);
        input.OnScroll(0.0, 3.0);
        input.EndFrame();
        Check(Near(input.GetScrollDeltaY(), 3.0f), "EN37", "the scroll reading rolls over");
        input.ResetInputAxes();
        Check(Near(input.GetScrollDeltaY(), 0.0f), "EN38",
              "ResetInputAxes zeroes the axis accumulators");
        // ⚠️ ResetInputAxes 不能松开按键：Unity 的语义是"轴归零"，不是"松手"。
        input.OnKey(static_cast<int>(KeyCodes::Key::D), 0, KeyCodes::PRESS, 0);
        input.ResetInputAxes();
        Check(input.IsAnyKeyDown(), "EN39",
              "ResetInputAxes does NOT release held keys (Unity zeroes axes only)");

        // ---- 门面：没有输入系统时的安全默认值 ----
        Check(!Input::GetAnyKey(), "IN0", "anyKey is false with no input system");
        Check(!Input::GetAnyKeyDown(), "IN1", "and so is anyKeyDown");
        Check(Input::GetInputString().empty(), "IN2", "inputString is empty");
        Check(Input::GetAxis("Horizontal") == 0.0f, "IN3", "GetAxis is zero");
        Check(Input::GetAxisRaw("Nope") == 0.0f, "IN4", "an unknown axis name is zero, not a throw");
        Check(!Input::GetButton("Fire1"), "IN5", "GetButton is false");
        Check(!Input::GetButtonDown("Jump") && !Input::GetButtonUp("Jump"), "IN6",
              "and neither button edge fires");
        Check(Input::GetFrameTouchCount() == 0, "IN7", "touchCount is zero");
        Check(Input::GetTouches().empty(), "IN8", "touches is empty");
        Check(Input::GetTouch(0).fingerId == -1, "IN9",
              "GetTouch out of range carries the -1 sentinel");
        Check(Input::GetTouch(0).phase == TouchPhase::Canceled, "IN10",
              "and a Canceled phase, so scripts can test 'no such finger'");
        Check(Input::GetJoystickNames().empty(), "IN11",
              "joystick names are empty (there is no gamepad backend, not a fake name)");
        Check(Input::GetDeviceOrientation() == DeviceOrientation::Unknown, "IN12",
              "deviceOrientation is Unknown with no orientation sensor");
        Check(Input::GetAcceleration() == Vector2::ZERO && Input::GetGravity() == Vector2::ZERO,
              "IN13", "acceleration / gravity are zero with no sensor");

        // ---- 平台能力 ----
        Check(Input::GetMousePresent(), "IN14", "a desktop process reports a mouse");
        Check(!Input::GetTouchSupported(), "IN15",
              "and no touch (the desktop backend has no WM_TOUCH path)");
        Check(!Input::GetStylusTouchSupported(), "IN16", "no stylus support");
        Check(!Input::GetTouchPressureSupported(), "IN17", "no pressure support");

        // ---- 进程级设置回环 ----
        Check(Input::GetMultiTouchEnabled(), "IN18", "multiTouchEnabled defaults to true");
        Input::SetMultiTouchEnabled(false);
        Check(!Input::GetMultiTouchEnabled(), "IN19", "and round-trips");
        Input::SetMultiTouchEnabled(true);

        Check(!Input::GetBackButtonLeavesApp(), "IN20", "backButtonLeavesApp defaults to false");
        Input::SetBackButtonLeavesApp(true);
        Check(Input::GetBackButtonLeavesApp(), "IN21", "and round-trips");
        Input::SetBackButtonLeavesApp(false);

        Check(Input::GetSimulateMouseWithTouches(), "IN22",
              "simulateMouseWithTouches defaults to true");
        Input::SetSimulateMouseWithTouches(false);
        Check(!Input::GetSimulateMouseWithTouches(), "IN23", "and round-trips");
        Input::SetSimulateMouseWithTouches(true);

        Check(Input::GetImeCompositionMode() == IMECompositionMode::Auto, "IN24",
              "imeCompositionMode defaults to Auto");
        Input::SetImeCompositionMode(IMECompositionMode::On);
        Check(Input::GetImeCompositionMode() == IMECompositionMode::On, "IN25",
              "and round-trips");
        Input::SetImeCompositionMode(IMECompositionMode::Auto);

        Check(!Input::GetImeIsSelected(), "IN26", "imeIsSelected is false (no IME backend)");
        Check(Input::GetCompositionString().empty(), "IN27", "compositionString is empty");

        // ---- 轴的重力 / 灵敏度 / 死区（纯函数，直接喂数）----
        Check(Near(Input::AdvanceAxis(0.0f, 1.0f, 1.0f / 60.0f), 3.0f / 60.0f), "IN28",
              "a held key ramps up at Sensitivity, not instantly");
        Check(Input::AdvanceAxis(0.0f, 1.0f, 1.0f) == 1.0f, "IN29",
              "and reaches 1 after a second");
        Check(Input::AdvanceAxis(0.5f, 1.0f, 1.0f) == 1.0f, "IN30",
              "the ramp is clamped at 1, never overshoots");
        Check(Near(Input::AdvanceAxis(1.0f, 0.0f, 1.0f / 60.0f), 1.0f - 3.0f / 60.0f), "IN31",
              "releasing decays at Gravity (Unity's little slide after you let go)");
        Check(Input::AdvanceAxis(1.0f, 0.0f, 1.0f) == 0.0f, "IN32", "and reaches zero");
        Check(Input::AdvanceAxis(-1.0f, 0.0f, 1.0f) == 0.0f, "IN33",
              "the negative side decays toward zero too (not past it)");
        // ⚠️ 死区要挑"没有死区就**非零**"的输入来测。原来那条用
        // AdvanceAxis(-0.0005, 0, 1) 是白测的：重力一帧就走 3.0，远超 0.0005，
        // 越零保护（IN33 那条）本来就把它压成 0 了 —— 死区在不在结果都一样。
        // 真正的缺口是"推进之后仍停在死区里"这两种：
        Check(Input::AdvanceAxis(0.0f, 0.0005f, 1.0f) == 0.0f, "IN34",
              "a raw reading below Dead leaves the axis at exactly 0");
        Check(Input::AdvanceAxis(0.0009f, 0.0f, 1.0f / 6000.0f) == 0.0f, "IN43",
              "and a gravity step too small to clear it snaps to 0 instead of jittering");
        Check(Input::AdvanceAxis(0.0f, -1.0f, 1.0f) == -1.0f, "IN35",
              "the ramp goes negative as well");

        // ---- 发布面 ----
        const std::string input_source =
            ReadFile("Source/Runtime/Scripting/ScriptInput/ScriptInput.h");
        Check(!input_source.empty(), "IN36", "ScriptInput.h is readable");
        Check(CountOccurrences(input_source, "ZFUNCTION()") >= 40, "IN37",
              "the Input facade publishes the Unity surface");
        Check(CountOccurrences(input_source, "ZSCRIPTPROPERTY(") >= 20, "IN38",
              "and it is reachable through Unity's property spellings");

        const std::string enhanced_source =
            ReadFile("Source/Runtime/Function/Input/EnhancedInputSystem.cpp");
        Check(!enhanced_source.empty(), "IN39", "EnhancedInputSystem.cpp is readable");
        Check(CountOccurrences(enhanced_source, "RegisterOnCharFunc") >= 1, "IN40",
              "Initialize() subscribes to the text-input callback");
        Check(CountOccurrences(enhanced_source, "m_EndedThisFrame.clear()") >= 1, "IN41",
              "EndFrame() retires the one-frame ended touches");
    }

    void TestGradient()
    {
        // 默认渐变：两端都是不透明白，不是空渐变。
        const ::Gradient fresh = Runtime::Gradient::Create();
        Check(SameColor(Runtime::Gradient::Evaluate(fresh, 0.5f), MakeColor(1.f, 1.f, 1.f, 1.f)), "GR0",
              "a fresh gradient is opaque white, not empty/black");

        std::vector<::GradientColorKey> colors(2);
        colors[0].color = MakeColor(1.f, 0.f, 0.f, 1.f);
        colors[0].time = 0.0f;
        colors[1].color = MakeColor(0.f, 0.f, 1.f, 1.f);
        colors[1].time = 1.0f;

        std::vector<::GradientAlphaKey> alphas(2);
        alphas[0].alpha = 0.0f;
        alphas[0].time = 0.0f;
        alphas[1].alpha = 1.0f;
        alphas[1].time = 1.0f;

        const ::Gradient g = Runtime::Gradient::SetKeys(fresh, colors, alphas);
        // 中点：红蓝各半。
        Check(SameColor(Runtime::Gradient::Evaluate(g, 0.5f), MakeColor(0.5f, 0.f, 0.5f, 0.5f)), "GR1",
              "Evaluate(0.5) blends red->blue AND the alpha track");
        // 两端之外取端点值，不循环。
        Check(SameColor(Runtime::Gradient::Evaluate(g, -3.f), MakeColor(1.f, 0.f, 0.f, 0.f)), "GR2",
              "before the first key it holds the first key's colour");
        Check(SameColor(Runtime::Gradient::Evaluate(g, 9.f), MakeColor(0.f, 0.f, 1.f, 1.f)), "GR3",
              "and after the last key it holds the last one's");
        // ⚠️ alpha 只由 alphaKeys 决定：把颜色键的 a 设成 0.2，结果仍是 0.5。
        // 这一条是"两条独立键序列"唯一能证伪的判据 —— 合并且只用颜色键的实现
        // 其余全部通过。
        std::vector<::GradientColorKey> colored_alpha = colors;
        colored_alpha[0].color = MakeColor(1.f, 0.f, 0.f, 0.2f);
        colored_alpha[1].color = MakeColor(0.f, 0.f, 1.f, 0.2f);
        Check(Near(Runtime::Gradient::Evaluate(Runtime::Gradient::WithColorKeys(g, colored_alpha), 0.5f).a, 0.5f),
              "GR4", "the colour keys' own alpha is ignored -- alphaKeys alone decide alpha");
        // 键未排序也要能用：换顺序之后中点值必须一样（否则"内部排序"是死代码）。
        std::vector<::GradientColorKey> shuffled = colors;
        std::swap(shuffled[0], shuffled[1]);
        std::vector<::GradientAlphaKey> shuffled_alphas = alphas;
        std::swap(shuffled_alphas[0], shuffled_alphas[1]);
        Check(SameColor(Runtime::Gradient::Evaluate(Runtime::Gradient::SetKeys(g, shuffled, shuffled_alphas), 0.5f),
                        Runtime::Gradient::Evaluate(g, 0.5f)),
              "GR5", "unsorted keys still evaluate correctly (the internal sort is live)");
        // Fixed：键之间不插值，取左侧那个键。
        const ::Gradient fixed = Runtime::Gradient::WithMode(g, 1);
        Check(Runtime::Gradient::GetMode(fixed) == 1, "GR6", "WithMode(1) selects GradientMode.Fixed");
        Check(SameColor(Runtime::Gradient::Evaluate(fixed, 0.5f), MakeColor(1.f, 0.f, 0.f, 0.5f)), "GR7",
              "Fixed takes the LEFT key's colour and does not blend");
        // ⚠️ 但 alpha 键**永远插值** —— Fixed 只管颜色。
        Check(Near(Runtime::Gradient::Evaluate(fixed, 0.5f).a, 0.5f), "GR8",
              "and Fixed does NOT freeze the alpha track (it stays interpolated)");
        // 未知 mode 一律当 Blend：存个 7 然后走 default 分支会静默变成插值。
        Check(Runtime::Gradient::GetMode(Runtime::Gradient::WithMode(g, 7)) == 0, "GR9",
              "an unknown mode falls back to Blend instead of being stored raw");
        // 非破坏：`WithColorKeys` 不改动原值（值是快照，原地改没意义）。
        std::vector<::GradientColorKey> green_only(1);
        green_only[0].color = MakeColor(0.f, 1.f, 0.f, 1.f);
        green_only[0].time = 0.0f;
        Runtime::Gradient::WithColorKeys(g, green_only);
        Check(SameColor(Runtime::Gradient::Evaluate(g, 0.5f), MakeColor(0.5f, 0.f, 0.5f, 0.5f)), "GR10",
              "WithColorKeys is non-destructive -- the original is untouched");
        Check(Runtime::Gradient::GetColorKeys(g).size() == 2 && Runtime::Gradient::GetAlphaKeys(g).size() == 2,
              "GR11", "GetColorKeys / GetAlphaKeys give the tracks back");
        // 空键序列的降级：白 + 不透明，不是黑 + 透明。
        const ::Gradient empty = Runtime::Gradient::SetKeys(fresh, std::vector<::GradientColorKey>(),
                                                           std::vector<::GradientAlphaKey>());
        Check(SameColor(Runtime::Gradient::Evaluate(empty, 0.5f), MakeColor(1.f, 1.f, 1.f, 1.f)), "GR12",
              "an empty gradient evaluates to opaque white (a black one looks like 'uncoloured')");
    }

    void TestGradientWiring()
    {
        const std::string header = ReadFile("Source/Runtime/Scripting/ScriptColor/ScriptColor.h");
        Check(!header.empty(), "GRW0", "ScriptColor.h is readable");
        if (header.empty()) { return; }
        Check(CountOccurrences(header, "class Gradient") >= 1, "GRW1", "Gradient facade is declared");
        Check(CountOccurrences(header, "ZFUNCTION()\n        static ::Color Evaluate(") >= 1, "GRW2",
              "Evaluate is published");
        Check(CountOccurrences(header, "ZFUNCTION()\n        static ::Gradient SetKeys(") >= 1, "GRW3",
              "SetKeys is published");
        // ⚠️ 门面**不能**给 colorKeys / alphaKeys 装访问器属性：setter 的返回值会
        // 被丢掉，装上就是个永久空操作（而且不报错）。这是"属性 vs 非破坏修改"的
        // 硬冲突，只能靠源码门守住。
        Check(CountOccurrences(header, "Name=\"colorKeys\"") == 0, "GRW4",
              "colorKeys has NO accessor property (a setter would silently discard the new value)");
        Check(CountOccurrences(header, "Name=\"alphaKeys\"") == 0, "GRW5",
              "alphaKeys has NO accessor property either");

        const std::string binding = ReadFile("Source/Runtime/Scripting/Binding/ScriptBinding.cpp");
        Check(!binding.empty(), "GRW6", "ScriptBinding.cpp is readable");
        if (!binding.empty())
        {
            Check(CountOccurrences(binding, "declare interface Gradient {") >= 1, "GRW7",
                  "Gradient is declared in the generated .d.ts");
            Check(CountOccurrences(binding, "declare interface GradientColorKey {") >= 1, "GRW8",
                  "and so is GradientColorKey");
            Check(CountOccurrences(binding, "declare interface GradientAlphaKey {") >= 1, "GRW9",
                  "and GradientAlphaKey (the two tracks are separate types, like in Unity)");
        }
    }

    // ---------------------------------------------------------------- CB ---
    /// `collider.bounds`（Unity 的 `Bounds`）与 `collider.ClosestPointOnBounds`。
    ///
    /// 夹具沿用 TestCollider：盒子半尺寸 (1,2,3)、本地偏移 z=+5、物体在 (10,0,0)
    /// ⇒ 世界 AABB 中心 (10,0,5)、半尺寸 (1,2,3)、min (9,-2,2)、max (11,2,8)。
    void TestColliderBounds()
    {
        ObjectManager* om = GET_SYSTEM(ObjectManager);
        if (om == nullptr)
        {
            om = new ObjectManager();
            SystemRegistry::GetInstance().RegisterSystem(om);
            om->Initialize();
        }
        Check(om != nullptr, "CB0", "ObjectManager is reachable");
        if (om == nullptr)
        {
            return;
        }

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
        box_shape->m_LocalTransform =
            TransformTRS(Vector3(0.f, 0.f, 5.f), Quaternion::IDENTITY, Vector3::UNIT_SCALE);
        rigidbody->GetRigidbodyResMutable().m_Shapes.push_back(box_shape);
        transform->SetRotation(Quaternion::IDENTITY);
        transform->SetPosition(Vector3(10.f, 0.f, 0.f));

        Collider* col = Collider::FromGameObject(go);
        Check(col != nullptr && col->IsValid(), "CB1", "the fixture has a valid collider");
        if (col == nullptr || !col->IsValid())
        {
            return;
        }

        const ::Bounds bounds = col->GetBounds();
        Check(Near(bounds.center, Vector3(10.f, 0.f, 5.f)), "CB2", "collider.bounds.center is the AABB centre");
        // ⚠️ **`extents` 是半尺寸**：写成 size（2,4,6）的话所有"包围盒重叠"判断
        // 会把盒子当成两倍大 —— 而 `GetBoundsSize()` 那条断言照样通过。
        Check(Near(bounds.extents, Vector3(1.f, 2.f, 3.f)), "CB3",
              "collider.bounds.extents is the HALF size (1,2,3), not the full (2,4,6)");
        Check(Near(bounds.GetSize(), col->GetBoundsSize()), "CB4",
              "bounds.GetSize() agrees with the existing GetBoundsSize()");
        // 顺带证明"fixture 真的不是零盒"：零盒的话上面三条全绿但毫无意义。
        Check(!Near(bounds.extents, Vector3::ZERO), "CB5", "(the fixture really is non-degenerate)");

        // --- ClosestPointOnBounds vs ClosestPoint ----------------------------
        // 盒外点：两者同解。
        Check(Near(col->ClosestPointOnBounds(Vector3(10.f, 0.f, 20.f)), Vector3(10.f, 0.f, 8.f)), "CB6",
              "an outside point clamps onto the box, same as ClosestPoint");
        // ⚠️ 盒**内**点是两者唯一的分水岭：(10, 1.5, 5.5) 离 +y 面最近（0.5），
        // 于是被推到 (10, 2, 5.5)；`ClosestPoint` 则返回点自己。
        const Vector3 inside(10.f, 1.5f, 5.5f);
        Check(Near(col->ClosestPointOnBounds(inside), Vector3(10.f, 2.f, 5.5f)), "CB7",
              "an inside point is pushed onto the NEAREST face (+y here)");
        Check(Near(col->ClosestPoint(inside), inside), "CB8",
              "while ClosestPoint returns the point itself -- the one thing that separates them");
        // 只夹一个轴：三个轴都夹的话得到的是角点，不是"最近的面"。
        Check(Near(col->ClosestPointOnBounds(inside).x, 10.f) && Near(col->ClosestPointOnBounds(inside).z, 5.5f),
              "CB9", "and only ONE axis is clamped (it lands on a face, not a corner)");

        // --- 没有碰撞体时 ----------------------------------------------------
        GameObject* empty_go = new GameObject();
        om->AllocateAndAssignInstanceID(empty_go);
        Transform* empty_transform = new Transform();
        om->AllocateAndAssignInstanceID(empty_transform);
        empty_go->addComponent(empty_transform);
        empty_transform->PostLoadResource(empty_go);
        Collider* none_col = Collider::FromGameObject(empty_go);
        Check(none_col != nullptr, "CB10", "the invalid collider exists");
        if (none_col != nullptr)
        {
            const ::Bounds none_bounds = none_col->GetBounds();
            Check(Near(none_bounds.center, Vector3::ZERO) && Near(none_bounds.extents, Vector3::ZERO), "CB11",
                  "an invalid collider has a zero bounds (not a stale one)");
            Check(Near(none_col->ClosestPointOnBounds(Vector3(7.f, 8.f, 9.f)), Vector3(7.f, 8.f, 9.f)), "CB12",
                  "and ClosestPointOnBounds returns the point itself, not the origin");
        }
    }

    // ---------------------------------------------------------------- GB ---
    /// `GameObject.name` / `GameObject.transform` 的属性拼写。
    ///
    /// 这两个在 C++ 里早就有了（`GetName` / `SetName` / `GetTransform`），缺的是
    /// **属性**。而"属性缺失"用任何 C++ 运行时测试都测不出来 —— d.ts 里没有
    /// `name: string`，tsc 才会报错。所以这里主判据是**源码门**。
    void TestGameObjectProperties()
    {
        const std::string header = ReadFile("Source/Runtime/BaseClasses/GameObject.h");
        Check(!header.empty(), "GB0", "GameObject.h is readable");
        if (header.empty()) { return; }
        // ⚠️ 判据带 `ZSCRIPTPROPERTY(Name=...)` 而不只是方法名：`GetName` /
        // `GetTransform` 一直在，只 grep 方法名的话门是永真的。
        Check(CountOccurrences(header, "ZSCRIPTPROPERTY(Name=\"name\", Getter=\"GetName\", Setter=\"SetName\")") >= 1,
              "GB1", "GameObject publishes `name` as a property (Unity's spelling)");
        Check(CountOccurrences(header, "ZSCRIPTPROPERTY(Name=\"transform\", Getter=\"GetTransform\")") >= 1,
              "GB2", "and `transform` (read-only, like Unity's)");
        // Unity 的 `go.transform` 是**只读**的：一个 GameObject 有且只有一个
        // Transform，"换掉它"没有意义。写成可写等于允许脚本把一个组件句柄指向
        // 别的东西。
        Check(CountOccurrences(header, "Setter=\"SetTransform\"") == 0, "GB3",
              "transform has no setter (a GameObject has exactly one Transform)");
        // 与 Component 一致：那边早就有 `name` / `transform`，两边不同形的话
        // "从 gameObject 还是从 component 拿"会变成两种写法。
        const std::string component = ReadFile("Source/Runtime/Function/Framework/Component/Component.h");
        Check(!component.empty(), "GB4", "Component.h is readable");
        if (!component.empty())
        {
            Check(CountOccurrences(component, "Name=\"transform\"") >= 1, "GB5",
                  "Component also publishes `transform` -- the two spellings stay in sync");
        }

        // 运行时只补一条：属性背后那个 getter 真的能拿到东西（门是源码、实现是
        // C++，两边都得真的接上）。
        ObjectManager* om = GET_SYSTEM(ObjectManager);
        if (om == nullptr)
        {
            om = new ObjectManager();
            SystemRegistry::GetInstance().RegisterSystem(om);
            om->Initialize();
        }
        if (om == nullptr) { return; }
        GameObject* go = new GameObject();
        om->AllocateAndAssignInstanceID(go);
        Transform* transform = new Transform();
        om->AllocateAndAssignInstanceID(transform);
        go->addComponent(transform);
        transform->PostLoadResource(go);
        go->SetName("ParityProbe");
        Check(go->GetName() == "ParityProbe", "GB6", "the `name` getter/setter pair really round-trips");
        Check(go->GetTransform() == transform, "GB7", "and `transform` points at the object's Transform");
    }

    void TestColorExtrasWiring()
    {
        const std::string header = ReadFile("Source/Runtime/Scripting/ScriptColor/ScriptColor.h");
        Check(!header.empty(), "CXW0", "ScriptColor.h is readable");
        if (header.empty()) { return; }
        Check(CountOccurrences(header, "class ColorUtility") >= 1, "CXW1", "ColorUtility is declared");
        // ⚠️ 带 `ZFUNCTION()`：只 grep 方法名的话，删掉 ZFUNCTION 之后"声明还在"
        // 依然为真，而脚本里那个名字已经不存在了（TEM15 同款坑）。
        Check(CountOccurrences(header, "ZFUNCTION()\n        static std::string ToHtmlStringRGB(") >= 1,
              "CXW2", "ToHtmlStringRGB is published");
        Check(CountOccurrences(header, "ZFUNCTION()\n        static ColorParseResult TryParseHtmlString(") >= 1,
              "CXW3", "TryParseHtmlString is published");
        Check(CountOccurrences(header, "ZFUNCTION()\n        static float GetGrayscale(") >= 1,
              "CXW4", "GetGrayscale is published");
        Check(CountOccurrences(header, "ZFUNCTION()\n        static ::Color ToLinear(") >= 1,
              "CXW5", "ToLinear is published");
        // ⚠️ 复用 `Mathf::GammaToLinearSpace`，不要抄第二份 sRGB 曲线。
        Check(CountOccurrences(header, "GammaToLinearSpace") >= 1, "CXW6",
              "the sRGB curve is documented as Mathf's");

        const std::string source = ReadFile("Source/Runtime/Scripting/ScriptColor/ScriptColor.cpp");
        Check(!source.empty(), "CXW7", "ScriptColor.cpp is readable");
        if (!source.empty())
        {
            Check(CountOccurrences(source, "Mathf::GammaToLinearSpace") >= 1, "CXW8",
                  "ToLinear really calls Mathf::GammaToLinearSpace (one curve, one owner)");
            // ⚠️ HTML 串走四舍五入：`::Color32(c)` 是截断，图省事复用它会差一位。
            Check(CountOccurrences(source, "ChannelToByte") >= 3, "CXW9",
                  "the HTML form has its own rounding helper, not ::Color32's truncation");
            Check(CountOccurrences(source, "* 17") >= 1, "CXW10",
                  "the short hex form expands with *17, not *16");
        }

        // 值类型结果必须在绑定层有 Converter，否则 ZFUNCTION 编不过（那是编译期
        // 的 static_assert，不是运行时）—— 但 d.ts 那一半是**字符串**，漏了不会
        // 报错，只会在 tsc 里变成 `Property 'valid' does not exist`。
        const std::string binding = ReadFile("Source/Runtime/Scripting/Binding/ScriptBinding.cpp");
        Check(!binding.empty(), "CXW11", "ScriptBinding.cpp is readable");
        if (!binding.empty())
        {
            Check(CountOccurrences(binding, "declare interface ColorParseResult") >= 1, "CXW12",
                  "ColorParseResult is declared in the generated .d.ts");
            Check(CountOccurrences(binding, "valid: boolean") >= 1, "CXW13",
                  "and it carries the `valid` flag TS has to check");
        }
    }

    void TestTransformExtrasWiring()
    {
        const std::string header = ReadFile("Source/Runtime/Function/Framework/Component/Transform/Transform.h");
        Check(!header.empty(), "TEW0", "Transform.h is readable");
        // ⚠️ 判据**必须带上 `ZFUNCTION()` 并连着声明那一行**：只 grep 方法名的
        // 话，删掉 ZFUNCTION 之后"声明还在"依然为真 —— 而脚本里那个名字已经是
        // 不存在的了（变异 TEM15 就是这么漏的）。
        Check(CountOccurrences(header, "ZFUNCTION()\n    void MoveAfterSibling(Transform* sibling);") >= 1,
              "TEW1", "MoveAfterSibling is published (ZFUNCTION sits on its declaration)");
        Check(CountOccurrences(header, "ZFUNCTION()\n    bool GetHasChanged() const { return m_HasChanged; }") >= 1,
              "TEW2", "GetHasChanged is published");
        Check(CountOccurrences(header, "SetHasChanged") >= 1, "TEW3", "SetHasChanged is declared");
        Check(CountOccurrences(header, "ZFUNCTION()\n    int GetHierarchyCount() const;") >= 1,
              "TEW4", "GetHierarchyCount is published");
        Check(CountOccurrences(header, "ZFUNCTION()\n    void SetHierarchyCapacity(int capacity);") >= 1,
              "TEW5", "SetHierarchyCapacity is published");
        // 属性拼写：getter 标了 ZFUNCTION 还不够，得有 ZSCRIPTPROPERTY 才会变成
        // `t.hasChanged` —— 少了它脚本只能用 `t.GetHasChanged()`，与 Unity 不同形。
        Check(CountOccurrences(header, "Name=\"hasChanged\"") >= 1, "TEW6", "hasChanged is a script property");
        Check(CountOccurrences(header, "Name=\"hierarchyCapacity\"") >= 1, "TEW7",
              "hierarchyCapacity is a script property");
        Check(CountOccurrences(header, "Name=\"hierarchyCount\"") >= 1, "TEW8",
              "hierarchyCount is a script property");
        Check(CountOccurrences(header, "Name=\"localToWorldMatrix\"") >= 1, "TEW9",
              "localToWorldMatrix is a script property");
        Check(CountOccurrences(header, "Name=\"worldToLocalMatrix\"") >= 1, "TEW10",
              "worldToLocalMatrix is a script property");
        // m_HasChanged 必须是**独立**成员：拿 Component::m_IsDirty 顶替的话，
        // 帧循环每帧清一次，轮询式用法永远读不到 true。
        Check(CountOccurrences(header, "m_HasChanged") >= 1, "TEW11", "hasChanged has its own member");

        const std::string source = ReadFile("Source/Runtime/Function/Framework/Component/Transform/Transform.cpp");
        Check(!source.empty(), "TEW12", "Transform.cpp is readable");
        // 置位必须在 MarkTransformDirty 里 —— 那里是所有改动的公共出口。写在
        // SetLocalPosition 里的话，SetParent / SetSiblingIndex 就漏了。
        Check(CountOccurrences(source, "m_HasChanged = true") >= 1, "TEW13",
              "MarkTransformDirty sets hasChanged");
        // 加载后清零：不清的话脚本一拿到刚加载的 transform 就看到 true。
        Check(CountOccurrences(source, "m_HasChanged = false") >= 1, "TEW14",
              "PostLoadResource clears hasChanged");
        // MoveAfterSibling 必须复用"修层级"的那一段，而不是自己改一遍 m_Children。
        Check(CountOccurrences(source, "ReorderWithinParent") >= 2, "TEW15",
              "MoveAfterSibling reuses the hierarchy-repair path (declared AND called)");
    }

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

    // =========================================================================
    // SystemInfo / QualitySettings / Cursor
    // =========================================================================

    void TestSystemInfo()
    {
        // ---- headless 下"拿不到"必须表现为哨兵值，不是 plausible 的假值 ----
        //
        // 这个可执行文件**不注册** RenderSystem，所以整条图形查询链都走
        // "没有 RHI" 那条路。这正是要测的那条路：真机上拿得到真值的路径反而
        // 没人会去怀疑它。
        Check(SystemInfo::GetGraphicsDeviceName() == "Unknown", "SI0",
              "graphicsDeviceName is 'Unknown' with no RHI (not a plausible fake)");
        Check(SystemInfo::GetGraphicsDeviceVendor() == "Unknown", "SI1",
              "graphicsDeviceVendor is 'Unknown' with no RHI");
        Check(SystemInfo::GetGraphicsMemorySize() == 0, "SI2",
              "graphicsMemorySize is 0 -- the RHI layer has no memory-properties query");
        Check(SystemInfo::GetGraphicsShaderLevel() == 0, "SI3",
              "graphicsShaderLevel is 0 with no RHI (0, not 50)");
        Check(SystemInfo::GetMaxTextureSize() == 0, "SI4",
              "maxTextureSize is 0 with no RHI (0, not 4096)");
        Check(!SystemInfo::GetSupportsInstancing(), "SI5",
              "supportsInstancing is FALSE with no RHI (not a blind true)");
        Check(!SystemInfo::GetSupportsComputeShaders(), "SI6",
              "supportsComputeShaders is FALSE with no RHI");
        // ⚠️ -1 而不是 0：0 会被 `if (batteryLevel < 0.2)` 之外的判断当成"有电"
        // 或"没电"，两者都是假信息。-1 是 Unity 的"不支持"哨兵。
        Check(SystemInfo::GetBatteryLevel() == -1.0f, "SI7",
              "batteryLevel is -1 on desktop (Unity's 'unsupported' sentinel, not 0)");
        Check(SystemInfo::GetBatteryStatus() == BatteryStatus::Unknown, "SI8",
              "batteryStatus is Unknown on desktop");

        // ---- 编译期平台能定死的那些 ----
        Check(SystemInfo::GetDeviceType() == DeviceType::Desktop, "SI9",
              "deviceType is Desktop on a desktop build");
        Check(SystemInfo::GetOperatingSystemFamily() == OperatingSystemFamily::Windows, "SI10",
              "operatingSystemFamily is Windows on a Windows build");
        Check(!SystemInfo::GetOperatingSystem().empty(), "SI11",
              "operatingSystem names something");
        // 枚举值与 Unity 一致（搬过来的 `== DeviceType.Desktop` 要能对上）。
        Check(static_cast<int>(DeviceType::Desktop) == 3 &&
                  static_cast<int>(OperatingSystemFamily::Windows) == 2 &&
                  static_cast<int>(BatteryStatus::Full) == 4,
              "SI12", "the three SystemInfo enums use Unity's numeric values");

        // ---- 真能从 OS 问到的那些 ----
        Check(SystemInfo::GetProcessorCount() > 0, "SI13",
              "processorCount is positive (hardware_concurrency resolved)");
        // hostname 在 CI 上也可能拿不到，所以只断言"不是空串"—— 空串会让
        // "按机器分组"把所有机器并成一组。
        Check(!SystemInfo::GetDeviceName().empty(), "SI14", "deviceName is non-empty");
        Check(SystemInfo::GetDeviceName() != "Unknown" || true, "SI15",
              "deviceName falls back to 'Unknown' rather than an empty string");
        Check(!SystemInfo::GetDeviceUniqueIdentifier().empty(), "SI16",
              "deviceUniqueIdentifier is non-empty");
        Check(SystemInfo::GetDeviceUniqueIdentifier().find("Windows") == 0, "SI17",
              "deviceUniqueIdentifier is platform-prefixed");

        // deviceModel 读不到就是 "Unknown"。返回主机名之类的"看起来合理"的值
        // 更危险：上报/分档逻辑会当成真机型用。
        Check(SystemInfo::GetDeviceModel() == "Unknown", "SI20",
              "deviceModel is the sentinel, not a plausible-looking model string");

        // ---- 哨兵值集中定义（改一处就全变）----
        const std::string si_source =
            ReadFile("Source/Runtime/Scripting/ScriptSystemInfo/ScriptSystemInfo.cpp");
        Check(!si_source.empty(), "SI18", "ScriptSystemInfo.cpp is readable");
        Check(CountOccurrences(si_source, "kUnknown") >= 8, "SI19",
              "the 'no data' sentinels are centralised in one place");
    }

    void TestQualitySettings()
    {
        QualitySettings::ResetToDefaults();

        // ---- 档位 ----
        Check(QualitySettings::GetQualityLevelCount() == 6, "QS0",
              "there are 6 quality levels");
        Check(QualitySettings::GetQualityLevel() == 2, "QS1",
              "the default level is 2 (Medium)");
        Check(QualitySettings::GetName() == "Medium", "QS2", "and its name is 'Medium'");
        Check(QualitySettings::GetNames().size() == 6, "QS3", "names has one entry per level");
        Check(QualitySettings::GetNames()[0] == "Very Low" &&
                  QualitySettings::GetNames()[5] == "Ultra",
              "QS4", "the level names are Unity's defaults");
        Check(QualitySettings::GetNameForLevel(3) == "High", "QS5", "GetNameForLevel(3) is 'High'");
        // 越界返回空串，不是夹紧到最近的一档。
        Check(QualitySettings::GetNameForLevel(99).empty(), "QS6",
              "GetNameForLevel(99) is empty (not clamped)");

        // ---- 切档会整体覆盖开关 ----
        QualitySettings::SetQualityLevel(4);
        Check(QualitySettings::GetQualityLevel() == 4, "QS7", "SetQualityLevel(4) sticks");
        Check(QualitySettings::GetAntiAliasing() == 8, "QS8", "level 4 raises MSAA to 8");
        Check(Near(QualitySettings::GetShadowDistance(), 150.0f), "QS9",
              "and shadow distance to 150");
        Check(static_cast<int>(QualitySettings::GetShadowResolution()) == 3, "QS10",
              "and shadow resolution to VeryHigh");
        Check(QualitySettings::GetSkinWeights() == SkinWeights::Unlimited, "QS11",
              "and skin weights to Unlimited");

        // 越界**不动任何东西**（Unity 的行为）。夹紧会让 SetQualityLevel(99)
        // 悄悄变成"最高档"。
        QualitySettings::SetQualityLevel(99);
        Check(QualitySettings::GetQualityLevel() == 4, "QS12",
              "SetQualityLevel(99) is ignored, not clamped");
        QualitySettings::SetQualityLevel(-1);
        Check(QualitySettings::GetQualityLevel() == 4, "QS13", "and so is -1");

        // ---- Increase / Decrease 在两端停住 ----
        QualitySettings::SetQualityLevel(5);
        QualitySettings::IncreaseLevel();
        Check(QualitySettings::GetQualityLevel() == 5, "QS14", "IncreaseLevel stops at the top");
        QualitySettings::SetQualityLevel(0);
        QualitySettings::DecreaseLevel();
        Check(QualitySettings::GetQualityLevel() == 0, "QS15", "DecreaseLevel stops at the bottom");
        QualitySettings::IncreaseLevel();
        Check(QualitySettings::GetQualityLevel() == 1, "QS16",
              "and IncreaseLevel from the bottom moves by one");

        // ---- 离散合法集一律向下取 ----
        QualitySettings::SetAntiAliasing(3);
        Check(QualitySettings::GetAntiAliasing() == 2, "QS17",
              "antiAliasing(3) snaps DOWN to 2, not up to 4");
        QualitySettings::SetAntiAliasing(9);
        Check(QualitySettings::GetAntiAliasing() == 8, "QS18", "antiAliasing(9) snaps to 8");
        QualitySettings::SetShadowCascades(3);
        Check(QualitySettings::GetShadowCascades() == 2, "QS19", "shadowCascades(3) snaps to 2");
        QualitySettings::SetShadowCascades(9);
        Check(QualitySettings::GetShadowCascades() == 4, "QS20", "and 9 snaps to 4, not to 2");

        // ---- 夹取 ----
        QualitySettings::SetMasterTextureLimit(7);
        Check(QualitySettings::GetMasterTextureLimit() == 3, "QS21",
              "masterTextureLimit is clamped to 3 (more would skip the whole texture)");
        QualitySettings::SetMaxQueuedFrames(0);
        Check(QualitySettings::GetMaxQueuedFrames() == 1, "QS22",
              "maxQueuedFrames is clamped up to 1");
        QualitySettings::SetVSyncCount(-5);
        Check(QualitySettings::GetVSyncCount() == 0, "QS23", "negative vSyncCount clamps to 0");
        QualitySettings::SetShadowDistance(-100.0f);
        Check(QualitySettings::GetShadowDistance() == 0.0f, "QS24",
              "a negative shadowDistance clamps to 0");

        // ---- 非法枚举值回落到合法值，不是"照单全收" ----
        //
        // 枚举是从脚本的 number 强转过来的，可能是任意整数。一个越界的
        // shadowResolution 会在建 shadow map 时才炸，而那时已经离调用点很远了。
        QualitySettings::SetShadowResolution(static_cast<ShadowResolution>(99));
        Check(QualitySettings::GetShadowResolution() == ShadowResolution::Medium, "QS25",
              "an out-of-range shadowResolution falls back to Medium");
        QualitySettings::SetSkinWeights(static_cast<SkinWeights>(7));
        Check(QualitySettings::GetSkinWeights() == SkinWeights::FourBones, "QS26",
              "and an out-of-range skinWeights falls back to FourBones");

        // ---- 枚举值与 Unity 一致 ----
        Check(static_cast<int>(SkinWeights::FourBones) == 4 &&
                  static_cast<int>(SkinWeights::Unlimited) == 255,
              "QS27", "SkinWeights uses Unity's 1/2/4/255 values, not 0/1/2/3");
        Check(static_cast<int>(AnisotropicFiltering::ForceEnable) == 2, "QS28",
              "AnisotropicFiltering uses Unity's values");

        QualitySettings::ResetToDefaults();
        Check(QualitySettings::GetQualityLevel() == 2 && QualitySettings::GetAntiAliasing() == 2,
              "QS29", "ResetToDefaults restores level 2 and its preset");

        // 最高两档的名字顺序：档次表与名字表是**两张表**，靠下标对齐。错位了
        // 只有这两条能看出来（"Very High" 落在档 5 上，设置面板显示错档名）。
        Check(QualitySettings::GetNameForLevel(4) == "Very High" &&
                  QualitySettings::GetNameForLevel(5) == "Ultra" &&
                  QualitySettings::GetNames()[5] == "Ultra",
              "QS30", "the top two levels are 'Very High' (4) then 'Ultra' (5)");
    }

    void TestCursorFacade()
    {
        // 这个可执行文件里**没有** WindowSystem，所以走的是"只记状态"那条路。
        // 那正是要钉住的契约：设什么读回什么，与后端有没有接无关。
        Cursor::ResetToDefaults();
        Check(Cursor::GetVisible(), "CU0", "the cursor starts visible");
        Check(Cursor::GetLockState() == CursorLockMode::None, "CU1",
              "and unlocked");

        Cursor::SetVisible(false);
        Check(!Cursor::GetVisible(), "CU2", "SetVisible(false) reads back as false");
        Cursor::SetLockState(CursorLockMode::Locked);
        Check(Cursor::GetLockState() == CursorLockMode::Locked, "CU3",
              "SetLockState(Locked) reads back as Locked");

        Cursor::SetCursorState(true, CursorLockMode::Confined);
        Check(Cursor::GetVisible() && Cursor::GetLockState() == CursorLockMode::Confined, "CU4",
              "SetCursorState sets both in one go");

        Cursor::ResetToDefaults();
        Check(Cursor::GetVisible() && Cursor::GetLockState() == CursorLockMode::None, "CU5",
              "ResetToDefaults restores visible + unlocked");

        // 枚举值与 Unity 一致。
        Check(static_cast<int>(CursorLockMode::None) == 0 &&
                  static_cast<int>(CursorLockMode::Locked) == 1 &&
                  static_cast<int>(CursorLockMode::Confined) == 2,
              "CU6", "CursorLockMode uses Unity's numeric values");

        // ---- 接线门：Windows 后端必须真的去改 OS 光标 ----
        const std::string win_window =
            ReadFile("Source/Runtime/Function/Render/Platform/Windows/WindowsWindow.cpp");
        Check(!win_window.empty(), "CU7", "WindowsWindow.cpp is readable");
        Check(CountOccurrences(win_window, "::ShowCursor(") >= 2, "CU8",
              "WindowsWindow calls ShowCursor (both directions of the refcount)");
        Check(CountOccurrences(win_window, "::ClipCursor(") >= 3, "CU9",
              "and ClipCursor for Confined / Locked / unlock");
        // ⚠️ 必须是**循环**到越过 0，不是调一次：ShowCursor 是引用计数。
        Check(CountOccurrences(win_window, "while (::ShowCursor(") == 2, "CU10",
              "ShowCursor is looped past zero, not called once (it is a refcount)");

        const std::string generic_window =
            ReadFile("Source/Runtime/Function/Render/Platform/Generic/GenericWindow.h");
        Check(!generic_window.empty(), "CU11", "GenericWindow.h is readable");
        Check(CountOccurrences(generic_window, "SetCursorVisible") >= 1 &&
                  CountOccurrences(generic_window, "SetCursorLockMode") >= 1,
              "CU12", "GenericWindow declares both cursor hooks for other backends");
        // ClipCursor 吃的是**屏幕**坐标；把 GetClientRect 的客户区坐标直接传进去，
        // 在有标题栏/边框的窗口上会把光标锁到偏移了几十像素的一块区域。
        Check(CountOccurrences(win_window, "::ClientToScreen(") >= 2, "CU13",
              "the client rect is converted to screen coords before ClipCursor");
    }

    // =========================================================================
    // Time 的两个闸门 + 关卡时钟
    // =========================================================================

    void TestTimeGates()
    {
        TimeState& time_state = TimeState::Get();
        time_state.Reset();
        time_state.ResetTimeScale();
        time_state.SetMaximumDeltaTime(TimeState::kDefaultMaximumDeltaTime);
        time_state.SetCaptureFramerate(0);

        Check(Near(time_state.GetMaximumDeltaTime(), 1.0f / 3.0f), "TG0",
              "maximumDeltaTime defaults to 1/3 s (Unity's default)");
        Check(time_state.GetCaptureFramerate() == 0, "TG1",
              "captureFramerate defaults to off");

        // ---- 闸门作用在"本帧 delta 诞生之前" ----
        time_state.BeginFrame(2.0f, 2.0f, true);
        Check(Near(time_state.GetDeltaTime(), 1.0f / 3.0f), "TG2",
              "a 2-second hitch is clamped to maximumDeltaTime, not passed through");
        // ⚠️ 闸门在**缩放之前**：timeScale=0.1 时上限仍是 1/3 秒，不是 1/30。
        time_state.Reset();
        time_state.SetTimeScale(0.1f);
        time_state.BeginFrame(2.0f, 2.0f, true);
        Check(Near(time_state.GetDeltaTime(), (1.0f / 3.0f) * 0.1f), "TG3",
              "the clamp is applied BEFORE timeScale, so 0.1x still allows 1/3 s of raw step");
        time_state.ResetTimeScale();

        // ---- captureFramerate 钉死步长 ----
        time_state.Reset();
        time_state.SetCaptureFramerate(30);
        time_state.BeginFrame(0.005f, 0.005f, true);
        Check(Near(time_state.GetDeltaTime(), 1.0f / 30.0f, 1e-5f), "TG4",
              "captureFramerate=30 pins the step at 1/30 regardless of the real frame time");
        time_state.BeginFrame(0.5f, 0.5f, true);
        Check(Near(time_state.GetDeltaTime(), 1.0f / 30.0f, 1e-5f), "TG5",
              "and a slow frame does not change it either");
        time_state.SetCaptureFramerate(0);
        time_state.BeginFrame(0.016f, 0.016f, true);
        Check(Near(time_state.GetDeltaTime(), 0.016f), "TG6",
              "turning captureFramerate off restores the real step");
        // <= 0 一律当"关闭"，不夹到 1fps。
        time_state.SetCaptureFramerate(-60);
        Check(time_state.GetCaptureFramerate() == 0, "TG7",
              "a negative captureFramerate means OFF, not 1 fps");

        // ---- maximumDeltaTime 的下限 ----
        time_state.SetMaximumDeltaTime(0.0f);
        Check(time_state.GetMaximumDeltaTime() > 0.0f, "TG8",
              "maximumDeltaTime cannot be zeroed (0 would freeze the world)");
        // ⚠️ 必须还原：上面那一步把上限压到了 1ms，后面的 BeginFrame(0.1) 会被
        // 夹成 0.001 —— 于是"累计时间"那几条断言全错，而错因看起来像关卡时钟
        // 坏了。测试之间的全局状态要在离开这一步时还回去。
        time_state.SetMaximumDeltaTime(TimeState::kDefaultMaximumDeltaTime);

        // ---- 关卡时钟 ----
        time_state.Reset();
        time_state.BeginFrame(0.1f, 0.1f, true);
        time_state.BeginFrame(0.1f, 0.1f, true);
        Check(Near(time_state.GetTimeSinceLevelLoad(), 0.2f), "TG9",
              "before any level load, timeSinceLevelLoad tracks Time.time");
        time_state.NotifyLevelLoaded();
        Check(Near(time_state.GetTimeSinceLevelLoad(), 0.0f), "TG10",
              "NotifyLevelLoaded resets the level clock");
        time_state.BeginFrame(0.05f, 0.05f, true);
        Check(Near(time_state.GetTimeSinceLevelLoad(), 0.05f), "TG11",
              "and it then measures from that point");
        Check(Near(time_state.GetTime(), 0.25f), "TG12",
              "while Time.time keeps counting from engine start");

        time_state.Reset();
        time_state.ResetTimeScale();
        time_state.SetMaximumDeltaTime(TimeState::kDefaultMaximumDeltaTime);

        // ---- 接线门：SceneManager 真的去打点 ----
        const std::string scene_source =
            ReadFile("Source/Runtime/Scripting/ScriptScene/ScriptScene.cpp");
        Check(!scene_source.empty(), "TG13", "ScriptScene.cpp is readable");
        // >= 3：LoadScene / LoadSceneAdditive / ReloadActiveScene。少一处就是
        // "有一条加载路径不重置关卡时钟"，而那条路的表现是"计时偶尔偏一点"。
        Check(CountOccurrences(scene_source, "NotifyLevelLoaded()") >= 3, "TG14",
              "all three scene-load paths reset the level clock");
    }

    // =========================================================================
    // Texture2D / RenderTexture
    // =========================================================================

    void TestTexture2DFacade()
    {
        Texture2D texture;
        // 空壳：还没有像素。
        Check(!texture.GetIsValid(), "TF0", "a default Texture2D is not valid");
        Check(!texture.GetHasUncompressedPixels(), "TF1",
              "and has no uncompressed pixel layout");
        Check(!texture.SetPixel(0, 0, ::Color32(255, 0, 0, 255)), "TF2",
              "SetPixel on an empty texture reports failure");
        Check(!texture.Resize(0, 0), "TF3", "Resize(0,0) is rejected");

        Check(texture.Resize(4, 2), "TF4", "Resize(4,2) succeeds");
        // `m_Width` / `m_Height` 是公开字段，脚本侧叫 `width` / `height`
        // （ZPROPERTY 剥掉 m_ 前缀）。
        Check(texture.m_Width == 4 && texture.m_Height == 2, "TF5",
              "and the dimensions stick");
        Check(texture.GetHasUncompressedPixels(), "TF6",
              "a resized texture has an uncompressed RGBA8 layout");
        Check(texture.GetMipmapCount() == 1, "TF7", "and exactly one mip");
        Check(texture.GetIsValid(), "TF8", "and it is now valid");

        // Resize 出来的是**全 0**（透明黑），不是"保留原内容"。
        Check(Near(texture.GetPixel(0, 0).a, 0.0f), "TF9", "a resized texture starts at zero");

        Check(texture.SetPixel(1, 1, ::Color32(255, 0, 0, 255)), "TF10",
              "SetPixel inside the bounds succeeds");
        const ::Color read_back = texture.GetPixel(1, 1);
        Check(Near(read_back.r, 1.0f) && Near(read_back.g, 0.0f) && Near(read_back.b, 0.0f) &&
                  Near(read_back.a, 1.0f),
              "TF11", "and GetPixel reads it back");
        // 只写了 (1,1)，(0,0) 必须还是 0 —— 写错位会让"只改一个像素"变成"改一片"。
        Check(Near(texture.GetPixel(0, 0).a, 0.0f), "TF12", "neighbouring pixels are untouched");

        // 越界：不夹紧，返回透明黑。
        Check(!texture.SetPixel(-1, 0, ::Color32(255, 0, 0, 255)), "TF13",
              "SetPixel(-1,0) is rejected (no clamping to the edge)");
        Check(!texture.SetPixel(4, 0, ::Color32(255, 0, 0, 255)), "TF14",
              "and so is x == width");
        // ⚠️ 先在一个**边缘**像素上写不透明色，再读越界坐标。直接读 (99,99)
        // 是抓不到"夹紧到边缘"那个实现的 —— 那时边缘还是透明的，两种实现的
        // 返回值一样。这是"空测试"的另一种形态：断言对了但证明不了东西。
        Check(texture.SetPixel(3, 1, ::Color32(255, 0, 0, 255)), "TF15a",
              "the bottom-right pixel can be made opaque");
        Check(Near(texture.GetPixel(-1, 0).a, 0.0f) && Near(texture.GetPixel(99, 99).a, 0.0f),
              "TF15", "out-of-range GetPixel is transparent black, not the edge pixel");

        // 通道夹紧：Color 的通道是 float，脚本能给 1.5 / -0.2。
        ::Color over_range;
        over_range.r = 2.0f;
        over_range.g = -1.0f;
        over_range.b = 0.0f;
        over_range.a = 1.0f;
        Check(texture.SetPixel(2, 0, over_range), "TF16", "SetPixel accepts an out-of-range Color");
        const ::Color clamped = texture.GetPixel(2, 0);
        Check(Near(clamped.r, 1.0f) && Near(clamped.g, 0.0f), "TF17",
              "and clamps the channels to 0..1 (no uint8_t wraparound)");

        // 舍入：0.5 必须落到 128/255（Unity 的 RoundToInt），不是 127/255
        // （截断）。差一位，肉眼看不出，但和 ColorUtility.ToHtmlStringRGB
        // 的 floor(v*255+0.5) 来回转换时就漂了。
        ::Color half;
        half.r = 0.5f;
        half.g = 0.5f;
        half.b = 0.5f;
        half.a = 1.0f;
        Check(texture.SetPixel(0, 1, half), "TF21", "SetPixel accepts a fractional Color");
        const ::Color rounded = texture.GetPixel(0, 1);
        Check(Near(rounded.r, 128.0f / 255.0f, 1e-4f), "TF22",
              "0.5 rounds to 128/255, not 127/255 (rounding, not truncation)");

        // ClearPixels
        Check(texture.ClearPixels(::Color32(0, 255, 0, 255)), "TF18", "ClearPixels succeeds");
        const ::Color cleared = texture.GetPixel(3, 1);
        Check(Near(cleared.g, 1.0f) && Near(cleared.r, 0.0f), "TF19",
              "and fills every pixel of mip 0");
        Check(Near(cleared.a, 1.0f), "TF23", "and the alpha channel is filled too");

        Check(!texture.Resize(99999, 1), "TF20",
              "Resize beyond 16384 is rejected (it could never be uploaded)");

        // 尺寸与数据对不上时不能报"有像素"。只看 w*h>0 的实现会在这里说 true，
        // 然后 GetPixel 越界读到 vector 外面。
        Texture2D partial;
        partial.Resize(4, 2);
        partial.m_Pixels.resize(8);  // 只够 2 个像素，不是 8 个
        Check(!partial.GetHasUncompressedPixels(), "TF24",
              "a pixel buffer shorter than w*h*4 does not count as 'hasUncompressedPixels'");
        Check(Near(partial.GetPixel(3, 1).a, 0.0f), "TF25",
              "and GetPixel on the missing tail is transparent black, not a heap over-read");
    }

    void TestRenderTextureFacade()
    {
        RenderTexture render_texture;
        // 默认构造出来的描述不能是不确定值 —— RenderTextureDesc 原来没有默认
        // 初始化器，`RenderTextureDesc m_Desc;` 会把 int 读成垃圾。
        Check(render_texture.GetWidth() == 0 && render_texture.GetHeight() == 0, "RF0",
              "a default RenderTexture has zeroed dimensions (RenderTextureDesc has defaults)");
        Check(!render_texture.IsCreated(), "RF1", "and is not created");

        render_texture.SetWidth(1280);
        render_texture.SetHeight(720);
        Check(render_texture.GetWidth() == 1280 && render_texture.GetHeight() == 720, "RF2",
              "width / height are writable");
        // <= 0 夹到 1，不是 0：0 宽的资源在 RHI 上是非法尺寸。
        render_texture.SetWidth(0);
        Check(render_texture.GetWidth() == 1, "RF3", "width 0 clamps to 1");

        render_texture.SetAntiAliasing(3);
        Check(render_texture.GetAntiAliasing() == 2, "RF4", "antiAliasing(3) snaps DOWN to 2");
        render_texture.SetDepth(20);
        Check(render_texture.GetDepth() == 16, "RF5", "depth(20) snaps to 16");
        render_texture.SetFormat(-7);
        Check(render_texture.GetFormat() == 0, "RF6", "a negative format clamps to 0 (unspecified)");

        // ---- 生命周期 ----
        RenderTexture sized;
        sized.SetWidth(64);
        sized.SetHeight(64);
        sized.Create();
        Check(sized.IsCreated(), "RF7", "Create() on valid dimensions succeeds");
        sized.Create();
        Check(sized.IsCreated(), "RF8", "and a second Create() is a no-op");
        sized.Release();
        Check(!sized.IsCreated(), "RF9", "Release() clears it");

        // 尺寸非法：不创建、也不置位。
        RenderTexture empty;
        empty.SetWidth(1);
        empty.SetHeight(1);
        empty.Release();
        empty.Create();
        Check(empty.IsCreated(), "RF10", "1x1 is a legal size and creates");

        // ⚠️ 尺寸非法的 RT：Create() 必须**不**置位。RF1 只看默认构造的状态，
        // 没走 Create() —— 所以"Create 把非法尺寸也标成已创建"这一条之前是漏的。
        RenderTexture degenerate;
        degenerate.Create();
        Check(!degenerate.IsCreated(), "RF16",
              "Create() on a zero-sized descriptor leaves isCreated false");
        // setter 会把 <=0 夹到 1，所以 0x0 只能来自默认构造 —— 也就意味着
        // Create() 里的尺寸检查是**唯一**能挡住它的地方。
        degenerate.Create();
        Check(!degenerate.IsCreated(), "RF17",
              "and a second Create() does not paper over it");

        // ---- 两个 mip 开关的自洽 ----
        RenderTexture mips;
        mips.SetAutoGenerateMips(true);
        Check(mips.GetUseMipMap(), "RF11", "autoGenerateMips=true turns useMipMap on");
        mips.SetUseMipMap(false);
        Check(!mips.GetAutoGenerateMips(), "RF12", "useMipMap=false turns autoGenerateMips off");

        // ---- active 是全局的 ----
        RenderTexture::SetActive(nullptr);
        Check(RenderTexture::GetActive() == nullptr, "RF13", "active starts as null");
        RenderTexture::SetActive(&sized);
        Check(RenderTexture::GetActive() == &sized, "RF14", "SetActive sticks");
        sized.Create();
        RenderTexture::SetActive(&sized);
        sized.Release();
        // 释放掉的 RT 不能继续当活动目标。
        Check(RenderTexture::GetActive() == nullptr, "RF15",
              "releasing the active render target clears 'active'");
    }

    // =========================================================================
    // 粒子播放控制
    // =========================================================================

    void TestParticlePlayback()
    {
        Runtime::ParticleComponent particles;

        Check(particles.IsPlaying(), "PP0", "a fresh particle component is playing");
        Check(particles.IsAlive(), "PP1", "and therefore alive");
        Check(!particles.IsPaused() && !particles.IsStopped(), "PP2",
              "and neither paused nor stopped");

        particles.Pause();
        Check(particles.IsPaused(), "PP3", "Pause() on a playing system pauses it");
        Check(!particles.IsPlaying(), "PP4", "and it is no longer playing");
        Check(particles.IsAlive(), "PP5", "but it is still alive");

        particles.Play();
        Check(particles.IsPlaying() && !particles.IsPaused(), "PP6",
              "Play() resumes without resetting the clock");

        // SetPlayState 收的是脚本来的 int 强转，可能是任意值。越界档位不落回
        // Stopped 的话，后面所有 `switch` 都会走 default，状态机读出来是垃圾。
        particles.SetPlayState(static_cast<ParticlePlayState>(99));
        Check(particles.IsStopped(), "PP22",
              "an out-of-range playState falls back to Stopped, not to a garbage state");
        particles.SetPlayState(ParticlePlayState::Playing);
        Check(particles.IsPlaying(), "PP23", "and a legal state still round-trips");

        particles.Stop();
        Check(particles.IsStopped(), "PP7", "Stop() stops it");
        Check(!particles.IsAlive(), "PP8", "and a stopped system is not alive");

        // ⚠️ 已停止的系统不接受暂停：Unity 的 Pause 只对"正在播的"有意义。
        particles.Pause();
        Check(particles.IsStopped() && !particles.IsPaused(), "PP9",
              "Pause() on a stopped system is a no-op");

        particles.SetTime(5.0f);
        particles.Clear();
        Check(particles.IsStopped(), "PP10", "Clear() stops");
        Check(particles.GetTime() == 0.0f, "PP11", "and zeroes the clock");

        particles.SetTime(-3.0f);
        Check(particles.GetTime() == 0.0f, "PP12", "a negative time clamps to 0");

        // 三态枚举：两个"真"同时成立是不可能的。
        Check(static_cast<int>(ParticlePlayState::Stopped) == 0 &&
                  static_cast<int>(ParticlePlayState::Playing) == 1 &&
                  static_cast<int>(ParticlePlayState::Paused) == 2,
              "PP13", "ParticlePlayState is a 3-state enum, not three bools");
        particles.SetPlayState(ParticlePlayState::Paused);
        Check(particles.GetPlayState() == ParticlePlayState::Paused, "PP14",
              "SetPlayState takes the enum directly");

        // ---- Tick 真的按状态推进时钟 ----
        //
        // ⚠️ 这里调的是**真的 Tick**，不是"读一下状态"：状态机对了但 Tick 不按
        // 它分支，是这一类实现里最常见的漏法（两边都编译得过、都跑得动）。
        // 没有 RenderSystem 时 Tick 会在拿到 swap context 之后就返回，而时钟是
        // 在**那之前**累加的，所以这几条在没有渲染系统的进程里也成立。
        particles.Play();
        particles.SetTime(0.0f);
        particles.Tick(0.1f);
        Check(Near(particles.GetTime(), 0.1f), "PP19", "Tick advances the clock while playing");
        particles.Pause();
        particles.Tick(0.1f);
        Check(Near(particles.GetTime(), 0.1f), "PP20",
              "and does NOT advance it while paused (paused keeps the clock)");
        particles.Stop();
        particles.Tick(0.1f);
        Check(Near(particles.GetTime(), 0.1f), "PP21", "nor while stopped");

        // ---- 接线门：Tick 真的按状态分支 ----
        const std::string particle_source = ReadFile(
            "Source/Runtime/Function/Framework/Component/Particle/ParticleComponent.cpp");
        Check(!particle_source.empty(), "PP15", "ParticleComponent.cpp is readable");
        Check(CountOccurrences(particle_source, "ParticlePlayState::Stopped") >= 2, "PP16",
              "Tick early-returns when stopped");
        Check(CountOccurrences(particle_source, "AddTickParticleEmitter") >= 1 &&
                  particle_source.find("if (m_PlayState == ParticlePlayState::Playing)") !=
                      std::string::npos,
              "PP17", "the simulation tick is gated on Playing");
        // 暂停时**仍然**同步 transform —— 少了它，暂停中移动发射器会被吞掉。
        Check(particle_source.find("UpdateParticleTransform") != std::string::npos,
              "PP18", "the transform is still synced while paused");
    }

    // =========================================================================
    // Application 补项
    // =========================================================================

    void TestApplicationExtras()
    {
        // identifier 永远非空且长得像反转域名：空串会让"按包名分组"把全世界
        // 并成一组。
        const std::string identifier = Application::GetIdentifier();
        Check(!identifier.empty(), "AX0", "identifier is never empty");
        Check(identifier.find("com.") == 0, "AX1",
              "identifier uses the reversed-domain shape");

        Check(!Application::GetIsMobilePlatform(), "AX2",
              "a desktop build is not a mobile platform");
        Check(!Application::GetIsConsolePlatform(), "AX3",
              "isConsolePlatform is explicitly false (no console backend)");
        // ⚠️ 无窗口系统时返回 **true**：false 会让"失焦就停"的逻辑在 headless
        // 下把整个世界冻住。
        Check(Application::GetIsFocused(), "AX4",
              "isFocused is true with no window system (false would freeze a headless world)");

        // ---- OpenURL 的白名单（防 shell 注入）----
        Check(!Application::OpenURL("not-a-url"), "AX5",
              "OpenURL rejects a string with no allowed scheme");
        Check(!Application::OpenURL("http://x/a; rm -rf /"), "AX6",
              "and rejects a ';' even with a legal scheme (system() goes through a shell)");
        Check(!Application::OpenURL("http://x/$(whoami)"), "AX7", "and rejects $()");
        Check(!Application::OpenURL("http://x/a b"), "AX8", "and rejects whitespace");
        Check(!Application::OpenURL("http://"), "AX9", "and rejects an empty host");
        // 合法 URL 会真的去调 ShellExecute / xdg-open —— 测试里不该真的打开
        // 浏览器，所以这里只钉"校验不误杀"这一点：mailto: 是合法 scheme。
        const std::string app_source = ReadFile("Source/Runtime/Application/Application.cpp");
        Check(!app_source.empty(), "AX10", "Application.cpp is readable");
        Check(CountOccurrences(app_source, "mailto:") >= 1 &&
                  CountOccurrences(app_source, "https://") >= 1 &&
                  CountOccurrences(app_source, "file://") >= 1,
              "AX11", "the scheme allow-list covers http/https/mailto/file");
        Check(CountOccurrences(app_source, "kForbidden") >= 2, "AX12",
              "and there is an explicit shell-metacharacter blacklist");

        // ---- 系统语言：只映射认识的语言，其余 Unknown ----
        Check(Application::GetSystemLanguage() != SystemLanguage::Unknown ||
                  Application::GetSystemLanguage() == SystemLanguage::Unknown,
              "AX13", "systemLanguage resolves to a SystemLanguage value");
        Check(static_cast<int>(SystemLanguage::English) == 10 &&
                  static_cast<int>(SystemLanguage::Chinese) == 13,
              "AX14", "SystemLanguage uses Unity's numeric values");

        // ---- backgroundLoadingPriority ----
        Application::SetBackgroundLoadingPriority(ThreadPriority::High);
        Check(Application::GetBackgroundLoadingPriority() == ThreadPriority::High, "AX15",
              "backgroundLoadingPriority round-trips");
        Application::SetBackgroundLoadingPriority(ThreadPriority::Low);
        Check(static_cast<int>(ThreadPriority::Low) == 0 &&
                  static_cast<int>(ThreadPriority::High) == 3,
              "AX16", "ThreadPriority uses Unity's 0..3 values");

        // ---- 临时目录不 fallback 到 persistentDataPath ----
        Check(!Application::GetTemporaryCachePath().empty(), "AX17",
              "temporaryCachePath resolves to a real temp directory");

        // ---- 非法优先级回落 ----
        // 不回落的话，脚本里一次算错的枚举值会把加载线程优先级永久顶到最高
        // —— 而且没有任何日志。
        Application::SetBackgroundLoadingPriority(static_cast<ThreadPriority>(77));
        Check(Application::GetBackgroundLoadingPriority() == ThreadPriority::Normal, "AX18",
              "an out-of-range backgroundLoadingPriority falls back to Normal");
        Application::SetBackgroundLoadingPriority(ThreadPriority::High);

        // ---- shell 元字符黑名单的实际内容 ----
        // AX6-AX8 是**行为**门，但它们靠"ShellExecuteA 恰好失败"才过 —— 一个
        // 真能打开的 URL（file:///tmp/x;y 在部分机器上成立）会让它们放行。
        // 所以还要看那张黑名单里到底有没有那几个要命的字符。
        const std::size_t kb = app_source.find("kForbidden = \"");
        Check(kb != std::string::npos, "AX19", "the shell metacharacter blacklist exists");
        if (kb != std::string::npos)
        {
            const std::size_t begin = kb + std::strlen("kForbidden = \"");
            const std::size_t end   = app_source.find("\";", begin);
            Check(end != std::string::npos && end > begin, "AX20", "and it is a string literal");
            if (end != std::string::npos && end > begin)
            {
                const std::string literal = app_source.substr(begin, end - begin);
                bool all_present = true;
                // ⚠️ 比对的是**源码字面量**里的字符（\t 在文件里是反斜杠+t
                // 两个字符），不是它运行时的值。
                for (const char c : std::string(" \\\"'`$\\;|&<>(){}[]*?!#"))
                {
                    if (literal.find(c) == std::string::npos)
                    {
                        all_present = false;
                    }
                }
                Check(all_present, "AX21",
                      "and it lists every dangerous shell metacharacter");
            }
        }
    }
    // =========================================================================
    // Unity 的 Material / Light（slice 52）
    //
    /// 两个 sub-mesh，**不带材质资产**：`populateMaterialDesc` 在资产路径为空时
    /// 直接返回，于是整套规则在无 AssetManager、无 GPU 的进程里也能跑。
    std::vector<SubMeshRes> MakeSubMeshes(int count)
    {
        std::vector<SubMeshRes> sub_meshes;
        for (int i = 0; i < count; ++i)
        {
            SubMeshRes sub_mesh;
            sub_mesh.m_MeshAsset = "test/mesh.zasset";
            sub_meshes.push_back(sub_mesh);
        }
        return sub_meshes;
    }

    //

    // MI* —— `renderer.material` 那个句柄。判据挑的是"句柄"这个设计真正带来的
    //         差别：同一 renderer 上槽位之间互相隔离（MI8/MI10），句柄按槽位缓存
    //         所以 `renderer.material` 两次拿到同一个对象（MI3），以及没绑定目标
    //         的句柄不崩只返回安全默认（MI21）。
    // LI* —— `Light` 的 Unity 面。核心是 color × intensity 与引擎的 flux 必须是
    //         **可逆**的分解（LI2/LI3/LI4），以及 spotAngle 的"全锥角/度"与引擎
    //         的"半锥角/弧度"换算（LI9/LI10/LI12）—— 这两处写错都看不出明显故障，
    //         只会"看起来有点不对"。
    // =========================================================================
    void TestMaterialLightParity()
    {
        ObjectManager* om = GET_SYSTEM(ObjectManager);
        if (om == nullptr)
        {
            om = new ObjectManager();
            SystemRegistry::GetInstance().RegisterSystem(om);
            om->Initialize();
        }
        Check(om != nullptr, "MI0", "ObjectManager is reachable");

        // ---- renderer.material --------------------------------------------
        //
        // ⚠️ 渲染器必须走 `ObjectManager::NewObject`：句柄里存的是 `PPtr`，而 PPtr
        // 靠 instanceID 解析 —— 栈上 new 出来的 Object 没有注册过 ID（恒 0），
        // PPtr 会把它当成"已销毁"，调试构建下 UafReporter 直接 ASSERT。真实运行时
        // 的组件都来自反序列化 / AddComponent，ID 是有的。
        MeshRenderer* renderer = om->NewObject<MeshRenderer>();
        renderer->SetSubMeshes(MakeSubMeshes(2));

        Check(renderer->GetMaterialCount() == 2, "MI1",
              "GetMaterialCount() == sub-mesh count (Unity's materials.length)");
        Check(renderer->GetMaterialSlotCount() == 2, "MI2", "and it agrees with the slot count");

        MaterialInstance* mat0 = renderer->GetMaterial(0);
        MaterialInstance* mat1 = renderer->GetMaterial(1);
        Check(mat0 != nullptr && mat1 != nullptr, "MI3", "every slot hands out a material handle");
        Check(mat0 != mat1, "MI4", "and each slot gets its own");
        Check(renderer->GetMaterial(0) == mat0, "MI5",
              "renderer.material is cached (reading it twice is the same object)");
        Check(renderer->GetMainMaterial() == mat0, "MI6", "material is slot 0 (Unity's shape)");
        Check(renderer->GetMaterial(-1) == nullptr && renderer->GetMaterial(2) == nullptr, "MI7",
              "an out-of-range slot is null, not a handle into nowhere");
        Check(mat0->IsValid() && mat1->IsValid(), "MI8", "a bound handle reports IsValid()");

        // 主色：句柄与 renderer 是同一个量（w = alpha）。
        Color tint;
        tint.r = 0.25f;
        tint.g = 0.5f;
        tint.b = 0.75f;
        tint.a = 0.5f;
        mat0->SetMainColor(tint);
        Color read_back = mat0->GetMainColor();
        Check(Near(read_back.r, 0.25f) && Near(read_back.g, 0.5f) && Near(read_back.b, 0.75f) &&
                  Near(read_back.a, 0.5f),
              "MI9", "material.color round-trips through the renderer");
        Check(Near(renderer->GetBaseColor(0).x, 0.25f) && Near(renderer->GetBaseColor(0).w, 0.5f),
              "MI10", "and it is the renderer's base color (no second copy)");

        // 槽位隔离 —— 整个"per-sub-mesh 实例"这件事的意义就在这一条。
        mat1->SetMainColor(tint);
        Check(Near(renderer->GetBaseColor(1).x, 0.25f), "MI11", "slot 1 takes the tint");
        mat1->SetMainColor(Color {1.0f, 1.0f, 1.0f, 1.0f});
        Check(Near(mat0->GetMainColor().r, 0.25f), "MI12",
              "writing slot 1 leaves slot 0 alone (the bug per-slot overrides exist for)");

        // ---- 具名属性 ------------------------------------------------------
        Check(mat0->SetFloat("_DissolveAmount", 0.42f), "MI13",
              "SetFloat returns true for a name the engine knows");
        Check(Near(mat0->GetFloat("_DissolveAmount"), 0.42f), "MI14", "and reads it back");
        Check(Near(mat1->GetFloat("_DissolveAmount"), 0.0f), "MI15",
              "a property set on slot 0 is not visible on slot 1");
        Check(!mat0->SetFloat("__NoSuchProperty__", 1.0f), "MI16",
              "SetFloat returns false for a typo (silent failure is the worst kind)");
        Check(Near(mat0->GetFloat("__NoSuchProperty__"), 0.0f), "MI17",
              "and reading an unknown name gives 0");

        Check(mat0->HasProperty("_DissolveAmount"), "MI18", "HasProperty agrees with SetFloat");
        Check(!mat0->HasProperty("__NoSuchProperty__"), "MI19", "and with a typo");
        Check(mat0->HasFloat("_DissolveAmount") && !mat0->HasFloat("_DissolveEdgeColor"), "MI20",
              "HasFloat is per-kind (a vec4 is not a float)");
        Check(mat0->HasVector("_DissolveEdgeColor") && mat0->HasColor("_DissolveEdgeColor"), "MI21",
              "HasColor and HasVector are the same question here (colour is a vec4)");
        Check(mat0->HasTexture("_MainTex"), "MI22", "HasTexture finds the built-in _MainTex alias");
        // ⚠️ HasProperty 必须是三种类型的**并集**：只查标量的话，颜色与贴图属性会
        // 被报成"不存在"，于是脚本在写之前先问一遍的写法会跳过一半属性。
        Check(mat0->HasProperty("_DissolveEdgeColor"), "MI37",
              "HasProperty is true for a vector property too");
        Check(mat0->HasProperty("_MainTex"), "MI38", "and for a texture property");
        Check(!mat0->HasVector("_DissolveAmount"), "MI39",
              "but a float is not a vector (SetVector cannot hold it)");
        const Color missing_color = mat0->GetColor("__NoSuchProperty__");
        Check(Near(missing_color.r, 1.0f) && Near(missing_color.g, 1.0f) &&
                  Near(missing_color.b, 1.0f) && Near(missing_color.a, 1.0f),
              "MI40", "an unknown colour property reads as white, not transparent black");
        // ⚠️ 这一条是 MI40 的另一半：白色必须是"名字不存在"的哨兵，不能是"值为 0"
        // 的哨兵。一条存在且**真的是黑**的属性必须能读出黑 —— 拿返回值当哨兵会
        // 把"把溶解边缘调成黑色"这个完全合法的操作当成没设过。
        Color black;
        black.r = 0.0f;
        black.g = 0.0f;
        black.b = 0.0f;
        black.a = 0.0f;
        Check(mat0->SetColor("_DissolveEdgeColor", black), "MI41",
              "black is a settable colour, not 'no value'");
        const Color black_read = mat0->GetColor("_DissolveEdgeColor");
        Check(black_read.r == 0.0f && black_read.g == 0.0f && black_read.b == 0.0f, "MI42",
              "and it reads back black (the sentinel is HasProperty, not the value)");

        const Vector4 edge {1.0f, 0.0f, 0.0f, 1.0f};
        Check(mat0->SetVector("_DissolveEdgeColor", edge), "MI23", "SetVector on a vec4 property");
        const Vector4 edge_read = mat0->GetVector("_DissolveEdgeColor");
        Check(Near(edge_read.x, 1.0f) && Near(edge_read.y, 0.0f) && Near(edge_read.z, 0.0f) &&
                  Near(edge_read.w, 1.0f),
              "MI24", "and it round-trips");
        const Color edge_color = mat0->GetColor("_DissolveEdgeColor");
        Check(Near(edge_color.r, 1.0f) && Near(edge_color.g, 0.0f) && Near(edge_color.b, 0.0f),
              "MI25", "GetColor reads the same vec4");

        Check(mat0->SetTexture("_MainTex", "Textures/rust.asset"), "MI26",
              "SetTexture on a built-in slot");
        Check(mat0->GetTexture("_MainTex") == "Textures/rust.asset", "MI27",
              "GetTexture returns what was set (the project-relative spelling)");

        // ---- keyword -------------------------------------------------------
        Check(!mat0->IsKeywordEnabled("_EMISSION_ON"), "MI28", "a keyword starts off");
        mat0->EnableKeyword("_EMISSION_ON");
        Check(mat0->IsKeywordEnabled("_EMISSION_ON"), "MI29", "EnableKeyword turns it on");
        mat0->DisableKeyword("_EMISSION_ON");
        Check(!mat0->IsKeywordEnabled("_EMISSION_ON"), "MI30", "and DisableKeyword turns it off");

        // ---- 没绑定目标的句柄 ----------------------------------------------
        //
        // 与"renderer 被 Destroy 之后留下的句柄"是同一条代码路径（PPtr 解析成
        // nullptr），但这里更好复现：脚本 `new Material()` 拿到的就是这种句柄。
        MaterialInstance* orphan = om->NewObject<MaterialInstance>();
        Check(!orphan->IsValid(), "MI31", "an unbound handle is not valid");
        Check(!orphan->SetFloat("_DissolveAmount", 1.0f), "MI32",
              "its setters are no-ops that say so (false, not a crash)");
        const Color orphan_color = orphan->GetMainColor();
        Check(Near(orphan_color.r, 1.0f) && Near(orphan_color.g, 1.0f) &&
                  Near(orphan_color.b, 1.0f) && Near(orphan_color.a, 1.0f),
              "MI33", "and it reads back Unity's default white, not transparent black");

        // ---- Light：color × intensity <-> flux -----------------------------
        LightComponent light;

        Check(Near(light.GetIntensity(), 100.0f), "LI0",
              "the default flux (100,100,100) reads back as intensity 100");
        const Color default_color = light.GetLightColor();
        Check(Near(default_color.r, 1.0f) && Near(default_color.g, 1.0f) &&
                  Near(default_color.b, 1.0f) && Near(default_color.a, 1.0f),
              "LI1", "and as white (Unity's default light colour)");

        Color red;
        red.r = 1.0f;
        red.g = 0.5f;
        red.b = 0.25f;
        red.a = 1.0f;
        light.SetLightColor(red);
        const Vector3 flux = light.GetFlux();
        Check(Near(flux.x, 100.0f) && Near(flux.y, 50.0f) && Near(flux.z, 25.0f), "LI2",
              "setting colour keeps the intensity (flux = colour * intensity)");
        const Color color_again = light.GetLightColor();
        Check(Near(color_again.r, 1.0f) && Near(color_again.g, 0.5f) && Near(color_again.b, 0.25f),
              "LI3", "and the decomposition is reversible (no pink drift)");

        light.SetIntensity(200.0f);
        const Vector3 scaled = light.GetFlux();
        Check(Near(scaled.x, 200.0f) && Near(scaled.y, 100.0f) && Near(scaled.z, 50.0f), "LI4",
              "intensity scales the flux");
        Check(Near(light.GetIntensity(), 200.0f), "LI5", "and reads back unchanged");
        const Color hue_after_intensity = light.GetLightColor();
        Check(Near(hue_after_intensity.r, 1.0f) && Near(hue_after_intensity.g, 0.5f) &&
                  Near(hue_after_intensity.b, 0.25f),
              "LI6", "changing intensity does not change the hue");

        // 零通量：intensity 是 0，除法无意义 —— 这时报白，不是黑。
        light.SetIntensity(0.0f);
        Check(Near(light.GetIntensity(), 0.0f), "LI7", "a light with no flux has intensity 0");
        const Color zero_color = light.GetLightColor();
        Check(Near(zero_color.r, 1.0f) && Near(zero_color.g, 1.0f) && Near(zero_color.b, 1.0f),
              "LI8", "and reports white, not black (Unity: white at intensity 0)");
        light.SetIntensity(100.0f);
        const Vector3 revived = light.GetFlux();
        Check(Near(revived.x, 100.0f) && Near(revived.y, 100.0f) && Near(revived.z, 100.0f), "LI9",
              "so winding intensity back up from 0 gives a white light, not a black one");

        // ⚠️ 上面那条色相 (1, 0.5, 0.25) 的**红通道正好是最亮的那个**，于是
        // "按最大通道归一化"和"一律按红通道归一化"给出同一个答案 —— 后者是错的
        // （最亮的是绿/蓝通道时色相会漂）。所以红不是最大值的色相必须单独测。
        Color green_ish;
        green_ish.r = 0.25f;
        green_ish.g = 1.0f;
        green_ish.b = 0.5f;
        green_ish.a = 1.0f;
        light.SetLightColor(green_ish);
        const Vector3 green_flux = light.GetFlux();
        Check(Near(green_flux.x, 25.0f) && Near(green_flux.y, 100.0f) && Near(green_flux.z, 50.0f),
              "LI19", "a hue whose brightest channel is green normalises off the max, not off red");
        Check(Near(light.GetIntensity(), 100.0f), "LI20", "intensity is still that max channel");
        const Color green_read = light.GetLightColor();
        Check(Near(green_read.r, 0.25f) && Near(green_read.g, 1.0f) && Near(green_read.b, 0.5f),
              "LI21", "and the decomposition is reversible for that hue too (no drift)");

        // range <-> radius
        light.SetRange(12.0f);
        Check(Near(light.GetRadius(), 12.0f), "LI10", "range is the attenuation radius");
        Check(Near(light.GetRange(), 12.0f), "LI11", "and reads back");

        // ⚠️ spotAngle 是**全**锥角**度**，引擎存的是**半**锥角**弧度**。
        Check(Near(light.GetSpotAngle(), 90.0f, 0.01f), "LI12",
              "the default half-angle 45° reads back as spotAngle 90° (full angle)");
        light.SetSpotAngle(60.0f);
        Check(Near(light.GetOuterConeAngle(), 0.5236f, 1e-3f), "LI13",
              "spotAngle 60 stores a 30° half-angle in radians");
        Check(Near(light.GetSpotAngle(), 60.0f, 0.01f), "LI14", "and round-trips");
        light.SetInnerSpotAngle(30.0f);
        Check(Near(light.GetInnerConeAngle(), 0.2618f, 1e-3f), "LI15",
              "innerSpotAngle 30 stores a 15° half-angle");
        Check(Near(light.GetInnerSpotAngle(), 30.0f, 0.01f), "LI16", "and round-trips");

        // ---- 发布面源码门 --------------------------------------------------
        const std::string material_source =
            ReadFile("Source/Runtime/Scripting/ScriptMaterial/ScriptMaterial.h");
        Check(!material_source.empty(), "MI34", "ScriptMaterial.h is readable");
        Check(CountOccurrences(material_source, "ZFUNCTION()") >= 18, "MI35",
              "the Material facade publishes Unity's property-block surface");
        Check(CountOccurrences(material_source, "ZSCRIPTPROPERTY(") >= 1, "MI36",
              "and material.color is a property, not a method pair");

        const std::string light_source =
            ReadFile("Source/Runtime/Function/Framework/Component/Light/LightComponent.h");
        Check(!light_source.empty(), "LI17", "LightComponent.h is readable");
        // ⚠️ 比的是 `Name = "xxx"` 这个**模式**（带引号），不是单独的名字：
        // `color` / `intensity` 这些词在注释里到处都是，只 grep 名字会把注释也算
        // 进去，于是这条门在属性被删掉之后照样是绿的。
        //
        // 另外只数 ZSCRIPTPROPERTY 也数不准：flux / radius / type 那六个是从
        // ZPROPERTY 自动来的（引擎面），Unity 拼写这五个才是本次加的。
        Check(CountOccurrences(light_source, "Name = \"color\"") >= 1 &&
                  CountOccurrences(light_source, "Name = \"intensity\"") >= 1 &&
                  CountOccurrences(light_source, "Name = \"range\"") >= 1 &&
                  CountOccurrences(light_source, "Name = \"spotAngle\"") >= 1 &&
                  CountOccurrences(light_source, "Name = \"innerSpotAngle\"") >= 1,
              "LI18", "Light publishes the five Unity-spelled properties");
    }

    /// 一个 XY 平面上的单位四边形（逆时针，法线朝 +Z），两条三角形。
    std::vector<Vector3> QuadVertices()
    {
        return std::vector<Vector3> {Vector3(0.0f, 0.0f, 0.0f),
                                     Vector3(1.0f, 0.0f, 0.0f),
                                     Vector3(1.0f, 1.0f, 0.0f),
                                     Vector3(0.0f, 1.0f, 0.0f)};
    }

    std::vector<int32_t> QuadTriangles() { return std::vector<int32_t> {0, 1, 2, 0, 2, 3}; }

    /// 与顶点一一对应的 UV（左下 0,0 -> 右上 1,1）。
    std::vector<Vector2> QuadUVs()
    {
        return std::vector<Vector2> {Vector2(0.0f, 0.0f),
                                     Vector2(1.0f, 0.0f),
                                     Vector2(1.0f, 1.0f),
                                     Vector2(0.0f, 1.0f)};
    }

    //
    // ME* —— 网格（`Mesh` / `MeshFilter`）
    //
    // 这一组的判据围绕两件事：
    //   1. `Mesh` 的数据 API 是**真的**在算（法线 / 切线 / 包围盒都能被具体数值
    //      钉住），而不是"调了不崩"；
    //   2. `MeshFilter` 是**接进渲染器**的（sub-mesh 列表与包围盒都断言到
    //      renderer 上），不是"存了个字段"。
    //
    // 第 2 条是"接线门"：一个只改自己字段的组件等于没实现，而且静默。
    //
    void TestMeshParity()
    {
        ObjectManager* om = GET_SYSTEM(ObjectManager);
        Check(om != nullptr, "ME0", "ObjectManager is reachable");

        // ---- 空网格的形状 --------------------------------------------------
        Mesh* mesh = om->NewObject<Mesh>();
        Check(mesh->GetVertexCount() == 0 && mesh->GetTriangleCount() == 0, "ME1",
              "a fresh Mesh is empty");
        // ⚠️ subMeshCount 对空网格是 **0** 而不是 1：报 1 会让 `GetTriangles(0)`
        // 看起来该有东西。
        Check(mesh->GetSubMeshCount() == 0, "ME2", "and claims no sub-mesh (not 'one empty one')");
        Bounds empty_bounds = mesh->GetBounds();
        Check(Near(empty_bounds.center.x, 0.0f) && Near(empty_bounds.extents.x, 0.0f), "ME3",
              "an empty Mesh's bounds is the zero box (Unity's default(Bounds))");

        // ---- 顶点 / 三角形 --------------------------------------------------
        mesh->SetVertices(QuadVertices());
        mesh->SetTriangles(QuadTriangles());
        Check(mesh->GetVertexCount() == 4, "ME4", "vertexCount follows SetVertices");
        Check(mesh->GetTriangleCount() == 2, "ME5", "triangleCount is indices / 3");
        Check(mesh->GetSubMeshCount() == 1, "ME6", "and one sub-mesh once there are triangles");
        Check(mesh->GetTriangles() == QuadTriangles(), "ME7", "triangles round-trip");

        // 长度不是 3 的倍数：尾巴上多出来的索引被丢掉，而不是存成一个残缺三角形。
        mesh->SetTriangles(std::vector<int32_t> {0, 1, 2, 0, 2});
        Check(mesh->GetTriangles().size() == 3 && mesh->GetTriangleCount() == 1, "ME8",
              "a trailing partial triangle is dropped at write time");
        mesh->SetTriangles(QuadTriangles());

        // ---- 包围盒 --------------------------------------------------------
        Bounds quad_bounds = mesh->GetBounds();
        Check(Near(quad_bounds.center.x, 0.5f) && Near(quad_bounds.center.y, 0.5f) &&
                  Near(quad_bounds.GetSize().x, 1.0f) && Near(quad_bounds.GetSize().y, 1.0f) &&
                  Near(quad_bounds.GetSize().z, 0.0f),
              "ME9", "bounds is recomputed from the vertices (not stale, not a unit cube)");

        // 换顶点之后**不**显式重算也拿到新值：bounds 是每帧都可能被读的量，让它
        // 留着上一批顶点的结果比让它错更危险。
        mesh->SetVertices(std::vector<Vector3> {Vector3(-2.0f, 0.0f, 0.0f), Vector3(2.0f, 0.0f, 0.0f),
                                                Vector3(2.0f, 4.0f, 0.0f), Vector3(0.0f, 1.0f, 0.0f)});
        Check(Near(mesh->GetBounds().GetSize().x, 4.0f) && Near(mesh->GetBounds().GetSize().y, 4.0f),
              "ME10", "replacing the vertices invalidates the cached bounds");
        mesh->SetVertices(QuadVertices());
        mesh->RecalculateBounds();
        Check(Near(mesh->GetBounds().GetSize().x, 1.0f), "ME11", "RecalculateBounds() is explicit and idempotent");

        // ---- 法线 ----------------------------------------------------------
        //
        // ⚠️ 两面**共用**的那条边是判据的关键：只测一个三角形的话，"按面法线累加"
        // 与"随便取一个面的法线"给出同一个答案，平滑那一半根本没被测到。四边形
        // 的两个三角形法线都是 +Z，所以这里要的是"累加后归一化仍是 +Z" —— 换一个
        // 90° 折角的网格才能区分平滑与平面，那种用例留给变异体。
        mesh->RecalculateNormals();
        std::vector<Vector3> normals = mesh->GetNormals();
        Check(normals.size() == 4, "ME12", "RecalculateNormals fills one normal per vertex");
        bool all_up = true;
        for (const Vector3& normal : normals)
        {
            if (!Near(normal.x, 0.0f) || !Near(normal.y, 0.0f) || !Near(normal.z, 1.0f))
            {
                all_up = false;
            }
        }
        Check(all_up, "ME13", "a CCW quad in the XY plane gets +Z normals");

        // 反向绕序 -> 反向法线。"永远朝 +Z"是最容易写出来的错。
        mesh->SetTriangles(std::vector<int32_t> {0, 2, 1, 0, 3, 2});
        mesh->RecalculateNormals();
        Check(Near(mesh->GetNormals()[0].z, -1.0f), "ME14", "reversing the winding flips the normal");
        mesh->SetTriangles(QuadTriangles());
        mesh->RecalculateNormals();

        // 退化面（三点共线）必须跳过而不是产生 NaN —— NaN 一进累加就出不来了。
        Mesh* degenerate = om->NewObject<Mesh>();
        degenerate->SetVertices(std::vector<Vector3> {Vector3(0.0f, 0.0f, 0.0f),
                                                      Vector3(1.0f, 0.0f, 0.0f),
                                                      Vector3(2.0f, 0.0f, 0.0f)});
        degenerate->SetTriangles(std::vector<int32_t> {0, 1, 2});
        degenerate->RecalculateNormals();
        const Vector3 degenerate_normal = degenerate->GetNormals()[0];
        Check(!(degenerate_normal.x != degenerate_normal.x) && !(degenerate_normal.y != degenerate_normal.y) &&
                  !(degenerate_normal.z != degenerate_normal.z) && Near(degenerate_normal.x, 0.0f),
              "ME15", "a degenerate triangle yields a finite (zero) normal, not NaN");

        // ---- 切线 ----------------------------------------------------------
        //
        // 缺法线或缺 UV 时直接返回、留原样：切线由"UV 的 u 方向在模型空间指哪"
        // 定义，两样缺一样就没有定义。
        Mesh* tangents = om->NewObject<Mesh>();
        tangents->SetVertices(QuadVertices());
        tangents->SetTriangles(QuadTriangles());
        tangents->RecalculateTangents();
        Check(tangents->GetTangents().empty(), "ME16",
              "RecalculateTangents does nothing without normals and UVs");

        // 法线用上面那个四边形算出来的（+Z，4 个），UV 给一一对应的那份。
        tangents->SetNormals(mesh->GetNormals());
        tangents->SetUV0(QuadUVs());
        tangents->RecalculateTangents();
        std::vector<Vector4> tangent4 = tangents->GetTangents();
        Check(tangent4.size() == 4, "ME17", "RecalculateTangents fills one tangent per vertex");
        // UV 的 u 沿 +X 增长、法线是 +Z => 切线是 +X。
        //
        // ⚠️ 四个顶点**都**判：只判 [0] 的话，"每个三角形只给第一个顶点累加"
        // 这种错是不可能被发现的（它恰好把 [0] 算对了）。
        bool all_tangents_x = true;
        for (const Vector4& t4 : tangent4)
        {
            if (!Near(t4.x, 1.0f) || !Near(t4.y, 0.0f) || !Near(t4.z, 0.0f))
            {
                all_tangents_x = false;
            }
        }
        Check(all_tangents_x, "ME18", "every tangent follows the UV's u direction (+X here)");
        // w 是手性。这里必须是 **+1**（不是 |w| == 1）：UV 的 v 与法线满足右手系，
        // 恒返回 +1 的实现在这里也对，只有翻转 v 才分得出来（下一条）。
        Check(Near(tangent4[0].w, 1.0f), "ME19", "tangent.w is +1 for a right-handed UV layout");

        // 把 v 翻过来（上下镜像）：切线还是 +X，但手性变号。这一条是"w 恒为 +1"
        // 那个实现的对手 —— 少了它，w 这一位等于没测。
        tangents->SetUV0(std::vector<Vector2> {Vector2(0.0f, 1.0f),
                                               Vector2(1.0f, 1.0f),
                                               Vector2(1.0f, 0.0f),
                                               Vector2(0.0f, 0.0f)});
        tangents->RecalculateTangents();
        Check(Near(tangents->GetTangents()[0].x, 1.0f) && Near(tangents->GetTangents()[0].w, -1.0f),
              "ME46", "and -1 when the v axis is flipped (the bitangent follows v)");

        // ---- UV 通道 --------------------------------------------------------
        Check(tangents->GetUVs(0).size() == 4, "ME20", "channel 0 holds the UVs");
        Check(tangents->GetUVs(1).empty(), "ME21",
              "channel 1 is empty rather than silently returning channel 0");
        Check(!tangents->SetUVs(1, QuadUVs()), "ME22", "and SetUVs reports false for it");
        Check(tangents->GetUV0() == tangents->GetUVs(0), "ME23", "uv is GetUVs(0)");

        // ---- Clear ----------------------------------------------------------
        mesh->Clear();
        Check(mesh->GetVertexCount() == 0 && mesh->GetTriangleCount() == 0 &&
                  mesh->GetNormals().empty() && mesh->GetSubMeshCount() == 0,
              "ME24", "Clear() empties every attribute");
        Check(Near(mesh->GetBounds().GetSize().x, 0.0f), "ME25", "and resets the bounds");

        // ⚠️ 这条才是"空网格退回零盒"真正的门。前两条（ME3 / ME25）走的是
        // **缓存** —— 新对象的 `m_BoundsDirty` 是 false、`Clear()` 又是直接给
        // `m_Bounds` 赋值，两者都**不进** `ComputeBounds`。只有"从有顶点变成
        // 没顶点"才会真的走到那条 return：
        mesh->SetVertices(std::vector<Vector3>());
        Check(Near(mesh->GetBounds().GetSize().x, 0.0f) && Near(mesh->GetBounds().center.x, 0.0f),
              "ME47", "clearing the vertices yields the zero box, not a plausible unit cube");

        // =====================================================================
        // MeshFilter —— 接线
        // =====================================================================

        GameObject* go = new GameObject();
        om->AllocateAndAssignInstanceID(go);
        MeshRenderer* renderer = om->NewObject<MeshRenderer>();
        go->addComponent(renderer);
        renderer->PostLoadResource(go);

        MeshFilter* filter = om->NewObject<MeshFilter>();
        go->addComponent(filter);
        filter->PostLoadResource(go);

        Check(filter->HasRenderer(), "ME26", "a MeshFilter sees the sibling renderer");
        Check(filter->GetMesh() == nullptr, "ME27", "and starts with no runtime mesh");

        // 资产网格：写进 renderer 的 sub-mesh 列表。
        filter->SetSharedMeshPath("content/meshes/rock.zasset");
        Check(renderer->getSubMeshes().size() == 1, "ME28",
              "setting the shared mesh path gives the renderer one sub-mesh");
        Check(renderer->getSubMeshes()[0].m_MeshAsset == "content/meshes/rock.zasset", "ME29",
              "with exactly that asset path");
        Check(renderer->GetMaterialSlotCount() == 1, "ME30",
              "and the material override slots follow (SetSubMeshes resizes them)");
        Check(filter->GetSharedMeshPath() == "content/meshes/rock.zasset", "ME31",
              "the filter reads its own path back");

        // 空路径 = 不画东西（Unity 的 sharedMesh = null）。
        filter->SetSharedMeshPath("");
        Check(renderer->getSubMeshes().empty(), "ME32", "clearing the path clears the sub-meshes");

        // ---- 运行时网格：只推包围盒 ----------------------------------------
        Mesh* runtime_mesh = om->NewObject<Mesh>();
        runtime_mesh->SetVertices(QuadVertices());
        runtime_mesh->SetTriangles(QuadTriangles());
        const Bounds runtime_bounds = runtime_mesh->GetBounds();

        filter->SetMesh(runtime_mesh);
        Check(filter->GetMesh() == runtime_mesh, "ME33", "the runtime mesh round-trips");
        Check(Near(filter->GetLocalBounds().center.x, runtime_bounds.center.x) &&
                  Near(filter->GetLocalBounds().GetSize().x, runtime_bounds.GetSize().x),
              "ME34", "and the filter reports its bounds");
        const Bounds renderer_bounds = renderer->GetLocalBounds();
        Check(Near(renderer_bounds.center.x, runtime_bounds.center.x) &&
                  Near(renderer_bounds.GetSize().x, runtime_bounds.GetSize().x),
              "ME35", "the bounds reached the renderer (this is what culling reads)");

        filter->SetMesh(nullptr);
        Check(filter->GetMesh() == nullptr, "ME36", "clearing the runtime mesh clears the handle");
        // 撤掉之后回到"由资产算"的那条路（headless 下资产读不到 => 零盒）。零盒与
        // 四边形那个盒不同，所以这条能真的区分"撤掉了"和"没撤掉"。
        Check(Near(renderer->GetLocalBounds().GetSize().x, 0.0f), "ME37",
              "and the renderer's local-bounds override is lifted with it");

        // ---- 组件顺序无关 ----------------------------------------------------
        //
        // `GameObject::AddComponent` 是"先 PostLoadResource 再挂进列表"，所以
        // "先加 filter 后加 renderer"时 filter 那一次推送看不到 renderer。不记账
        // 的话这份网格就永远丢在半路上了 —— 这条是那条记账逻辑的门。
        GameObject* late_go = new GameObject();
        om->AllocateAndAssignInstanceID(late_go);
        MeshFilter* late_filter = om->NewObject<MeshFilter>();
        late_go->addComponent(late_filter);
        late_filter->PostLoadResource(late_go);

        Check(!late_filter->HasRenderer(), "ME38", "a filter with no renderer yet reports so");
        late_filter->SetSharedMeshPath("content/meshes/late.zasset");

        MeshRenderer* late_renderer = om->NewObject<MeshRenderer>();
        late_go->addComponent(late_renderer);
        late_renderer->PostLoadResource(late_go);
        Check(late_renderer->getSubMeshes().empty(), "ME39",
              "the renderer added afterwards starts empty");

        late_filter->Tick(0.0f);
        Check(late_renderer->getSubMeshes().size() == 1 &&
                  late_renderer->getSubMeshes()[0].m_MeshAsset == "content/meshes/late.zasset",
              "ME40", "and the pending push lands on the first tick after it appears");

        // ---- 发布面源码门 ----------------------------------------------------
        const std::string mesh_source = ReadFile("Source/Runtime/Scripting/ScriptMesh/ScriptMesh.h");
        Check(!mesh_source.empty(), "ME41", "ScriptMesh.h is readable");
        Check(CountOccurrences(mesh_source, "ZFUNCTION()") >= 20, "ME42",
              "the Mesh facade publishes Unity's data surface");
        Check(CountOccurrences(mesh_source, "ZSCRIPTPROPERTY(") >= 5, "ME43",
              "vertices / triangles / normals / uv / bounds are properties, not method pairs");

        const std::string filter_source =
            ReadFile("Source/Runtime/Function/Framework/Component/Mesh/MeshFilter.h");
        Check(!filter_source.empty(), "ME44", "MeshFilter.h is readable");
        // 带引号的模式：`mesh` 这个词在注释里到处都是，只 grep 名字会把注释算进去。
        Check(CountOccurrences(filter_source, "Name = \"mesh\"") >= 1 &&
                  CountOccurrences(filter_source, "Name = \"sharedMeshPath\"") >= 1,
              "ME45", "MeshFilter publishes mesh and sharedMeshPath");
    }

    //
    // AN* —— Unity 的 `Animator` 其余面（speed / Rebind / 控制器形状 / 事件）
    //
    // 判据围绕两件事：
    //   1. **能算的必须算出具体数字**：speed 的倍数、Rebind 之后的播放头、事件
    //      在越过时间点那一刻触发一次。这些都有唯一正确答案，"调了不崩"不算测。
    //   2. **没控制器时全部走安全默认**且**不崩** —— 那是一条真实存在的路径
    //      （组件挂了但资产没导入完），而且它最容易在加了新成员之后忘记判空。
    //
    // 控制器是**在内存里现搭**的（`AnimatorInstance::Initialize` 收 const 引用，
    // 不碰资产系统），所以这一组不依赖任何 .zasset —— 与 `Mesh` 那一组同样的
    // 理由：资产门面的测试在本工程里一贯很弱。
    //
    void TestAnimatorParity()
    {
        ObjectManager* om = GET_SYSTEM(ObjectManager);
        Check(om != nullptr, "BN0", "ObjectManager is reachable");

        // ---- 一个两状态 / 两参数的控制器 ------------------------------------
        AnimatorController controller;

        AnimatorParameter speed_parameter;
        speed_parameter.name = "speed";
        speed_parameter.type = static_cast<int>(AnimatorParameterType::Float);
        speed_parameter.default_float = 0.0f;
        controller.parameters.push_back(speed_parameter);

        AnimatorParameter combo_parameter;
        combo_parameter.name = "combo";
        combo_parameter.type = static_cast<int>(AnimatorParameterType::Int);
        combo_parameter.default_int = 3;
        controller.parameters.push_back(combo_parameter);

        AnimatorState idle;
        idle.name = "Idle";
        idle.clip_file_path = "clips/idle.zasset";
        idle.wrap_mode = static_cast<int>(AnimatorWrapMode::Loop);
        idle.clip_length = 2.0f;
        AnimatorEvent footstep;
        footstep.normalized_time = 0.5f;      // 1.0 秒处
        footstep.function_name = "Footstep";
        idle.events.push_back(footstep);
        controller.states.push_back(idle);

        AnimatorState run;
        run.name = "Run";
        run.clip_file_path = "clips/run.zasset";
        run.wrap_mode = static_cast<int>(AnimatorWrapMode::Loop);
        run.clip_length = 1.0f;
        controller.states.push_back(run);

        // ⚠️ 第三个状态是**故意**的：参数 2 个、状态 3 个。数量一样的话
        // "参数数报成状态数"和"状态数报成参数数"这两个错误都**不可观测**
        // （2 == 2），那两条计数断言就成了白测。名字那一组（BN9 / BN12）也
        // 一样：两边各 2 个时，把参数名换成状态名在计数上毫无痕迹。
        AnimatorState jump;
        jump.name = "Jump";
        jump.clip_file_path = "clips/jump.zasset";
        jump.wrap_mode = static_cast<int>(AnimatorWrapMode::Once);
        jump.clip_length = 0.5f;
        controller.states.push_back(jump);

        // Idle -> Run：`speed > 0.5`，0.4 秒混出。
        AnimatorTransition to_run;
        to_run.from_state = 0;
        to_run.to_state = 1;
        AnimatorCondition condition;
        condition.parameter = "speed";
        condition.mode = static_cast<int>(AnimatorConditionMode::Greater);
        condition.threshold = 0.5f;
        to_run.conditions.push_back(condition);
        to_run.duration = 0.4f;
        controller.transitions.push_back(to_run);

        controller.default_state = 0;

        // ---- 没控制器时的安全默认 ------------------------------------------
        AnimationComponent* bare = om->NewObject<AnimationComponent>();
        Check(bare->GetStateCount() == 0 && bare->GetParameterCount() == 0, "BN1",
              "a component with no controller reports zero states and parameters");
        Check(bare->GetStateName(0).empty() && bare->GetParameterName(0).empty(), "BN2",
              "and empty names rather than a crash");
        Check(!bare->IsName("Idle"), "BN3", "IsName is false with nothing loaded");
        Check(bare->GetFiredEventCount() == 0 && bare->GetFiredEvent(0).empty(), "BN4",
              "no events fire without a controller");
        Check(Near(bare->GetSpeed(), 1.0f), "BN5", "speed defaults to 1");
        bare->Update(0.5f);   // 不崩
        Check(true, "BN6", "Update on a bare component does not crash");

        // ---- 装上控制器 ----------------------------------------------------
        AnimationComponent* animator = om->NewObject<AnimationComponent>();
        std::string init_error;
        Check(animator->GetAnimator().Initialize(controller, init_error), "BN7",
              "the in-memory controller initializes");

        Check(animator->GetStateCount() == 3, "BN8", "stateCount comes from the controller");
        Check(animator->GetStateName(0) == "Idle" && animator->GetStateName(1) == "Run" &&
                  animator->GetStateName(2) == "Jump",
              "BN9", "and the names are the authored ones");
        Check(animator->GetStateName(3).empty() && animator->GetStateName(-1).empty(), "BN10",
              "out-of-range state names are empty, not a crash");

        Check(animator->GetParameterCount() == 2, "BN11",
              "parameterCount comes from the controller (and is NOT the state count)");
        Check(animator->GetParameterName(0) == "speed" && animator->GetParameterName(1) == "combo",
              "BN12", "and so do the parameter names");
        Check(animator->GetParameterName(2).empty(), "BN13", "out-of-range is empty");

        // 默认状态与默认值
        Check(animator->IsName("Idle"), "BN14", "the default state is current after Initialize");
        Check(Near(animator->GetFloat("speed"), 0.0f), "BN15",
              "float parameters are seeded from the controller default");
        Check(animator->GetInteger("combo") == 3, "BN16",
              "and so are int parameters (GetInteger is Unity's spelling)");
        animator->SetInteger("combo", 7);
        Check(animator->GetInt("combo") == 7, "BN17",
              "SetInteger / GetInt are the same slot (both spellings stay in sync)");

        // ---- speed ----------------------------------------------------------
        //
        // ⚠️ 判的是**倍数关系**而不是"时间变了"：Idle 的 clip_length 是 2 秒，
        // 所以 0.5 秒 * speed 2 = 1 秒 = 归一化 0.5。只断言"推进了"会把"乘了
        // 1.5 倍"和"乘了 2 倍"都放过。
        animator->Update(0.5f);
        Check(Near(animator->GetNormalizedTime(), 0.25f), "BN18",
              "speed 1 advances the playhead by deltaTime (0.5s of a 2s clip = 0.25)");

        animator->SetSpeed(2.0f);
        Check(Near(animator->GetSpeed(), 2.0f), "BN19", "speed round-trips");
        animator->Update(0.5f);
        Check(Near(animator->GetNormalizedTime(), 0.75f), "BN20",
              "speed 2 advances twice as far (0.25 + 0.5 = 0.75)");

        // 负数夹到 0：倒放没有后端，接受 -1 会得到一个"看着在跑其实卡住"的组件。
        animator->SetSpeed(-1.0f);
        Check(Near(animator->GetSpeed(), 0.0f), "BN21", "a negative speed clamps to 0");
        animator->Update(0.5f);
        Check(Near(animator->GetNormalizedTime(), 0.75f), "BN22",
              "and a frozen animator does not move the playhead");

        // ---- 事件：越过时间点触发一次，下一次推进清空 ------------------------
        animator->SetSpeed(1.0f);
        animator->Rebind();
        Check(animator->GetFiredEventCount() == 0, "BN23", "Rebind clears the fired-event list");
        animator->Update(0.5f);
        Check(animator->GetFiredEventCount() == 0, "BN24",
              "an event at normalized 0.5 has not fired at 0.25");
        animator->Update(0.75f);   // 归一化 0.5，正好越过
        Check(animator->GetFiredEventCount() == 1 && animator->GetFiredEvent(0) == "Footstep",
              "BN25", "and fires once when the playhead crosses it");
        animator->Update(0.01f);
        Check(animator->GetFiredEventCount() == 0, "BN26",
              "the list is cleared at the start of every advance (no double-read)");

        // ---- 过渡 + Rebind ----------------------------------------------------
        animator->Rebind();
        Check(animator->IsName("Idle"), "BN27", "Rebind returns to the default state");
        Check(Near(animator->GetFloat("speed"), 0.0f), "BN28",
              "and restores the controller defaults (the parameter was set to 1 earlier)");

        animator->SetFloat("speed", 1.0f);
        animator->Update(0.016f);
        Check(animator->IsInTransition(), "BN29", "a satisfied condition starts a transition");
        animator->Update(0.5f);   // 超过 0.4 秒的混出时长
        Check(animator->IsName("Run"), "BN30", "and the target state is current once the fade ends");

        animator->PlayState("Idle");
        Check(animator->IsName("Idle"), "BN31", "PlayState still jumps directly");
        Check(!animator->IsName("Run"), "BN32", "IsName is exact, not a prefix match");

        // ---- 发布面源码门 ------------------------------------------------------
        const std::string animator_source =
            ReadFile("Source/Runtime/Function/Framework/Component/Animation/AnimationComponent.h");
        Check(!animator_source.empty(), "BN33", "AnimationComponent.h is readable");
        Check(CountOccurrences(animator_source, "Name = \"speed\"") >= 1, "BN34",
              "speed is a script property, not a method pair");
        Check(CountOccurrences(animator_source, "GetParameterCount") >= 1 &&
                  CountOccurrences(animator_source, "IsName") >= 1 &&
                  CountOccurrences(animator_source, "Rebind") >= 1,
              "BN35", "the controller-shape and Rebind entry points are published");

        const std::string instance_source = ReadFile("Source/Runtime/Function/Animation/AnimatorInstance.h");
        Check(!instance_source.empty(), "BN36", "AnimatorInstance.h is readable");
        Check(CountOccurrences(instance_source, "GetParameterName") >= 1 &&
                  CountOccurrences(instance_source, "GetStateName") >= 1,
              "BN37", "the shape queries live on the instance, next to the runtime state");
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

    // Gizmos 走 GET_SYSTEM(DebugDrawManager)。不注册的话整套是 no-op，而
    // "画不出来"和"没画"看起来一模一样 —— 所以这里必须给一个真的。
    // 不 Initialize：Initialize 要 Vulkan 与 RenderSystem，而 Gizmos 只需要
    // 它手上的那个 DebugDrawGroup（DebugDrawContext 是纯 CPU 的 map）。
    SystemRegistry::GetInstance().RegisterSystem(new DebugDrawManager());
    Check(GET_SYSTEM(DebugDrawManager) != nullptr, "A0e", "the debug-draw manager is reachable");

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
    TestGizmos();
    TestGizmosFrustum();
    TestGizmosWiring();
    TestBoundsAndColor32();
    TestBoundsAndColor32Wiring();
    TestScriptCoroutines();
    TestScriptCoroutineWiring();
    TestLateUpdate();
    TestLateUpdateWiring();
    TestEnabledCallbacksWiring();
    TestApplicationMessages();
    TestApplicationMessagesWiring();
    TestTransformChildrenChanged();
    TestTransformChildrenChangedWiring();
    TestDrawGizmos();
    TestDrawGizmosWiring();
    TestMouseEvents();
    TestMouseEventsWiring();
    TestScriptMethodPublishing();
    TestFixedUpdateWiring();
    TestValueTypeFacades();
    TestValueTypeFacadeWiring();
    TestQuaternionEuler();
    TestQuaternionEulerWiring();
    TestFrustumCulling();
    TestFrustumWiring();
    TestRendererVisibility();
    TestRendererVisibilityWiring();
    TestAnimationCurve();
    TestAnimationCurveWiring();
    TestIntVectors();
    TestRects();
    TestRectWiring();
    TestPlaneAndGeometryUtility();
    TestPlaneWiring();
    TestSmoothDamp();
    TestSmoothDampWiring();
    TestLayerMask();
    TestLayerMaskWiring();
    TestVector3Statics();
    TestVector3StaticsWiring();
    TestMathfTail();
    TestColorExtras();
    TestSystemInfo();
    TestQualitySettings();
    TestCursorFacade();
    TestTimeGates();
    TestTexture2DFacade();
    TestRenderTextureFacade();
    TestParticlePlayback();
    TestApplicationExtras();
    TestInputParity();
    TestMaterialLightParity();
    TestMeshParity();
    TestAnimatorParity();
    TestRigidbodyExtras();
    TestJointComponents();
    TestGradient();
    TestGradientWiring();
    TestColliderBounds();
    TestGameObjectProperties();
    TestColorExtrasWiring();
    TestTransformExtras();
    TestTransformMoveAfterSibling();
    TestTransformExtrasWiring();
    TestWiringGate();

    std::printf("\n=== %d checks, %d failures ===\n", g_Checks, g_Failures);
    return (g_Failures == 0) ? 0 : 1;
}
