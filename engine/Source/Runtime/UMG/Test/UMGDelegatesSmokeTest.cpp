// ============================================================================
// UMGDelegatesSmokeTest — 命名事件注册表（UMGDelegates.h）的无头验证
// ----------------------------------------------------------------------------
// 验证 ZUMG::UMGEventRegistry 的语义：
//   - Bind 返回 token；Invoke 触发该 id 下所有 handler。
//   - 同一 id 可绑多个 handler（多监听者），Invoke 按序全触发。
//   - Unbind(token) 精确移除一个；UnbindEvent(id) 移除该 id 全部；Clear 清空。
//   - Invoke 未知 id 是 no-op（不是错误——资产可能引用本会话没绑的事件）。
//   - handler 内 Unbind 自身/同 id 其他 handler 不崩（Invoke 前拷贝 handler 列表）。
//   - token 唯一性（0 保留为 invalid）。
//
// 这是 P0 数据绑定的核心：Designer 资产存事件名，游戏按名 Bind，widget 点击 Invoke。
// 纯 C++ 容器，无引擎依赖，只链 include root。
//
// 构建（任意主机，纯 CPU，headless）：
//   cmake -B build -DZENGINE_BUILD_UMG_DELEGATES_SMOKE_TEST=ON
//   cmake --build build --target UMGDelegatesSmokeTest
// ============================================================================

#include "Runtime/UMG/Core/UMGDelegates.h"

#include <cstdio>

namespace
{
    int g_Failures = 0;

    void Check(bool cond, const char* msg)
    {
        if (cond)
        {
            std::printf("  [ ok ] %s\n", msg);
        }
        else
        {
            std::printf("  [FAIL] %s\n", msg);
            ++g_Failures;
        }
    }
}  // namespace

int main()
{
    std::printf("UMGDelegatesSmokeTest (named-event registry)\n");
    using ZUMG::UMGEventRegistry;
    using ZUMG::UMGEventToken;

    // --- Bind + Invoke: 基本触发 ---------------------------------------------
    {
        UMGEventRegistry reg;
        int count = 0;
        reg.Bind("click", [&count]() { ++count; });
        reg.Invoke("click");
        reg.Invoke("click");
        Check(count == 2, "Bind + Invoke: handler fires once per Invoke");
    }

    // --- 多监听者：同一 id 绑多个 handler，Invoke 全触发 ---------------------
    {
        UMGEventRegistry reg;
        int a = 0, b = 0;
        reg.Bind("evt", [&a]() { ++a; });
        reg.Bind("evt", [&b]() { ++b; });
        Check(reg.Count("evt") == 2, "multiple handlers under one id are allowed");
        reg.Invoke("evt");
        Check(a == 1 && b == 1, "Invoke fires ALL handlers under the id");
    }

    // --- 不同 id 互不干扰 -----------------------------------------------------
    {
        UMGEventRegistry reg;
        int a = 0, b = 0;
        reg.Bind("evt_a", [&a]() { ++a; });
        reg.Bind("evt_b", [&b]() { ++b; });
        reg.Invoke("evt_a");
        Check(a == 1 && b == 0, "invoking one id does not fire another id's handlers");
    }

    // --- Invoke 未知 id 是 no-op（不崩、不报错）------------------------------
    {
        UMGEventRegistry reg;
        reg.Invoke("never_bound");
        Check(true, "Invoke of unknown id is a safe no-op");
    }

    // --- Unbind(token) 精确移除一个 handler ----------------------------------
    {
        UMGEventRegistry reg;
        int a = 0, b = 0;
        const UMGEventToken ta = reg.Bind("evt", [&a]() { ++a; });
        reg.Bind("evt", [&b]() { ++b; });
        Check(reg.Unbind(ta), "Unbind returns true for a live token");
        reg.Invoke("evt");
        Check(a == 0 && b == 1, "Unbind removes only the targeted handler");
        Check(!reg.Unbind(ta), "Unbind of an already-removed token returns false");
    }

    // --- UnbindEvent(id) 移除该 id 全部 --------------------------------------
    {
        UMGEventRegistry reg;
        int a = 0, b = 0;
        reg.Bind("evt", [&a]() { ++a; });
        reg.Bind("evt", [&b]() { ++b; });
        reg.Bind("other", [&a]() { ++a; });
        Check(reg.UnbindEvent("evt") == 2, "UnbindEvent returns the removed count");
        reg.Invoke("evt");
        Check(a == 0 && b == 0, "UnbindEvent removes all handlers under the id");
        Check(reg.Count("evt") == 0, "count for the removed id is now 0");
        Check(reg.Count("other") == 1, "other ids are untouched");
    }

    // --- Clear 清空全部 -------------------------------------------------------
    {
        UMGEventRegistry reg;
        int n = 0;
        reg.Bind("x", [&n]() { ++n; });
        reg.Bind("y", [&n]() { ++n; });
        reg.Clear();
        Check(reg.TotalCount() == 0, "Clear empties the whole registry");
        reg.Invoke("x");
        reg.Invoke("y");
        Check(n == 0, "no handlers fire after Clear");
    }

    // --- handler 内 Unbind 自身不崩（Invoke 前拷贝 handler 列表）-------------
    {
        UMGEventRegistry reg;
        UMGEventToken self_token = 0;
        int fires = 0;
        self_token = reg.Bind("evt", [&]() {
            ++fires;
            reg.Unbind(self_token);  // remove itself during invocation
        });
        reg.Invoke("evt");
        Check(fires == 1, "self-unbinding handler still fires for this Invoke");
        // Second Invoke: handler is gone, so no fire, and no crash.
        reg.Invoke("evt");
        Check(fires == 1, "self-unbound handler does not fire again (no dangling)");
    }

    // --- handler 内 Invoke 同 id 另一 handler 不崩 ---------------------------
    {
        UMGEventRegistry reg;
        int a = 0, b = 0;
        const UMGEventToken ta = reg.Bind("evt", [&a]() { ++a; });
        reg.Bind("evt", [&]() {
            ++b;
            reg.Unbind(ta);  // remove the OTHER handler during invocation
        });
        reg.Invoke("evt");
        Check(a == 1 && b == 1, "reentrant Unbind during Invoke is safe (both fire this pass)");
    }

    // --- token 唯一性（0 保留为 invalid）------------------------------------
    {
        UMGEventRegistry reg;
        const UMGEventToken t1 = reg.Bind("a", [] {});
        const UMGEventToken t2 = reg.Bind("b", [] {});
        Check(t1 != t2, "each Bind returns a distinct token");
        Check(t1 != 0 && t2 != 0, "tokens are never 0 (0 reserved as invalid)");
    }

    // --- 端到端：模拟 Designer 资产 → 游戏按名绑定 → widget 触发 -------------
    {
        // Asset stores OnClickEvent="OnStartClicked". Game binds by name. A click
        // "invokes" the event (the UButton lambda does exactly this).
        UMGEventRegistry reg;
        bool started = false;
        const UMGEventToken token = reg.Bind("OnStartClicked", [&started]() { started = true; });
        // (widget click path) — the button's lambda would call Invoke here:
        reg.Invoke("OnStartClicked");
        Check(started, "end-to-end: asset event name -> game Bind -> widget Invoke fires");
        // Owner teardown: unbind by token so a destroyed owner leaves no dangling.
        reg.Unbind(token);
        Check(reg.TotalCount() == 0, "end-to-end: owner teardown unbinds cleanly");
    }

    // --- 全局注册表单例可用 ---------------------------------------------------
    {
        ZUMG::UMGEventRegistry& g = ZUMG::GetUMGEventRegistry();
        g.Clear();  // isolate from other TUs
        int n = 0;
        const auto tok = g.Bind("__smoke_global__", [&n]() { ++n; });
        ZUMG::GetUMGEventRegistry().Invoke("__smoke_global__");
        Check(n == 1, "process-wide registry is reachable and functional");
        g.Unbind(tok);
        g.Clear();
    }

    if (g_Failures == 0)
    {
        std::printf("UMGDelegatesSmokeTest: ALL CHECKS PASSED\n");
        return 0;
    }
    std::printf("UMGDelegatesSmokeTest: %d CHECK(S) FAILED\n", g_Failures);
    return 1;
}
