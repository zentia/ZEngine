// ==========================================================================
// 脚本 VM 端到端冒烟测试
//
// 为什么需要它
// ------------
// ScriptMathSmokeTest / ScriptTimeSmokeTest / ScriptInputSmokeTest 测的都是
// "C++ 直接调用这些静态方法，返回值对不对"。它们**没有一行**走过真正的脚本链路：
//
//     JS 源码 -> pesapi_eval -> QuickJS -> 生成的 trampoline
//            -> ZScript::Converter 取参 -> C++ 实现 -> Converter 回填 -> JS
//
// 也就是说下面这些直到今天一次都没被执行过（只被"编译过"和"生成过"）：
//
//   1. ScriptBindingRegistry::InstallAll  生成的 ZClassScriptBinding -> pesapi 类
//   2. ScriptBindingRegistry::InstallGlobals  create_class 后挂成 globalThis 上的名字
//   3. ZENUM -> globalThis 上的同名普通对象（Key.Space）
//   4. ScriptEnv 的 console/Debug/__zNewInstance shim
//   5. 模块加载器（IIFE 包装 + module.exports）+ 实例生命周期
//
// 这个测试就是补这一环：**真起一个 QuickJS VM**，往里 Eval 真的 JS，把结果读回来断言。
//
// 关于"为什么这些 .gen.cpp 必须显式编进来"
// ---------------------------------------
// 静态注册器（ZClassScriptBindingRegistrar）住在
// build/Generated/ZHT/ZRuntime/Src/*.gen.cpp 里，靠静态初始化把自己挂进
// ScriptBindingRegistry 的侵入式链表。链接器从 libZRuntime.a 里**只会拉被引用的
// 成员**，而这些注册器没有任何外部引用，所以静态链接时它们根本不会被拉进来
// —— 结果是 InstallAll 看到一张空表，所有 global 都是 undefined。
//
// 本测试因此把用到的几个 .gen.cpp 通过 --extra-source 一起编（见同目录
// CMakeLists.txt 和 scripts/run_smoke_test.py）。
//
// 关于 headless
// -------------
// ScriptEnv 的构造只用到 LOG_* 宏，不碰任何 IEngineSystem。LoadModule 里那次
// GET_SYSTEM(AssetBundleManager) 是 nullptr 容忍的。所以这里不需要起引擎、
// 不需要窗口、不需要渲染器。
// ==========================================================================

#if !PAPI_QUICKJS
#error "ScriptVMSmokeTest 需要 QuickJS 后端（PAPI_QUICKJS）。BackendQuickJS.h 在非 QuickJS 构建下是空的。"
#endif

#include "Runtime/Scripting/Backends/QuickJS/BackendQuickJS.h"
#include "Runtime/Scripting/Binding/ScriptBinding.h"
#include "Runtime/Scripting/Native/PuertsNative.h"
#include "Runtime/Scripting/ScriptInput/ScriptInput.h"
#include "Runtime/Scripting/ScriptMath/ScriptMath.h"
#include "Runtime/Scripting/ScriptTime/ScriptTime.h"
#include "Runtime/Scripting/Wrapper/ScriptEnv.h"

#include "Runtime/Core/Time/TimeState.h"
#include "Runtime/Function/Input/KeyCodes.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

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
            std::printf("  [FAIL] %-6s %s\n", id, detail.c_str());
        }
        else
        {
            std::printf("  [ ok ] %-6s %s\n", id, detail.c_str());
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

    // -----------------------------------------------------------------------
    // Eval + 回读
    // -----------------------------------------------------------------------
    // ScriptEnv::Eval 只负责跑并打日志，不返回值。pesapi_eval 本身也不返回
    // 表达式结果，所以约定：每段被测 JS 把结论写进 globalThis.__zResult（字符串），
    // C++ 侧再开一个 scope 把它读回来。这样"跑了"和"值对"是两件分开断言的事。
    struct EvalResult
    {
        bool threw = false;
        std::string exception;
        std::string result;  // "<none>" = __zResult 不是字符串 / 没被设置
        bool hasResult = false;
    };

    EvalResult EvalJs(ScriptEnv& env, const std::string& code, const char* name)
    {
        EvalResult out;
        pesapi_ffi* apis = env.GetPapis();
        pesapi_env_ref ref = env.GetEnvRef();
        if (apis == nullptr || ref == nullptr)
        {
            out.threw = true;
            out.exception = "env 没有 papis / env_ref";
            return out;
        }

        pesapi_scope scope = pesapi_open_scope(apis, ref);
        pesapi_env e = pesapi_get_env_from_ref(apis, ref);

        pesapi_eval(apis, e, reinterpret_cast<const uint8_t*>(code.data()), code.size(), name);
        if (pesapi_has_caught(apis, scope))
        {
            const char* msg = pesapi_get_exception_as_string(apis, scope, 1);
            out.threw = true;
            out.exception = msg ? msg : "<no message>";
            pesapi_close_scope(apis, scope);
            return out;
        }

        pesapi_value global = pesapi_global(apis, e);
        pesapi_value v = pesapi_get_property(apis, e, global, "__zResult");
        if (v != nullptr && pesapi_is_string(apis, e, v))
        {
            char buf[1024];
            size_t len = sizeof(buf) - 1;
            const char* s = pesapi_get_value_string_utf8(apis, e, v, buf, &len);
            if (s != nullptr)
            {
                out.result.assign(s, len);
                // 不同后端对 len 是否含结尾 NUL 的处理不一致，统一剪掉。
                while (!out.result.empty() && out.result.back() == '\0')
                {
                    out.result.pop_back();
                }
                out.hasResult = true;
            }
        }
        pesapi_close_scope(apis, scope);
        return out;
    }

    /// 只读回 __zResult，不跑任何代码。用于"上一步已经把结果写好了"的场景
    ///（比如 InvokeExportedFunction / InvokeInstanceMethod 之后）。
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

    /// 断言：这段代码没抛异常，且 __zResult == want。
    void CheckJs(const char* id, const char* what, ScriptEnv& env, const std::string& code, const std::string& want)
    {
        EvalResult r = EvalJs(env, code, id);
        if (r.threw)
        {
            std::string d = what;
            d += "  [抛异常] ";
            d += r.exception;
            Report(id, false, d);
            return;
        }
        CheckEq(id, what, r.result, want);
    }

    // -----------------------------------------------------------------------
    // 一个最小但"像 tsc 产物"的模块，写给模块加载器吃
    // -----------------------------------------------------------------------
    //
    // tsconfig 被钉在 "module": "commonjs"，所以 LoadModuleInternal 看到的永远
    // 是 `exports.X = ...` 这种形状。这里手写等价物，避免把 tsc 拖进测试依赖。
    //
    // 关键点：Mover.OnUpdate 会**穿过 C++ 的 VectorMath**，所以这个用例同时证明
    // "JS 实例 -> C++ 绑定 -> JS" 这条双向链路是通的。
    const char kHelloModule[] =
        "\"use strict\";\n"
        "Object.defineProperty(exports, \"__esModule\", { value: true });\n"
        "var Mover = (function () {\n"
        "    function Mover() {\n"
        "        this.speed = 2;\n"
        "        this.origin = { x: 0, y: 0, z: 0 };\n"
        "        this.ticks = 0;\n"
        "        this.lastDt = 0;\n"
        "    }\n"
        "    Mover.prototype.OnUpdate = function (dt) {\n"
        "        this.ticks += 1;\n"
        "        this.lastDt = dt;\n"
        "        this.origin = VectorMath.Add(this.origin,\n"
        "            VectorMath.Scale(VectorMath.UnitX(), this.speed * dt));\n"
        "    };\n"
        "    Mover.prototype.snapshot = function () {\n"
        "        globalThis.__zResult = __fx(this.origin.x) + ',' + this.ticks;\n"
        "    };\n"
        "    return Mover;\n"
        "}());\n"
        "exports.Mover = Mover;\n"
        "function topLevel() {\n"
        "    globalThis.__zResult = 'topLevel:' + __fx(Mathf.Clamp01(0.5));\n"
        "}\n"
        "exports.topLevel = topLevel;\n";

    bool WriteTempModule(const std::filesystem::path& root)
    {
        std::error_code ec;
        std::filesystem::create_directories(root, ec);
        std::ofstream f(root / "Hello.js", std::ios::binary | std::ios::trunc);
        if (!f)
        {
            return false;
        }
        f.write(kHelloModule, static_cast<std::streamsize>(sizeof(kHelloModule) - 1));
        return f.good();
    }
}  // namespace

int main()
{
    std::printf("=== 脚本 VM 端到端冒烟测试 ===\n\n");

    // -----------------------------------------------------------------------
    // 起 VM
    // -----------------------------------------------------------------------
    BackendQuickJS backend;
    ScriptEnv script_env(&backend);

    CheckTrue("V1", "ScriptEnv 构造完成，papis 可用", script_env.GetPapis() != nullptr);
    CheckTrue("V2", "ScriptEnv 构造完成，env_ref 可用", script_env.GetEnvRef() != nullptr);
    CheckTrue("V3", "注册表里至少有一个类被 InstallAll 定义了",
              ScriptBindingRegistry::Classes() != nullptr);

    // 预置一个浮点格式化助手，后面所有数值断言都走它，避免 0.1+0.2 这类
    // 二进制浮点尾巴把断言写成"看起来应该过但就是不过"。
    EvalResult prelude = EvalJs(script_env,
                                "globalThis.__fx = function(n){ return Number(n).toFixed(4); };\n"
                                "globalThis.__zResult = 'prelude ok';\n",
                                "prelude");
    CheckTrue("V4", "Eval 基本可用（prelude 未抛异常）", !prelude.threw);
    CheckEq("V5", "回读 globalThis.__zResult 拿到 'prelude ok'", prelude.result, "prelude ok");

    std::printf("\n--- A. 类 / 枚举被发布成 global ---\n");

    // typeof 只用于"在不在"，真正的可用性由后面真实调用证明。
    CheckJs("A1", "VectorMath 已发布且带 Sub 方法", script_env,
            "globalThis.__zResult = (typeof VectorMath) + '|' + (typeof VectorMath.Sub);",
            "function|function");
    CheckJs("A2", "QuaternionMath / Mathf / Time / Input / Key 全部已发布", script_env,
            "globalThis.__zResult = [typeof QuaternionMath, typeof Mathf, typeof Time,\n"
            "                       typeof Input, typeof Key].join('|');",
            "function|function|function|function|object");
    CheckJs("A3", "console / Debug / print / __zNewInstance shim 已注入", script_env,
            "globalThis.__zResult = [typeof console.log, typeof Debug.Log, typeof Debug.LogWarning,\n"
            "                       typeof print, typeof __zNewInstance, typeof Behaviour].join('|');",
            "function|function|function|function|function|function");

    std::printf("\n--- B. VectorMath：JS -> C++ -> JS 往返 ---\n");

    CheckJs("B1", "Sub((3,5,7),(1,2,3))", script_env,
            "var s = VectorMath.Sub({x:3,y:5,z:7},{x:1,y:2,z:3});\n"
            "globalThis.__zResult = __fx(s.x)+','+__fx(s.y)+','+__fx(s.z);",
            "2.0000,3.0000,4.0000");
    CheckJs("B2", "Add / Scale / Negate", script_env,
            "var v = VectorMath.Scale(VectorMath.Add({x:1,y:2,z:3},{x:1,y:1,z:1}), 2.5);\n"
            "var n = VectorMath.Negate(v);\n"
            "globalThis.__zResult = __fx(v.x)+','+__fx(v.y)+','+__fx(v.z)+'|'+__fx(n.x);",
            "5.0000,7.5000,10.0000|-5.0000");
    CheckJs("B3", "Magnitude / SqrMagnitude / Distance", script_env,
            "globalThis.__zResult = __fx(VectorMath.Magnitude({x:3,y:4,z:0})) + '|' +\n"
            "                       __fx(VectorMath.SqrMagnitude({x:3,y:4,z:0})) + '|' +\n"
            "                       __fx(VectorMath.Distance({x:0,y:0,z:0},{x:0,y:0,z:5}));",
            "5.0000|25.0000|5.0000");
    CheckJs("B4", "Dot / Cross", script_env,
            "var c = VectorMath.Cross({x:1,y:0,z:0},{x:0,y:1,z:0});\n"
            "globalThis.__zResult = __fx(VectorMath.Dot({x:1,y:2,z:3},{x:4,y:5,z:6})) + '|' +\n"
            "                       __fx(c.x)+','+__fx(c.y)+','+__fx(c.z);",
            "32.0000|0.0000,0.0000,1.0000");
    CheckJs("B5", "Normalize / Lerp", script_env,
            "var n = VectorMath.Normalize({x:0,y:3,z:0});\n"
            "var l = VectorMath.Lerp({x:0,y:0,z:0},{x:10,y:10,z:10}, 0.25);\n"
            "globalThis.__zResult = __fx(n.x)+','+__fx(n.y)+','+__fx(n.z)+'|'+\n"
            "                       __fx(l.x)+','+__fx(l.y)+','+__fx(l.z);",
            "0.0000,1.0000,0.0000|2.5000,2.5000,2.5000");
    CheckJs("B6", "ClampMagnitude / MoveTowards / IsZero", script_env,
            "var c = VectorMath.ClampMagnitude({x:3,y:4,z:0}, 2);\n"
            "var m = VectorMath.MoveTowards({x:0,y:0,z:0},{x:10,y:0,z:0}, 3);\n"
            "globalThis.__zResult = __fx(c.x)+','+__fx(c.y)+'|'+__fx(m.x)+'|'+\n"
            "                       VectorMath.IsZero({x:0,y:0,z:0}) + '|' + VectorMath.IsZero({x:0,y:0,z:1});",
            "1.2000,1.6000|3.0000|true|false");
    CheckJs("B7", "Min / Max / Angle", script_env,
            "var mn = VectorMath.Min({x:1,y:9,z:3},{x:4,y:2,z:8});\n"
            "var mx = VectorMath.Max({x:1,y:9,z:3},{x:4,y:2,z:8});\n"
            "globalThis.__zResult = __fx(mn.x)+','+__fx(mn.y)+'|'+__fx(mx.x)+','+__fx(mx.y)+'|'+\n"
            "                       __fx(VectorMath.Angle({x:1,y:0,z:0},{x:0,y:1,z:0}));",
            "1.0000,2.0000|4.0000,9.0000|90.0000");

    std::printf("\n--- C. QuaternionMath / Mathf ---\n");

    // 这条断言把 ToEuler 的分量 -> 轴的映射钉死。ScriptMath.h 里那句"等真机
    // 验证确定轴向"就是靠这里落地的：绕 X 转 90 落在 x 分量，绕 Y 转 90 落在
    // z 分量，绕 Z 转 90 落在 y 分量 —— 也就是
    //     x = pitch = 绕 X，y = yaw = 绕 Z，z = roll = 绕 Y
    // yaw 绕 Z 正是 Z-up 该有的样子（竖直轴是 Z）。改了引擎的欧拉约定这里会红。
    CheckJs("C1", "ToEuler 分量->轴映射：绕X->x / 绕Y->z / 绕Z->y", script_env,
            "function fmt(q){ var e = QuaternionMath.ToEuler(q);\n"
            "                 return __fx(e.x)+','+__fx(e.y)+','+__fx(e.z); }\n"
            "globalThis.__zResult =\n"
            "    fmt(QuaternionMath.AngleAxis(90, {x:1,y:0,z:0})) + ' | ' +\n"
            "    fmt(QuaternionMath.AngleAxis(90, {x:0,y:1,z:0})) + ' | ' +\n"
            "    fmt(QuaternionMath.AngleAxis(90, {x:0,y:0,z:1}));",
            "90.0000,0.0000,0.0000 | 0.0000,0.0000,90.0000 | 0.0000,90.0000,0.0000");
    CheckJs("C2", "Mathf.Clamp / Clamp01 / Lerp / Sign", script_env,
            "globalThis.__zResult = [__fx(Mathf.Clamp(5,0,3)), __fx(Mathf.Clamp01(-2)),\n"
            "                        __fx(Mathf.Lerp(0,10,0.3)), __fx(Mathf.Sign(-7))].join('|');",
            "3.0000|0.0000|3.0000|-1.0000");
    // Unity 的 `Mathf.PI` / `Mathf.Deg2Rad` / `Mathf.Rad2Deg` 都是**常量**
    // （不是函数），所以这里是 `180 * Mathf.Deg2Rad` 而不是 `Deg2Rad(180)`。
    CheckJs("C3", "Mathf.PI / Deg2Rad / Rad2Deg 是静态常量", script_env,
            "globalThis.__zResult = __fx(Mathf.PI)+'|'+__fx(180 * Mathf.Deg2Rad)+'|'+__fx(Mathf.PI * Mathf.Rad2Deg);",
            "3.1416|3.1416|180.0000");

    std::printf("\n--- D. Time：JS 读 C++ 状态，JS 写 C++ 状态 ---\n");

    // D1：还没有任何帧时，TimeState 是初始状态。
    CheckJs("D1", "未推进任何帧时 deltaTime / time / frameCount 全 0", script_env,
            "globalThis.__zResult = [__fx(Time.deltaTime), __fx(Time.time),\n"
            "                        Time.frameCount, __fx(Time.timeScale)].join('|');",
            "0.0000|0.0000|0|1.0000");

    // D2：C++ 推一帧，脚本必须立刻看到（单向 C++ -> JS）。
    TimeState::Get().BeginFrame(0.016f, 0.02f, true);
    CheckJs("D2", "BeginFrame(0.016, 0.02) 后 deltaTime / unscaled / frameCount", script_env,
            "globalThis.__zResult = [__fx(Time.deltaTime), __fx(Time.unscaledDeltaTime),\n"
            "                        Time.frameCount, Time.IsLogicUpdating()].join('|');",
            "0.0160|0.0200|1|true");

    // D3：JS **赋值**给静态属性 —— Unity 的 `Time.timeScale = 2`。这是本切片最
    // 关键的一条：属性装的是 getter/setter 描述符，赋值必须真的走到 C++。
    CheckJs("D3", "JS 写 Time.timeScale = 2 后 C++ 读到 2", script_env,
            "Time.timeScale = 2;\n"
            "globalThis.__zResult = __fx(Time.timeScale);",
            "2.0000");
    TimeState::Get().BeginFrame(0.016f, 0.02f, true);
    CheckJs("D4", "timeScale=2 时脚本看到的 deltaTime 是原始值的两倍", script_env,
            "globalThis.__zResult = __fx(Time.deltaTime)+'|'+__fx(Time.unscaledDeltaTime);",
            "0.0320|0.0200");

    CheckJs("D5", "Time.ResetTimeScale() 把 scale 还原", script_env,
            "Time.ResetTimeScale();\n"
            "globalThis.__zResult = __fx(Time.timeScale);",
            "1.0000");

    // D6：暂停帧（should_update_logic=false）—— 逻辑 delta 归零但 unscaled 还在走。
    TimeState::Get().BeginFrame(0.016f, 0.02f, false);
    CheckJs("D6", "暂停帧：deltaTime=0、IsLogicUpdating=false、unscaled 仍在走", script_env,
            "globalThis.__zResult = [__fx(Time.deltaTime), __fx(Time.unscaledDeltaTime),\n"
            "                        Time.IsLogicUpdating(), __fx(Time.time)].join('|');",
            "0.0000|0.0200|false|0.0480");

    // D7：静态属性的另一半 —— 访问器被从方法表里摘掉了（否则 MSVC 把同一个
    // MethodCallback 编进两个 COMDAT 段，LNK1179）。所以 TS 里只有 Unity 的
    // 拼写，Get* 版本不该存在。
    CheckJs("D7", "deltaTime 是属性；GetDeltaTime() 已不在 TS 面上", script_env,
            "globalThis.__zResult = typeof Time.deltaTime + '/' + typeof Time.GetDeltaTime;",
            "number/undefined");
    // D8：可写属性的第二个样本（fixedDeltaTime），并确认改完能读回来。
    CheckJs("D8", "fixedDeltaTime 可读写，fixedTime 是数", script_env,
            "var before = Time.fixedDeltaTime;\n"
            "Time.fixedDeltaTime = 0.05;\n"
            "var mid = __fx(Time.fixedDeltaTime);\n"
            "Time.fixedDeltaTime = before;\n"
            "globalThis.__zResult = mid + '/' + (typeof Time.fixedTime);",
            "0.0500/number");
    // D9：两个名字共用同一个 getter（Unity 的 unscaledTime / realtimeSinceStartup）。
    CheckJs("D9", "realtimeSinceStartup 与 unscaledTime 同一个值", script_env,
            "globalThis.__zResult = String(Time.realtimeSinceStartup === Time.unscaledTime) + '/' +\n"
            "                       (typeof Time.realtimeSinceStartup);",
            "true/number");

    std::printf("\n--- E. ZENUM 发布 + Input 无系统时的安全降级 ---\n");

    // ZENUM -> globalThis 同名对象这条路径此前从未被真正执行过。
    const int cpp_space = static_cast<int>(KeyCodes::Key::Space);
    CheckJs("E1", "Key.Space 的数值与 C++ 的 KeyCodes::Key::Space 一致", script_env,
            "globalThis.__zResult = String(Key.Space);",
            std::to_string(cpp_space));

    CheckJs("E2", "无 InputSystem 时 GetKey/GetKeyDown/GetKeyUp 全是 false（不是崩、不是 undefined）", script_env,
            "globalThis.__zResult = [Input.GetKey(Key.Space), Input.GetKeyDown(Key.Space),\n"
            "                        Input.GetKeyUp(Key.Space)].join('|');",
            "false|false|false");
    CheckJs("E3", "无 InputSystem 时鼠标/触摸查询返回零值", script_env,
            "var p = Input.GetMousePosition(); var s = Input.GetMouseScrollDelta();\n"
            "globalThis.__zResult = __fx(p.x)+','+__fx(p.y)+'|'+__fx(s.x)+','+__fx(s.y)+'|'+\n"
            "                       Input.GetTouchCount()+'|'+Input.GetTouchIdAt(0)+'|'+\n"
            "                       Input.IsActionActive('Jump');",
            "0.0000,0.0000|0.0000,0.0000|0|-1|false");

    std::printf("\n--- F. console / Debug 通道 ---\n");

    CheckJs("F1", "Debug.Log / console.log / print 都能调用且不抛", script_env,
            "Debug.Log('hello from script');\n"
            "console.log('via console');\n"
            "print('via print');\n"
            "Debug.LogWarning('warn');\n"
            "Debug.LogError('error');\n"
            "globalThis.__zResult = 'logged';",
            "logged");
    CheckJs("F2", "Debug.Log 能吞下非字符串参数（走 String() 兜底）", script_env,
            "Debug.Log(1, 'two', 3.5, true, null, {a:1});\n"
            "globalThis.__zResult = 'ok';",
            "ok");

    std::printf("\n--- G. 模块加载 + 实例生命周期（TypeScriptComponent 走的正是这条路）---\n");

    const std::filesystem::path js_root = std::filesystem::temp_directory_path() / "zengine_script_vm_e2e";
    CheckTrue("G1", "临时模块目录可写", WriteTempModule(js_root));

    script_env.SetJsRoot(js_root.string());
    CheckTrue("G2", "LoadModule('Hello') 成功", script_env.LoadModule("Hello"));
    CheckTrue("G3", "IsModuleLoaded('Hello') 为真", script_env.IsModuleLoaded("Hello"));
    CheckTrue("G4", "重复 LoadModule 走缓存、仍然成功", script_env.LoadModule("Hello"));
    CheckTrue("G5", "LoadModule 一个不存在的模块返回 false", !script_env.LoadModule("NoSuchModule"));

    CheckTrue("G6", "InvokeExportedFunction('Hello','topLevel') 成功",
              script_env.InvokeExportedFunction("Hello", "topLevel"));
    CheckEq("G7", "模块顶层函数里调 Mathf.Clamp01 生效", ReadResult(script_env), "topLevel:0.5000");

    pesapi_value_ref mover = script_env.CreateInstance("Hello", "Mover");
    CheckTrue("G8", "CreateInstance('Hello','Mover') 拿到实例", mover != nullptr);

    if (mover != nullptr)
    {
        // 两个 dt=0.5 的 tick，speed=2 => 每帧 +1 => x = 2
        CheckTrue("G9", "InvokeInstanceMethodNumber(OnUpdate, 0.5) 第一帧",
                  script_env.InvokeInstanceMethodNumber(mover, "OnUpdate", 0.5));
        CheckTrue("G10", "InvokeInstanceMethodNumber(OnUpdate, 0.5) 第二帧",
                  script_env.InvokeInstanceMethodNumber(mover, "OnUpdate", 0.5));
        CheckTrue("G11", "InvokeInstanceMethod(snapshot) 成功",
                  script_env.InvokeInstanceMethod(mover, "snapshot"));
        CheckEq("G12", "JS 实例的 OnUpdate 穿过 C++ VectorMath 累加出 x=2 / ticks=2",
                ReadResult(script_env), "2.0000,2");

        // 缺方法 = no-op 成功（对齐 Unity MonoBehaviour 语义），不是失败。
        CheckTrue("G13", "调用不存在的生命周期方法是 no-op 成功",
                  script_env.InvokeInstanceMethod(mover, "OnStart"));

        // P7 字段桥：C++ 把 "10" 推进去，JS 侧 speed 必须变成数字 10。
        CheckTrue("G14", "ApplyInstanceField(speed='10') 成功",
                  script_env.ApplyInstanceField(mover, "speed", "10"));
        CheckTrue("G15", "再 tick 一帧", script_env.InvokeInstanceMethodNumber(mover, "OnUpdate", 0.5));
        CheckTrue("G16", "snapshot 再读一次", script_env.InvokeInstanceMethod(mover, "snapshot"));
        CheckEq("G17", "ApplyInstanceField 改到 speed=10 后这一帧走了 5（2 -> 7），ticks=3",
                ReadResult(script_env), "7.0000,3");

        auto fields = script_env.EnumerateInstanceFields(mover);
        bool saw_speed = false;
        bool saw_ticks = false;
        bool saw_no_method = true;
        for (const auto& f : fields)
        {
            const std::string& name = std::get<0>(f);
            const std::string& type = std::get<1>(f);
            const std::string& value = std::get<2>(f);
            if (name == "speed")
            {
                saw_speed = (type == "number" && value == "10");
            }
            if (name == "ticks")
            {
                saw_ticks = (type == "number" && value == "3");
            }
            if (name == "OnUpdate" || name == "snapshot")
            {
                saw_no_method = false;  // 原型上的方法不该被当成字段列出来
            }
        }
        CheckTrue("G18", "EnumerateInstanceFields 列出 speed=10 (number)", saw_speed);
        CheckTrue("G19", "EnumerateInstanceFields 列出 ticks=3 (number)", saw_ticks);
        CheckTrue("G20", "EnumerateInstanceFields 不把原型方法当字段", saw_no_method);

        script_env.DestroyInstance(mover);
    }

    CheckTrue("G21", "UnloadModule('Hello') 后 IsModuleLoaded 为假",
              [&] {
                  script_env.UnloadModule("Hello");
                  return !script_env.IsModuleLoaded("Hello");
              }());
    CheckTrue("G22", "InvokeExportedFunction 对已卸载模块返回 false",
              !script_env.InvokeExportedFunction("Hello", "topLevel"));

    std::printf("\n--- H. 异常与可恢复性 ---\n");

    {
        EvalResult r = EvalJs(script_env, "throw new Error('boom');", "H1");
        CheckTrue("H1", "JS 抛异常能被 pesapi 捕获", r.threw);
        CheckTrue("H2", "异常信息里带得有 'boom'", r.exception.find("boom") != std::string::npos);
    }
    CheckJs("H3", "抛异常之后 VM 仍然可用（不是一炸就废）", script_env,
            "globalThis.__zResult = 'still alive';", "still alive");
    {
        // 参数类型不对：Converter 读不到 x/y/z，得到的是 NaN 而不是崩。
        EvalResult r = EvalJs(script_env,
                              "var v = VectorMath.Magnitude({});\n"
                              "globalThis.__zResult = String(v);\n",
                              "H4");
        CheckTrue("H4", "给 Vector3 参数传空对象不会崩（NaN 而非 crash）", !r.threw);
    }

    std::printf("\n--- I. Color：值类型往返 + 静态属性（pesapi Variables）---\n");

    // 这一节钉的是"静态属性真的装到了构造器上"。
    //
    // 静态属性走的是 pesapi 的 `Variables` 表（实例属性走 `Properties`），后端把
    // 前者装到**类对象**上、后者装到 prototype（papi-quickjs 的
    // FindOrCreateClass 对两张表分别调 InitProperty）。这两条路只要有一条接错，
    // 症状是 `Color.red` 为 undefined —— 类型检查照样过，因为 .d.ts 是从同一张
    // 表生成的。所以这里必须真的从 JS 里取值，不能只看声明。
    CheckJs("I1", "Color 已发布，且 red 是对象而不是 undefined", script_env,
            "globalThis.__zResult = typeof Color + '/' + typeof Color.red;", "function/object");
    CheckJs("I2", "Color.red 的四个通道", script_env,
            "var c = Color.red;\n"
            "globalThis.__zResult = c.r + ',' + c.g + ',' + c.b + ',' + c.a;\n",
            "1,0,0,1");
    CheckJs("I3", "clear / white / black / yellow 的通道", script_env,
            "function f(c){ return c.r.toFixed(3) + ',' + c.g.toFixed(3) + ',' + c.b.toFixed(3) + ',' + c.a.toFixed(3); }\n"
            "globalThis.__zResult = [Color.clear, Color.white, Color.black, Color.yellow].map(f).join(' | ');\n",
            "0.000,0.000,0.000,0.000 | 1.000,1.000,1.000,1.000 | 0.000,0.000,0.000,1.000 | "
            "1.000,0.922,0.016,1.000");
    CheckJs("I4", "gray 与 grey 是同一个值", script_env,
            "globalThis.__zResult = String(Color.gray.g === Color.grey.g);\n", "true");
    // 静态属性是 readonly：写不进去也不能崩。
    CheckJs("I5", "给只读静态属性赋值不改变它（也不是异常）", script_env,
            "Color.red = {r: 9, g: 9, b: 9, a: 9};\n"
            "globalThis.__zResult = String(Color.red.r);\n",
            "1");

    // 值类型往返：JS 字面量 -> C++ -> JS。
    CheckJs("I6", "Lerp 两端 + t=0.5", script_env,
            "var c = Color.Lerp(Color.black, Color.white, 0.5);\n"
            "globalThis.__zResult = c.r.toFixed(3) + ',' + c.g.toFixed(3) + ',' + c.b.toFixed(3) + ',' + c.a.toFixed(3);\n",
            "0.500,0.500,0.500,1.000");
    // t 被夹住 —— 不夹会得到 2，写进材质就是溢出。
    CheckJs("I7", "Lerp 把 t 夹到 [0,1]", script_env,
            "globalThis.__zResult = Color.Lerp(Color.black, Color.white, 2).r.toFixed(3) + '/' + "
            "Color.Lerp(Color.black, Color.white, -1).r.toFixed(3);\n",
            "1.000/0.000");
    CheckJs("I8", "LerpUnclamped 不夹（外推到 2）", script_env,
            "globalThis.__zResult = Color.LerpUnclamped(Color.black, Color.white, 2).r.toFixed(3);\n", "2.000");
    // 脚本写颜色经常只给 rgb：alpha 必须默认 1，否则"看不见"。
    CheckJs("I9", "缺 alpha 的字面量按 1 处理（不是 0 / 不是 NaN）", script_env,
            "var c = Color.Lerp({r: 0, g: 0, b: 0}, {r: 1, g: 1, b: 1}, 0.5);\n"
            "globalThis.__zResult = c.r.toFixed(3) + ',' + c.a.toFixed(3);\n",
            "0.500,1.000");
    CheckJs("I10", "HSVToRGB 三个基色", script_env,
            "function f(c){ return c.r.toFixed(2) + ',' + c.g.toFixed(2) + ',' + c.b.toFixed(2); }\n"
            "globalThis.__zResult = [f(Color.HSVToRGB(0, 1, 1)), f(Color.HSVToRGB(1/3, 1, 1)), "
            "f(Color.HSVToRGB(2/3, 1, 1))].join(' | ');\n",
            "1.00,0.00,0.00 | 0.00,1.00,0.00 | 0.00,0.00,1.00");
    CheckJs("I11", "RGBToHSV 返回 Vector3(h, s, v) 且能往返", script_env,
            "var hsv = Color.RGBToHSV(Color.HSVToRGB(0.25, 0.5, 1));\n"
            "globalThis.__zResult = hsv.x.toFixed(3) + ',' + hsv.y.toFixed(3) + ',' + hsv.z.toFixed(3);\n",
            "0.250,0.500,1.000");
    // Unity 的约定：灰的 h 是 1/3（不是 0），只有纯黑的 h 才是 0。
    CheckJs("I12", "灰色是 (h=1/3, s=0, v=0.5)，纯黑才是 (0,0,0)", script_env,
            "function g(c){ var v = Color.RGBToHSV(c); return v.x.toFixed(3) + ',' + v.y.toFixed(3) + ',' + v.z.toFixed(3); }\n"
            "globalThis.__zResult = g(Color.gray) + ' | ' + g(Color.black);\n",
            "0.333,0.000,0.500 | 0.000,0.000,0.000");
    CheckJs("I13", "静态常量是真 getter（每次取到新对象，不是共享的可变单例）", script_env,
            "var a = Color.red; a.r = 0.25;\n"
            "globalThis.__zResult = String(Color.red.r);\n",
            "1");
    // ⚠️ 通道必须**按名字**对上，不能按位置。这里的样本三个通道互不相等
    // （0.1 / 0.2 / 0.4）：此前所有样例都是灰的或 g == b，于是 FromJs 把 g、b
    // 读反了也全绿 —— 只有非对称样本才分得出来。t = 0 让 Lerp 原样返回 a。
    CheckJs("I14", "JS 字面量按名字映射到通道（g/b 不互换）", script_env,
            "var c = Color.Lerp({r: 0.1, g: 0.2, b: 0.4}, Color.white, 0);\n"
            "globalThis.__zResult = c.r.toFixed(3) + ',' + c.g.toFixed(3) + ',' + c.b.toFixed(3) + ',' + c.a.toFixed(3);\n",
            "0.100,0.200,0.400,1.000");
    // 色相环绕：负的、且跨过 -1 的 h（见 UnityParitySmokeTest 的 AZ28 —— 只测
    // -0.25 分不出 floor 有没有生效）。
    CheckJs("I16", "h = -1.25 与 h = 0.75 同色（floor 环绕）", script_env,
            "function f(c){ return c.r.toFixed(2) + ',' + c.g.toFixed(2) + ',' + c.b.toFixed(2); }\n"
            "globalThis.__zResult = f(Color.HSVToRGB(-1.25, 1, 1));\n",
            "0.50,0.00,1.00");
    // 字面量里给了 a 就得用它（上面的缺省分支不能反过来把显式值盖掉）。
    CheckJs("I15", "字面量显式给出的 alpha 被保留", script_env,
            "globalThis.__zResult = Color.Lerp({r: 0, g: 0, b: 0, a: 0.25}, Color.white, 0).a.toFixed(3);\n",
            "0.250");

    std::printf("\n--- J. Unity 的静态字段：Screen / Mathf / Application ---\n");

    // J1：Screen 三个量都是 number（无窗口系统时是 0 / 0 / 1，但类型不能是
    // undefined —— 静态属性没装上就是 undefined）。
    CheckJs("J1", "Screen.width / height / dpi 是静态属性", script_env,
            "globalThis.__zResult = typeof Screen.width + '/' + typeof Screen.height + '/' + typeof Screen.dpi;",
            "number/number/number");
    // J2：Mathf 的常量。Unity 里它们是**字段**，这里也必须能当字段读。
    CheckJs("J2", "Mathf.PI / Epsilon / Infinity / NegativeInfinity", script_env,
            "globalThis.__zResult = __fx(Mathf.PI) + '/' + (Mathf.Epsilon > 0) + '/' +\n"
            "                       (Mathf.Infinity > 0) + '/' + (Mathf.NegativeInfinity < 0) + '/' +\n"
            "                       __fx(Mathf.Deg2Rad) + '/' + __fx(Mathf.Rad2Deg);",
            "3.1416/true/true/true/0.0175/57.2958");
    // J3：Application 的字符串 / 布尔字段。
    CheckJs("J3", "Application.isPlaying / dataPath / productName 是字段", script_env,
            "globalThis.__zResult = typeof Application.isPlaying + '/' + typeof Application.dataPath +\n"
            "                       '/' + typeof Application.productName + '/' + typeof Application.platform;",
            "boolean/string/string/number");
    // J4/J5：这两个是**系统状态**（`Application::m_TargetFrameRate` /
    // `m_RunInBackground`），本测试没有 Application 系统，所以读的是文档里写明的
    // 降级值（-1 / true），写是空操作。这里要守的是"有降级值、不崩、不是
    // undefined"；真正的写路径由 D3（Time.timeScale）与
    // D8（Time.fixedDeltaTime）覆盖 —— 那两个是纯静态状态。
    CheckJs("J4", "无 Application 系统时 targetFrameRate 降级为 -1（可写但不生效）", script_env,
            "Application.targetFrameRate = 60;\n"
            "globalThis.__zResult = typeof Application.targetFrameRate + '/' + Application.targetFrameRate;",
            "number/-1");
    CheckJs("J5", "无 Application 系统时 runInBackground 降级为 true", script_env,
            "Application.runInBackground = false;\n"
            "globalThis.__zResult = typeof Application.runInBackground + '/' + Application.runInBackground;",
            "boolean/true");
    // J6：Get* 那一套已经从 TS 面上撤掉（静态访问器被 codegen 移出方法表）。
    CheckJs("J6", "Screen.GetWidth / Application.GetDataPath 已不在 TS 面上", script_env,
            "globalThis.__zResult = typeof Screen.GetWidth + '/' + typeof Application.GetDataPath;",
            "undefined/undefined");

    std::printf("\n========================================\n");
    std::printf("总计 %d 项检查，失败 %d 项\n", g_Checks, g_Failed);
    std::printf("========================================\n");
    return g_Failed == 0 ? 0 : 1;
}
