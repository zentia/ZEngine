#pragma once

// ============================================================================
// GasPeriodic — 周期性 GameplayEffect 的执行计时纯算术核心（无引擎依赖，header-only）
// ----------------------------------------------------------------------------
// 把"周期性效果每隔 period 秒触发一次"这件事里唯一有语义风险的部分——计时累加
// 与"本帧该触发几次"的判定——抽成不依赖任何引擎类型的 inline 函数，与
// gas_cooldown 同一手法：核心是纯函数，可脱离 ASC/GameplayEffect/GameplayEffectSpec
// 单独单测（GasPeriodicSmokeTest），而 GameplayEffectSpec 只做薄封装（存一个
// float 距上次触发的计时 + 转发到这里的算术）。
//
// 语义：
//   - 一个周期性效果每隔 period 秒执行一次 onPeriodicExecute。
//   - 每帧把"距上次触发的计时"推进 delta_time；推进后只要计时跨过 period，就
//     触发一次并把计时减去 period（而不是清零）——这样在低帧率/卡顿下，一次
//     Update 跨过多个周期时能正确补触发多次，不会丢拍。
//   - period <= 0 视为"无有效周期"，永不触发（防御异常配置，避免除零/死循环）。
//   - delta_time < 0 视为 0（不回退计时）。
//
// execute_on_application（应用时立即执行一次）不属于计时逻辑，由 ASC 在 ApplyEffect
// 里直接调用一次 onPeriodicExecute 即可，这里不涉及。
// ============================================================================

namespace gas_periodic
{
    // 推进周期计时一帧，返回本帧应触发的执行次数（0 表示本帧不触发）。
    // elapsed 是"距上次触发的秒数"，调用方据返回值调用 onPeriodicExecute 若干次。
    inline int PendingExecutions(float& elapsed, float period, float delta_time)
    {
        if (period <= 0.0f)
            return 0;
        if (delta_time < 0.0f)
            delta_time = 0.0f;

        elapsed += delta_time;

        int count = 0;
        while (elapsed >= period)
        {
            elapsed -= period;
            ++count;
            // 防御：极小 period + 极大 dt 时避免无谓的长循环（理论上不会发生，
            // 但一个周期最多补触发一次的意义不大，封顶防止病态输入拖死帧）。
            if (count >= 64)
            {
                elapsed = 0.0f;
                break;
            }
        }
        return count;
    }

    // 是否为有效周期配置（period > 0）。ASC 据此决定是否参与计时。
    inline bool IsValidPeriod(float period)
    {
        return period > 0.0f;
    }
}  // namespace gas_periodic
