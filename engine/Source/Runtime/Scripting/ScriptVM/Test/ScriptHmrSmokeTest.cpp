// ==========================================================================
// P8 HMR（Hot Module Replacement）冒烟测试
//
// 为什么需要它
// ------------
// HMR 的价值只有一句话：**热路径不重建实例 ⇒ 运行时状态保留**。而它跟 P6 的
// 全量重建在外部看来几乎一样（都是"文件变了、代码换了"），差别全在
// `HmrReloadResult::hot_replaced` 这一个 bool 上 —— 一旦这个 bool 判断反了、
// 或者 accept 回调压根没注册进来，表现就是"热重载照常工作"，只是每次都把实例
// 拆了重建。这是典型的"接上了、不报错、只是没生效"。
//
// 这里钉的是四类静默失效：
//   A. `module.hot.accept()` 有没有真的从 JS 走到 C++（早先的纯 JS 存根版本
//      全程没进过 m_HmrStates，HasHmrAcceptCallback 恒为 false）。
//   B. 热路径有没有真的换掉 exports（换不掉 = 新代码永远不生效）。
//   C. decline / 没 accept 的模块有没有正确回落全量重建。
//   D. cascade：require 采集的依赖边对不对、顺序对不对、被改模块的依赖方
//      有没有跟着重载（只重载被改的那个会留下"半新半旧"的闭包）。
//
// 为什么不用 TypeScriptComponent
// ------------------------------
// "实例没被重建"这件事在 ScriptComponentSmokeTest 的 E 段（P6）已经是反向
// 验证过的：那边断言 updateCount 归零。这里直接驱动 ScriptEnv，能更密集地
// 覆盖判定表；组件侧的语义由 ScriptingManager::ReloadModule 里那句
// "hot_replaced 就不通知观察者"单独保证。
// ==========================================================================

#if !PAPI_QUICKJS
#error "ScriptHmrSmokeTest 需要 QuickJS 后端（PAPI_QUICKJS）。"
#endif

#include "Runtime/Scripting/Backends/QuickJS/BackendQuickJS.h"
#include "Runtime/Scripting/Native/PuertsNative.h"
#include "Runtime/Scripting/Wrapper/ScriptEnv.h"

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
        if (got != want)
        {
            d += "  (期望 '" + want + "'，实际 '" + got + "')";
        }
        Report(id, got == want, d);
    }

    std::string ReadGlobal(ScriptEnv& env, const char* name)
    {
        pesapi_ffi*     apis = env.GetPapis();
        pesapi_env_ref  ref = env.GetEnvRef();
        if (apis == nullptr || ref == nullptr)
            return "<no env>";
        pesapi_scope scope = pesapi_open_scope(apis, ref);
        pesapi_env   e = pesapi_get_env_from_ref(apis, ref);
        std::string  out = "<none>";
        pesapi_value v = pesapi_get_property(apis, e, pesapi_global(apis, e), name);
        if (v != nullptr && pesapi_is_string(apis, e, v))
        {
            char         buf[512];
            size_t       len = sizeof(buf) - 1;
            const char*  s = pesapi_get_value_string_utf8(apis, e, v, buf, &len);
            if (s)
                out.assign(s, len);
        }
        pesapi_close_scope(apis, scope);
        return out;
    }

    void EvalChunk(ScriptEnv& env, const std::string& js)
    {
        std::string chunk = js;
        env.Eval(chunk, "hmr_smoke");
    }

    // ---- 临时模块目录 ----------------------------------------------------
    std::filesystem::path g_Root;

    bool WriteModule(const std::string& rel_path, const std::string& body)
    {
        const std::filesystem::path p = g_Root / rel_path;
        std::error_code             ec;
        std::filesystem::create_directories(p.parent_path(), ec);
        std::ofstream f(p, std::ios::binary | std::ios::trunc);
        if (!f)
            return false;
        f.write(body.data(), static_cast<std::streamsize>(body.size()));
        return f.good();
    }

    // 一个"版本化"模块：report() 把当前 exports.version 写进 __zResult，
    // 用来判断 reload 之后生效的是新代码还是旧代码。
    std::string VersionedModule(int version, const std::string& hot_tail)
    {
        std::string s = "exports.version = " + std::to_string(version) + ";\n";
        s += "exports.report = function() { globalThis.__zResult = String(exports.version); };\n";
        s += hot_tail;
        return s;
    }

    std::string JoinIds(const std::vector<std::string>& v)
    {
        std::string out;
        for (const std::string& s : v)
        {
            if (!out.empty())
                out += ",";
            out += s;
        }
        return out;
    }
}  // namespace

int main()
{
    // 管道下 stdout 全缓冲，进程退出时会丢 —— 见 SpineGpuBenchmark 那条老坑。
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    std::printf("=== P8 HMR 冒烟测试 ===\n\n");

    g_Root = std::filesystem::temp_directory_path() / "zengine_script_hmr_e2e";
    std::error_code ec;
    std::filesystem::remove_all(g_Root, ec);
    std::filesystem::create_directories(g_Root, ec);

    BackendQuickJS backend;
    ScriptEnv      env(&backend);
    env.SetJsRoot(g_Root.generic_string());

    // -----------------------------------------------------------------------
    // A. accept 有没有真的从 JS 走到 C++
    // -----------------------------------------------------------------------
    std::printf("--- A. module.hot.accept 的注册链路 ---\n");
    CheckTrue("A1", "HmrAccept.js 写入成功",
              WriteModule("HmrAccept.js", VersionedModule(1, "if (module.hot) { module.hot.accept(); }\n")));
    CheckTrue("A2", "LoadModule('HmrAccept') 成功", env.LoadModule("HmrAccept"));
    CheckTrue("A3", "HasHmrAcceptCallback → true（accept 真的进了 C++ 状态）",
              env.HasHmrAcceptCallback("HmrAccept"));

    // 对照：完全没碰 module.hot 的模块
    CheckTrue("A4", "Plain.js 写入成功", WriteModule("Plain.js", VersionedModule(1, "")));
    CheckTrue("A5", "LoadModule('Plain') 成功", env.LoadModule("Plain"));
    CheckTrue("A6", "没调 accept 的模块 HasHmrAcceptCallback 为 false",
              !env.HasHmrAcceptCallback("Plain"));

    env.InvokeExportedFunction("HmrAccept", "report");
    CheckEq("A7", "重载前是 v1", ReadGlobal(env, "__zResult"), "1");

    // -----------------------------------------------------------------------
    // B. 热路径真的换掉 exports（新代码生效）+ 不重建
    // -----------------------------------------------------------------------
    std::printf("\n--- B. 热替换：换 exports、保留实例 ---\n");
    CheckTrue("B1", "HmrAccept.js 改成 v2",
              WriteModule("HmrAccept.js", VersionedModule(2, "if (module.hot) { module.hot.accept(); }\n")));
    ScriptEnv::HmrReloadResult r = env.ReloadModule("HmrAccept");
    CheckTrue("B2", "reload ok", r.ok);
    CheckTrue("B3", "hot_replaced = true（走的是热路径，实例未重建）", r.hot_replaced);
    CheckEq("B4", "reloaded 只含自己", JoinIds(r.reloaded), "HmrAccept");
    env.InvokeExportedFunction("HmrAccept", "report");
    CheckEq("B5", "换过之后 report 读到 v2（新代码生效）", ReadGlobal(env, "__zResult"), "2");

    // -----------------------------------------------------------------------
    // C. accept(cb) 拿到的是新 exports；dispose 在旧 exports 丢弃前被调用
    // -----------------------------------------------------------------------
    std::printf("\n--- C. accept 回调 / dispose 回调 ---\n");
    const std::string cb_tail =
        "if (module.hot) {\n"
        "  module.hot.accept(function(newExports) {\n"
        "    globalThis.__zAccepted = String(newExports.version);\n"
        "  });\n"
        "  module.hot.dispose(function() {\n"
        "    globalThis.__zDisposed = String((Number(globalThis.__zDisposed) || 0) + 1);\n"
        "  });\n"
        "}\n";
    CheckTrue("C1", "HmrCb.js 写入成功", WriteModule("HmrCb.js", VersionedModule(1, cb_tail)));
    CheckTrue("C2", "LoadModule('HmrCb') 成功", env.LoadModule("HmrCb"));
    CheckTrue("C3", "HmrCb.js 改成 v2", WriteModule("HmrCb.js", VersionedModule(2, cb_tail)));
    r = env.ReloadModule("HmrCb");
    CheckTrue("C4", "reload ok", r.ok);
    CheckTrue("C5", "hot_replaced = true", r.hot_replaced);
    CheckEq("C6", "accept 回调拿到的是**新** exports（v2）", ReadGlobal(env, "__zAccepted"), "2");
    CheckEq("C7", "dispose 回调被调用了一次", ReadGlobal(env, "__zDisposed"), "1");

    // -----------------------------------------------------------------------
    // D. decline / 无 accept ⇒ 回落全量重建
    // -----------------------------------------------------------------------
    std::printf("\n--- D. 回落：decline 与未 accept ---\n");
    CheckTrue("D1", "HmrDecline.js 写入成功",
              WriteModule("HmrDecline.js", VersionedModule(1, "if (module.hot) { module.hot.decline(); }\n")));
    CheckTrue("D2", "LoadModule('HmrDecline') 成功", env.LoadModule("HmrDecline"));
    CheckTrue("D3", "HasHmrDeclined 为 true", env.HasHmrDeclined("HmrDecline"));
    CheckTrue("D4", "HmrDecline.js 改成 v2",
              WriteModule("HmrDecline.js", VersionedModule(2, "if (module.hot) { module.hot.decline(); }\n")));
    r = env.ReloadModule("HmrDecline");
    CheckTrue("D5", "reload 仍然 ok（回落不是失败）", r.ok);
    CheckTrue("D6", "decline 的模块 hot_replaced = false（走全量重建）", !r.hot_replaced);
    env.InvokeExportedFunction("HmrDecline", "report");
    CheckEq("D7", "回落之后新代码同样生效（v2）", ReadGlobal(env, "__zResult"), "2");

    CheckTrue("D8", "Plain.js 改成 v2", WriteModule("Plain.js", VersionedModule(2, "")));
    r = env.ReloadModule("Plain");
    CheckTrue("D9", "未 accept 的模块 reload ok", r.ok);
    CheckTrue("D10", "未 accept 的模块 hot_replaced = false", !r.hot_replaced);

    // -----------------------------------------------------------------------
    // E. cascade：require 采集依赖边 + 依赖方跟着重载
    // -----------------------------------------------------------------------
    std::printf("\n--- E. cascade（依赖方跟着重载）---\n");
    const std::string accept_tail = "if (module.hot) { module.hot.accept(); }\n";
    CheckTrue("E1", "HmrChild.js 写入成功",
              WriteModule("HmrChild.js", "exports.tag = 'child1';\n" + accept_tail));
    CheckTrue("E2", "HmrParent.js 写入成功",
              WriteModule("HmrParent.js",
                          "var c = require('./HmrChild');\n"
                          "exports.report = function() { globalThis.__zResult = String(c.tag); };\n" +
                              accept_tail));
    CheckTrue("E3", "LoadModule('HmrParent') 成功（顺带把 child 加载进来）", env.LoadModule("HmrParent"));
    CheckTrue("E4", "child 因为被 require 而加载了", env.IsModuleLoaded("HmrChild"));

    CheckEq("E5", "cascade(HmrChild) = [child, parent]（被改者在前）",
            JoinIds(env.GetCascadeReloadOrder("HmrChild")), "HmrChild,HmrParent");
    CheckEq("E6", "cascade(HmrParent) = [parent]（没人依赖 parent）",
            JoinIds(env.GetCascadeReloadOrder("HmrParent")), "HmrParent");

    env.InvokeExportedFunction("HmrParent", "report");
    CheckEq("E7", "重载前 parent 抓的是 child1", ReadGlobal(env, "__zResult"), "child1");

    CheckTrue("E8", "HmrChild.js 改成 child2",
              WriteModule("HmrChild.js", "exports.tag = 'child2';\n" + accept_tail));
    r = env.ReloadModule("HmrChild");
    CheckTrue("E9", "reload ok", r.ok);
    CheckTrue("E10", "整条 cascade 都走了热路径", r.hot_replaced);
    CheckEq("E11", "reloaded = [child, parent]", JoinIds(r.reloaded), "HmrChild,HmrParent");
    env.InvokeExportedFunction("HmrParent", "report");
    CheckEq("E12", "parent 重新求值后抓到 child2（不是半新半旧）",
            ReadGlobal(env, "__zResult"), "child2");

    // -----------------------------------------------------------------------
    // F. 子目录里的相对解析（base_dir 这一半以前根本没跑过）
    // -----------------------------------------------------------------------
    std::printf("\n--- F. 子目录相对解析 ---\n");
    CheckTrue("F1", "sub/Dep.js 写入成功",
              WriteModule("sub/Dep.js", "exports.tag = 'dep1';\n" + accept_tail));
    CheckTrue("F2", "sub/Main.js 写入成功",
              WriteModule("sub/Main.js",
                          "var d = require('./Dep');\n"
                          "exports.report = function() { globalThis.__zResult = String(d.tag); };\n" +
                              accept_tail));
    CheckTrue("F3", "LoadModule('sub/Main') 成功", env.LoadModule("sub/Main"));
    CheckTrue("F4", "require('./Dep') 在 sub/ 下解析成 sub/Dep（不是根目录的 Dep）",
              env.IsModuleLoaded("sub/Dep"));
    CheckEq("F5", "cascade(sub/Dep) = [sub/Dep, sub/Main]",
            JoinIds(env.GetCascadeReloadOrder("sub/Dep")), "sub/Dep,sub/Main");

    // -----------------------------------------------------------------------
    // G. cascade 里有一个 decline ⇒ 整条回落
    // -----------------------------------------------------------------------
    std::printf("\n--- G. cascade 全 accept 才走热路径 ---\n");
    CheckTrue("G1", "HmrChild 改成 decline",
              WriteModule("HmrChild.js", "exports.tag = 'child3';\nif (module.hot) { module.hot.decline(); }\n"));
    r = env.ReloadModule("HmrChild");
    CheckTrue("G2", "reload ok", r.ok);
    CheckTrue("G3", "parent accept 但 child decline ⇒ 整条回落", !r.hot_replaced);
    env.InvokeExportedFunction("HmrParent", "report");
    CheckEq("G4", "回落之后 parent 也换成了新 child", ReadGlobal(env, "__zResult"), "child3");

    // -----------------------------------------------------------------------
    // H. 判定必须基于**新代码**的声明，不能基于内存里的旧状态
    //
    // 这一条是 G3 抓到的真 bug 的另一半：上一版 accept 过、这一版把 accept 删了
    // （而不是改成 decline），如果沿用内存里残留的 accepted=true，模块照样被
    // 判成可热替换 ⇒ "删掉 accept 就不热替换了"这件事静默失效。
    // -----------------------------------------------------------------------
    std::printf("\n--- H. 新代码不再 accept ⇒ 回落 ---\n");
    CheckTrue("H1", "HmrAccept 改成不再 accept（v3）",
              WriteModule("HmrAccept.js", VersionedModule(3, "")));
    r = env.ReloadModule("HmrAccept");
    CheckTrue("H2", "reload ok", r.ok);
    CheckTrue("H3", "新代码没 accept ⇒ hot_replaced = false（不沿用旧状态）", !r.hot_replaced);
    env.InvokeExportedFunction("HmrAccept", "report");
    CheckEq("H4", "回落之后新代码生效（v3）", ReadGlobal(env, "__zResult"), "3");

    CheckTrue("H5", "HmrAccept 改回 accept（v4）",
              WriteModule("HmrAccept.js", VersionedModule(4, "if (module.hot) { module.hot.accept(); }\n")));
    r = env.ReloadModule("HmrAccept");
    CheckTrue("H6", "重新 accept 之后又回到热路径", r.hot_replaced);
    env.InvokeExportedFunction("HmrAccept", "report");
    CheckEq("H7", "热路径之后是 v4", ReadGlobal(env, "__zResult"), "4");

    // -----------------------------------------------------------------------
    std::printf("\n%d checks, %d failures\n", g_Checks, g_Failed);
    return g_Failed == 0 ? 0 : 1;
}
