// ==========================================================================
// TypeScript 全链路冒烟测试：.ts -> tsc -> .js -> ScriptEnv -> C++
//
// 为什么需要它
// ------------
// ScriptVMSmokeTest 证明了"pesapi 绑定链能跑"，但它喂给 ScriptEnv 的是**手写**
// 的 CommonJS。用户真正走的那条路一次都没跑过：
//
//     写 .ts  ->  tsc 按项目 tsconfig 编译  ->  产物 .js
//             ->  ScriptEnv::LoadModule     ->  new 出来  ->  每帧 OnUpdate
//
// 这条路上真正会出问题的三件事，编译期一个都看不出来：
//
//   1. tsc 的 CommonJS 产物（`Object.defineProperty(exports, "__esModule", ...)`
//      + ES2020 原生 class）能不能被 LoadModuleInternal 的 IIFE 包装正确执行；
//   2. `class Mover extends Behaviour` 在 target=es2020 下是**原生 class 继承**，
//      原型链能不能被 `__zNewInstance` 正确 new 出来 —— `instanceof Behaviour`
//      必须为真，否则 TypeScriptComponent 注入的 gameObject / transform 会落在
//      一个没有原型的对象上；
//   3. 生成的 zengine.d.ts 到底能不能被真正的类型检查器接受。此前对这个文件只有
//      字符串形式的断言（ReflectionCodegenSmokeTest），从没交给 tsc。
//
// 两件工具各管一半
// ----------------
//   * 本测试：吃 tsc 的产物，验证"跑得起来 + 值对"。不需要 node。
//   * scripts/check_ts_chain.py：重新生成 d.ts -> 用 tsc 编译 -> 与签入的
//     TscOutput/ diff -> 再跑本测试。它证明"产物没漂移"，需要 node。
//
// 用法
// ----
//   ScriptTsChainSmokeTest                      跑断言（js root 用编译期定义）
//   ScriptTsChainSmokeTest --js-root <dir>      指定 js root（脚本用）
//   ScriptTsChainSmokeTest --dump-dts <file>    只写出 zengine.d.ts 然后退出
// ==========================================================================

#if !PAPI_QUICKJS
#error "ScriptTsChainSmokeTest 需要 QuickJS 后端（PAPI_QUICKJS）。"
#endif

#include "Runtime/Scripting/Backends/QuickJS/BackendQuickJS.h"
#include "Runtime/Scripting/Binding/ScriptBinding.h"
#include "Runtime/Scripting/Native/PuertsNative.h"
#include "Runtime/Scripting/Wrapper/ScriptEnv.h"

#include "Runtime/Core/Time/TimeState.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifndef ZENGINE_TS_CHAIN_TSCOUT_DIR
#define ZENGINE_TS_CHAIN_TSCOUT_DIR ""
#endif

namespace
{
    int g_Checks = 0;
    int g_Failed = 0;

    void Report(const char* id, bool ok, const std::string& detail)
    {
        ++g_Checks;
        if (!ok)
        {
            ++g_Failed;
            std::printf("  [FAIL] %-5s %s\n", id, detail.c_str());
        }
        else
        {
            std::printf("  [ ok ] %-5s %s\n", id, detail.c_str());
        }
    }

    void CheckTrue(const char* id, const char* what, bool cond)
    {
        std::string d = what;
        if (!cond)
        {
            d += "  (期望 true，实际 false)";
        }
        Report(id, cond, d);
    }

    void CheckEq(const char* id, const char* what, const std::string& got, const std::string& want)
    {
        std::string d = what;
        d += "  got='";
        d += got;
        d += "'";
        if (got != want)
        {
            d += " want='";
            d += want;
            d += "'";
        }
        Report(id, got == want, d);
    }

    /// 从 EnumerateInstanceFields 的结果里取一个字段的值字符串。
    bool FindField(const std::vector<std::tuple<std::string, std::string, std::string>>& fields,
                   const std::string& name,
                   std::string* type_out,
                   std::string* value_out)
    {
        for (const auto& f : fields)
        {
            if (std::get<0>(f) == name)
            {
                if (type_out)
                {
                    *type_out = std::get<1>(f);
                }
                if (value_out)
                {
                    *value_out = std::get<2>(f);
                }
                return true;
            }
        }
        return false;
    }

    bool HasField(const std::vector<std::tuple<std::string, std::string, std::string>>& fields,
                  const std::string& name)
    {
        return FindField(fields, name, nullptr, nullptr);
    }
}  // namespace

int main(int argc, char** argv)
{
    // -----------------------------------------------------------------------
    // 模式 1：把 zengine.d.ts 写出来。scripts/check_ts_chain.py 用它拿到"当前"
    // 这份声明文件，再交给 tsc —— 编辑器之外唯一能复现同一份内容的入口。
    // -----------------------------------------------------------------------
    for (int i = 1; i + 1 < argc; ++i)
    {
        if (std::string(argv[i]) == "--dump-dts")
        {
            const std::filesystem::path out = argv[i + 1];
            std::error_code ec;
            std::filesystem::create_directories(out.parent_path(), ec);
            std::ofstream f(out, std::ios::binary | std::ios::trunc);
            if (!f)
            {
                std::printf("[dump-dts] 打不开 %s\n", out.string().c_str());
                return 2;
            }
            const std::string dts = ScriptBindingRegistry::BuildTypeScriptDeclarationFile();
            f.write(dts.data(), static_cast<std::streamsize>(dts.size()));
            if (!f.good())
            {
                std::printf("[dump-dts] 写失败 %s\n", out.string().c_str());
                return 2;
            }
            std::printf("[dump-dts] %s (%zu 字节)\n", out.string().c_str(), dts.size());
            return 0;
        }
    }

    std::string js_root = ZENGINE_TS_CHAIN_TSCOUT_DIR;
    for (int i = 1; i + 1 < argc; ++i)
    {
        if (std::string(argv[i]) == "--js-root")
        {
            js_root = argv[i + 1];
        }
    }
    if (js_root.empty())
    {
        std::printf("js root 未指定：编译期没给 ZENGINE_TS_CHAIN_TSCOUT_DIR，"
                    "也没有 --js-root。\n");
        return 2;
    }

    std::printf("=== TypeScript 全链路冒烟测试 ===\n");
    std::printf("js root: %s\n\n", js_root.c_str());

    CheckTrue("T0", "tsc 产物目录里有 Mover.js",
              std::filesystem::exists(std::filesystem::path(js_root) / "Mover.js"));

    BackendQuickJS backend;
    ScriptEnv script_env(&backend);
    script_env.SetJsRoot(js_root);

    std::printf("\n--- A. tsc 产物能被加载器吃下 ---\n");

    CheckTrue("T1", "LoadModule('Mover') 成功（IIFE 包装 + exports 正常）",
              script_env.LoadModule("Mover"));
    CheckTrue("T2", "IsModuleLoaded('Mover') 为真", script_env.IsModuleLoaded("Mover"));

    pesapi_value_ref mover = script_env.CreateInstance("Mover", "Mover");
    CheckTrue("T3", "CreateInstance('Mover','Mover') 拿到实例"
                    "（ES2020 原生 class 能被 __zNewInstance new 出来）",
              mover != nullptr);

    if (mover == nullptr)
    {
        std::printf("\n========================================\n");
        std::printf("总计 %d 项检查，失败 %d 项\n", g_Checks, g_Failed);
        std::printf("========================================\n");
        return 1;
    }

    std::printf("\n--- B. 原型链：extends Behaviour 真的接上了 ---\n");

    CheckTrue("T4", "OnAwake 调用成功", script_env.InvokeInstanceMethod(mover, "OnAwake"));
    CheckTrue("T5", "OnStart 调用成功", script_env.InvokeInstanceMethod(mover, "OnStart"));

    {
        // 这两个字段由 Mover 自己写：instanceof 与"原型上继承来的方法在不在"。
        // instanceof 为假意味着 __extends 没把原型链接好 —— 那样 TypeScriptComponent
        // 注入的 gameObject / transform 会落在一个没有原型的对象上。
        auto fields = script_env.EnumerateInstanceFields(mover);
        std::string v;
        CheckTrue("T6", "字段 isBehaviour 存在", FindField(fields, "isBehaviour", nullptr, &v));
        CheckEq("T7", "this instanceof Behaviour 为真", v, "true");
        CheckTrue("T8", "字段 inheritedHook 存在", FindField(fields, "inheritedHook", nullptr, &v));
        CheckEq("T9", "Behaviour.prototype 上的 OnDestroy 被继承到了", v, "function");
        CheckTrue("T10", "字段 hasGameObject 存在", FindField(fields, "hasGameObject", nullptr, &v));
        CheckEq("T11", "未注入时 this.gameObject 是 null（注入是 TypeScriptComponent 的活）",
                v, "false");
    }

    CheckTrue("T12", "调用继承来的默认 OnDestroy 是 no-op 成功",
              script_env.InvokeInstanceMethod(mover, "OnDestroy"));

    std::printf("\n--- C. 每帧 OnUpdate 穿过 C++ 绑定 ---\n");

    // 脚本里读的是 Time.GetDeltaTime()，所以这里推一帧把值定下来。
    TimeState::Get().BeginFrame(0.016f, 0.02f, true);

    CheckTrue("T13", "OnUpdate(0.5) 第一帧", script_env.InvokeInstanceMethodNumber(mover, "OnUpdate", 0.5));
    CheckTrue("T14", "OnUpdate(0.5) 第二帧", script_env.InvokeInstanceMethodNumber(mover, "OnUpdate", 0.5));
    CheckTrue("T15", "snapshot() 调用成功", script_env.InvokeInstanceMethod(mover, "snapshot"));

    {
        auto fields = script_env.EnumerateInstanceFields(mover);
        std::string v;
        CheckTrue("T16", "字段 lastSnapshot 存在", FindField(fields, "lastSnapshot", nullptr, &v));
        // origin.x | ticks | lastDt | sawKeyDown | Time.GetDeltaTime()
        CheckEq("T17", "两帧后 origin.x=2 / ticks=2 / dt=0.5 / 无按键 / deltaTime=0.016",
                v, "2.0000,2,0.5,false,0.0160");
        std::string ticks_type;
        FindField(fields, "ticks", &ticks_type, nullptr);
        CheckEq("T18", "ticks 是 number（字段桥的 typeof 判定没错位）", ticks_type, "number");
    }

    std::printf("\n--- D. C++ 改字段 -> 脚本下一帧生效 ---\n");

    CheckTrue("T19", "ApplyInstanceField(speed='10') 成功",
              script_env.ApplyInstanceField(mover, "speed", "10"));
    CheckTrue("T20", "OnUpdate(0.5) 第三帧", script_env.InvokeInstanceMethodNumber(mover, "OnUpdate", 0.5));
    CheckTrue("T21", "snapshot() 再调一次", script_env.InvokeInstanceMethod(mover, "snapshot"));
    {
        auto fields = script_env.EnumerateInstanceFields(mover);
        std::string v;
        FindField(fields, "lastSnapshot", nullptr, &v);
        CheckEq("T22", "speed=10 后这一帧走了 5（2 -> 7），ticks=3", v, "7.0000,3,0.5,false,0.0160");
        std::string speed_value;
        FindField(fields, "speed", nullptr, &speed_value);
        CheckEq("T23", "speed 字段被改成 10", speed_value, "10");
    }

    std::printf("\n--- E. 字段枚举的边界 ---\n");

    {
        auto fields = script_env.EnumerateInstanceFields(mover);
        CheckTrue("T24", "原型方法不被当成字段（OnUpdate / snapshot 都不该出现）",
                  !HasField(fields, "OnUpdate") && !HasField(fields, "snapshot"));
        CheckTrue("T25", "字段初始值都还在（speed/origin/ticks/lastDt/lastSnapshot）",
                  HasField(fields, "speed") && HasField(fields, "origin") &&
                      HasField(fields, "ticks") && HasField(fields, "lastDt") &&
                      HasField(fields, "lastSnapshot"));
    }

    std::printf("\n--- F. 抛异常的模块不能留在缓存里 ---\n");

    // Scripts/Broken.ts 在模块求值阶段就 throw。它钉住的是：LoadModule 返回
    // false 之后，这个半成品**不能**被当成已加载留在缓存里 —— 否则改好源码再热
    // 重载，拿到的还是那次抛异常留下的空壳。
    CheckTrue("T26", "抛异常的模块 LoadModule 返回 false",
              !script_env.LoadModule("Broken"));
    CheckTrue("T27", "抛异常的模块不会被留在缓存里", !script_env.IsModuleLoaded("Broken"));
    CheckTrue("T28", "再 LoadModule 一次仍然失败（不是命中缓存返回 true）",
              !script_env.LoadModule("Broken"));

    std::printf("\n--- G. 模块卸载 ---\n");

    script_env.DestroyInstance(mover);
    script_env.UnloadModule("Mover");
    CheckTrue("T29", "UnloadModule 后 IsModuleLoaded 为假",
              !script_env.IsModuleLoaded("Mover"));
    CheckTrue("T30", "对已卸载模块 CreateInstance 返回 nullptr",
              script_env.CreateInstance("Mover", "Mover") == nullptr);

    std::printf("\n========================================\n");
    std::printf("总计 %d 项检查，失败 %d 项\n", g_Checks, g_Failed);
    std::printf("========================================\n");
    return g_Failed == 0 ? 0 : 1;
}
