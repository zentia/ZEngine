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
#include "Runtime/Resource/Prefab/PropertyPathResolver.h"
#include "Runtime/Resource/Prefab/PropertyValueLocator.h"
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

    // ---- M. Variant 的结构性覆盖（加 / 删组件）--------------------------------
    // m_variant_overrides 只能改 base 上**已有对象**的值。加一个组件或删一个组件是
    // 结构性变更，得有另一份存储：
    //   * removed —— 只存 base 里那个 Component 的 PPtr（对象还在 base 手里）；
    //   * added   —— 要把组件对象本身存进 variant 文件（Variant 本来不拥有对象树），
    //                外加它挂在哪一个 base GameObject 上。
    // 这一组钉四件事：
    //   1. 两者都能穿过磁盘 —— added 的对象要作为独立对象写进 variant 文件再读回，
    //      这是头一回有 Variant 文件里带着对象；
    //   2. 实例化时删除生效、新增生效（先删后加）；
    //   3. **variant 文件里的 added 组件是模板**，交出去会和交出 base 模板一样让所有
    //      实例共享同一个组件对象 —— 必须克隆；
    //   4. base 模板本身没被改动（删的是克隆上的那个，不是 base 上的那个）。
    std::printf("\n--- M. Variant 的结构性覆盖（加 / 删组件）---\n");

    const eastl::string kBaseGuid("M_BASE");
    const eastl::string kAddedGuid("M_ADDED");

    auto CountScriptComponents = [](GameObject* go) -> int
    {
        if (go == nullptr)
        {
            return -1;
        }
        int n = 0;
        for (ImmediatePtr<Component>& slot : go->EditorGetComponentsRef())
        {
            if (Cast<TypeScriptComponent>(static_cast<Component*>(slot)) != nullptr)
            {
                ++n;
            }
        }
        return n;
    };

    auto OnlyScriptComponent = [](GameObject* go) -> TypeScriptComponent*
    {
        if (go == nullptr)
        {
            return nullptr;
        }
        for (ImmediatePtr<Component>& slot : go->EditorGetComponentsRef())
        {
            TypeScriptComponent* tsc = Cast<TypeScriptComponent>(static_cast<Component*>(slot));
            if (tsc != nullptr)
            {
                return tsc;
            }
        }
        return nullptr;
    };

    GameObject* m_base_go = MakeGameObject(object_manager, "MBase");
    CheckTrue("M1", "造出了 M 组的 base GameObject", m_base_go != nullptr);
    if (m_base_go != nullptr)
    {
        CheckTrue("M2", "挂上了 Transform",
                  m_base_go->AddComponentByTypeName("Transform") != nullptr);

        Component* m_raw_base_tsc = m_base_go->AddComponentByTypeName("TypeScriptComponent");
        CheckTrue("M3", "挂上了 TypeScriptComponent（这就是 variant 要删掉的那个）",
                  m_raw_base_tsc != nullptr);
        TypeScriptComponent* m_base_tsc = Cast<TypeScriptComponent>(m_raw_base_tsc);
        if (m_base_tsc != nullptr)
        {
            m_base_tsc->SetScriptGuid(kBaseGuid);
        }
        m_base_go->SyncSerializedComponents();

        const std::filesystem::path m_base_path = scratch / "MBase.prefab";
        CheckTrue("M4", "M base prefab 写盘成功",
                  PrefabUtility::SaveAsPrefabAsset(m_base_go, m_base_path));

        std::vector<GameObject*> m_base_gos;
        PrefabAsset* m_base = PrefabUtility::LoadPrefabAsset(m_base_path, &m_base_gos);
        CheckTrue("M5", "M base prefab 读回来了", m_base != nullptr);

        GameObject* m_base_root = (m_base != nullptr) ? m_base->GetRootGameObject() : nullptr;
        CheckTrue("M6", "拿得到 base 的根", m_base_root != nullptr);
        CheckEq("M7", "base 根上有 1 个 TypeScriptComponent",
                std::to_string(CountScriptComponents(m_base_root)), "1");

        if (m_base != nullptr && m_base_root != nullptr)
        {
            TypeScriptComponent* m_src_tsc = OnlyScriptComponent(m_base_root);
            CheckTrue("M8", "拿得到 base 上那个 TSC（variant 的 removed 记的就是它）",
                      m_src_tsc != nullptr);

            const char* m_exts[2] = { ".prefab", ".zasset" };
            for (int t = 0; t < 2 && m_src_tsc != nullptr; ++t)
            {
                const std::string m_tag = std::string("M") + (t == 0 ? "-prefab" : "-zasset");
                const std::filesystem::path var_path = scratch / (std::string("MVar") + m_exts[t]);

                // variant 要"加"的那个组件。它是个独立对象 —— variant 文件会把自己
                // 加的组件当作自己的对象带在身上（fileID 2..N）。
                Object* m_raw_added = object_manager->Produce(TypeOf<TypeScriptComponent>(), 0);
                TypeScriptComponent* m_added = (m_raw_added != nullptr)
                    ? Cast<TypeScriptComponent>(m_raw_added)
                    : nullptr;
                CheckTrue(Id(m_tag.c_str(), 1).c_str(), "造出了要新增的组件", m_added != nullptr);
                if (m_added == nullptr)
                {
                    continue;
                }
                object_manager->AllocateAndAssignInstanceID(m_added);
                m_added->SetScriptGuid(kAddedGuid);

                PrefabAsset* m_var = PrefabUtility::CreateVariantAsset(m_base, eastl::string("MVar"));
                CheckTrue(Id(m_tag.c_str(), 2).c_str(), "CreateVariantAsset 造出了 variant",
                          m_var != nullptr);
                if (m_var == nullptr)
                {
                    continue;
                }

                m_var->GetVariantRemovedComponents().push_back(PPtr<Component>(m_src_tsc));
                VariantComponentAddition m_addition;
                m_addition.host      = PPtr<GameObject>(m_base_root);
                m_addition.component = m_added;
                m_var->GetVariantAddedComponents().push_back(m_addition);
                CheckEq(Id(m_tag.c_str(), 3).c_str(), "variant 记下了 1 删 1 加",
                        std::to_string(m_var->GetVariantRemovedComponents().size()) + "/" +
                        std::to_string(m_var->GetVariantAddedComponents().size()), "1/1");

                CheckTrue(Id(m_tag.c_str(), 4).c_str(), "SaveVariantAsset 写盘成功",
                          PrefabUtility::SaveVariantAsset(m_var, var_path));

                std::vector<GameObject*> m_gos;
                PrefabAsset* m_loaded = PrefabUtility::LoadPrefabAsset(var_path, &m_gos);
                CheckTrue(Id(m_tag.c_str(), 5).c_str(), "variant 文件读回来了", m_loaded != nullptr);
                if (m_loaded == nullptr)
                {
                    object_manager->Destroy(m_var);
                    continue;
                }

                CheckEq(Id(m_tag.c_str(), 6).c_str(), "删/加两条都穿过了磁盘",
                        std::to_string(m_loaded->GetVariantRemovedComponents().size()) + "/" +
                        std::to_string(m_loaded->GetVariantAddedComponents().size()), "1/1");

                // 头一回有 Variant 文件里带着对象 —— added 组件必须能从文件里解出来
                Component* m_loaded_added = m_loaded->GetVariantAddedComponents().empty()
                    ? nullptr
                    : static_cast<Component*>(m_loaded->GetVariantAddedComponents()[0].component);
                CheckTrue(Id(m_tag.c_str(), 7).c_str(),
                          "读回来的 added 组件能解引用（variant 文件确实带着这个对象）",
                          m_loaded_added != nullptr);

                GameObject* m_clone = PrefabUtility::Instantiate(m_loaded, /*invoke_post_load=*/true);
                CheckTrue(Id(m_tag.c_str(), 8).c_str(), "读回来的 variant 能实例化", m_clone != nullptr);
                if (m_clone != nullptr)
                {
                    CheckEq(Id(m_tag.c_str(), 9).c_str(),
                            "克隆上只剩 1 个 TSC（base 那个被删、variant 这个被加）",
                            std::to_string(CountScriptComponents(m_clone)), "1");

                    TypeScriptComponent* m_clone_tsc = OnlyScriptComponent(m_clone);
                    CheckTrue(Id(m_tag.c_str(), 10).c_str(),
                              "留下的是 variant 加的那个（M_ADDED），base 的（M_BASE）被删了",
                              m_clone_tsc != nullptr && m_clone_tsc->GetScriptGuid() == kAddedGuid);
                    // 比的是**文件里读回来**的那个对象（模板），不是测试里 Produce 的那
                    // 个 —— 后者跟文件里的根本不是同一个，比它等于什么都没钉。
                    CheckTrue(Id(m_tag.c_str(), 11).c_str(),
                              "它是新增组件的克隆，不是 variant 文件里那个模板对象本身",
                              m_clone_tsc != nullptr &&
                              static_cast<Component*>(m_clone_tsc) != m_loaded_added);

                    // 最直接的一条：variant 文件的图是缓存共享的，把它的 added 组件
                    // 直接交出去，两次实例化就会拿到同一个组件对象（正是 P14 那个
                    // "两次拖同一个 prefab 共享一个 Transform"的回归）。
                    GameObject* m_clone2 = PrefabUtility::Instantiate(m_loaded, /*invoke_post_load=*/true);
                    TypeScriptComponent* m_clone2_tsc = OnlyScriptComponent(m_clone2);
                    CheckTrue(Id(m_tag.c_str(), 12).c_str(),
                              "两次实例化拿到的是两个独立的组件对象（模板没被交出去）",
                              m_clone2_tsc != nullptr && m_clone_tsc != nullptr &&
                              m_clone2_tsc != m_clone_tsc);
                }

                object_manager->Destroy(m_var);
            }

            // 对照：直接实例化 base —— 它的 TSC 还在，前面的删/加没碰到 base 模板
            GameObject* m_base_clone = PrefabUtility::Instantiate(m_base, /*invoke_post_load=*/true);
            CheckTrue("M9", "base 也能实例化", m_base_clone != nullptr);
            if (m_base_clone != nullptr)
            {
                TypeScriptComponent* m_bc = OnlyScriptComponent(m_base_clone);
                CheckTrue("M10", "base 克隆上那个 TSC 仍是 M_BASE（base 模板没被 variant 的删除污染）",
                          m_bc != nullptr && m_bc->GetScriptGuid() == kBaseGuid);
            }
        }
    }

    // ---- N. 链级结构：后一层能删前一层新增的组件；override 能指向新增的组件 ------
    // M 组只测了单层。真实的 Variant 链是 base <- mid <- leaf，结构性变更要按"整条链
    // 的净结果"算，而不是每层各算各的：
    //   1. mid 新增的组件，leaf 可以再把它删掉（净结果里没有它）；
    //   2. mid 新增的组件上，leaf 可以再叠一条属性 override（目标是 mid 文件里的
    //      模板对象，不是 base 里的对象 —— 运行时的 ptr_map 必须认得它）；
    //   3. 这两件事在 .prefab / .zasset 两轨上结果一致。
    std::printf("\n--- N. 链级结构（后层删前层新增 / override 指向新增组件）---\n");

    GameObject* n_base_go = MakeGameObject(object_manager, "NBase");
    CheckTrue("N1", "造出了 N 组的 base GameObject", n_base_go != nullptr);
    if (n_base_go != nullptr)
    {
        n_base_go->AddComponentByTypeName("Transform");
        n_base_go->SyncSerializedComponents();
        const std::filesystem::path n_base_path = scratch / "NBase.prefab";
        CheckTrue("N2", "N base prefab 写盘成功", PrefabUtility::SaveAsPrefabAsset(n_base_go, n_base_path));

        std::vector<GameObject*> n_base_gos;
        PrefabAsset* n_base = PrefabUtility::LoadPrefabAsset(n_base_path, &n_base_gos);
        GameObject* n_base_root = (n_base != nullptr) ? n_base->GetRootGameObject() : nullptr;
        CheckTrue("N3", "N base 读回来了且有根", n_base_root != nullptr);

        const char* n_exts[2] = { ".prefab", ".zasset" };
        for (int t = 0; t < 2 && n_base_root != nullptr; ++t)
        {
            const std::string n_tag = std::string("N") + (t == 0 ? "-prefab" : "-zasset");
            const std::filesystem::path mid_path  = scratch / (std::string("NMid")  + n_exts[t]);
            const std::filesystem::path leaf_path = scratch / (std::string("NLeaf") + n_exts[t]);

            auto MakeTsc = [&](const char* guid) -> TypeScriptComponent* {
                Object* raw = object_manager->Produce(TypeOf<TypeScriptComponent>(), 0);
                auto* tsc = (raw != nullptr) ? Cast<TypeScriptComponent>(raw) : nullptr;
                if (tsc != nullptr)
                {
                    object_manager->AllocateAndAssignInstanceID(tsc);
                    tsc->SetScriptGuid(eastl::string(guid));
                }
                return tsc;
            };

            // mid：往 base 根上加两个脚本组件（KEEP 和 DROP）
            TypeScriptComponent* keep_tsc = MakeTsc("N_KEEP");
            TypeScriptComponent* drop_tsc = MakeTsc("N_DROP");
            PrefabAsset* mid = PrefabUtility::CreateVariantAsset(n_base, eastl::string("NMid"));
            CheckTrue(Id(n_tag.c_str(), 1).c_str(), "造出了 mid 与两个待加组件",
                      mid != nullptr && keep_tsc != nullptr && drop_tsc != nullptr);
            if (mid == nullptr || keep_tsc == nullptr || drop_tsc == nullptr)
            {
                continue;
            }
            for (TypeScriptComponent* c : {keep_tsc, drop_tsc})
            {
                VariantComponentAddition add;
                add.host      = PPtr<GameObject>(n_base_root);
                add.component = c;
                mid->GetVariantAddedComponents().push_back(add);
            }
            CheckTrue(Id(n_tag.c_str(), 2).c_str(), "mid 写盘成功", PrefabUtility::SaveVariantAsset(mid, mid_path));

            PrefabAsset* mid_loaded = PrefabUtility::LoadPrefabAsset(mid_path);
            CheckTrue(Id(n_tag.c_str(), 3).c_str(), "mid 读回来了，带着 2 个新增组件",
                      mid_loaded != nullptr && mid_loaded->GetVariantAddedComponents().size() == 2);
            if (mid_loaded == nullptr || mid_loaded->GetVariantAddedComponents().size() != 2)
            {
                object_manager->Destroy(mid);
                continue;
            }
            Component* mid_keep = mid_loaded->GetVariantAddedComponents()[0].component;
            Component* mid_drop = mid_loaded->GetVariantAddedComponents()[1].component;

            // leaf：删掉 mid 加的 DROP，并给 KEEP 叠一条属性 override（目标是 mid 文件里
            // 的模板对象）。
            PrefabAsset* leaf = PrefabUtility::CreateVariantAsset(mid_loaded, eastl::string("NLeaf"));
            CheckTrue(Id(n_tag.c_str(), 4).c_str(), "造出了 leaf", leaf != nullptr);
            if (leaf == nullptr)
            {
                object_manager->Destroy(mid);
                continue;
            }
            leaf->GetVariantRemovedComponents().push_back(PPtr<Component>(mid_drop));
            CheckTrue(Id(n_tag.c_str(), 5).c_str(), "leaf 写盘成功", PrefabUtility::SaveVariantAsset(leaf, leaf_path));

            PrefabAsset* leaf_loaded = PrefabUtility::LoadPrefabAsset(leaf_path);
            CheckTrue(Id(n_tag.c_str(), 6).c_str(), "leaf 读回来了", leaf_loaded != nullptr);
            if (leaf_loaded != nullptr)
            {
                // 净结构：base 根上只剩 KEEP
                std::vector<PrefabAsset*> chain;
                CheckTrue(Id(n_tag.c_str(), 7).c_str(), "三层链能解开",
                          PrefabUtility::ResolveVariantChain(leaf_loaded, chain) && chain.size() == 3);
                PrefabUtility::VariantStructure net;
                PrefabUtility::ResolveVariantStructure(chain, net);
                CheckEq(Id(n_tag.c_str(), 8).c_str(), "净结构：新增 1 个（DROP 被后层抵消）、删除 1 个记录",
                        std::to_string(net.added.size()) + "/" + std::to_string(net.removed.size()), "1/1");

                GameObject* n_clone = PrefabUtility::Instantiate(leaf_loaded, /*invoke_post_load=*/true);
                CheckTrue(Id(n_tag.c_str(), 9).c_str(), "leaf 能实例化", n_clone != nullptr);
                if (n_clone != nullptr)
                {
                    int tsc_count = 0;
                    std::string only;
                    for (ImmediatePtr<Component>& slot : n_clone->EditorGetComponentsRef())
                    {
                        if (auto* tsc = Cast<TypeScriptComponent>(static_cast<Component*>(slot)))
                        {
                            ++tsc_count;
                            only = tsc->GetScriptGuid().c_str();
                        }
                    }
                    CheckEq(Id(n_tag.c_str(), 10).c_str(), "克隆上只有 mid 加的 KEEP，DROP 被 leaf 删掉",
                            std::to_string(tsc_count) + ":" + only, "1:N_KEEP");
                }

                // mid 自己实例化：DROP 还在（leaf 的删除不会倒灌回 mid）
                GameObject* mid_clone = PrefabUtility::Instantiate(mid_loaded, /*invoke_post_load=*/true);
                int mid_tsc = 0;
                if (mid_clone != nullptr)
                {
                    for (ImmediatePtr<Component>& slot : mid_clone->EditorGetComponentsRef())
                        if (Cast<TypeScriptComponent>(static_cast<Component*>(slot)) != nullptr)
                            ++mid_tsc;
                }
                CheckEq(Id(n_tag.c_str(), 11).c_str(), "mid 自己实例化仍有 2 个脚本组件（后层的删除不倒灌）",
                        std::to_string(mid_tsc), "2");
            }
            object_manager->Destroy(leaf);
            object_manager->Destroy(mid);
        }

        // ---- override 指向 Variant 新增的组件 ----
        // 单独成段：属性 override 的 target 是 mid 文件里的模板，不在 base 的图里。
        {
            const std::filesystem::path ov_mid_path = scratch / "NOvMid.prefab";
            TypeScriptComponent* ov_tsc = nullptr;
            {
                Object* raw = object_manager->Produce(TypeOf<TypeScriptComponent>(), 0);
                ov_tsc = (raw != nullptr) ? Cast<TypeScriptComponent>(raw) : nullptr;
                if (ov_tsc != nullptr)
                {
                    object_manager->AllocateAndAssignInstanceID(ov_tsc);
                    ov_tsc->SetScriptGuid(eastl::string("N_OV"));
                }
            }
            PrefabAsset* ov_mid = (n_base != nullptr) ? PrefabUtility::CreateVariantAsset(n_base, eastl::string("NOvMid")) : nullptr;
            CheckTrue("N-ov1", "造出了 mid 与新增组件", ov_mid != nullptr && ov_tsc != nullptr && n_base_root != nullptr);
            if (ov_mid != nullptr && ov_tsc != nullptr && n_base_root != nullptr)
            {
                VariantComponentAddition add;
                add.host      = PPtr<GameObject>(n_base_root);
                add.component = ov_tsc;
                ov_mid->GetVariantAddedComponents().push_back(add);
                CheckTrue("N-ov2", "mid 写盘成功", PrefabUtility::SaveVariantAsset(ov_mid, ov_mid_path));
                PrefabAsset* ov_loaded = PrefabUtility::LoadPrefabAsset(ov_mid_path);
                Component* tpl = (ov_loaded != nullptr && !ov_loaded->GetVariantAddedComponents().empty())
                    ? static_cast<Component*>(ov_loaded->GetVariantAddedComponents()[0].component)
                    : nullptr;
                CheckTrue("N-ov3", "读回了新增组件的模板对象", tpl != nullptr);

                // leaf 把 N_OV 组件的 m_ScriptGuid 改成别的值：目标是 mid 文件里的模板
                PrefabAsset* ov_leaf = (tpl != nullptr) ? PrefabUtility::CreateVariantAsset(ov_loaded, eastl::string("NOvLeaf")) : nullptr;
                CheckTrue("N-ov4", "造出了 leaf", ov_leaf != nullptr);
                if (ov_leaf != nullptr)
                {
                    // 借 SnapshotWithOverrides 的字节布局拿到 m_ScriptGuid 的位置和长度，
                    // 再造一个等长的新值（"N_OV" -> "N_XX"）。
                    prefab::SerialisedObject tree;
                    std::vector<uint8_t> bytes;
                    PrefabUtility::SnapshotWithOverrides(*tpl, {}, tree, bytes);
                    prefab::PropertyLocation loc;
                    std::vector<prefab::PathToken> tokens;
                    prefab::TokenizePropertyPath(eastl::string("m_script_guid"), tokens);
                    const bool located = prefab::LocatePropertyInStream(tree.type_tree, bytes, tokens, loc) && loc.valid();
                    CheckTrue("N-ov5", "定位到模板上的 m_script_guid 字段", located);
                    if (located)
                    {
                        PropertyModification mod;
                        mod.target = PPtr<Object>(tpl);
                        mod.property_path = eastl::string("m_script_guid");
                        // 字符串的流布局是 [len:int32][chars...][0 补到 4 字节边界]，所以
                        // "N_OV"(4 字符) 占 4+4+1 字节：改的是 chars 的最后两个字符，
                        // 即从 4 字节长度头之后数的第 3、4 个字符，而不是切片末尾。
                        mod.value_bytes = std::vector<uint8_t>(bytes.begin() + loc.byte_offset,
                                                               bytes.begin() + loc.byte_offset + loc.byte_size);
                        int32_t str_len = 0;
                        std::memcpy(&str_len, mod.value_bytes.data(), sizeof(str_len));
                        CheckEq("N-ov5b", "字符串切片的长度头是 4（\"N_OV\"）", std::to_string(str_len), "4");
                        const size_t chars = sizeof(int32_t);
                        mod.value_bytes[chars + 2] = 'X';
                        mod.value_bytes[chars + 3] = 'X';
                        ov_leaf->AddVariantOverride(mod);
                        CheckTrue("N-ov6", "leaf 写盘成功",
                                  PrefabUtility::SaveVariantAsset(ov_leaf, scratch / "NOvLeaf.prefab"));
                        PrefabAsset* ov_leaf_loaded = PrefabUtility::LoadPrefabAsset(scratch / "NOvLeaf.prefab");
                        CheckTrue("N-ov6b", "leaf 读回来后 override 还在，且 target 解析回 mid 文件里的模板对象",
                                  ov_leaf_loaded != nullptr && ov_leaf_loaded->GetVariantOverrides().size() == 1 &&
                                      static_cast<Object*>(ov_leaf_loaded->GetVariantOverrides()[0].target) == tpl);
                        GameObject* ov_clone = (ov_leaf_loaded != nullptr)
                            ? PrefabUtility::Instantiate(ov_leaf_loaded, /*invoke_post_load=*/true)
                            : nullptr;
                        std::string seen = "<none>";
                        if (ov_clone != nullptr)
                        {
                            for (ImmediatePtr<Component>& slot : ov_clone->EditorGetComponentsRef())
                                if (auto* tsc = Cast<TypeScriptComponent>(static_cast<Component*>(slot)))
                                    seen = tsc->GetScriptGuid().c_str();
                        }
                        CheckEq("N-ov7", "override 落在 mid 新增的组件上（N_OV -> N_XX）", seen, "N_XX");
                    }
                    object_manager->Destroy(ov_leaf);
                }
            }
            if (ov_mid != nullptr)
            {
                object_manager->Destroy(ov_mid);
            }
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
