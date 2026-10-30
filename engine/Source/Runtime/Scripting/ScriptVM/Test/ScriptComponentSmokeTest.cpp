// ==========================================================================
// TypeScriptComponent 真身冒烟测试：把 .ts 编译产物挂到 GameObject 上跑起来。
//
// 为什么需要它
// ------------
// 前面两个测试各钉一段：
//   ScriptVMSmokeTest      手写 CommonJS 能被 ScriptEnv 加载、实例能跑
//   ScriptTsChainSmokeTest tsc 产物能被 ScriptEnv 加载、原型链对
//
// 但它们都是**直接 new 一个 ScriptEnv 手喂模块**。用户真正走的路是：
//
//   <project>/Scripts/Foo.ts
//     -> tsc -> <project>/Intermediate/Scripts/Foo.js
//     -> ScriptRegistry 扫到它，给一个确定性 GUID
//     -> 用户把脚本拖到 Inspector / 组件上记下这个 GUID
//     -> TypeScriptComponent::PostLoadResource 拿 GUID 去 ScriptRegistry 查
//        -> ResolveScript: guid -> ScriptAsset -> module id + 类名
//        -> LoadModule -> CreateInstance
//        -> 注入 gameObject / transform / component
//        -> OnAwake / OnStart
//     -> 每帧 Tick 转发 OnUpdate
//     -> 保存 .ts 后热重载：OnDestroy -> 重建实例
//
// 这条路上任何一环断掉（guid 查不到、类名解不出来、module id 拼错、注入时机
// 晚了、热重载没重新实例化），前面两个测试全都不会红。
//
// 关于本机怎么构造 GameObject
// ---------------------------
// 走的是引擎里那条正规路：ObjectManager::NewObject<GameObject>() +
// GameObject::AddComponentByTypeName()，也就是编辑器拖组件时真正跑的
// PostLoadResource -> addComponent 顺序。两者都依赖 TypeManager 的类型表。
//
// 类型表由 IMPLEMENT_REGISTER_CLASS 的静态注册器填，而注册器最终调到
// TypeManager 里的 GlobalRegisterType(desc)。本机把 bin/libZRuntime.a 和现编的
// .gen.cpp 混链，TypeRegistrationDesc 的布局在 .a 那份 TypeManager.cpp 里是旧
// 版，RegisterType 读到的 desc.type 是错位后的 nullptr —— 于是 GameObject /
// Transform / TypeScriptComponent 全都注册不进去，ClassNameToType 返回 null，
// AddComponentByTypeName 只会打一条 warning 然后返回 nullptr。
//
// 解法是把 TypeManager.cpp 也列进 --extra-source，让 GlobalRegisterType 和
// desc 的构造方用同一份布局（实测：加进来之后 GameObject idx=1、Transform idx=5、
// TypeScriptComponent idx=3，GetTransform() 拿到真指针）。完整 cmake 构建里两
// 边同一份源码，不需要这条。
//
//   --extra-source engine/Source/Runtime/BaseClasses/TypeManager.cpp
//
// 关于 headless
// -------------
// 只注册 CommandSystem / ProjectInfo / ScriptRegistry / ScriptingManager +
// RegisterCore()。没有窗口、没有渲染器、没有关卡。
// ==========================================================================

#if !PAPI_QUICKJS
#error "ScriptComponentSmokeTest 需要 QuickJS 后端（PAPI_QUICKJS）。"
#endif

#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/RegisterRuntime.h"
#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/BaseClasses/ObjectManager.h"
#include "Runtime/BaseClasses/TypeManager.h"
#include "Runtime/Function/Command/CommandSystem.h"
#include "Runtime/Function/Framework/Component/Script/TypeScriptComponent.h"
#include "Runtime/Function/Framework/Component/Transform/Transform.h"
#include "Runtime/Project/ProjectInfo.h"
#include "Runtime/Scripting/Native/PuertsNative.h"
#include "Runtime/Scripting/ScriptRegistry.h"
#include "Runtime/Scripting/ScriptingManager.h"
#include "Runtime/Scripting/Wrapper/ScriptEnv.h"

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

    /// ScriptRegistry 在 Windows 上把相对路径整体小写化（ScriptRegistry.cpp 的
    /// normaliseInternal：`#if defined(_WIN32)` 分支），分隔符统一成 '/'。所以凡是
    /// 拿 m_SourceRelPath / m_CompiledRelPath 做比较的地方都必须先折叠大小写，
    /// 否则这份测试在 Windows 上一条都过不去、在 macOS/Linux 上又全过 —— 典型的
    /// "只在一个平台跑过"的断言。折叠后比的是规范化形式，不是放宽断言。
    std::string NormalisedPath(const std::string& in)
    {
        std::string s = in;
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }

    /// 读回 globalThis.__zResult（字符串）。pesapi_eval 不返回表达式结果。
    std::string ReadResult(ScriptEnv& env)
    {
        pesapi_ffi* apis = env.GetPapis();
        pesapi_env_ref ref = env.GetEnvRef();
        if (apis == nullptr || ref == nullptr)
        {
            return "<no env>";
        }
        pesapi_scope scope = pesapi_open_scope(apis, ref);
        pesapi_env e = pesapi_get_env_from_ref(apis, ref);
        std::string out = "<none>";
        pesapi_value v = pesapi_get_property(apis, e, pesapi_global(apis, e), "__zResult");
        if (v != nullptr && pesapi_is_string(apis, e, v))
        {
            char buf[1024];
            size_t len = sizeof(buf) - 1;
            const char* s = pesapi_get_value_string_utf8(apis, e, v, buf, &len);
            if (s != nullptr)
            {
                out.assign(s, len);
                while (!out.empty() && out.back() == '\0')
                {
                    out.pop_back();
                }
            }
        }
        pesapi_close_scope(apis, scope);
        return out;
    }

    /// 跑一段 JS，把结论写进 globalThis.__zResult 再读回来。
    std::string EvalToResult(ScriptEnv& env, const std::string& code)
    {
        pesapi_ffi* apis = env.GetPapis();
        pesapi_env_ref ref = env.GetEnvRef();
        if (apis == nullptr || ref == nullptr)
        {
            return "<no env>";
        }
        pesapi_scope scope = pesapi_open_scope(apis, ref);
        pesapi_env e = pesapi_get_env_from_ref(apis, ref);
        pesapi_eval(apis, e, reinterpret_cast<const uint8_t*>(code.data()), code.size(), "<probe>");
        const bool threw = pesapi_has_caught(apis, scope);
        pesapi_close_scope(apis, scope);
        if (threw)
        {
            return "<threw>";
        }
        return ReadResult(env);
    }

    /// globalThis.__hostedDestroyCount（HostedMover.OnDestroy 写的地方）。
    std::string ReadDestroyCount(ScriptEnv& env)
    {
        return EvalToResult(env,
                            "globalThis.__zResult = String("
                            "globalThis.__hostedDestroyCount == null ? 'null' : "
                            "globalThis.__hostedDestroyCount);");
    }

    /// 从活实例上读一个字段的字符串值。
    std::string FieldValue(ScriptingManager* sm, pesapi_value_ref inst, const char* key)
    {
        if (inst == nullptr)
        {
            return "<no instance>";
        }
        for (const auto& entry : sm->EnumerateInstanceFields(inst))
        {
            if (std::get<0>(entry) == key)
            {
                return std::get<2>(entry);
            }
        }
        return "<missing>";
    }

    /// 造一个 GameObject，走的是 ObjectManager::Produce（普通 new）+ 分配真的
    /// InstanceID。AddComponentByTypeName 造组件用的也是 Produce，所以宿主和组件
    /// 的分配/释放是同一套（ObjectManager::Destroy 内部也是 delete）—— 混用
    /// NewObject<>()（mimalloc）会让一边的释放进错堆。
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
}  // namespace

int main(int argc, char** argv)
{
    // 不缓冲：崩在半路时还能看到已经跑到哪一步（stdout 走管道/文件时默认是全缓冲，
    // 崩了就什么都看不到，排查全靠猜）。
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

    std::printf("=== TypeScriptComponent 真身冒烟测试 ===\n");

    // ---- 1. 造一个临时工程 ------------------------------------------------
    std::error_code ec;
    const std::filesystem::path scratch = std::filesystem::weakly_canonical(
        std::filesystem::temp_directory_path() / "zengine_script_component_smoke", ec);
    std::filesystem::remove_all(scratch, ec);
    std::filesystem::create_directories(scratch, ec);

    const std::filesystem::path project_file = scratch / "Comp.zproject";
    WriteTextFile(project_file,
                  "name: Comp\n"
                  "version: 1.0.0\n"
                  "scripts_dir: Scripts\n"
                  "intermediate_dir: Intermediate\n");

    // .ts 源（ScriptRegistry 扫的就是这两份）+ tsc 产物（ScriptingManager 加载的）
    const std::filesystem::path ts_dir = scratch / "Scripts";
    const std::filesystem::path js_dir = scratch / "Intermediate" / "Scripts";
    std::filesystem::create_directories(ts_dir, ec);
    std::filesystem::create_directories(js_dir, ec);

    bool staged = true;
    for (const char* name : {"HostedMover.ts", "Mover.ts"})
    {
        std::string text;
        if (!ReadTextFile(std::filesystem::path(resource_dir) / "Scripts" / name, text) ||
            !WriteTextFile(ts_dir / name, text))
        {
            std::printf("FATAL: 读不到/写不出 %s（resource_dir=%s）\n", name, resource_dir.c_str());
            staged = false;
        }
    }
    for (const char* name : {"HostedMover.js", "Mover.js"})
    {
        std::string text;
        if (!ReadTextFile(std::filesystem::path(resource_dir) / "TscOutput" / name, text) ||
            !WriteTextFile(js_dir / name, text))
        {
            std::printf("FATAL: 读不到/写不出 %s（resource_dir=%s）\n", name, resource_dir.c_str());
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
    // CommandSystem::Initialize() 从 argv 解析 -p/--project，测试进程没这个参数，
    // 它会把 project_path 冲成空，于是 ProjectInfo::LoadFromFile 失败、整个
    // InitializeAll 回滚。手工填好并标成"已初始化"让 InitializeAll 跳过它。
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
    if (sm == nullptr || registry == nullptr || sm->GetEnv() == nullptr)
    {
        std::printf("FATAL: ScriptingManager / ScriptRegistry / ScriptEnv 没起来。\n");
        return 77;
    }

    // 先钉住"类型表是齐的"：下面 AddComponentByTypeName / GetTransform 都靠它。
    // 本机如果忘了把 TypeManager.cpp 列进 --extra-source，这里就会红（见文件头）。
    CheckTrue("A0a", "GameObject 注册进了类型表",
              TypeManager::GetInstance().ClassNameToType("GameObject") != nullptr);
    CheckTrue("A0b", "Transform 注册进了类型表",
              TypeManager::GetInstance().ClassNameToType("Transform") != nullptr);
    CheckTrue("A0c", "TypeScriptComponent 注册进了类型表",
              TypeManager::GetInstance().ClassNameToType("TypeScriptComponent") != nullptr);
    CheckTrue("A0d", "GameObject 有 factory（否则 AddComponentByTypeName 造不出对象）",
              TypeOf<GameObject>()->GetFactory() != nullptr);
    if (TypeManager::GetInstance().ClassNameToType("GameObject") == nullptr ||
        TypeManager::GetInstance().ClassNameToType("TypeScriptComponent") == nullptr)
    {
        std::printf("FATAL: 类型表是空的，后面的挂载路径无从测试。\n");
        return 77;
    }

    // ---- A. ScriptRegistry：.ts 被扫到，且 guid / 类名 / 编译产物都解得出 ----
    std::printf("\n--- A. ScriptRegistry 扫描与解析 ---\n");
    CheckEq("A1", "js root 指向 <project>/Intermediate/Scripts",
            sm->GetJsRoot().string(), (scratch / "Intermediate" / "Scripts").string());

    auto all_scripts = registry->GetAll();
    CheckEq("A2", "注册表扫到 2 个 .ts", std::to_string(all_scripts.size()), "2");

    ScriptAsset* hosted = nullptr;
    ScriptAsset* exported_by_list = nullptr;
    for (auto* asset : all_scripts)
    {
        const std::string rel = NormalisedPath(asset->m_SourceRelPath.c_str());
        if (rel.find("hostedmover.ts") != std::string::npos)
        {
            hosted = asset;
        }
        if (rel.find("mover.ts") != std::string::npos && rel.find("hosted") == std::string::npos)
        {
            exported_by_list = asset;
        }
    }
    CheckTrue("A2b", "HostedMover.ts 被扫进注册表", hosted != nullptr);
    if (hosted == nullptr)
    {
        return 1;
    }
    CheckEq("A2c", "源路径是相对工程根的（Scripts/ 前缀），且已规范化成小写 + '/'",
            NormalisedPath(hosted->m_SourceRelPath.c_str()), "scripts/hostedmover.ts");
    CheckEq("A3", "guid 是 32 位小写 hex", std::to_string(hosted->m_Guid.size()), "32");
    CheckEq("A4", "`export class X extends` 能解出默认类名",
            std::string(hosted->m_DefaultClassName.c_str()), "HostedMover");
    CheckEq("A5", "编译产物路径被配对上（同样已规范化）",
            NormalisedPath(hosted->m_CompiledRelPath.c_str()),
            "intermediate/scripts/hostedmover.js");

    CheckTrue("A6", "Mover.ts 被扫进注册表", exported_by_list != nullptr);
    if (exported_by_list == nullptr)
    {
        return 1;
    }
    // 钉住现状：`class X ...; export { X };` 这种写法 ParseDefaultClassName 认不出。
    CheckEq("A7", "`export { X }` 写法的默认类名是空的（只能靠组件上的 m_ClassName）",
            std::string(exported_by_list->m_DefaultClassName.c_str()), "");

    // ---- B. 挂载：PostLoadResource -> LoadModule -> CreateInstance ----------
    std::printf("\n--- B. 组件挂载与引擎上下文注入 ---\n");
    // 走编辑器拖组件那条真路：ObjectManager 造 GameObject（带真的 InstanceID），
    // AddComponentByTypeName 造组件并自己跑 PostLoadResource -> addComponent。
    auto* object_manager = GET_SYSTEM(ObjectManager);
    if (object_manager == nullptr)
    {
        std::printf("FATAL: ObjectManager 没起来。\n");
        return 77;
    }
    auto* go = MakeGameObject(object_manager, "Host");
    if (go == nullptr)
    {
        std::printf("FATAL: Produce(GameObject) 返回 nullptr。\n");
        return 77;
    }

    auto* transform = Cast<Transform>(go->AddComponentByTypeName("Transform"));
    CheckTrue("B0a", "AddComponentByTypeName(\"Transform\") 造出了真的 Transform",
              transform != nullptr);
    CheckTrue("B0b", "GetTransform() 能把它找回来（IsBaseOf 范围测试是通的）",
              go->GetTransform() == transform);

    auto* comp = Cast<TypeScriptComponent>(go->AddComponentByTypeName("TypeScriptComponent"));
    if (comp == nullptr)
    {
        std::printf("FATAL: AddComponentByTypeName(\"TypeScriptComponent\") 返回 nullptr。\n");
        return 1;
    }
    CheckTrue("B1", "还没设 guid 时组件是休眠的（不绑、不报错）", !comp->HasLiveInstance());

    // 编辑器里的顺序：先挂组件（此时 guid 为空 -> 休眠），再把脚本拖上去。
    // SetScriptGuid 内部自己 BindAndAwake。
    comp->SetScriptGuid(hosted->m_Guid);

    CheckTrue("B2", "设了 guid 之后拿到活实例", comp->HasLiveInstance());
    CheckEq("B3", "module id 是 jroot 下的相对路径（无 .js）",
            comp->GetModuleId(), "HostedMover");
    CheckTrue("B4", "模块确实被加载了", sm->IsModuleLoaded("HostedMover"));

    pesapi_value_ref inst = nullptr;
    // 活实例藏在 m_JsInstance 里，测试拿不到；用 ScriptingManager 重新造一个同
    // 类的实例去读类字段是不对的（那是另一个对象）。这里改走 EnumerateLiveFields
    // —— 它读的就是组件手上那个实例。
    auto live_fields = comp->EnumerateLiveFields();
    auto live_value = [&live_fields](const char* key) -> std::string {
        for (const auto& entry : live_fields)
        {
            if (std::get<0>(entry) == key)
            {
                return std::get<2>(entry);
            }
        }
        return "<missing>";
    };

    CheckEq("B5", "OnAwake 被调用了一次", live_value("awakeCount"), "1");
    CheckEq("B6", "OnStart 在 OnAwake 之后被调用了一次", live_value("startCount"), "1");
    CheckEq("B7", "this.gameObject 注入成功", live_value("gotGameObject"), "true");
    CheckEq("B8", "this.component 注入成功", live_value("gotComponent"), "true");
    CheckEq("B9", "OnAwake 里就拿到了宿主的（这是唯一的注入时机）",
            live_value("hostName"), "Host");
    CheckEq("B10", "this.transform 注入成功", live_value("gotTransform"), "true");

    // ---- C. 每帧 Tick -> OnUpdate ----------------------------------------
    std::printf("\n--- C. Tick 转发 OnUpdate ---\n");
    CheckTrue("C1", "还没 Tick 时 updateCount 是 0", live_value("updateCount") == "0");
    comp->Tick(0.5f);
    comp->Tick(0.5f);
    live_fields = comp->EnumerateLiveFields();
    CheckEq("C2", "两帧后 OnUpdate 被调了两次", live_value("updateCount"), "2");
    CheckEq("C3", "speed=2 * dt=0.5 * 2 帧 => originX=2", live_value("lastSnapshot"),
            "2.0000,2,0.5");

    // ---- D. 字段覆盖（Phase 7）-------------------------------------------
    std::printf("\n--- D. Inspector 字段覆盖 ---\n");
    CheckTrue("D1", "LiveSetField(speed=10) 成功", comp->LiveSetField("speed", "10"));
    live_fields = comp->EnumerateLiveFields();
    CheckEq("D2", "speed 立刻变成 10", live_value("speed"), "10");
    comp->Tick(0.5f);
    live_fields = comp->EnumerateLiveFields();
    CheckEq("D3", "这一帧走了 5（2 -> 7）", live_value("lastSnapshot"), "7.0000,3,0.5");

    // ---- E. 热重载：OnDestroy -> 重建（状态不保留，但 override 保留）-------
    std::printf("\n--- E. 热重载 ---\n");
    CheckEq("E1", "重载前还没有 OnDestroy 发生过", ReadDestroyCount(*sm->GetEnv()), "null");
    CheckTrue("E2", "ReloadModule 成功", sm->ReloadModule(comp->GetModuleId()));
    CheckEq("E3", "旧实例的 OnDestroy 被调了一次", ReadDestroyCount(*sm->GetEnv()), "1");

    live_fields = comp->EnumerateLiveFields();
    CheckTrue("E4", "重载后组件仍然绑着（不是变空壳）", comp->HasLiveInstance());
    CheckEq("E5", "新实例是全新的：awakeCount 重新从 1 开始", live_value("awakeCount"), "1");
    CheckEq("E6", "updateCount 归零（热重载是无状态重建）", live_value("updateCount"), "0");
    CheckEq("E7", "originX 归零（游戏状态不跨重载保留）", live_value("originX"), "0");
    // 这条是 Phase 7 的关键：Inspector 里改的是 authoring data，不是游戏状态。
    CheckEq("E8", "Inspector 改过的 speed=10 跨重载保留", live_value("speed"), "10");

    comp->Tick(0.5f);
    live_fields = comp->EnumerateLiveFields();
    CheckEq("E9", "重载后第一帧就按 speed=10 走（0 -> 5）", live_value("lastSnapshot"),
            "5.0000,1,0.5");

    // ---- F. 类名解析：显式 m_ClassName 兜住 `export { X }` 写法 ------------
    std::printf("\n--- F. 类名解析的兜底 ---\n");
    auto* go2 = MakeGameObject(object_manager, "Host2");
    if (go2 == nullptr)
    {
        std::printf("FATAL: 第二个 GameObject 没造出来。\n");
        return 1;
    }
    go2->AddComponentByTypeName("Transform");
    auto* comp2 = Cast<TypeScriptComponent>(go2->AddComponentByTypeName("TypeScriptComponent"));
    if (comp2 == nullptr)
    {
        std::printf("FATAL: 第二个 TypeScriptComponent 没造出来。\n");
        return 1;
    }

    // Mover.ts 用 `export { Mover }`，DefaultClassName 是空的。
    comp2->SetScriptGuid(exported_by_list->m_Guid);
    CheckTrue("F1", "没给 m_ClassName 时绑不上（默认类名也是空的）", !comp2->HasLiveInstance());

    comp2->SetClassName(eastl::string("Mover"));
    CheckTrue("F2", "显式给了 m_ClassName 之后绑上了", comp2->HasLiveInstance());
    CheckEq("F3", "Mover 的 OnStart 跑过了（isBehaviour 为真）",
            [&] {
                auto f = comp2->EnumerateLiveFields();
                for (const auto& entry : f)
                {
                    if (std::get<0>(entry) == "isBehaviour")
                    {
                        return std::get<2>(entry);
                    }
                }
                return std::string("<missing>");
            }(),
            "true");

    // ---- G. 卸载：OnDestroy -> 不再有活实例 -------------------------------
    // 宿主和组件都是 Produce() 用 `new` 造的，所以统一走 ObjectManager::Destroy
    // （内部也是 delete）。混用 NewObject<>()（mimalloc）就得换
    // MemoryManager::DestroyObject，两边不能串 —— 见 ObjectManager.h 的说明。
    // 先销毁 GameObject：它不拥有组件（~GameObject 只清列表），晚销毁反而会在
    // 它的组件表里留下已释放的指针。
    std::printf("\n--- G. 组件销毁 ---\n");
    object_manager->Destroy(go2);
    object_manager->Destroy(comp2);
    CheckEq("G1", "Mover 没有 OnDestroy（计数不变）", ReadDestroyCount(*sm->GetEnv()), "1");

    // 解绑（SetScriptGuid 传空）走的是 TearDown，这是唯一能观察到"活实例真的没了"
    // 的时机 —— 组件析构之后指针就废了。
    comp->SetScriptGuid(eastl::string(""));
    CheckTrue("G2", "解绑后不再有活实例", !comp->HasLiveInstance());
    CheckEq("G3", "解绑把 HostedMover 的 OnDestroy 补上了", ReadDestroyCount(*sm->GetEnv()), "2");

    // 负向验证：已经解绑过一次，析构里的 TearDown 不能再触发第二次 OnDestroy。
    // 同样先销毁宿主再销毁组件（顺序反了会在宿主的组件表里留下已释放的指针）。
    object_manager->Destroy(go);
    object_manager->Destroy(comp);
    CheckEq("G4", "析构不会再补一次 OnDestroy（TearDown 是幂等的）",
            ReadDestroyCount(*sm->GetEnv()), "2");

    std::printf("\n========================================\n");
    std::printf("总计 %d 项检查，失败 %d 项\n", g_Checks, g_Failed);
    std::printf("========================================\n");
    return g_Failed == 0 ? 0 : 1;
}
