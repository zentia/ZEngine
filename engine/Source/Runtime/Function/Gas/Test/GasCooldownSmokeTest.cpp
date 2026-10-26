// ============================================================================
// GasCooldownSmokeTest — GAS 冷却时间纯算术核心（GasCooldown.h）的无头验证
// ----------------------------------------------------------------------------
// 验证 gas_cooldown 命名空间里的三个 inline 纯函数：
//   - StepCooldown(remaining, dt)：剩余冷却递减一帧，不低于 0；负 dt 归零。
//   - IsOnCooldown(remaining)：严格 > 0 才算在冷却中。
//   - StartCooldown(duration)：激活后起冷却，负时长归零。
//
// 为什么只测纯函数、不测 ASC：冷却的"状态与递减"全部落在 GameplayAbilitySpec
// 上（薄封装，转发到这里的算术），ASC 只是每帧遍历调用。真正的语义风险都在
// 这三行算术里（钳制、边界、符号），把它们抽成 header-only 纯函数即可像
// EnhancedInputSystem::ComputeFrameDelta 一样脱离引擎单测——无需 ASC /
// GameplayAbility / AttributeSet / GameObject。spec 层的接线由编译期类型检查
// 保证（GasTypes.h 里 StartCooldown/StepCooldown/IsOnCooldown 直接转发）。
//
// 构建（任意主机，纯 CPU，headless）：
//   cmake -B build -DZENGINE_BUILD_GAS_COOLDOWN_SMOKE_TEST=ON
//   cmake --build build --target GasCooldownSmokeTest
// ============================================================================

#include "Runtime/Function/Gas/GasCooldown.h"

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

    // 浮点近似比较：1.0f/30.0f*60 之类的非可精确表示的帧增量累加后会与字面值差
    // 几个 ULP，故按 epsilon 判等（不是缺陷——是 float 现实）。
    void CheckNear(float a, float b, float eps, const char* msg)
    {
        const float diff = a - b;
        Check(diff >= -eps && diff <= eps, msg);
    }

    // 模拟"激活后按固定帧率跑若干帧，返回最终剩余冷却"，验证真实使用路径。
    float Simulate(float initial, float dt, int frames)
    {
        float remaining = initial;
        for (int i = 0; i < frames; ++i)
            remaining = gas_cooldown::StepCooldown(remaining, dt);
        return remaining;
    }
}  // namespace

int main()
{
    std::printf("GasCooldownSmokeTest (pure arithmetic core)\n");

    // --- StartCooldown --------------------------------------------------------
    Check(gas_cooldown::StartCooldown(2.0f) == 2.0f, "StartCooldown: returns the given duration");
    Check(gas_cooldown::StartCooldown(0.0f) == 0.0f, "StartCooldown: zero duration stays 0");
    Check(gas_cooldown::StartCooldown(-1.5f) == 0.0f, "StartCooldown: negative duration clamped to 0");

    // --- IsOnCooldown ---------------------------------------------------------
    Check(gas_cooldown::IsOnCooldown(0.001f), "IsOnCooldown: tiny positive -> on cooldown");
    Check(!gas_cooldown::IsOnCooldown(0.0f), "IsOnCooldown: exactly 0 -> not on cooldown");
    Check(!gas_cooldown::IsOnCooldown(-0.5f), "IsOnCooldown: negative -> not on cooldown");

    // --- StepCooldown: 基本递减 ----------------------------------------------
    Check(gas_cooldown::StepCooldown(3.0f, 1.0f) == 2.0f, "StepCooldown: 3.0 - 1.0 == 2.0");

    // 帧率无关性：30fps 跑 60 帧 与 60fps 跑 120 帧，同样 2 秒，剩余应一致
    // （按 epsilon 判等：帧增量 1/30、1/60 非可精确表示，累加后差几个 ULP）。
    {
        const float a = Simulate(5.0f, 1.0f / 30.0f, 60);   // 2 秒
        const float b = Simulate(5.0f, 1.0f / 60.0f, 120);  // 2 秒
        // 1e-4 容差：1/30、1/60 非精确表示，各自累加 60/120 次后的 ULP 级噪声
        // 会略超 1e-5；1e-4 仍足以抓出真正的帧率相关 bug（如漏乘 delta）。
        CheckNear(a, b, 1e-4f, "StepCooldown: framerate-independent (30fps/60f == 60fps/120f)");
        CheckNear(a, 3.0f, 1e-4f, "StepCooldown: 5.0 - 2.0 ~= 3.0 after 2s");
    }

    // --- StepCooldown: 钳制到 0（不会变负）------------------------------------
    Check(gas_cooldown::StepCooldown(0.5f, 1.0f) == 0.0f, "StepCooldown: overshoot clamps to 0 (no negative)");
    Check(gas_cooldown::StepCooldown(0.0f, 1.0f) == 0.0f, "StepCooldown: already 0 stays 0");

    // 大 dt（卡顿/掉帧）也不会跌到负数。
    Check(gas_cooldown::StepCooldown(0.1f, 999.0f) == 0.0f, "StepCooldown: huge dt clamps to 0");

    // --- StepCooldown: 负 dt 归零（防御异常输入）------------------------------
    Check(gas_cooldown::StepCooldown(2.0f, -1.0f) == 2.0f, "StepCooldown: negative dt treated as 0 (no rewind)");

    // --- 完整冷却周期：从满冷却跑足够帧后应回到 0 且可再次激活 -----------------
    {
        // 2 秒冷却，用可精确表示的 0.5s 帧增量跑 4 帧（4 * 0.5 == 2.0 位精确）
        // -> 恰好归 0。避开 1/60 这类非精确增量累加后偏上 0 的浮点现实。
        const float at_2s = Simulate(2.0f, 0.5f, 4);
        Check(at_2s == 0.0f, "full cooldown: expires exactly at 2s (4 x 0.5s frames)");
        Check(!gas_cooldown::IsOnCooldown(at_2s), "full cooldown: not on cooldown once expired");

        // 再跑一帧 -> 仍为 0，IsOnCooldown false（可再次激活）。
        const float after = gas_cooldown::StepCooldown(at_2s, 1.0f / 60.0f);
        Check(after == 0.0f && !gas_cooldown::IsOnCooldown(after),
              "full cooldown: stays 0 after expiry, re-activatable");
    }

    // --- 激活闸门语义：冷却未结束前不可再次激活 --------------------------------
    {
        float cd = gas_cooldown::StartCooldown(1.0f);
        Check(gas_cooldown::IsOnCooldown(cd), "gate: on cooldown right after activation");
        // 跑半秒（60fps * 30 帧）：还剩约 0.5s，仍在冷却。
        cd = Simulate(cd, 1.0f / 60.0f, 30);
        Check(gas_cooldown::IsOnCooldown(cd), "gate: still on cooldown at 0.5s remaining");
        // 跑满剩余：冷却结束。
        cd = Simulate(cd, 1.0f / 60.0f, 60);
        Check(!gas_cooldown::IsOnCooldown(cd), "gate: off cooldown once remaining reaches 0");
    }

    if (g_Failures == 0)
    {
        std::printf("GasCooldownSmokeTest: ALL CHECKS PASSED\n");
        return 0;
    }
    std::printf("GasCooldownSmokeTest: %d CHECK(S) FAILED\n", g_Failures);
    return 1;
}
