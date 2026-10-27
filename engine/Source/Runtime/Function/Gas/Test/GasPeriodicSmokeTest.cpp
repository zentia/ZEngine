// ============================================================================
// GasPeriodicSmokeTest — 周期性效果计时纯算术核心（GasPeriodic.h）的无头验证
// ----------------------------------------------------------------------------
// 验证 gas_periodic 命名空间里的两个 inline 纯函数：
//   - PendingExecutions(elapsed, period, dt)：推进周期计时，返回本帧应触发的执行
//     次数；跨过多个周期时补触发多次（不清零而是减去 period），低帧率不丢拍。
//   - IsValidPeriod(period)：period > 0 才算有效周期。
//
// 为什么只测纯函数、不测 ASC：周期触发的"计时与次数判定"全部落在
// GameplayEffectSpec 上（薄封装，转发到这里的算术），ASC 只是每帧遍历调用并按
// 返回次数调 onPeriodicExecute。真正的语义风险都在这两行算术里（跨周期补触发、
// 除零/病态输入防御、符号），把它们抽成 header-only 纯函数即可脱离引擎单测——
// 无需 ASC / GameplayEffect / GameplayEffectSpec / GameObject。spec 层接线由编译
// 期类型检查保证（GasTypes.h 里 StepPeriodic 直接转发）。
//
// 构建（任意主机，纯 CPU，headless）：
//   cmake -B build -DZENGINE_BUILD_GAS_PERIODIC_SMOKE_TEST=ON
//   cmake --build build --target GasPeriodicSmokeTest
// ============================================================================

#include "Runtime/Function/Gas/GasPeriodic.h"

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

    // 模拟"每 period 触发一次，跑总时长 total 秒、按 dt 步进"，返回累计触发次数
    // 与最终 elapsed。验证真实使用路径（ASC 每帧遍历）。
    int Simulate(float period, float dt, float total, float& out_elapsed)
    {
        float elapsed = 0.0f;
        int total_exec = 0;
        const int frames = static_cast<int>(total / dt + 0.5f);
        for (int i = 0; i < frames; ++i)
        {
            total_exec += gas_periodic::PendingExecutions(elapsed, period, dt);
        }
        out_elapsed = elapsed;
        return total_exec;
    }
}  // namespace

int main()
{
    std::printf("GasPeriodicSmokeTest (pure arithmetic core)\n");

    // --- IsValidPeriod --------------------------------------------------------
    Check(gas_periodic::IsValidPeriod(0.5f), "IsValidPeriod: positive period is valid");
    Check(!gas_periodic::IsValidPeriod(0.0f), "IsValidPeriod: zero period is invalid");
    Check(!gas_periodic::IsValidPeriod(-1.0f), "IsValidPeriod: negative period is invalid");

    // --- PendingExecutions: 基本单触发 ---------------------------------------
    {
        float elapsed = 0.0f;
        // period=1.0，推进 0.5s -> 不触发
        Check(gas_periodic::PendingExecutions(elapsed, 1.0f, 0.5f) == 0,
              "PendingExecutions: 0.5s < 1.0s period -> 0 executions");
        Check(elapsed > 0.0f, "PendingExecutions: elapsed accumulated even without trigger");
        // 再推进 0.5s -> 累计 1.0s，恰好跨过 -> 触发 1 次，elapsed 归 0
        Check(gas_periodic::PendingExecutions(elapsed, 1.0f, 0.5f) == 1,
              "PendingExecutions: reaches 1.0s -> 1 execution");
        Check(elapsed == 0.0f, "PendingExecutions: elapsed reset to 0 after exact-period trigger");
    }

    // --- PendingExecutions: 不触发时不清零 -----------------------------------
    {
        float elapsed = 0.0f;
        gas_periodic::PendingExecutions(elapsed, 2.0f, 0.5f);
        gas_periodic::PendingExecutions(elapsed, 2.0f, 0.5f);
        Check(elapsed > 0.99f && elapsed < 1.01f,
              "PendingExecutions: elapsed keeps accumulating below period (no premature reset)");
    }

    // --- PendingExecutions: 跨多个周期补触发（低帧率不丢拍）-------------------
    {
        float elapsed = 0.0f;
        // period=0.5，一次推进 1.5s -> 应触发 3 次，elapsed 归 0
        const int n = gas_periodic::PendingExecutions(elapsed, 0.5f, 1.5f);
        Check(n == 3, "PendingExecutions: 1.5s with 0.5s period -> 3 catch-up executions");
        Check(elapsed == 0.0f, "PendingExecutions: elapsed 0 after multi-period catch-up");
    }

    // --- PendingExecutions: 病态输入防御 -------------------------------------
    {
        float elapsed = 0.0f;
        // 无效 period -> 永不触发，也不推进
        Check(gas_periodic::PendingExecutions(elapsed, 0.0f, 1.0f) == 0,
              "PendingExecutions: zero period -> 0 executions");
        Check(gas_periodic::PendingExecutions(elapsed, -1.0f, 1.0f) == 0,
              "PendingExecutions: negative period -> 0 executions");
        // 负 dt 视为 0 -> 不推进、不触发
        float e2 = 0.0f;
        Check(gas_periodic::PendingExecutions(e2, 1.0f, -5.0f) == 0,
              "PendingExecutions: negative dt treated as 0 -> no trigger");
        Check(e2 == 0.0f, "PendingExecutions: negative dt does not advance elapsed");
        // 极小 period + 极大 dt：封顶 64，不死循环
        float e3 = 0.0f;
        const int capped = gas_periodic::PendingExecutions(e3, 1e-6f, 1e6f);
        Check(capped <= 64, "PendingExecutions: pathological input capped (no infinite loop)");
    }

    // --- 端到端：period=1.0 跑 5 秒应触发 5 次 --------------------------------
    // 用可精确表示的 0.25s 帧增量（每周期 4 帧，20 帧共 5 秒）：避开 1/60 这类
    // 非精确增量累加 60 次后略小于 1.0、导致周期边界处差之毫厘漏一次触发的浮点
    // 现实（与 GasCooldownSmokeTest 同一取舍）。
    {
        float elapsed = 0.0f;
        const int exec = Simulate(1.0f, 0.25f, 5.0f, elapsed);
        Check(exec == 5, "end-to-end: period=1.0 over 5s -> 5 executions");
    }

    // --- 端到端：帧率无关性（不同帧率跑同样 4 秒，period=2.0）-----------------
    // 两个帧率的 dt 都取可精确表示的值（0.5s 与 0.25s），使累计触发次数只由
    // 总时长与周期决定，不受 float 累加噪声影响。
    {
        float e_a = 0.0f, e_b = 0.0f;
        const int a = Simulate(2.0f, 0.5f, 4.0f, e_a);
        const int b = Simulate(2.0f, 0.25f, 4.0f, e_b);
        Check(a == b, "end-to-end: framerate-independent execution count (dt=0.5 == dt=0.25)");
        Check(a == 2, "end-to-end: period=2.0 over 4s -> 2 executions");
    }

    // --- 端到端：低帧率（10fps）跨周期补触发，仍不丢拍 ------------------------
    {
        // period=0.5，10fps（dt=0.1）跑 2 秒 -> 应触发 4 次
        float elapsed = 0.0f;
        const int exec = Simulate(0.5f, 0.1f, 2.0f, elapsed);
        Check(exec == 4, "end-to-end: 10fps with 0.5s period over 2s -> 4 executions (no dropped beats)");
    }

    if (g_Failures == 0)
    {
        std::printf("GasPeriodicSmokeTest: ALL CHECKS PASSED\n");
        return 0;
    }
    std::printf("GasPeriodicSmokeTest: %d CHECK(S) FAILED\n", g_Failures);
    return 1;
}
