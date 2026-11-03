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
#include "Runtime/Function/Framework/Coroutine/Coroutine.h"
#include "Runtime/Function/Framework/Component/Script/TypeScriptComponent.h"
#include "Runtime/Function/Framework/Component/Transform/Transform.h"
#include "Runtime/Function/Framework/Component/Camera/CameraComponent.h"
#include "Runtime/Function/Framework/Component/Mesh/BaseRenderer.h"
#include "Runtime/Function/Render/DebugDraw/DebugDrawManager.h"
#include "Runtime/Project/ProjectInfo.h"
#include "Runtime/Scripting/Native/PuertsNative.h"
#include "Runtime/Scripting/ScriptTime/ScriptTime.h"
#include "Runtime/Scripting/ScriptGizmos/ScriptGizmos.h"
#include "Runtime/Scripting/ScriptRegistry.h"
#include "Runtime/Scripting/ScriptingManager.h"
#include "Runtime/Scripting/Wrapper/ScriptEnv.h"

// `g_isEditor` 由编辑器侧写入（与 g_isPlaying 同一机制：Runtime 不能依赖
// Editor 模块）。P8 / P9 要临时把它翻成 true 才能验 OnValidate 的编辑器语义。
extern bool g_isEditor;
// `GameObject::Tick` / `FixedTick` 都过 `shouldComponentTick`，而它在非播放态下
// 只放行 `g_editorTickComponentTypes` 白名单里的类型。T 段要验的是"固定步那条
// 链在 Player 下的行为"，所以要临时翻它（与上面 P8 / P9 翻 g_isEditor 同一手
// 法），用完立刻还原。
extern bool g_isPlaying;

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

    /// m_SourceRelPath / m_CompiledRelPath 保留**磁盘上的原大小写**（小写化只发生
    /// 在 ScriptRegistry 内部的查找 key 上、且只在 Windows）。这两条断言是按原大小写
    /// 比的 —— 折叠成小写再比会把"大小写被吃掉"这个回归放过去。
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

    /// globalThis.__hostedDisableCount（HostedMover.OnDisable 写的地方）。
    /// 和 Destroy 计数同一个写法：跨得过实例销毁。
    std::string ReadDisableCount(ScriptEnv& env)
    {
        return EvalToResult(env,
                            "globalThis.__zResult = String("
                            "globalThis.__hostedDisableCount == null ? 'null' : "
                            "globalThis.__hostedDisableCount);");
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

    /// 与 MakeGameObject 同一套分配路径造组件。
    ///
    /// ⚠️ 不能图省事直接 `new T()`：`Produce` 之外造出来的对象没跑过
    /// `InitializeRuntimeTypeInfo()`，于是 `Object::GetTypeName()` 里的
    /// `types[m_CachedTypeIndex]` 还是空 —— 一问名字就是一次空指针解引用。
    /// 而崩点离现场极远（`GameObject::FixedTick` 的白名单查询、序列化、
    /// `Cast<T>`），看起来跟"刚 new 出来的这个组件"一点关系都没有；直接的
    /// 用法（调它自己的方法）又全是好的，所以只在别人遍历到它时才炸。
    template<typename T>
    T* MakeComponent(ObjectManager* object_manager)
    {
        Object* raw = object_manager->Produce(TypeOf<T>(), 0);
        if (raw == nullptr)
        {
            return nullptr;
        }
        object_manager->AllocateAndAssignInstanceID(raw);
        return static_cast<T*>(raw);
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

    // 一份**旧版**注册表：path 是小写、guid 由小写 key 派生 —— 正是本改动之前
    // Windows 上写出来的样子。把它先铺下去，是为了让 A2c / A3b / A3d 顺带验证
    // 升级路径：rescan 必须把路径改回磁盘真实大小写，而 guid 一个字节都不能变
    // （变了 = 工程里所有 m_script_guid 引用断链，且全程不报错）。
    const std::filesystem::path registry_file = scratch / "AssetRegistry" / "script_registry.json";
    std::filesystem::create_directories(registry_file.parent_path(), ec);
    WriteTextFile(registry_file,
                  "{\n"
                  "  \"version\": 1,\n"
                  "  \"entries\": [\n"
                  "    { \"guid\": \"bc8cd2d47011b24f68b540318442ecc8\",\n"
                  "      \"path\": \"scripts/hostedmover.ts\",\n"
                  "      \"class\": \"HostedMover\" }\n"
                  "  ]\n"
                  "}\n");

    // ---- 2. 起系统 --------------------------------------------------------
    RegisterCore();
    REGISTER_SYSTEM(CommandSystem);
    REGISTER_SYSTEM(ProjectInfo);
    REGISTER_SYSTEM(ScriptRegistry);
    REGISTER_SYSTEM(ScriptingManager);
    // StartCoroutine 走 Behaviour -> GET_SYSTEM(CoroutineScheduler)。这里必须
    // 注册一个真的：没有它时 StartCoroutine 会记一条 warning 并返回 0，于是
    // "协程根本没跑"和"协程没被驱动"看起来是一模一样的结果。
    REGISTER_SYSTEM(CoroutineScheduler);
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

    // Gizmos 走 GET_SYSTEM(DebugDrawManager)。不注册的话整套是 no-op，而
    // "画不出来"和"没画"看起来一模一样。
    //
    // ⚠️ 必须放在 InitializeAll **之后**：`DebugDrawManager::Initialize()` 要
    // Vulkan 与 RenderSystem（GetDependencies 里就写着），而这里两个都没有 ——
    // 让它进 InitializeAll 会直接把整个进程挂死（踩过一次，卡了 10 分钟没输出）。
    // Gizmos 只需要它手上的 DebugDrawGroup（DebugDrawContext 是纯 CPU 的 map），
    // 所以注册但不 Initialize 就够了。
    REGISTER_SYSTEM(DebugDrawManager);

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
    // 原大小写必须保住（'/' 分隔 + 磁盘上的大小写），否则工程搬到区分大小写的
    // 文件系统后会打不开、被判成删除+新增、guid 重算、所有引用静默断链。
    CheckEq("A2c", "源路径是相对工程根的，且保留磁盘上的原大小写",
            std::string(hosted->m_SourceRelPath.c_str()), "Scripts/HostedMover.ts");
    CheckTrue("A2c2", "源路径没有被折叠成小写",
              std::string(hosted->m_SourceRelPath.c_str()) !=
                  NormalisedPath(hosted->m_SourceRelPath.c_str()));
    CheckEq("A3", "guid 是 32 位小写 hex", std::to_string(hosted->m_Guid.size()), "32");
    // 迁移关键：guid 必须用**查找 key**（Windows 上小写）做种子，不能用保留大小写的
    // m_SourceRelPath。用后者会让所有现存工程的 m_script_guid 全部失效。
    // 这个字面量是 FNV-1a 双种子("scripts/hostedmover.ts")，独立复算过。
    CheckEq("A3b", "guid 仍以小写化的 key 为种子（换成就地大小写即断链）",
            std::string(hosted->m_Guid.c_str()), "bc8cd2d47011b24f68b540318442ecc8");

    // 自愈必须落盘，否则只是内存里好看，下一次启动又回到小写。
    std::string registry_text;
    ReadTextFile(registry_file, registry_text);
    CheckTrue("A3d", "自愈结果已写回注册表文件",
              registry_text.find("Scripts/HostedMover.ts") != std::string::npos);
    CheckTrue("A3e", "注册表文件里不再残留小写路径",
              registry_text.find("scripts/hostedmover.ts") == std::string::npos);
    CheckEq("A4", "`export class X extends` 能解出默认类名",
            std::string(hosted->m_DefaultClassName.c_str()), "HostedMover");
    // ⚠️ A5 在 Windows 上是**弱**断言：`std::filesystem::relative()` 定义为
    // weakly_canonical(p).lexically_relative(weakly_canonical(base))，会把真实磁盘
    // 大小写还原回来，所以即使 m_SourceRelPath 被折叠成小写，这条照样是绿的。
    // 真正守住"大小写没被吃掉"的是上面的 A2c / A2c2。
    CheckEq("A5", "编译产物路径被配对上",
            std::string(hosted->m_CompiledRelPath.c_str()),
            "Intermediate/Scripts/HostedMover.js");

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

    // ---- C2 组：LateUpdate ------------------------------------------------
    // Component::LateTick 是新的，TypeScriptComponent::LateTick 转发到 JS 的
    // OnLateUpdate。这一段钉两件事：真的转到了，且排在同一帧 Update **之后**。
    CheckEq("C2b", "还没 LateTick 时 lateCount 是 0", live_value("lateCount"), "0");
    comp->Tick(0.5f);
    comp->LateTick(0.5f);
    live_fields = comp->EnumerateLiveFields();
    CheckEq("C2c", "OnLateUpdate 被调了一次", live_value("lateCount"), "1");
    // 顺序探针：本帧 updateCount 已经是 3（前面两帧 2 次 + 这一帧 1 次）。
    // LateUpdate 若被排到 Update 之前，这里会是 2。
    CheckEq("C2d", "LateUpdate 看到的是本帧 Update 之后的 updateCount（3，不是 2）",
            live_value("lateSawUpdateCount"), "3");
    CheckEq("C2e", "LateUpdate 拿的是帧间隔（0.5），不是固定步长", live_value("lateDt"), "0.5");
    // 反向的一半：只 LateTick 不该顺带又跑一次 Update。
    comp->LateTick(0.5f);
    live_fields = comp->EnumerateLiveFields();
    CheckEq("C2f", "只 LateTick 不会再跑一次 Update", live_value("updateCount"), "3");
    CheckEq("C2g", "而 LateUpdate 又走了一次", live_value("lateCount"), "2");

    // ---- C3 组：OnEnable / OnDisable --------------------------------------
    // Unity 的触发条件是 `enabled && activeInHierarchy` 的**合取**，不是某一
    // 个条件自己变了就发。下面每条都是成对测的：能触发的那次 + 不该触发的那次。
    CheckEq("C2h", "绑上的时候（开着 + enabled）收过一次 OnEnable",
            live_value("enableCount"), "1");
    CheckEq("C2i", "还没禁用过", live_value("disableCount"), "0");

    comp->SetEnabled(false);
    live_fields = comp->EnumerateLiveFields();
    CheckEq("C2j", "enabled=false 触发 OnDisable", live_value("disableCount"), "1");
    comp->SetEnabled(true);
    live_fields = comp->EnumerateLiveFields();
    CheckEq("C2k", "enabled=true 触发 OnEnable", live_value("enableCount"), "2");
    comp->SetEnabled(true);
    live_fields = comp->EnumerateLiveFields();
    CheckEq("C2l", "重复设成同一个值不再触发（不是每帧一次）", live_value("enableCount"), "2");

    go->SetActive(false);
    live_fields = comp->EnumerateLiveFields();
    CheckEq("C2m", "SetActive(false) 也触发 OnDisable", live_value("disableCount"), "2");
    // 互补的一半：对象关着时把 enabled 来回拨，不该有任何 OnEnable —— 少了这条，
    // "只判 enabled 不判 activeInHierarchy"的实现是绿的。
    comp->SetEnabled(false);
    comp->SetEnabled(true);
    live_fields = comp->EnumerateLiveFields();
    CheckEq("C2n", "关着的时候 enabled 来回拨不触发 OnEnable（是合取，不是或）",
            live_value("enableCount"), "2");
    go->SetActive(true);
    live_fields = comp->EnumerateLiveFields();
    CheckEq("C2o", "重新激活触发 OnEnable", live_value("enableCount"), "3");
    // 反过来的一半：对象开着但组件 disabled 时激活不该发。
    comp->SetEnabled(false);
    go->SetActive(false);
    go->SetActive(true);
    live_fields = comp->EnumerateLiveFields();
    CheckEq("C2p", "组件 disabled 时激活对象不触发 OnEnable", live_value("enableCount"), "3");
    comp->SetEnabled(true);
    live_fields = comp->EnumerateLiveFields();
    CheckEq("C2q", "再把 enabled 打开就触发了", live_value("enableCount"), "4");

    // ---- D. 字段覆盖（Phase 7）-------------------------------------------
    std::printf("\n--- D. Inspector 字段覆盖 ---\n");
    CheckTrue("D1", "LiveSetField(speed=10) 成功", comp->LiveSetField("speed", "10"));
    live_fields = comp->EnumerateLiveFields();
    CheckEq("D2", "speed 立刻变成 10", live_value("speed"), "10");
    comp->Tick(0.5f);
    live_fields = comp->EnumerateLiveFields();
    // 3 -> 8：C2 组多跑了一帧 Update（originX 2 -> 3），所以这里起点是 3。
    CheckEq("D3", "这一帧走了 5（3 -> 8）", live_value("lastSnapshot"), "8.0000,4,0.5");

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

    // ---- F2. 协程：TypeScript 生成器 <-> 引擎调度器 ------------------------
    //
    // 这一段是整条链唯一的端到端证明。UnityParitySmokeTest 的 CoroutineProbe
    // 自己回答 Start/Step/End 三个钩子，所以它全程绿也不能说明 JS 侧接上了：
    // shim 里的 `__zWait` 标记写错一个字母、ScriptEnv 少读一个分支、tsc 把
    // generator 降级成别的东西，C++ 那边都看不见。这里三个相位各推一次，
    // 每一步的步数都不同（1 / 3 / 7），因此"停在第几步"是一个数字就能读出的。
    std::printf("\n--- F2. 协程（StartCoroutine / yield）---\n");
    auto* scheduler = GET_SYSTEM(CoroutineScheduler);
    CheckTrue("H0", "CoroutineScheduler 注册了", scheduler != nullptr);
    if (scheduler != nullptr)
    {
        scheduler->StopAll();
        auto mover_value = [&](const char* key) {
            for (const auto& entry : comp2->EnumerateLiveFields())
            {
                if (std::get<0>(entry) == key)
                {
                    return std::get<2>(entry);
                }
            }
            return std::string("<missing>");
        };

        CheckTrue("H1", "InvokeMethod(\"StartMyRoutine\") 送到了 JS 侧",
                  comp2->InvokeMethod("StartMyRoutine"));
        // Unity 的形状：StartCoroutine 返回时第一段已经同步跑完。
        CheckEq("H2", "第一段同步跑了（steps=1）", mover_value("routineSteps"), "1");
        CheckEq("H3", "StartCoroutine 交回了句柄（不是 null）",
                mover_value("routineHandleOk"), "true");

        // WaitForSeconds(0.2)：先给 0.1，不够。
        scheduler->TickUpdate(0.1f, 0.1f);
        CheckEq("H4", "0.1s 时 WaitForSeconds 还没到期", mover_value("routineSteps"), "1");
        // 世界暂停（缩放 0、墙钟 0.5）：WaitForSeconds 必须停住。少了这条，
        // "shim 把 WaitForSeconds 标记成了 secondsRealtime" 完全看不出来 ——
        // 之前的用例 scaled 和 unscaled 一直相等，两者无从分辨（变异 SC12）。
        scheduler->TickUpdate(0.f, 0.5f);
        CheckEq("H4b", "timeScale=0 时 WaitForSeconds 冻结（不吃墙钟）",
                mover_value("routineSteps"), "1");
        scheduler->TickUpdate(0.1f, 0.1f);
        CheckEq("H5", "0.2s 时进入第二段", mover_value("routineSteps"), "3");

        // WaitForEndOfFrame：Update 相位不许推它。
        scheduler->TickUpdate(0.1f, 0.1f);
        CheckEq("H6", "WaitForEndOfFrame 不会被 Update 推着走",
                mover_value("routineSteps"), "3");
        scheduler->TickEndOfFrame();
        CheckEq("H7", "EndOfFrame 相位把它推到收尾", mover_value("routineSteps"), "7");
        CheckEq("H8", "三次 yield 按顺序都跑到了", mover_value("routineKinds"), "abc");
        CheckEq("H9", "并且协程体自己结束了", mover_value("routineDone"), "true");
        // `done` 没被翻译出来的话，泵会一直 next() 下去：生成器每次都老实回答
        // {done:true}，步数停在 7，看起来"跑完了"，但调度器里多了一个永远
        // 不会结束的条目。数目才是那条断言。
        CheckEq("H9b", "跑完之后调度器把它摘掉了", std::to_string(scheduler->GetRunningCount()), "0");
        scheduler->TickUpdate(0.1f, 0.1f);
        scheduler->TickEndOfFrame();
        CheckEq("H9c", "之后再怎么 tick 也不会多跑一步", mover_value("routineSteps"), "7");

        // 跑完之后 StopAllCoroutines 必须是安全的（生成器已经释放过一次）。
        comp2->StopAllCoroutines();
        CheckTrue("H10", "跑完之后 StopAllCoroutines 不炸", comp2->HasLiveInstance());
        scheduler->StopAll();

        // 反过来：中途停掉。这是 C++ 侧唯一需要自己释放生成器的路径
        // （调度器 Stop 会直接丢掉协程帧，泵永远到不了自己的出口）。
        CheckTrue("H11", "第二次启动协程", comp2->InvokeMethod("StartMyRoutine"));
        CheckEq("H12", "又同步跑了第一段", mover_value("routineSteps"), "1");
        comp2->StopAllCoroutines();
        CheckEq("H12b", "停的时候调度器里的那条也摘掉了",
                std::to_string(scheduler->GetRunningCount()), "0");
        scheduler->TickUpdate(0.1f, 0.1f);
        scheduler->TickUpdate(0.1f, 0.1f);
        scheduler->TickEndOfFrame();
        CheckEq("H13", "停掉之后不再推进", mover_value("routineSteps"), "1");
        CheckEq("H14", "也不影响实例本身", mover_value("routineHandleOk"), "true");

        // 第三次：走 **JS shim** 停（`this.StopAllCoroutines()` 转发到
        // component）。C++ 直接调只能证明 Behaviour 那半边是对的。
        CheckTrue("H15", "第三次启动协程", comp2->InvokeMethod("StartMyRoutine"));
        CheckEq("H16", "又同步跑了第一段", mover_value("routineSteps"), "1");
        CheckTrue("H17", "StopMyRoutine 送到了 JS 侧", comp2->InvokeMethod("StopMyRoutine"));
        CheckEq("H17b", "shim 转发之后调度器也空了",
                std::to_string(scheduler->GetRunningCount()), "0");
        scheduler->TickUpdate(0.1f, 0.1f);
        scheduler->TickUpdate(0.1f, 0.1f);
        scheduler->TickEndOfFrame();
        CheckEq("H18", "shim 转发之后确实停了", mover_value("routineSteps"), "1");

        // ---- WaitUntil / WaitWhile（带闭包的等待）--------------------------
        //
        // 这一段是整条链唯一能证明"谓词真的被接到每帧轮询上"的地方：
        // shim 的 `g.WaitUntil`、ScriptEnv::GeneratorNext 为 `fn` 建的强引用、
        // ScriptYield 的 kUntil/kWhile、Behaviour 的 pump 转发、调度器的轮询，
        // 少任何一环都是"协程永远挂着"，而且没有任何报错。
        scheduler->StopAll();
        CheckTrue("K0", "InvokeMethod(\"StartUntilRoutine\") 送到了 JS 侧",
                  comp2->InvokeMethod("StartUntilRoutine"));
        CheckEq("K1", "第一段同步跑了（untilSteps=1）", mover_value("untilSteps"), "1");
        CheckEq("K2", "还没有被轮询过", mover_value("untilPolls"), "0");

        scheduler->TickUpdate(0.1f, 0.1f);
        CheckEq("K3", "gateReady=false 时不恢复", mover_value("untilSteps"), "1");
        CheckEq("K4", "但谓词被问了一次", mover_value("untilPolls"), "1");
        scheduler->TickUpdate(0.1f, 0.1f);
        CheckEq("K5", "下一帧仍然不恢复", mover_value("untilSteps"), "1");
        // 互补的一半：只在 yield 那一帧问一次（快照）的实现 polls 恒为 1，
        // 而 K3/K5 在那种实现下也是绿的 —— 只有这里是真的判据。
        CheckEq("K6", "谓词每帧都被重新问（不是快照一次）", mover_value("untilPolls"), "2");

        // gateReady 是 boolean 字段，LiveSetField 按已有槽位的 typeof 强转。
        CheckTrue("K7", "LiveSetField(gateReady=true) 成功",
                  comp2->LiveSetField("gateReady", "true"));
        scheduler->TickUpdate(0.1f, 0.1f);
        CheckEq("K8", "gateReady 翻面后恢复（untilSteps=3）", mover_value("untilSteps"), "3");

        // 现在是 WaitWhile：gateReady 为 true，所以它必须**挡住**。
        scheduler->TickUpdate(0.1f, 0.1f);
        CheckEq("K9", "WaitWhile 在谓词为真时挡住（不是当成 WaitUntil）",
                mover_value("untilSteps"), "3");
        CheckEq("K10", "并且也在每帧轮询", mover_value("untilPolls"), "4");
        CheckTrue("K11", "LiveSetField(gateReady=false) 成功",
                  comp2->LiveSetField("gateReady", "false"));
        scheduler->TickUpdate(0.1f, 0.1f);
        CheckEq("K12", "谓词变假后恢复（untilSteps=7）", mover_value("untilSteps"), "7");
        // 协程体走完了：调度器里不该还留着它。
        CheckEq("K13", "协程结束后台数清零",
                std::to_string(scheduler->GetRunningCount()), "0");

        // ---- `yield return StartCoroutine(other)` ------------------------
        //
        // 唯一能区分"真的在等内层"与"把句柄当成 yield null"的判据是**内层还在
        // 跑的那一帧外层必须停住**：内层两段 yield（跨 3 帧），而"当成 null"
        // 的实现只让外层晚一帧，最后 nestedSteps 照样走到 7。所以 N5 是这组里
        // 唯一的真判据，N7/N8 只是收尾。
        scheduler->StopAll();
        CheckTrue("N0", "InvokeMethod(\"StartNestedRoutine\") 送到了 JS 侧",
                  comp2->InvokeMethod("StartNestedRoutine"));
        CheckEq("N1", "外层第一段同步跑了（nestedSteps=1）", mover_value("nestedSteps"), "1");
        // 内层是在外层执行到那一行时同步 start 的（Unity 同），所以这里已经是 1。
        CheckEq("N2", "内层也同步跑了第一段（nestedInner=1）", mover_value("nestedInner"), "1");
        CheckEq("N3", "两个协程都进了调度器",
                std::to_string(scheduler->GetRunningCount()), "2");

        scheduler->TickUpdate(0.1f, 0.1f);
        CheckEq("N4", "内层推进到第二段（nestedInner=3）", mover_value("nestedInner"), "3");
        CheckEq("N5", "内层还没跑完时外层必须停住（nestedSteps 仍是 1）",
                mover_value("nestedSteps"), "1");

        scheduler->TickUpdate(0.1f, 0.1f);
        CheckEq("N6", "内层跑完（nestedInner=7）", mover_value("nestedInner"), "7");
        // 补一条互补的：调度器的 Stop 发生在 resume 循环**之后**，所以内层跑完
        // 的这一帧外层还醒不过来。缺了这条，"外层在 T2 就恢复"的实现是绿的 ——
        // 那本身无害，但它说明帧序被改动过，N7 的帧数就得跟着改。
        CheckEq("N6b", "内层收尾这一帧外层还没恢复", mover_value("nestedSteps"), "1");

        scheduler->TickUpdate(0.1f, 0.1f);
        CheckEq("N7", "内层结束后外层才继续（nestedSteps=3）", mover_value("nestedSteps"), "3");
        scheduler->TickUpdate(0.1f, 0.1f);
        CheckEq("N8", "外层收尾（nestedSteps=7）", mover_value("nestedSteps"), "7");
        CheckEq("N9", "两个都结束后调度器空了",
                std::to_string(scheduler->GetRunningCount()), "0");

        // ---- Unity 的应用层消息 ------------------------------------------
        //
        // UnityParitySmokeTest 的 AM 组证明的是"C++ 组件收得到"，这一段证明的是
        // "**脚本**收得到" —— 中间隔着 TypeScriptComponent 的五个转发和一个
        // 带 bool 参数的 InvokeInstanceMethod，那一层少任何一个都是静默的。
        scheduler->StopAll();
        CheckEq("P0", "初始没有任何焦点记录", mover_value("focusLog"), "");
        go2->BroadcastAppEvent(AppEventKind::kFocus, true);
        CheckEq("P1", "OnApplicationFocus(true) 到了脚本", mover_value("focusLog"), "T");
        go2->BroadcastAppEvent(AppEventKind::kFocus, false);
        // 两个方向都测：只转发 true 的实现（比如把参数写死）在这里就露了。
        CheckEq("P2", "OnApplicationFocus(false) 也到了（两个方向都要通）",
                mover_value("focusLog"), "TF");
        go2->BroadcastAppEvent(AppEventKind::kPause, true);
        CheckEq("P3", "OnApplicationPause(true) 到了脚本", mover_value("pauseLog"), "T");
        go2->BroadcastAppEvent(AppEventKind::kQuit, false);
        CheckEq("P4", "OnApplicationQuit 到了脚本", mover_value("quitCount"), "1");

        // --- 互补的一半：不判 enabled / activeInHierarchy ------------------
        // "应用要退出了"跟某个对象激不激活无关。这里补上激活判据的实现，
        // 会让被停用的对象存不上档 —— 而且只在"恰好退出时被停用"才复现。
        comp2->SetEnabled(false);
        go2->SetActive(false);
        CheckTrue("P5", "SetActive(false) 真的失活了（否则 P6 是空的）",
                  !go2->IsActiveInHierarchy());
        go2->BroadcastAppEvent(AppEventKind::kQuit, false);
        CheckEq("P6", "停用 + 失活的组件照样收到 OnApplicationQuit",
                mover_value("quitCount"), "2");
        go2->SetActive(true);
        comp2->SetEnabled(true);

        // --- OnTransformParentChanged：真的换父级 --------------------------
        go2->GetTransform()->SetParent(go->GetTransform(), true);
        CheckEq("P7", "改父级之后脚本收到 OnTransformParentChanged",
                mover_value("parentChanged"), "1");
        // 派发点必须在层级更新**之后**：回调里读 `transform.parent` 必须是新的。
        // 早一步派发的话回调照样被调一次（P7 绿），但读到的还是旧父级。
        CheckEq("P7b", "回调里读到的父级已经是新的（不是旧的 null）",
                mover_value("parentWasNullAtCallback"), "false");

        // --- 互补的另一半：OnTransformChildrenChanged ----------------------
        //
        // 与 P7 是同一件事的两半，但**收的人不同**：那一个发给"被搬的对象"
        // （go2），这一个是"我的孩子多了/少了一个"，所以这次必须让 go2 当
        // **父级**。名字只差一个词，转发写反（两个都写成 Parent）在这里是唯一
        // 能看见的地方 —— 写反的话 P7 和 P10 会一起红。
        auto* kid = MakeGameObject(object_manager, "Kid");
        CheckTrue("P10", "Kid 造出来了", kid != nullptr);
        kid->AddComponentByTypeName("Transform");
        CheckTrue("P11", "把 Kid 挂到 go2 下面成功",
                  kid->GetTransform()->SetParent(go2->GetTransform(), true));
        CheckEq("P12", "孩子挂上来时**父级**的脚本收到 OnTransformChildrenChanged",
                mover_value("childrenChanged"), "1");
        // 同一条"派发点必须在层级更新之后"的判据，换到这一半上再验一次：
        // 只看次数的话，早一步派发照样是 1。
        CheckEq("P13", "回调里读到的 childCount 已经是新的（1）",
                mover_value("childCountAtCallback"), "1");
        // 互补的一半：摘走也要发。只做"加"不做"减"的实现在这里才露。
        CheckTrue("P14", "把 Kid 摘走成功", kid->GetTransform()->SetParent(nullptr, true));
        CheckEq("P15", "孩子摘走时旧父级同样收到", mover_value("childrenChanged"), "2");
        CheckEq("P16", "摘走后回调里 childCount 是 0", mover_value("childCountAtCallback"), "0");
        // 收的人必须**不是**被搬的对象自己：把 go2 再搬一次（与 P7 同款操作），
        // 它作为"被搬的那个"不该收到 childrenChanged —— 那一条是发给它父级的。
        // 两个回调合成一条（谁都发）的实现在这里才露。
        CheckTrue("P17", "把 go2 摘回场景根成功", go2->GetTransform()->SetParent(nullptr, true));
        CheckEq("P18", "搬自己不会给自己发 OnTransformChildrenChanged（收的是父级）",
                mover_value("childrenChanged"), "2");
        // 还原 P7 之后的层级：下面几段不依赖它，但保持"P 段结束时 go2 还在
        // go 下面"更好追。
        go2->GetTransform()->SetParent(go->GetTransform(), true);

        // --- OnValidate：编辑器那条路（OnSerializedFieldsUpdated）----------
        //
        // 互补的一对：Player 里（g_isEditor=false）**不该**发，编辑器里该发。
        // 只测"发了"的话，"忘了判 g_isEditor"这种实现是绿的。
        comp2->OnSerializedFieldsUpdated();
        CheckEq("P8", "Player 态下 OnSerializedFieldsUpdated 不发 OnValidate",
                mover_value("validateCount"), "0");
        const bool was_editor = g_isEditor;
        g_isEditor = true;
        comp2->OnSerializedFieldsUpdated();
        CheckEq("P9", "编辑态下它转发成 OnValidate", mover_value("validateCount"), "1");
        g_isEditor = was_editor;

        // --- Unity 的两个 gizmo 消息（R 段）--------------------------------
        //
        // C++ 侧的 DG 组（UnityParitySmokeTest）断言的是引擎自己的组件；这一段
        // 断言的是**脚本**那条链路：`GameObject::BroadcastDrawGizmos` →
        // `TypeScriptComponent::OnDrawGizmos` → JS。少中间任何一环，DG 全绿而
        // 用户写的 `OnDrawGizmos()` 永远不来。
        //
        // 判据是这一段的重点：只判 activeInHierarchy，**不判 enabled**。

        // 未选中：只来常驻的那一条。
        go2->BroadcastDrawGizmos(false);
        CheckEq("R0", "BroadcastDrawGizmos 送到了 JS 的 OnDrawGizmos",
                mover_value("gizmoDraws"), "1");
        CheckEq("R1", "没选中时 OnDrawGizmosSelected 不来",
                mover_value("gizmoSelectedDraws"), "0");
        CheckEq("R2", "回调里 Gizmos 面是可用的（读到的是刚设进去的绿色）",
                mover_value("gizmoAtDraw"), "1");

        // 选中：两个都来，且 Selected 是**额外**的（不是二选一）。
        go2->BroadcastDrawGizmos(true);
        CheckEq("R3", "选中时常驻那条照样来（是叠加，不是替换）",
                mover_value("gizmoDraws"), "2");
        CheckEq("R4", "并且额外来一次 OnDrawGizmosSelected",
                mover_value("gizmoSelectedDraws"), "1");

        // ⚠️ 核心差异：停用**不**挡 gizmo。
        comp2->SetEnabled(false);
        go2->BroadcastDrawGizmos(true);
        CheckEq("R5", "停用的脚本照样收到 OnDrawGizmos（Unity 明文：enabled 只挡 "
                      "Start/Awake/Update/FixedUpdate/OnGUI）",
                mover_value("gizmoDraws"), "3");
        CheckEq("R6", "Selected 那条同样不受 enabled 影响",
                mover_value("gizmoSelectedDraws"), "2");
        comp2->SetEnabled(true);

        // 互补的一半：activeInHierarchy **要**挡。只做上面那对的话，"两个判据
        // 全都不要了"这种实现也是绿的。
        go2->SetActive(false);
        CheckTrue("R7", "go2 真的停用了", !go2->IsActiveInHierarchy());
        go2->BroadcastDrawGizmos(true);
        CheckEq("R8", "停用的对象一个都不发（Selected 也不能覆盖这条判据）",
                mover_value("gizmoDraws"), "3");
        CheckEq("R9", "Selected 那条同样不发",
                mover_value("gizmoSelectedDraws"), "2");
        go2->SetActive(true);

        // --- Unity 的七个鼠标消息（S 段）----------------------------------
        //
        // 与 R 段同一条判据：只判 activeInHierarchy，**不判 enabled**。停用的脚本
        // 照样收到 OnMouse*。
        //
        // ⚠️ 七条各用一个不同的字母，而不是共用一个计数器：共用计数时
        // "OnMouseUp 与 OnMouseUpAsButton 转发到同一个 JS 名字"是绿的（计数照样
        // +2）。逐字母比对是唯一能看见它的判据。
        go2->BroadcastMouseEvent(MouseEventKind::kEnter);
        CheckEq("S0", "BroadcastMouseEvent 送到了 JS 的 OnMouseEnter",
                mover_value("mouseLog"), "E");
        go2->BroadcastMouseEvent(MouseEventKind::kOver);
        CheckEq("S1", "OnMouseOver 是独立的一条", mover_value("mouseLog"), "EO");
        go2->BroadcastMouseEvent(MouseEventKind::kExit);
        CheckEq("S2", "OnMouseExit 是独立的一条", mover_value("mouseLog"), "EOX");

        go2->BroadcastMouseEvent(MouseEventKind::kDown);
        CheckEq("S3", "OnMouseDown 是独立的一条", mover_value("mouseLog"), "EOXD");
        CheckEq("S4", "鼠标回调里 gameObject 注入属性是可读的",
                mover_value("mouseDownHadGameObject"), "true");

        go2->BroadcastMouseEvent(MouseEventKind::kDrag);
        CheckEq("S5", "OnMouseDrag 是独立的一条", mover_value("mouseLog"), "EOXDG");
        go2->BroadcastMouseEvent(MouseEventKind::kUp);
        CheckEq("S6", "OnMouseUp 是独立的一条", mover_value("mouseLog"), "EOXDGU");
        // ⚠️ 这条是**名字前缀**那个坑的正对面：OnMouseUp 是 OnMouseUpAsButton 的
        // 前缀。上面的 S6 已经证明 Up 来了，所以这里必须是 "B" 而不是又一个 "U"。
        go2->BroadcastMouseEvent(MouseEventKind::kUpAsButton);
        CheckEq("S7", "OnMouseUpAsButton 没有被 OnMouseUp 顶掉",
                mover_value("mouseLog"), "EOXDGUB");

        // ⚠️ 核心差异：停用**不**挡鼠标消息（Unity 明文：enabled 只挡
        // Start/Awake/Update/FixedUpdate/OnGUI）。
        comp2->SetEnabled(false);
        go2->BroadcastMouseEvent(MouseEventKind::kEnter);
        CheckEq("S8", "停用的脚本照样收到 OnMouseEnter",
                mover_value("mouseLog"), "EOXDGUBE");
        comp2->SetEnabled(true);

        // 互补的一半：activeInHierarchy **要**挡。只做上面那对的话，"两个判据
        // 全都不要了"这种实现也是绿的。
        go2->SetActive(false);
        CheckTrue("S9", "go2 真的停用了", !go2->IsActiveInHierarchy());
        go2->BroadcastMouseEvent(MouseEventKind::kEnter);
        go2->BroadcastMouseEvent(MouseEventKind::kDown);
        CheckEq("S10", "停用的对象一个都不发", mover_value("mouseLog"), "EOXDGUBE");
        go2->SetActive(true);

        // --- Unity 的 FixedUpdate（T 段）----------------------------------
        //
        // ⚠️ 这一段存在的全部理由是**发布面**：C++ 侧 `TypeScriptComponent::FixedTick`
        // 每个固定步都在调 `OnFixedUpdate`，但它在 d.ts 与 JS 原型里长期缺着 ——
        // 于是 TS 里写 `OnFixedUpdate(...)` **过不了 tsc**，而任何 C++ 测试都发现
        // 不了（`InvokeInstanceMethodNumber` 对"方法不存在"是静默无事发生的）。
        // Mover.ts 里那段 `OnFixedUpdate` 能过 `npx tsc`，才是这条链的判据。
        // ⚠️ 判据走 `go2->` 而不是 `comp2->`：enabled / 层级激活那两道判据活在
        // **GameObject** 的 Tick / FixedTick 里，直接调组件等于绕过它们，
        // T4 / T5 就会变成"永远绿"。
        const bool was_playing = g_isPlaying;
        g_isPlaying = true;
        const float fixed_step = ::Time::GetFixedDeltaTime();
        const std::string fixed_before = mover_value("fixedTicks");
        go2->FixedTick(fixed_step);
        CheckTrue("T0", "FixedTick 送到了 JS 的 OnFixedUpdate",
                  mover_value("fixedTicks") != fixed_before);
        CheckEq("T1", "回调里拿到的是 Time.fixedDeltaTime 本身（不是帧间隔）",
                mover_value("fixedDtMatches"), "true");
        // 同一帧里两个 delta **不是同一个数**：Update 吃帧间隔、FixedUpdate 吃
        // 固定步长。喂一个与固定步长明显不同的帧间隔，两者必须不相等 ——
        // 只数调用次数的话"把帧 delta 传下去"是绿的。
        const float frame_step = (fixed_step > 0.0f ? fixed_step * 0.5f : 0.016f);
        const std::string ticks_before = mover_value("ticks");
        go2->Tick(frame_step);
        CheckTrue("T2", "OnUpdate 也照常来了", mover_value("ticks") != ticks_before);
        CheckTrue("T3", "OnFixedUpdate 的步长与 OnUpdate 的帧间隔不是同一个数",
                  mover_value("lastFixedDt") != mover_value("lastDt"));
        // --- Unity 的 `Quaternion.Euler`（U 段）---------------------------
        //
        // 与 T 段同一类理由：引擎里此前**没有**反向的 Euler(x, y, z)，脚本只能拿
        // `AngleAxis` 自己拼，而拼出来的朝向是不是与 `transform.eulerAngles`
        // 同一套约定，编译期和运行期都看不出来。这一段钉的是"两处同源"。
        CheckTrue("U0", "ProbeQuaternionEuler 送到了 JS 侧",
                  comp2->InvokeMethod("ProbeQuaternionEuler"));
        // 往返：设进去再读回来必须还是 (20, 30, 40)。
        CheckEq("U1", "GetEulerAngles(Euler(v)) == v —— 与 Transform.eulerAngles 同源",
                mover_value("quatRoundTrip"), "20.0,30.0,40.0");
        // 90° 绕 X 把 +Y 抬到 +Z：Vector3.up 是 (0,0,1)，转完应该是 (0,-1,0)。
        CheckEq("U2", "90 度绕 X 把 +Z 转到 -Y（右手系，不是抄实现）",
                mover_value("quatYToZ"), "0,-1,0");

        // --- Unity 的 AnimationCurve（W 段）--------------------------------
        //
        // 与 `Bounds` / `Color32` 那一段（J 组）同一类理由：C++ 侧的算术门
        // （UnityParitySmokeTest 的 AC 组）证明不了 **tsc 认这两个名字** ——
        // `declare interface AnimationCurve` 与 `declare class AnimationCurve`
        // 的同名合并只在真正的类型检查器下才算成立。
        //
        // 一条串钉住六个量：两个工厂的形状、值语义的 AddKey、wrap 模式、以及
        // `WrapMode.Loop` 这个**枚举成员**（不是裸数字）。
        CheckTrue("W0", "ProbeAnimationCurve 送到了 JS 侧",
                  comp2->InvokeMethod("ProbeAnimationCurve"));
        CheckEq("W1", "曲线端到端：Linear / EaseInOut / AddKey / Loop / 枚举成员",
                mover_value("curveProbe"), "5.000|0.15625|2|2.000|5.000|true");

        // --- Unity 的 Vector2Int / Vector3Int / Rect / RectInt（X 段）-------
        //
        // 与 W 段同一类理由：C++ 的算术门（UnityParitySmokeTest 的 IV / RC 组）
        // 证明不了 **tsc 认这四个名字**。它们都是"值类型 interface + 同名静态
        // class"的合并 —— 少了 `declare interface`、少了 `Converter`、或者
        // ZCLASS 的类名对不上，都只在真正的类型检查器下才报错。
        CheckTrue("X0", "ProbeIntVectors 送到了 JS 侧",
                  comp2->InvokeMethod("ProbeIntVectors"));
        // ⚠️ `-1`（floor，不是截断的 0）与 `2`（平局取偶，不是 3）是这一段真正的
        // 判据 —— 换成正数或换成 0.5 都测不出这两条。
        CheckEq("X1", "整数向量端到端：floor / 平局取偶 / 常量 / Distance",
                mover_value("intVecProbe"), "-1,1|2,4|0,1,1|1.73205|1|14");
        CheckTrue("X2", "ProbeRects 送到了 JS 侧", comp2->InvokeMethod("ProbeRects"));
        // ⚠️ 三处反直觉各占一位：半开区间（false|true）、负 width 归一化（0）、
        // 贴边不算相交（末位 false）。
        CheckEq("X3", "矩形端到端：半开 Contains / 负 width / ClampToBounds / 严格 Overlaps",
                mover_value("rectProbe"), "false|true|0|0.25,0.25|7|1.5,2.5|false");

        // --- Unity 的 Plane / GeometryUtility（Y 段）-----------------------
        //
        // 与 X 段同一类理由：`Plane.h` / `Frustum.h` 在 C++ 里一直都在，但脚本
        // 侧零命中。这里证明 `Plane` 既是 `{normal, distance}` 值、又挂着静态
        // 方法（同名合并），`GeometryUtility` 在 tsc 下存在，且 `Plane[]` 能当
        // 参数传进去。
        CheckTrue("Y0", "ProbePlanes 送到了 JS 侧", comp2->InvokeMethod("ProbePlanes"));
        // ⚠️ 三个判据各占一位：`distance = -3`（符号反了整个视锥内外反过来）、
        // `4`（正面命中的 t）、`-1`（背面**不**命中）；末两位的 true/false 是
        // "六个面都判了"。
        CheckEq("Y1", "平面端到端：distance 符号 / Raycast 只打正面 / TestPlanesAABB",
                mover_value("planeProbe"), "-3|2|4|-1|5|true|false");

        // --- Unity 的 SmoothDamp（Z 段）-------------------------------------
        //
        // ⚠️ 这一段是**唯一**能看见"结构体返回值跨边界"的地方。C++ 的
        // `TestSmoothDamp`（UnityParitySmokeTest 的 SD 组）直接调 `Mathf::SmoothDamp`，
        // 压根不经过 `Converter<SmoothDampResult>`；少了那个 Converter，TS 侧
        // 拿到 `undefined`，`.value` 也是 `undefined`，`String(undefined)` 照样
        // 能拼进探针字符串 —— 只有这里会红。
        CheckTrue("Z0", "ProbeSmoothDamp 送到了 JS 侧", comp2->InvokeMethod("ProbeSmoothDamp"));
        // ⚠️ 四个判据各占一位：0.002061（Unity 的多项式，不是 std::exp 的
        // 0.002126）、0.001031 vs 2.061（限速上限是 maxSpeed*smoothTime，不是
        // *dt）、350.041（跨 0 走短弧，往上是 +20 不是 -340）、末位 0（已经在
        // 目标上的分量不动）。
        CheckEq("Z1", "SmoothDamp 端到端：多项式 exp / 限速 / 最短弧 / 分量式",
                mover_value("smoothDampProbe"),
                "0.002061|0.249485|0.001031|2.061|350.041|0.001031|0.001031|0");

        // --- Unity 的 LayerMask（LM 段）-------------------------------------
        //
        // C++ 那侧 `TagLayerManager` 与 `Physics.LayerToName` 一直都在，缺的正是
        // "tsc 认 `LayerMask` 这个名字" —— 而方法不存在在运行时是静默的。
        CheckTrue("Z2", "ProbeLayerMask 送到了 JS 侧", comp2->InvokeMethod("ProbeLayerMask"));
        // ⚠️ `-1`（不是 0）、`32`（未知层名跳过，不是 -1 = 所有层）是这一段真正的判据。
        CheckEq("Z3", "LayerMask 端到端：NameToLayer / LayerToName / GetMask / MaskContainsLayer",
                mover_value("layerMaskProbe"), "0|5|-1|UI|33|32|true|false");

        // --- Unity 的 Vector3 静态面（VX 段）-------------------------------
        //
        // 四个算法本来就在 `VectorMath` 上，缺的是 Unity 的拼写。这里同时证明
        // `Slerp` 与 `Lerp` **不是**同一个函数（长度 1.0000 vs 0.7071）。
        CheckTrue("Z4", "ProbeVector3Statics 送到了 JS 侧",
                  comp2->InvokeMethod("ProbeVector3Statics"));
        CheckEq("Z5", "Vector3 静态面端到端：Slerp 长度 / 非单位法线 / SignedAngle 符号",
                mover_value("vecStaticsProbe"),
                "0.7071|1.0000|0.7071|0.7071|0.0000|90.000|-90.000|0.13477");

        // --- Unity 的 Color 补齐面 + ColorUtility（CX 段）-------------------
        //
        // 与 TE 段同一类理由：一半判据在 tsc 那一步就走掉了（少 ZFUNCTION /
        // ZSCRIPTPROPERTY 就过不了类型检查），运行时这边抓的是"值" —— 尤其是
        // `80` vs `7F`（四舍五入 vs 截断）和 `#FFF` 必须是纯白这两条。
        CheckTrue("Z8", "ProbeColorExtras 送到了 JS 侧",
                  comp2->InvokeMethod("ProbeColorExtras"));
        CheckEq("Z9", "Color 补齐面端到端：grayscale / HTML 串 / 解析 / 线性",
                mover_value("colorExtrasProbe"),
                "0.299|0.587|0.114|0.9|808080|FF000040|FF0000|"
                "RGBA(1.000, 0.000, 0.000, 1.000)|0.214|0.500|"
                "true|true|true|FF0000FF|false|true|0.000|true");

        // --- SystemInfo / QualitySettings / Cursor / Time 补项 / Application 补项
        //
        // 判据分两处：tsc 那一步抓"名字和类型在不在"（少一个 declare class 就过
        // 不了类型检查），这里抓"值对不对" —— 尤其是 headless 下的**哨兵值**
        // 与"离散合法集向下取"这两条，它们在真机上根本走不到。
        CheckTrue("ZA0", "ProbePlatformFacades 送到了 JS 侧",
                  comp2->InvokeMethod("ProbePlatformFacades"));
        CheckEq("ZA1", "平台门面端到端：SystemInfo / QualitySettings / Cursor / Time / Application",
                mover_value("platformProbe"),
                // 逐段的含义（顺序与 Mover.ts 的 platformProbe 一致）：
                //   1-2   DeviceType.Desktop(3) / OperatingSystemFamily.Windows(2)
                //   3-4   逻辑处理器数 > 0、机器名非空
                //   5-9   headless 哨兵：显存 0、着色器等级 0、电量 -1、
                //         电池 Unknown(0)、supportsInstancing = false
                //   10-21 QualitySettings：默认档 2 / "Medium" / 6 档 /
                //         names[0] = "Very Low" / 切到 4 档后 MSAA 8、阴影 150 /
                //         越界 99 不动 / MSAA 3 -> 2（向下取）/ mip 上限 7 -> 3 /
                //         复位回 2
                //   22-26 Cursor：默认 true|None(0)，设完 false|Locked(1)，复位回来
                //   27-29 Time：maximumDeltaTime 1/3、captureFramerate 0、
                //         timeSinceLevelLoad >= 0
                //   30-37 Application：identifier 以 com. 开头、非移动非主机、
                //         有焦点、语种是数字、两条 URL 都被拒、优先级 High(3)
                "3|2|true|true|true|true|true|true|true|"
                "2|Medium|6|6|Very Low|4|8|150.0|4|2|3|2|"
                "true|0|false|1|true|0.3333|0|true|"
                "true|true|true|true|true|true|true|3");

        // --- Unity 的 Input 面（IP 段）--------------------------------------
        //
        // 与 ZA1 同一条路：tsc 抓"名字与类型在不在"，这里抓"值对不对"。这个进程
        // 没有 InputSystem，所以整串都是"安全默认值 + 可写设置回环" —— 真正要验
        // 的是 `Touch` 这个**值类型**被打包成 JS 对象时字段有没有对上（对不上时
        // TS 侧读到 undefined，而不是报错，所以光有 tsc 抓不到）。
        CheckTrue("ZA2", "ProbePlatformFacades 里那段 Input 探针跑到了",
                  comp2->InvokeMethod("ProbePlatformFacades"));
        CheckEq("ZA3", "Unity 的 Input 端到端：默认值 / Touch 值类型 / 设置回环",
                mover_value("inputProbe"),
                "true|true|true|true|true|true|true|true|true|true|"
                "true|true|true|true|true|true|true|true|true|true|"
                "true|true|true|true|true|true|true|true");
        CheckEq("ZA4", "Unity 的 Material / Light 发布面：类与方法确实进了绑定表",
                mover_value("materialLightProbe"),
                "true|true|true|true|true|true");
        CheckEq("ZA5", "Unity 的 Mesh / MeshFilter 发布面：数组返回类型也挂上了",
                mover_value("meshProbe"),
                "true|true|true|true|true|true|true");
        CheckEq("ZA6", "Unity 的 Animator 其余面：speed / Rebind / 控制器形状都进了绑定表",
                mover_value("animatorProbe"),
                "true|true|true|true|true|true");
        CheckEq("ZA7", "Unity 的 Rigidbody 其余面：constraints / 质心 / CCD / 点速度都进了绑定表",
                mover_value("rigidbodyProbe"),
                "true|true|true|true|true|true|true|true|true|true|true");
        CheckEq("ZA8", "Unity 的 Joint 一族：四个子类 + 基类门面 + 三个值类型都进了绑定表",
                mover_value("jointProbe"),
                "true|true|true|true|true|true|true|true");

        // --- Unity 的 Transform 补齐面（TE 段）------------------------------
        //
        // 这一段的判据在 tsc 那一步就已经走掉一半了：ZSCRIPTPROPERTY 少一条，
        // Mover.ts 里 `t.hasChanged` 过不了类型检查。运行时这边再抓 tsc 抓不到
        // 的 —— 属性与方法是不是同一个东西、矩阵属性和 TransformPoint 一不一致。
        CheckTrue("Z6", "ProbeTransformExtras 送到了 JS 侧",
                  comp2->InvokeMethod("ProbeTransformExtras"));
        CheckEq("Z7", "Transform 补齐面端到端：hasChanged / hierarchy* / 矩阵属性",
                mover_value("transformExtrasProbe"),
                "false|true|false|true|true|2|true|true|11.00,12.00,13.00");

        // --- Unity 的 OnBecameVisible / OnBecameInvisible（V 段）------------
        //
        // 与 T / U 段同一类理由：派发链是 `BaseRenderer` -> `GameObject` ->
        // `TypeScriptComponent` -> JS，任何一环断了的表现都只是"什么都不发生"。
        // 这里端到端走一遍，用 visibilityLog 里**字母的顺序**判方向 —— 合成一个
        // 计数器的话"进和出转发到了同一个 JS 名字"是绿的。
        {
            GameObject* camera_host = MakeGameObject(object_manager, "VisCameraHost");
            CheckTrue("V0", "可见性测试的相机宿主造出来了", camera_host != nullptr);
            if (camera_host != nullptr)
            {
                Transform* camera_transform = MakeComponent<Transform>(object_manager);
                camera_host->addComponent(camera_transform);
                camera_transform->PostLoadResource(camera_host);
                CameraComponent* camera = MakeComponent<CameraComponent>(object_manager);
                camera_host->addComponent(camera);
                camera->PostLoadResource(camera_host);
                camera->SetNearClipPlane(0.1f);
                camera->SetFarClipPlane(100.f);

                BaseRenderer* renderer_v = MakeComponent<BaseRenderer>(object_manager);
                go2->addComponent(renderer_v);
                renderer_v->PostLoadResource(go2);
                renderer_v->SetLocalBounds(
                    Bounds::FromCenterSize(Vector3::ZERO, Vector3(2.f, 2.f, 2.f)));

                // 相机默认在原点朝 -Y；把对象挪到正前方 10 米。
                go2->GetTransform()->SetPosition(Vector3(0.f, -10.f, 0.f));
                BaseRenderer::UpdateVisibilityForAll();
                CheckEq("V1", "进入视锥：OnBecameVisible 到了 JS 侧",
                        mover_value("visibilityLog"), "+");
                // 再刷一帧：不该再来（是"变化"事件，不是每帧事件）。
                BaseRenderer::UpdateVisibilityForAll();
                CheckEq("V2", "第二帧什么都不发（只在变化那一帧来一次）",
                        mover_value("visibilityLog"), "+");
                // 挪到相机背后 ⇒ 离开视锥。
                go2->GetTransform()->SetPosition(Vector3(0.f, 10.f, 0.f));
                BaseRenderer::UpdateVisibilityForAll();
                CheckEq("V3", "离开视锥：OnBecameInvisible 到了 JS 侧（方向没丢）",
                        mover_value("visibilityLog"), "+-");

                // 收尾：可见性是**全局**扫描（遍历所有渲染器 × 所有相机），
                // 所以这台临时相机和这个临时渲染器必须真的从两张表里摘掉，
                // 否则后面任何一组只要再调一次 UpdateVisibilityForAll 就会
                // 撞见它们 —— 而"多了一台相机"的表现只是"多一个对象突然
                // 变可见了"，跟这一段毫无关系，极难往回追。
                // 顺序：先从宿主上摘（宿主不拥有组件），再逐个 Destroy。
                go2->EditorRemoveComponent(renderer_v);
                object_manager->Destroy(renderer_v);
                object_manager->Destroy(camera_host);
                object_manager->Destroy(camera);
                object_manager->Destroy(camera_transform);
                go2->GetTransform()->SetPosition(Vector3::ZERO);
            }
        }
        // ⚠️ 与 Tick 同一套判据：enabled / activeInHierarchy 都要挡。
        // 只测"发了"的话，"FixedUpdate 不判 enabled"这种实现是绿的 —— 而它正是
        // "Update 停了但物理还在推"那一类 bug。
        comp2->SetEnabled(false);
        go2->FixedTick(fixed_step);
        CheckEq("T4", "停用的脚本不收 OnFixedUpdate",
                mover_value("fixedTicks"), "1");
        comp2->SetEnabled(true);
        go2->SetActive(false);
        go2->FixedTick(fixed_step);
        CheckEq("T5", "停用的对象同样不收", mover_value("fixedTicks"), "1");
        go2->SetActive(true);
        go2->FixedTick(fixed_step);
        CheckEq("T6", "恢复之后照样来（不是一次性熔断）",
                mover_value("fixedTicks"), "2");
        // ⚠️ 互补的一半：上面几条是在 `g_isPlaying = true` 下过的，所以必须证明
        // 它们**真的是从那条路进来的** —— 还原之后同样的调用必须什么都不发。
        // 少了这一条，T0 在"FixedTick 压根没接进 GameObject"时也是绿的。
        g_isPlaying = was_playing;
        go2->FixedTick(fixed_step);
        CheckEq("T7", "非播放态下不走白名单就什么都不发（T0 是从那条路来的）",
                mover_value("fixedTicks"), "2");

        // ---- 值类型的静态门面（Vector2 / Vector4 / Matrix4x4）------------
        //
        // C++ 侧的 VT 组（UnityParitySmokeTest）断言的是**算术**；这一段断言的是
        // "脚本真的拿得到" —— 中间隔着 `declare interface X` 与 `declare class X`
        // 的同名合并。合并失败时 tsc 是红的（Mover.ts 编不过），但**只有**在一个
        // 真的类型检查器跑过之后才会知道；这里读回来的快照是对那条链的运行时确认。
        std::printf("\n--- 值类型静态门面（TypeScript -> Vector2/Vector4/Matrix4x4）---\n");
        CheckTrue("Q0", "ProbeValueTypes 送到了 JS 侧", comp2->InvokeMethod("ProbeValueTypes"));
        // 一条串钉住 16 个值。拆成 16 条 CheckEq 读起来更友好，但那会让
        // "其中一个错了"变成"一次只能看见一个" —— 而且 mover_value 每次都要
        // 重新枚举字段。这里要的是"全对"，所以合成一条。
        CheckEq("Q1", "Vector2/Vector4/Matrix4x4 的静态面在 TS 里全对",
                mover_value("vtSnapshot"),
                "00|11|10|01|5.000|90.0|-90.0|10.0|20.0|5.000|0.800|234|111|true|false|323");
        // 跨平面那条：Vector2.right = +X，Vector3.right = -X。被"统一"成一个符号
        // 时 Q1 里那两个值也会跟着变，但那要读到第 3 段才看得出来 —— 单独拎一条。
        CheckEq("Q2", "Vector2.right 是 +X 而 Vector3.right 是 -X（不同平面，故意的）",
                mover_value("vtAxis"), "1/-1");

        scheduler->StopAll();
    }

    // ---- F3. Gizmos：从 TypeScript 里画 -----------------------------------
    //
    // UnityParitySmokeTest 的 GZ 组断言"画出了什么"（从 C++ 直接调），
    // ScriptVMSmokeTest 的 L 组断言"脚本看不看得见 Gizmos"（从 JS 直接调），
    // 只有这一段同时经过 **tsc 的类型检查** 和 **引擎后端**：`Mover.ts` 里那
    // 三行 `Gizmos.*` 编译不过的话，整个 TS 链是红的。
    std::printf("\n--- F3. Gizmos（TypeScript -> DebugDraw）---\n");
    auto* gizmo_group = GET_SYSTEM(DebugDrawManager)
                            ? GET_SYSTEM(DebugDrawManager)->TryGetOrCreateDebugDrawGroup("gizmos")
                            : nullptr;
#if defined(Z_HAS_VULKAN)
    CheckTrue("I0", "gizmo group 拿得到", gizmo_group != nullptr);
#else
    // 非 Vulkan 构建下 DebugDrawManager 是空实现（见 DebugDrawManager.cpp 顶部），
    // 拿不到 group 是这一层的正确行为。
    CheckTrue("I0", "非 Vulkan 构建：gizmo 后端被编译掉了", gizmo_group == nullptr);
#endif
    if (gizmo_group != nullptr)
    {
        gizmo_group->ClearData();
        // 发布面在这个 env 里也得在：绑定表装全局是 ScriptEnv 的事，而
        // `InvokeMethod` 对不存在的方法也返回 true（见 J 组），所以"真的调到了"
        // 只能靠下面 I2 的计数或这一条证明。
        CheckEq("I0b", "Gizmos 在这个 env 里是可见的", EvalToResult(*sm->GetEnv(), "globalThis.__zResult = typeof Gizmos;"),
                "function");
        CheckTrue("I1", "DrawGizmo 送到了 JS 侧", comp2->InvokeMethod("DrawGizmo"));
        // 1 条线 + 视锥 12 条 = 13。用视锥而不是再画一条线，是因为 12 是个
        // **有结构**的数：少一个循环（近/远/侧棱）就变成 9 或 8，而"1 条线"
        // 那种样本看不出少画了哪一组。
        CheckEq("I2", "一条线 + 一个视锥（12 条）+ 一个球（后端真的收到了）",
                std::to_string(gizmo_group->GetLineCount(false)) + "/" +
                    std::to_string(gizmo_group->GetUniformDynamicDataCount()),
                "13/1");
        gizmo_group->ClearData();
    }
    else
    {
        // 没有后端时也不能炸 —— 走到这里本身就是证明。
        CheckTrue("I1", "没有后端时 DrawGizmo 仍然是 no-op 而不是崩",
                  comp2->InvokeMethod("DrawGizmo"));
    }

    // ---- F4. Bounds + Color32：从 TypeScript 里用 -------------------------
    //
    // 同 F3 的道理：C++ 的 BC 组断言算术，ScriptVMSmokeTest 的 M 组断言发布面，
    // 只有这一段同时经过 **tsc 的类型检查** 和 **引擎**。尤其 `declare
    // interface Bounds` 与 `declare class Bounds` 的同名合并 —— 那是一个真正的
    // 类型检查器才验得了的性质（两段字符串各自都对，合起来可能重复声明）。
    std::printf("\n--- F4. Bounds + Color32（TypeScript -> 引擎）---\n");
    {
        auto mover_field = [&](const char* key) {
            for (const auto& entry : comp2->EnumerateLiveFields())
            {
                if (std::get<0>(entry) == key)
                {
                    return std::get<2>(entry);
                }
            }
            return std::string("<missing>");
        };

        CheckTrue("J1", "InvokeMethod(\"MeasureBounds\") 送到了 JS 侧",
                  comp2->InvokeMethod("MeasureBounds"));
        // 6,8,10 == Expand(2) 之后的新 size（旧的是 4,6,8）—— Unity 的 Expand
        // 先把 amount 除以 2，写成"加 2 到 extents"会得到 8,10,12。
        CheckEq("J2", "Bounds.Expand(2) 让 size 增大 2；Color32.FromColor 是字节；"
                      "Contains 命中；null 射线是不命中",
                mover_field("measured"), "6,8,10|255,0,0,255|true|-1");
    }

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
    // 4 = C3 组里 3 次（enabled=false / SetActive(false) / enabled=false）
    //   + 热重载那次 TearDown 的 1 次。这条是"销毁前的 OnDisable"的基线。
    CheckEq("G1b", "销毁前已经发过 4 次 OnDisable", ReadDisableCount(*sm->GetEnv()), "4");

    comp->SetScriptGuid(eastl::string(""));
    CheckTrue("G2", "解绑后不再有活实例", !comp->HasLiveInstance());
    CheckEq("G3", "解绑把 HostedMover 的 OnDestroy 补上了", ReadDestroyCount(*sm->GetEnv()), "2");
    // Unity 的顺序是 OnDisable -> OnDestroy。少了前者，任何"在 OnDisable 里退订
    // / 还池"的脚本都会漏，而它不报错、不崩溃，只是资源慢慢攒着。
    CheckEq("G3b", "解绑前先发了 OnDisable（顺序是 OnDisable -> OnDestroy）",
            ReadDisableCount(*sm->GetEnv()), "5");

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
