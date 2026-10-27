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
#include "Runtime/Core/Serialize/TransferUtility.h"
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

    // ---- G. 空白 prefab（Content Browser 新建资产那条路径的产出）-----------
    // ContentBrowserAssetActions::CreateNewAsset("Prefab") 产出的就是"一个只带
    // Transform 的空 GameObject"的 .prefab。这里用**同一套配方**（Produce +
    // AddComponentByTypeName("Transform") + SyncSerializedComponents +
    // SaveAsPrefabAsset）验证这种最小 prefab 存得回来：LoadPrefabAsset 见到
    // m_RootGameObject 为空的 PrefabAsset 头会直接返回 nullptr，所以只写头的
    // .prefab 是个打不开的坏资产 —— 这组就是为了挡住那个回归。
    std::printf("\n--- G. 空白 prefab（Content Browser 新建资产的产出）---\n");
    GameObject* blank = MakeGameObject(object_manager, "BlankPrefab");
    CheckTrue("G1", "造出了空 GameObject", blank != nullptr);
    if (blank != nullptr)
    {
        CheckTrue("G2", "挂上了 Transform（Prefab 的根必须有）",
                  blank->AddComponentByTypeName("Transform") != nullptr);
        blank->SyncSerializedComponents();

        const std::filesystem::path blank_path = scratch / "Blank.prefab";
        CheckTrue("G3", "空白 prefab 写盘成功", PrefabUtility::SaveAsPrefabAsset(blank, blank_path));
        CheckTrue("G4", ".prefab 真的落在磁盘上", std::filesystem::exists(blank_path, ec));

        GameObject* revived = PrefabUtility::InstantiateFromPath(blank_path, /*invoke_post_load=*/true);
        CheckTrue("G5", "空白 prefab 能反序列化回来（不是个打不开的坏资产）", revived != nullptr);
        if (revived != nullptr)
        {
            CheckEq("G6", "名字穿过了磁盘", std::string(revived->GetName().c_str()), "BlankPrefab");
            CheckTrue("G7", "根上的 Transform 也跟着回来了",
                      revived->GetComponentByTypeName("Transform") != nullptr);
            CheckTrue("G8", "回来的是新对象，不是模板本身", revived != blank);
        }
    }

    // ---- H. 多层层级 ------------------------------------------------------
    // 8.5 节"刻意没做"的第一条。driver 解析不出子 Transform 的 owner 时会
    // LOG_WARNING 并**跳过整棵子树**（PrefabUtility.cpp:209-212），那一层往下
    // 的组件就永远拿不到 PostLoadResource —— 不 Awake、TS 组件不绑。
    // 类注释（:55-60）说 legacy 二进制没有 flat GameObject list，所以多层一直
    // 受"Phase-1 限制"；但 Instantiate 已经用 clone.clones 拼出完整列表传给
    // driver 了（:329-348），那条注释很可能是陈的。这一组就是来判这个的。
    //
    // 判据不只看"绑没绑"，还看 Awake 时记下的 hostName：driver 是把 owner 当作
    // PostLoadResource(go) 的参数传下去的，hostName 等于**本层**的名字才能证明
    // 传下去的是对的那个 GameObject，而不是随便抓到一个。
    std::printf("\n--- H. 多层层级（Root → Child → GrandChild，每层一个组件）---\n");

    const char* kLevelNames[3] = {"DeepRoot", "DeepChild", "DeepGrandChild"};
    GameObject*          src_go[3]   = {nullptr, nullptr, nullptr};
    TypeScriptComponent* src_comp[3] = {nullptr, nullptr, nullptr};

    for (int i = 0; i < 3; ++i)
    {
        src_go[i] = MakeGameObject(object_manager, kLevelNames[i]);
        if (src_go[i] == nullptr)
        {
            break;
        }
        src_go[i]->AddComponentByTypeName("Transform");
        src_comp[i] = Cast<TypeScriptComponent>(src_go[i]->AddComponentByTypeName("TypeScriptComponent"));
        if (src_comp[i] != nullptr)
        {
            src_comp[i]->SetScriptGuid(hosted->m_Guid);
        }
        if (i > 0)
        {
            // worldPositionStays=false：保留子对象自己写的 local pose。
            src_go[i]->GetTransform()->SetParent(src_go[i - 1]->GetTransform(), false);
        }
    }

    CheckTrue("H1", "三层 GameObject 都造出来了",
              src_go[0] != nullptr && src_go[1] != nullptr && src_go[2] != nullptr);
    CheckTrue("H2", "三层都挂上了 TypeScriptComponent",
              src_comp[0] != nullptr && src_comp[1] != nullptr && src_comp[2] != nullptr);
    CheckEq("H3", "层级挂对了（Root / Child 各有 1 个孩子）",
            std::to_string(src_go[0]->GetTransform()->GetChildCount()) + "/" +
                std::to_string(src_go[1]->GetTransform()->GetChildCount()),
            "1/1");

    // 写盘前每层都要同步：writer 读的是序列化容器，不是活列表。
    for (int i = 0; i < 3; ++i)
    {
        if (src_go[i] != nullptr)
        {
            src_go[i]->SyncSerializedComponents();
        }
    }

    const std::filesystem::path deep_prefab = scratch / "Deep.prefab";
    const std::filesystem::path deep_zasset = scratch / "Deep.zasset";
    CheckTrue("H4", "三层 prefab 写成了 .prefab（YAML）",
              PrefabUtility::SaveAsPrefabAsset(src_go[0], deep_prefab));
    CheckTrue("H5", "三层 prefab 写成了 .zasset（二进制 SerializedFile）",
              PrefabUtility::SaveAsPrefabAsset(src_go[0], deep_zasset));

    auto DeepVerify = [&](const char* tag, const std::filesystem::path& path)
    {
        GameObject* root = PrefabUtility::InstantiateFromPath(path, /*invoke_post_load=*/true);
        CheckTrue(Id(tag, 1).c_str(), "根克隆出来了", root != nullptr);
        if (root == nullptr)
        {
            return;
        }

        GameObject* go = root;
        for (int depth = 0; depth < 3; ++depth)
        {
            if (depth > 0 && go != nullptr)
            {
                Transform* parent_tc = go->GetTransform();
                go = (parent_tc != nullptr && parent_tc->GetChildCount() > 0)
                         ? parent_tc->GetChild(0)->GetParentObject()
                         : nullptr;
            }

            const std::string level = std::string("第") + std::to_string(depth) + "层 " + kLevelNames[depth];
            CheckTrue(Id(tag, depth * 5 + 2).c_str(),
                      (level + " 沿 Transform.m_Children 走到了").c_str(), go != nullptr);
            if (go == nullptr)
            {
                return;
            }
            CheckEq(Id(tag, depth * 5 + 3).c_str(), (level + " 名字对").c_str(),
                    std::string(go->GetName().c_str()), kLevelNames[depth]);

            auto* comp = Cast<TypeScriptComponent>(go->GetComponentByTypeName("TypeScriptComponent"));
            CheckTrue(Id(tag, depth * 5 + 4).c_str(), (level + " 拿到了组件").c_str(), comp != nullptr);
            if (comp == nullptr)
            {
                return;
            }
            CheckTrue(Id(tag, depth * 5 + 5).c_str(),
                      (level + " 的组件也绑上了（PostLoadResource 发到了这一层）").c_str(),
                      comp->HasLiveInstance());
            if (comp->HasLiveInstance())
            {
                CheckEq(Id(tag, depth * 5 + 6).c_str(),
                        (level + " Awake 时宿主就是本层的 GameObject").c_str(),
                        LiveValue(comp, "hostName"), kLevelNames[depth]);
            }
        }
    };

    DeepVerify("H-prefab", deep_prefab);
    DeepVerify("H-zasset", deep_zasset);

    // ---- I. Prefab Variant（模型 A：真继承）--------------------------------
    // Variant **不拥有对象树**，只有 base 指针 + 自己这一层的 overrides。实例化
    // 它 = 最深 base 的对象树 + 逐层 overrides。这一组钉三件事：
    //   1. 链能解开，克隆出来的是 base 的那棵树；
    //   2. variant 的 override 真的落到了克隆体上（不是 base 的值）；
    //   3. 反过来直接实例化 base，override 不该出现（对照组，防"改了 base"）。
    //
    // override 打在 Transform 的 local position x 上：字段名是 m_LocalPositionD
    // （Transform 走手写 Transfer，m_LocalPosition 是 Vector3d，所以是 double）。
    //
    // value_bytes 是裸字节（host-endian），和 OverwriteScalarBytes 期待的布局一致。
    // 注意 42.0 的小端字节以 6 个 0x00 开头 —— 这正是以前用 eastl::string 当字节袋
    // 时被 YAML 截成空串的形状，K 组专测这个往返。
    auto DoubleBytes = [](double v) {
        std::vector<uint8_t> out(sizeof(double));
        std::memcpy(out.data(), &v, sizeof(double));
        return out;
    };

    std::printf("\n--- I. Prefab Variant（base 的对象树 + variant 的 overrides）---\n");

    GameObject* base_go = MakeGameObject(object_manager, "VariantBase");
    CheckTrue("I1", "造出了 base GameObject", base_go != nullptr);
    if (base_go == nullptr)
    {
        std::printf("  (skip: Produce(GameObject) 失败)\n");
    }
    else
    {
        CheckTrue("I2", "base 挂上了 Transform",
                  base_go->AddComponentByTypeName("Transform") != nullptr);
        base_go->SyncSerializedComponents();

        const std::filesystem::path base_path = scratch / "VBase.prefab";
        CheckTrue("I3", "base prefab 写盘成功", PrefabUtility::SaveAsPrefabAsset(base_go, base_path));

        std::vector<GameObject*> base_gos;
        PrefabAsset* base_asset = PrefabUtility::LoadPrefabAsset(base_path, &base_gos);
        CheckTrue("I4", "base prefab 读回来了", base_asset != nullptr);

        GameObject* base_root = (base_asset != nullptr) ? base_asset->GetRootGameObject() : nullptr;
        CheckTrue("I5", "base 有根 GameObject", base_root != nullptr);
        Transform* src_tf = (base_root != nullptr) ? base_root->GetTransform() : nullptr;
        CheckTrue("I6", "拿得到 base 的 Transform（override 的 target 就是它）", src_tf != nullptr);

        PrefabAsset* variant = nullptr;
        if (base_asset != nullptr && src_tf != nullptr)
        {
            Object* raw_variant = object_manager->Produce(TypeOf<PrefabAsset>(), 0);
            if (raw_variant != nullptr)
            {
                object_manager->AllocateAndAssignInstanceID(raw_variant);
                variant = static_cast<PrefabAsset*>(raw_variant);
                variant->SetVariantBase(base_asset);
            }
        }
        CheckTrue("I7", "造出了 variant 资产", variant != nullptr);

        if (variant != nullptr && src_tf != nullptr)
        {
            CheckTrue("I8", "variant 认自己是 variant（base 指针非空）", variant->IsVariant());

            PropertyModification mod;
            mod.target = PPtr<Object>(src_tf);
            mod.property_path = eastl::string("m_LocalPositionD.x");
            const double kOverrideX = 42.0;
            mod.value_bytes = DoubleBytes(kOverrideX);
            variant->AddVariantOverride(mod);
            CheckEq("I9", "variant 记下了 1 条 override",
                    std::to_string(variant->GetVariantOverrides().size()), "1");

            // 实例化 variant：应该拿到 base 的树，且 x 被 override 成 42
            GameObject* v_clone = PrefabUtility::Instantiate(variant, /*invoke_post_load=*/true);
            CheckTrue("I10", "variant 实例化出了根", v_clone != nullptr);
            if (v_clone != nullptr)
            {
                CheckEq("I11", "克隆出来的是 base 的对象树",
                        std::string(v_clone->GetName().c_str()), "VariantBase");
                CheckTrue("I12", "克隆是新对象，不是 base 的模板本身", v_clone != base_root);
                Transform* v_tf = v_clone->GetTransform();
                CheckTrue("I13", "克隆上有 Transform", v_tf != nullptr);
                if (v_tf != nullptr)
                {
                    CheckEq("I14", "variant 的 override 生效了（x=42）",
                            std::to_string(v_tf->GetLocalPositionD().x), std::to_string(42.0));
                }
            }

            // 对照组：直接实例化 base，override 不应该出现
            GameObject* b_clone = PrefabUtility::Instantiate(base_asset, /*invoke_post_load=*/true);
            CheckTrue("I15", "base 也实例化出了根", b_clone != nullptr);
            if (b_clone != nullptr)
            {
                Transform* b_tf = b_clone->GetTransform();
                CheckTrue("I16", "base 克隆上有 Transform", b_tf != nullptr);
                if (b_tf != nullptr)
                {
                    CheckEq("I17", "直接实例化 base 时 x 仍是 0（证明 42 确实来自 variant 那层）",
                            std::to_string(b_tf->GetLocalPositionD().x), std::to_string(0.0));
                }
            }
        }
    }

    // ---- J. 多层 Variant 链：每层 override 都生效，近的赢 ----------------------
    // I 组只证明了"单层 variant 能解开"。模型 A 真正卖的是**任意长度链**：链上
    // 每一环的 override.target 都指向最深 base 图里的同一个对象，靠
    // CloneResult::ptr_map 翻译，所以 base <- mid <- leaf 这种三层链也该套到
    // 同一份克隆树上。这一组钉三件事：
    //   1. 三层链能解开（不是只解一层就停）；
    //   2. 不同层改不同字段时，各层的值都在；
    //   3. 同一字段被两层都改时，近的那层赢。
    std::printf("\n--- J. 多层 Variant 链（近的覆盖远的）---\n");

    auto PosStr = [](Transform* tf) -> std::string
    {
        if (tf == nullptr)
        {
            return std::string("<null>");
        }
        const auto p = tf->GetLocalPositionD();
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%.1f/%.1f/%.1f", p.x, p.y, p.z);
        return std::string(buf);
    };

    GameObject* j_base_go = MakeGameObject(object_manager, "JBase");
    CheckTrue("J1", "造出了 J 组的 base GameObject", j_base_go != nullptr);
    if (j_base_go != nullptr)
    {
        CheckTrue("J2", "J base 挂上了 Transform",
                  j_base_go->AddComponentByTypeName("Transform") != nullptr);
        j_base_go->SyncSerializedComponents();

        const std::filesystem::path j_base_path = scratch / "JBase.prefab";
        CheckTrue("J3", "J base prefab 写盘成功",
                  PrefabUtility::SaveAsPrefabAsset(j_base_go, j_base_path));

        std::vector<GameObject*> j_base_gos;
        PrefabAsset* j_base = PrefabUtility::LoadPrefabAsset(j_base_path, &j_base_gos);
        CheckTrue("J4", "J base prefab 读回来了", j_base != nullptr);

        GameObject* j_base_root = (j_base != nullptr) ? j_base->GetRootGameObject() : nullptr;
        Transform* j_src_tf = (j_base_root != nullptr) ? j_base_root->GetTransform() : nullptr;
        CheckTrue("J5", "拿得到 J base 的 Transform（链上每层的 target 都指向它）",
                  j_src_tf != nullptr);

        if (j_base != nullptr && j_src_tf != nullptr)
        {
            // 造一层 variant：base 指针 + 若干 override（都打在同一个 base 对象上）
            auto MakeVariant = [&](PrefabAsset* base,
                                   std::initializer_list<std::pair<const char*, double>> overrides) -> PrefabAsset*
            {
                Object* raw = object_manager->Produce(TypeOf<PrefabAsset>(), 0);
                if (raw == nullptr)
                {
                    return nullptr;
                }
                object_manager->AllocateAndAssignInstanceID(raw);
                PrefabAsset* v = static_cast<PrefabAsset*>(raw);
                v->SetVariantBase(base);
                for (const auto& kv : overrides)
                {
                    PropertyModification mod;
                    mod.target = PPtr<Object>(j_src_tf);
                    mod.property_path = eastl::string(kv.first);
                    const double d = kv.second;
                    mod.value_bytes = DoubleBytes(d);
                    v->AddVariantOverride(mod);
                }
                return v;
            };

            // base <- mid(x=10, z=3) <- leaf(x=99, y=7)
            PrefabAsset* j_mid = MakeVariant(j_base, {{"m_LocalPositionD.x", 10.0},
                                                      {"m_LocalPositionD.z", 3.0}});
            PrefabAsset* j_leaf = MakeVariant(j_mid, {{"m_LocalPositionD.x", 99.0},
                                                      {"m_LocalPositionD.y", 7.0}});
            CheckTrue("J6", "造出了三层链（base <- mid <- leaf）",
                      j_mid != nullptr && j_leaf != nullptr);

            if (j_mid != nullptr && j_leaf != nullptr)
            {
                CheckEq("J7", "leaf 认 base 是三层深（IsVariant 一路为真）",
                        std::to_string(j_leaf->IsVariant() && j_mid->IsVariant() && !j_base->IsVariant()),
                        std::to_string(true));

                // leaf：x 两层都改了，取近的 99；y 只有 leaf 改了；z 只有 mid 改了
                // 且 leaf 没动它 —— 三个值分别来自链上的不同层。
                GameObject* j_leaf_clone = PrefabUtility::Instantiate(j_leaf, /*invoke_post_load=*/true);
                CheckTrue("J8", "leaf 实例化出了根", j_leaf_clone != nullptr);
                if (j_leaf_clone != nullptr)
                {
                    CheckEq("J9", "leaf 克隆的仍是 base 的那棵树",
                            std::string(j_leaf_clone->GetName().c_str()), "JBase");
                    CheckEq("J10", "三层链：x 取近的 leaf(99)、y 取 leaf(7)、z 取远的 mid(3)",
                            PosStr(j_leaf_clone->GetTransform()), "99.0/7.0/3.0");
                }

                // mid：只有两层，x=10 / z=3，y 没人动过
                GameObject* j_mid_clone = PrefabUtility::Instantiate(j_mid, /*invoke_post_load=*/true);
                CheckTrue("J11", "mid 也实例化出了根", j_mid_clone != nullptr);
                if (j_mid_clone != nullptr)
                {
                    CheckEq("J12", "两层链：x=10 / y=0（没人改） / z=3",
                            PosStr(j_mid_clone->GetTransform()), "10.0/0.0/3.0");
                }

                // 对照：base 自己没有被链上任何一层污染
                GameObject* j_base_clone = PrefabUtility::Instantiate(j_base, /*invoke_post_load=*/true);
                CheckTrue("J13", "base 也实例化出了根", j_base_clone != nullptr);
                if (j_base_clone != nullptr)
                {
                    CheckEq("J14", "base 仍是 0/0/0（证明这些值确实来自 variant 各层）",
                            PosStr(j_base_clone->GetTransform()), "0.0/0.0/0.0");
                }
            }
        }
    }

    // ---- K. Variant 落盘再读回（Create / Save / Load 往返）----------------------
    // I / J 两组的 variant 都是内存里现造的。真实用法是把它存成一个 .prefab 文件，
    // 而 Variant 文件的形状很特殊：**里面只有一个 PrefabAsset 头，没有 root** ——
    // 这恰恰是 LoadPrefabAsset 以前会直接判失败的形状。这一组钉四件事：
    //   1. CreateVariantAsset + SaveVariantAsset 写得出来；
    //   2. LoadPrefabAsset 肯读回一个"没有 root 但 IsVariant()"的资产；
    //   3. 读回来的 base 指针指向的还是同一个 base 资产（跨文件 PPtr 通）；
    //   4. 实例化读回来的 variant，override 照样生效。
    std::printf("\n--- K. Variant 落盘再读回（无 root 的单对象 prefab）---\n");

    GameObject* k_base_go = MakeGameObject(object_manager, "KBase");
    CheckTrue("K1", "造出了 K 组的 base GameObject", k_base_go != nullptr);
    if (k_base_go != nullptr)
    {
        CheckTrue("K2", "K base 挂上了 Transform",
                  k_base_go->AddComponentByTypeName("Transform") != nullptr);
        k_base_go->SyncSerializedComponents();

        const std::filesystem::path k_base_path = scratch / "KBase.prefab";
        CheckTrue("K3", "K base prefab 写盘成功",
                  PrefabUtility::SaveAsPrefabAsset(k_base_go, k_base_path));

        std::vector<GameObject*> k_base_gos;
        PrefabAsset* k_base = PrefabUtility::LoadPrefabAsset(k_base_path, &k_base_gos);
        CheckTrue("K4", "K base prefab 读回来了", k_base != nullptr);

        GameObject* k_base_root = (k_base != nullptr) ? k_base->GetRootGameObject() : nullptr;
        Transform* k_src_tf = (k_base_root != nullptr) ? k_base_root->GetTransform() : nullptr;
        CheckTrue("K5", "拿得到 K base 的 Transform", k_src_tf != nullptr);

        if (k_base != nullptr && k_src_tf != nullptr)
        {
            // 两轨各跑一遍：.prefab（YAML 对象图）和 .zasset（二进制 SerializedFile）
            const char* k_exts[2] = { ".prefab", ".zasset" };
            for (int t = 0; t < 2; ++t)
            {
                const std::string k_tag = std::string("K") + (t == 0 ? "-prefab" : "-zasset");
                const std::filesystem::path var_path =
                    scratch / (std::string("KVar") + k_exts[t]);

                PrefabAsset* k_var = PrefabUtility::CreateVariantAsset(k_base, eastl::string("KVar"));
                CheckTrue(Id(k_tag.c_str(), 1).c_str(), "CreateVariantAsset 造出了 variant",
                          k_var != nullptr);
                if (k_var == nullptr)
                {
                    continue;
                }

                CheckTrue(Id(k_tag.c_str(), 2).c_str(),
                          "新 variant 没有 root（Variant 本来就不拥有对象树）",
                          k_var->GetRootGameObject() == nullptr);

                PropertyModification k_mod;
                k_mod.target = PPtr<Object>(k_src_tf);
                k_mod.property_path = eastl::string("m_LocalPositionD.x");
                const double k_x = 42.0;
                k_mod.value_bytes = DoubleBytes(k_x);
                k_var->AddVariantOverride(k_mod);

                CheckTrue(Id(k_tag.c_str(), 3).c_str(), "SaveVariantAsset 写盘成功",
                          PrefabUtility::SaveVariantAsset(k_var, var_path));
                CheckTrue(Id(k_tag.c_str(), 4).c_str(), "文件确实落盘了",
                          std::filesystem::exists(var_path));

                // 读回 —— 这个文件只有一个 PrefabAsset 头、没有 root
                std::vector<GameObject*> k_var_gos;
                PrefabAsset* k_loaded = PrefabUtility::LoadPrefabAsset(var_path, &k_var_gos);
                CheckTrue(Id(k_tag.c_str(), 5).c_str(), "无 root 的 variant 文件能读回来",
                          k_loaded != nullptr);
                if (k_loaded != nullptr)
                {
                    CheckTrue(Id(k_tag.c_str(), 6).c_str(), "读回来的确实认自己是 Variant",
                              k_loaded->IsVariant());
                    CheckTrue(Id(k_tag.c_str(), 7).c_str(),
                              "base 指针指回了同一个 base 资产（跨文件 PPtr 通）",
                              k_loaded->GetVariantBase() == k_base);
                    CheckEq(Id(k_tag.c_str(), 8).c_str(), "override 跟着穿过了磁盘",
                            std::to_string(k_loaded->GetVariantOverrides().size()), "1");

                    GameObject* k_clone = PrefabUtility::Instantiate(k_loaded, /*invoke_post_load=*/true);
                    CheckTrue(Id(k_tag.c_str(), 9).c_str(), "读回来的 variant 能实例化",
                              k_clone != nullptr);
                    if (k_clone != nullptr)
                    {
                        CheckEq(Id(k_tag.c_str(), 10).c_str(), "克隆的是 base 的那棵树",
                                std::string(k_clone->GetName().c_str()), "KBase");
                        CheckEq(Id(k_tag.c_str(), 11).c_str(),
                                "落盘再读回后 override 照样生效（x=42）",
                                std::to_string(k_clone->GetTransform()->GetLocalPositionD().x),
                                std::to_string(42.0));
                    }
                }

                // Produce() 配 ObjectManager::Destroy —— 见 SaveAsPrefabAsset 里那段
                // 关于 mimalloc / mi_usable_size 的说明。
                object_manager->Destroy(k_var);
            }
        }
    }

    // ---- L. 字节层拼接：override 只进字节，不碰共享的源对象 --------------------
    // Editor 的 Revert / MergeFromSource 要算"一个 Variant 实例该回退到什么值" =
    // base 模板的字节 + variant 自己的 override。base 模板是**所有该 base 的实例
    // 共享**的对象，直接往它身上 splice 会污染每一个实例，所以必须只改字节。
    // 这一组钉三点：
    //   1. 字节里 override 生效了；
    //   2. **源对象本身没被改**（最关键的一条）；
    //   3. 路径不存在 / 值尺寸不匹配时是失败，不是静默写坏字节。
    std::printf("\n--- L. 字节层拼接（override 只进字节，不碰共享的源对象）---\n");

    GameObject* l_go = MakeGameObject(object_manager, "LSrc");
    CheckTrue("L1", "造出了 L 组的 GameObject", l_go != nullptr);
    if (l_go != nullptr)
    {
        CheckTrue("L2", "挂上了 Transform",
                  l_go->AddComponentByTypeName("Transform") != nullptr);
        l_go->SyncSerializedComponents();

        Transform* l_tf = l_go->GetTransform();
        CheckTrue("L3", "拿得到 Transform", l_tf != nullptr);

        if (l_tf != nullptr)
        {
            CheckEq("L4", "起始 x 是 0",
                    std::to_string(l_tf->GetLocalPositionD().x), std::to_string(0.0));

            PropertyModification l_mod;
            l_mod.target = PPtr<Object>(l_tf);
            l_mod.property_path = eastl::string("m_LocalPositionD.x");
            l_mod.value_bytes = DoubleBytes(42.0);

            prefab::SerialisedObject l_tree;
            std::vector<uint8_t> l_bytes;
            CheckTrue("L5", "SnapshotWithOverrides 成功",
                      PrefabUtility::SnapshotWithOverrides(*l_tf, {l_mod}, l_tree, l_bytes));

            // 最关键的一条：源对象没被污染
            CheckEq("L6", "源对象本身没被改（共享模板安全）",
                    std::to_string(l_tf->GetLocalPositionD().x), std::to_string(0.0));
            CheckTrue("L7", "快照字节非空", !l_bytes.empty());

            // 字节里确实带着 override —— 读回同一个对象验证
            const bool read_back = TransferUtility::ReadObjectFromVector(*l_tf, l_bytes);
            CheckTrue("L8", "字节里 override 生效了（读回后 x=42）",
                      read_back && l_tf->GetLocalPositionD().x == 42.0);

            // 负向：路径不存在 / 尺寸不匹配，都应该是失败而不是乱写
            PropertyModification l_bad = l_mod;
            l_bad.property_path = eastl::string("m_DoesNotExist.x");
            CheckTrue("L9", "路径不存在时拼接失败（不静默写坏字节）",
                      !PrefabUtility::SpliceModificationIntoBytes(l_bytes, l_tree, l_bad));

            PropertyModification l_short = l_mod;
            l_short.value_bytes = std::vector<uint8_t>(4, 0);  // double 字段只给 4 字节
            CheckTrue("L10", "值尺寸不匹配时拼接失败",
                      !PrefabUtility::SpliceModificationIntoBytes(l_bytes, l_tree, l_short));
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
