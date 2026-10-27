// ==========================================================================
// TypeScriptComponent 的 prefab / zasset 反序列化冒烟测试
//
// 为什么需要它
// ------------
// ScriptComponentSmokeTest 走的是 **运行时挂载** 那条路：
//
//   ObjectManager::Produce(GameObject) -> AddComponentByTypeName(...)
//     -> SetScriptGuid -> PostLoadResource -> ResolveScript -> LoadModule
//     -> CreateInstance -> OnAwake / OnStart -> Tick -> OnDestroy
//
// 但用户真正拿到一个带脚本的 GameObject，绝大多数时候不是这么来的 —— 是从
// **资产文件**里反序列化出来的。那条路多出两件事，前一个测试一件都没碰：
//
//   1. m_ScriptGuid / m_ClassName / m_SerializedFields 能不能穿过磁盘往返
//      （三个字段都是 ZPROPERTY 且在 -- Serialised -- 段内，理论上会，但没验过）；
//   2. 还原出来的 guid 能不能在 runtime 侧解析到编译产物并真的绑上 —— 也就是
//      PrefabPostLoadDriver 那一趟 PostLoadResource 有没有把组件带起来；
//   3. Inspector 的字段 override（m_SerializedFields）能不能跟着一起还原，并在
//      ApplySerializedFields() 里生效。
//
// 资产分两轨（.object.json 已废弃）：
//   * editor：`.prefab` —— YAML 对象图，扩展名即权威，不用读 header，
//              AssetManager::WriteObjectsToYaml 写；
//   * 真机 / legacy：`.zasset` —— 二进制 SerializedFile（AssetFileHeader
//              176 字节），所有 ZEngine 资产共用的统一容器（类比 UE 的 .uasset），
//              AssetManager::WriteObjectsToDiskThreadSafe 写。
//   分流点是 AssetManager::IsYamlAuthoringAssetPath()：`.prefab` 在
//   YamlAuthoringAssetExtensions() 里，`.zasset` 不在。两轨共用同一份有序对象
//   表和同一套引用模型（文件内 ImmediatePtr/fileID，跨文件 PPtr/GUID）。
//
// 测的路径（两种格式各一遍）
// ------------------------
//   PrefabUtility::SaveAsPrefabAsset(go, path)
//     -> PrefabUtility::InstantiateFromPath(path, invoke_post_load)
//        -> LoadPrefabAsset（整图读回，读一次然后缓存）
//        -> Instantiate：CloneSubtree 深克隆（模板不动）
//        -> PrefabPostLoadDriver::Run 沿 Transform.m_Children DFS
//           逐组件下发 PostLoadResource
//
// 关于"为什么 override 用 speed=7"
// --------------------------------
// HostedMover 的类默认 speed 是 2。反序列化出来的实例如果走到 3.5（=7*0.5）
// 而不是 1（=2*0.5），就说明 override 真的穿过了磁盘并在 ApplySerializedFields
// 里生效了 —— 只看"字段在不在"是不够的，得看它有没有起作用。
//
// 关于本机怎么跑
// --------------
// 和 ScriptComponentSmokeTest 一样：TypeManager.cpp 必须列进 --extra-source，
// 否则 .a 里那份旧的 TypeRegistrationDesc 布局会让类型表是空的。
// 另外这里还要把 PrefabAsset / PrefabRefComponent / RigidbodyComponent 的
// .gen.cpp 编进来 —— PrefabUtility.cpp 里的 TypeOf<PrefabAsset> /
// Cast<PrefabRefComponent> / Cast<RigidBodyComponent> 要它们的
// TypeContainer<T>::m_Type（见同目录 CMakeLists.txt）。
//
// 比那再多一层：这条路径穿过序列化层，而 bin/libZRuntime.a 里的几个成员比
// 源码旧，**对象的构造方和它的生成式 Transfer() 不是同一份布局**，于是
// Transform 被按旧布局 new 出来、又被按新布局序列化 —— 读回来就是越界
// （si_addr 是个小负数）。必须把这几份也现编，并把 .a 里的同名成员踢掉：
//
//   --extra-source engine/Source/Runtime/Core/YamlSerialize/YamlObjectGraph.cpp
//   --extra-source engine/Source/Runtime/Resource/Asset/AssetManager.cpp
//   --extra-source engine/Source/Runtime/BaseClasses/ImmediatePtr.cpp
//   --extra-source engine/Source/Runtime/BaseClasses/IPPtrResolver.cpp
//   --extra-source engine/Source/Runtime/Function/Framework/Component/Transform/Transform.cpp
//   --exclude-archive-member YamlObjectGraph.cpp.o AssetManager.cpp.o
//   --exclude-archive-member ImmediatePtr.cpp.o IPPtrResolver.cpp.o Transform.cpp.o
//
//   （另外 PrefabUtility.cpp / PrefabSubtreeCloner.cpp 也要现编：.a 那份里
//   根本没有 LoadPrefabAsset —— 整图加载是后来加的。）
//
// 完整 cmake 构建里所有 TU 都是同一份源码，这些一条都不需要。
//
// 关于清理
// --------
// 刻意不做：克隆出来的对象由 PrefabSubtreeCloner 分配，走哪套分配器取决于它
// 内部实现，拿 ObjectManager::Destroy（= delete）去收 mimalloc 出来的指针会在
// mi_usable_size 里 SIGSEGV（ScriptComponentSmokeTest 里踩过一次）。测试进程
// 退出即回收，不值得为此引入崩溃风险。
// ==========================================================================

#if !PAPI_QUICKJS
#error "ScriptComponentPrefabTest 需要 QuickJS 后端（PAPI_QUICKJS）。"
#endif

#include "Runtime/BaseClasses/Cast.h"
#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/BaseClasses/ObjectManager.h"
#include "Runtime/BaseClasses/TypeManager.h"
#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Function/Command/CommandSystem.h"
#include "Runtime/Function/Framework/Component/Script/TypeScriptComponent.h"
#include "Runtime/Function/Framework/Component/Transform/Transform.h"
#include "Runtime/Project/ProjectInfo.h"
#include "Runtime/RegisterRuntime.h"
#include "Runtime/Resource/Asset/RuntimeAssetManager.h"
#include "Runtime/Resource/Prefab/PrefabAsset.h"
#include "Runtime/Resource/Prefab/PrefabUtility.h"
#include "Runtime/Scripting/ScriptRegistry.h"
#include "Runtime/Scripting/ScriptingManager.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifndef ZENGINE_SCRIPT_COMPONENT_RESOURCE_DIR
#define ZENGINE_SCRIPT_COMPONENT_RESOURCE_DIR ""
#endif

namespace
{
    int g_Checks = 0;
    int g_Failed = 0;

    std::string Id(const char* tag, int n)
    {
        return std::string(tag) + std::to_string(n);
    }

    void CheckTrue(const char* id, const char* what, bool cond)
    {
        ++g_Checks;
        if (!cond)
        {
            ++g_Failed;
            std::printf("  [FAIL] %-5s %s\n", id, what);
        }
        else
        {
            std::printf("  [ ok ] %-5s %s\n", id, what);
        }
    }

    void CheckEq(const char* id, const char* what, const std::string& got, const std::string& want)
    {
        ++g_Checks;
        if (got != want)
        {
            ++g_Failed;
            std::printf("  [FAIL] %-5s %s  got='%s' want='%s'\n", id, what, got.c_str(), want.c_str());
        }
        else
        {
            std::printf("  [ ok ] %-5s %s  got='%s'\n", id, what, got.c_str());
        }
    }

    bool WriteTextFile(const std::filesystem::path& path, const std::string& text)
    {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream out(path, std::ios::binary);
        if (!out)
        {
            return false;
        }
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        return true;
    }

    bool ReadTextFile(const std::filesystem::path& path, std::string& out)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
        {
            return false;
        }
        out.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        return true;
    }

    /// 造一个 GameObject，走 ObjectManager::Produce（普通 new）+ 真 InstanceID。
    /// 与 AddComponentByTypeName 内部同一套分配，避免两边分配器不配对。
    GameObject* MakeGameObject(ObjectManager* object_manager, const char* name)
    {
        Object* raw = object_manager->Produce(TypeOf<GameObject>(), 0);
        if (raw == nullptr)
        {
            return nullptr;
        }
        object_manager->AllocateAndAssignInstanceID(raw);
        GameObject* go = static_cast<GameObject*>(raw);
        go->SetName(eastl::string(name));
        return go;
    }

    /// 从活实例上读一个字段的字符串值（每次都重新读，不用缓存）。
    std::string LiveValue(TypeScriptComponent* comp, const char* key)
    {
        for (const auto& entry : comp->EnumerateLiveFields())
        {
            if (std::get<0>(entry) == key)
            {
                return std::get<2>(entry);
            }
        }
        return "<missing>";
    }

    /// 反序列化一份资产文件并逐项检查。返回克隆出来的根 GameObject（未绑的实例
    /// 也返回），失败返回 nullptr。
    GameObject* VerifyRoundTrip(const char*                    tag,
                                const std::filesystem::path&   asset_path,
                                const eastl::string&           expected_guid,
                                GameObject*                    original_go,
                                TypeScriptComponent*           original_comp)
    {
        std::printf("\n--- %s. %s 反序列化 ---\n", tag, asset_path.filename().string().c_str());

        GameObject* clone = PrefabUtility::InstantiateFromPath(asset_path, /*invoke_post_load=*/true);
        CheckTrue(Id(tag, 1).c_str(), "InstantiateFromPath 返回了非空的根 GameObject", clone != nullptr);
        if (clone == nullptr)
        {
            return nullptr;
        }
        CheckTrue(Id(tag, 2).c_str(), "克隆是新对象，不是模板本身", clone != original_go);
        CheckEq(Id(tag, 3).c_str(), "GameObject 名字穿过了磁盘",
                std::string(clone->GetName().c_str()), "PrefabHost");

        Component* raw_comp = clone->GetComponentByTypeName("TypeScriptComponent");
        CheckTrue(Id(tag, 4).c_str(), "克隆上找得到 TypeScriptComponent", raw_comp != nullptr);
        if (raw_comp == nullptr)
        {
            return clone;
        }
        auto* comp = Cast<TypeScriptComponent>(raw_comp);
        CheckTrue(Id(tag, 5).c_str(), "Cast<TypeScriptComponent> 成功", comp != nullptr);
        if (comp == nullptr)
        {
            return clone;
        }
        CheckTrue(Id(tag, 6).c_str(), "组件也是新的（不是原来那个）", comp != original_comp);

        // 下面三条是这条路径真正要钉的
        CheckEq(Id(tag, 7).c_str(), "m_ScriptGuid 从资产文件里还原出来了",
                std::string(comp->GetScriptGuid().c_str()), std::string(expected_guid.c_str()));
        CheckTrue(Id(tag, 8).c_str(),
                  "PostLoadResource 凭还原出来的 guid 把组件绑上了", comp->HasLiveInstance());
        CheckEq(Id(tag, 9).c_str(), "Inspector override 跟着一起还原了",
                std::string(comp->GetSerializedField(eastl::string("speed")).c_str()), "7");

        if (!comp->HasLiveInstance())
        {
            return clone;
        }

        // 还原出来还得**起作用**：类默认 speed 是 2，override 是 7。
        CheckEq(Id(tag, 10).c_str(), "ApplySerializedFields 把 override 推到了活实例上",
                LiveValue(comp, "speed"), "7");
        CheckEq(Id(tag, 11).c_str(), "新实例只 Awake 了一次", LiveValue(comp, "awakeCount"), "1");
        CheckEq(Id(tag, 12).c_str(), "宿主注入进了克隆体（this.gameObject 非空）",
                LiveValue(comp, "gotGameObject"), "true");

        comp->Tick(0.5f);
        CheckEq(Id(tag, 13).c_str(), "一帧走了 3.5（speed=7 * dt=0.5，不是类默认的 2*0.5）",
                LiveValue(comp, "lastSnapshot"), "3.5000,1,0.5");

        return clone;
    }
}  // namespace

int main(int argc, char** argv)
{
    // 不缓冲：崩在半路时还能看到已经跑到哪一步。
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    std::string resource_dir = ZENGINE_SCRIPT_COMPONENT_RESOURCE_DIR;
    for (int i = 1; i + 1 < argc; ++i)
    {
        if (std::strcmp(argv[i], "--resource-dir") == 0)
        {
            resource_dir = argv[i + 1];
        }
    }
    if (resource_dir.empty())
    {
        std::printf("FATAL: 需要 --resource-dir <ScriptVM/Test 目录>（.ts 与 TscOutput 在这儿）。\n");
        return 77;
    }

    std::printf("=== TypeScriptComponent prefab / zasset 反序列化冒烟测试 ===\n");

    // ---- 1. 造一个临时工程 ------------------------------------------------
    std::error_code ec;
    const std::filesystem::path scratch = std::filesystem::weakly_canonical(
        std::filesystem::temp_directory_path() / "zengine_script_prefab_smoke", ec);
    std::filesystem::remove_all(scratch, ec);
    std::filesystem::create_directories(scratch, ec);

    const std::filesystem::path project_file = scratch / "Prefab.zproject";
    WriteTextFile(project_file,
                  "name: Prefab\n"
                  "version: 1.0.0\n"
                  "scripts_dir: Scripts\n"
                  "intermediate_dir: Intermediate\n");

    const std::filesystem::path ts_dir = scratch / "Scripts";
    const std::filesystem::path js_dir = scratch / "Intermediate" / "Scripts";
    std::filesystem::create_directories(ts_dir, ec);
    std::filesystem::create_directories(js_dir, ec);

    bool staged = true;
    {
        std::string text;
        if (!ReadTextFile(std::filesystem::path(resource_dir) / "Scripts" / "HostedMover.ts", text) ||
            !WriteTextFile(ts_dir / "HostedMover.ts", text))
        {
            std::printf("FATAL: 读不到/写不出 HostedMover.ts（resource_dir=%s）\n", resource_dir.c_str());
            staged = false;
        }
    }
    {
        std::string text;
        if (!ReadTextFile(std::filesystem::path(resource_dir) / "TscOutput" / "HostedMover.js", text) ||
            !WriteTextFile(js_dir / "HostedMover.js", text))
        {
            std::printf("FATAL: 读不到/写不出 HostedMover.js（resource_dir=%s）\n", resource_dir.c_str());
            staged = false;
        }
    }
    if (!staged)
    {
        return 77;
    }

    // ---- 2. 起系统 --------------------------------------------------------
    RegisterCore();
    REGISTER_SYSTEM(CommandSystem);
    REGISTER_SYSTEM(ProjectInfo);
    REGISTER_SYSTEM(ScriptRegistry);
    REGISTER_SYSTEM(ScriptingManager);
    // PrefabUtility 全程走 GET_SYSTEM(AssetManager)。Runtime 侧那份就够（只有
    // path<->fileID 映射，不碰编辑器的资产库）。
    REGISTER_SYSTEM_AS(RuntimeAssetManager, AssetManager);
    GET_SYSTEM(CommandSystem)->GetProjectPath() = project_file;
    GET_SYSTEM(CommandSystem)->GetWorkingDir() = scratch;
    GET_SYSTEM(CommandSystem)->SetInitialized(true);

    if (!SystemRegistry::GetInstance().InitializeAll(false))
    {
        std::printf("FATAL: SystemRegistry::InitializeAll(false) 失败。\n");
        return 77;
    }

    auto* sm = GET_SYSTEM(ScriptingManager);
    auto* registry = GET_SYSTEM(ScriptRegistry);
    auto* object_manager = GET_SYSTEM(ObjectManager);
    if (sm == nullptr || registry == nullptr || object_manager == nullptr || sm->GetEnv() == nullptr)
    {
        std::printf("FATAL: ScriptingManager / ScriptRegistry / ObjectManager / ScriptEnv 没起来。\n");
        return 77;
    }

    // ---- A0. 类型表前提 ---------------------------------------------------
    std::printf("\n--- A0. 类型表前提 ---\n");
    CheckTrue("A0a", "GameObject 注册进了类型表",
              TypeManager::GetInstance().ClassNameToType("GameObject") != nullptr);
    CheckTrue("A0b", "Transform 注册进了类型表",
              TypeManager::GetInstance().ClassNameToType("Transform") != nullptr);
    CheckTrue("A0c", "TypeScriptComponent 注册进了类型表",
              TypeManager::GetInstance().ClassNameToType("TypeScriptComponent") != nullptr);
    CheckTrue("A0d", "PrefabAsset 注册进了类型表（PrefabUtility 的 TypeOf<PrefabAsset> 靠它）",
              TypeManager::GetInstance().ClassNameToType("PrefabAsset") != nullptr);
    CheckTrue("A0e", "GameObject 有 factory（否则 AddComponentByTypeName 造不出对象）",
              TypeOf<GameObject>()->GetFactory() != nullptr);
    if (TypeManager::GetInstance().ClassNameToType("GameObject") == nullptr ||
        TypeManager::GetInstance().ClassNameToType("TypeScriptComponent") == nullptr ||
        TypeManager::GetInstance().ClassNameToType("PrefabAsset") == nullptr)
    {
        std::printf("FATAL: 类型表不全，后面的存取路径无从测试。\n");
        return 77;
    }

    // ---- A. 脚本注册表：拿到 guid ----------------------------------------
    std::printf("\n--- A. ScriptRegistry ---\n");
    ScriptAsset* hosted = nullptr;
    for (auto* asset : registry->GetAll())
    {
        const std::string rel(asset->m_SourceRelPath.c_str());
        if (rel.find("HostedMover.ts") != std::string::npos)
        {
            hosted = asset;
        }
    }
    CheckTrue("A1", "HostedMover.ts 被扫进注册表", hosted != nullptr);
    if (hosted == nullptr)
    {
        return 1;
    }
    CheckEq("A2", "guid 是 32 位小写 hex", std::to_string(hosted->m_Guid.size()), "32");

    // ---- B. 造宿主并存盘 --------------------------------------------------
    std::printf("\n--- B. 造宿主并写两份资产（.prefab / .zasset）---\n");
    auto* go = MakeGameObject(object_manager, "PrefabHost");
    if (go == nullptr)
    {
        std::printf("FATAL: Produce(GameObject) 返回 nullptr。\n");
        return 77;
    }
    CheckTrue("B1", "AddComponentByTypeName(\"Transform\") 成功（Prefab 的根必须有 Transform）",
              go->AddComponentByTypeName("Transform") != nullptr);

    auto* comp = Cast<TypeScriptComponent>(go->AddComponentByTypeName("TypeScriptComponent"));
    CheckTrue("B2", "挂上了 TypeScriptComponent", comp != nullptr);
    if (comp == nullptr)
    {
        return 1;
    }
    comp->SetScriptGuid(hosted->m_Guid);
    CheckTrue("B3", "内存里这份是绑着的（后面要用它和反序列化出来的那份对比）",
              comp->HasLiveInstance());

    // Inspector override。注意**不要**用 LiveSetField：那只改活实例 + 存快照，
    // 这里要的是"authoring 数据"本身落盘。内存里这份的活实例仍是类默认的 2，
    // 所以后面克隆体上看到 7 就一定是文件里带过来的。
    comp->SetSerializedField(eastl::string("speed"), eastl::string("7"));
    CheckEq("B4", "override 记下了 speed=7",
            std::string(comp->GetSerializedField(eastl::string("speed")).c_str()), "7");
    CheckEq("B5", "内存里这份的活实例 speed 还是类默认的 2（对照组）",
            LiveValue(comp, "speed"), "2");

    // 写盘前必须把运行期组件表同步回序列化容器（m_Components）。
    go->SyncSerializedComponents();

    const std::filesystem::path prefab_path = scratch / "Host.prefab";
    const std::filesystem::path zasset_path = scratch / "Host.zasset";
    CheckTrue("B6", "SaveAsPrefabAsset 写出了 .prefab（YAML）",
              PrefabUtility::SaveAsPrefabAsset(go, prefab_path));
    CheckTrue("B7", ".prefab 真的落在磁盘上", std::filesystem::exists(prefab_path, ec));
    CheckTrue("B8", "SaveAsPrefabAsset 写出了 .zasset（二进制 SerializedFile）",
              PrefabUtility::SaveAsPrefabAsset(go, zasset_path));
    CheckTrue("B9", ".zasset 真的落在磁盘上", std::filesystem::exists(zasset_path, ec));

    // ---- C / D. 两种格式各反序列化一遍 ------------------------------------
    GameObject* from_prefab = VerifyRoundTrip("C", prefab_path, hosted->m_Guid, go, comp);
    GameObject* from_zasset = VerifyRoundTrip("D", zasset_path, hosted->m_Guid, go, comp);

    // ---- E. 反证：绑的动作是 PostLoadResource 那一趟做的 -------------------
    // invoke_post_load=false 时整棵树照常克隆、字段照常还原，但 driver 不发
    // PostLoadResource —— 组件就该是休眠的。这两条一起钉住"还原"和"绑"是两件事。
    std::printf("\n--- E. invoke_post_load=false（还原但不绑）---\n");
    const std::filesystem::path* paths[2] = {&prefab_path, &zasset_path};
    for (int i = 0; i < 2; ++i)
    {
        const char* tag = (i == 0) ? "E-prefab" : "E-zasset";
        GameObject* raw = PrefabUtility::InstantiateFromPath(*paths[i], /*invoke_post_load=*/false);
        CheckTrue(Id(tag, 1).c_str(), "整棵树照常克隆出来了", raw != nullptr);
        if (raw == nullptr)
        {
            continue;
        }
        auto* raw_comp = Cast<TypeScriptComponent>(raw->GetComponentByTypeName("TypeScriptComponent"));
        CheckTrue(Id(tag, 2).c_str(), "组件也在", raw_comp != nullptr);
        if (raw_comp == nullptr)
        {
            continue;
        }
        CheckEq(Id(tag, 3).c_str(), "guid 照常从文件里还原出来了",
                std::string(raw_comp->GetScriptGuid().c_str()), std::string(hosted->m_Guid.c_str()));
        CheckTrue(Id(tag, 4).c_str(), "但不发 PostLoadResource 就不绑（证明绑是那一趟做的）",
                  !raw_comp->HasLiveInstance());
    }

    // ---- F. 两轨一致性 ----------------------------------------------------
    std::printf("\n--- F. 两轨一致性 ---\n");
    CheckTrue("F1", ".prefab 与 .zasset 都克隆出了根", from_prefab != nullptr && from_zasset != nullptr);
    if (from_prefab != nullptr && from_zasset != nullptr)
    {
        auto* c_prefab = Cast<TypeScriptComponent>(from_prefab->GetComponentByTypeName("TypeScriptComponent"));
        auto* c_zasset = Cast<TypeScriptComponent>(from_zasset->GetComponentByTypeName("TypeScriptComponent"));
        CheckTrue("F2", "两份都拿到了组件", c_prefab != nullptr && c_zasset != nullptr);
        if (c_prefab != nullptr && c_zasset != nullptr)
        {
            CheckEq("F3", "两种格式还原出的 guid 一致",
                    std::string(c_prefab->GetScriptGuid().c_str()),
                    std::string(c_zasset->GetScriptGuid().c_str()));
            CheckEq("F4", "两种格式还原出的 override 一致",
                    std::string(c_prefab->GetSerializedField(eastl::string("speed")).c_str()),
                    std::string(c_zasset->GetSerializedField(eastl::string("speed")).c_str()));
            CheckEq("F5", "两种格式下活实例的 speed 一致",
                    LiveValue(c_prefab, "speed"), LiveValue(c_zasset, "speed"));
        }
    }

    std::printf("\n---------------------------------------------\n");
    std::printf("checks=%d failed=%d\n", g_Checks, g_Failed);
    if (g_Failed > 0)
    {
        std::printf("RESULT: FAILED\n");
        return 1;
    }
    std::printf("RESULT: ALL PASSED\n");
    return 0;
}
