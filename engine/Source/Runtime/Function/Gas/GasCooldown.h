#pragma once

// ============================================================================
// GasCooldown — 冷却时间的纯算术核心（无引擎依赖，header-only）
// ----------------------------------------------------------------------------
// 把"剩余冷却秒数随帧递减 + 判定是否在冷却中"这两件事抽成不依赖任何引擎类型
// 的 inline 函数，与 EnhancedInputSystem::ComputeFrameDelta 同一手法：核心是纯
// 函数，可脱离 ASC/GameplayAbility/AttributeSet 单独单测（GasCooldownSmokeTest），
// 而 GameplayAbilitySpec 只做薄封装（存一个 float 剩余量 + 转发到这里的算术）。
//
// 语义：
//   - remaining 表示"此能力还要等多少秒才能再次激活"。remaining > 0 即在冷却中。
//   - 每帧把 remaining 减去 delta_time，但不低于 0（冷却不会变成负数）。
//   - 冷却为 0 的能力（cooldown_duration == 0）永远不在冷却中——StartCooldown
//     根本不该被调用；这里仍保证 remaining<=0 时 IsOnCooldown 恒 false。
// ============================================================================

namespace gas_cooldown
{
    // 剩余冷却递减一帧。delta_time 必须 >= 0；remaining 不会低于 0。
    inline float StepCooldown(float remaining, float delta_time)
    {
        if (delta_time < 0.0f)
            delta_time = 0.0f;
        const float next = remaining - delta_time;
        return next < 0.0f ? 0.0f : next;
    }

    // 是否仍在冷却中（严格大于 0）。
    inline bool IsOnCooldown(float remaining)
    {
        return remaining > 0.0f;
    }

    // 激活后起冷却：把剩余量设为完整冷却时长。duration < 0 视为 0（无冷却）。
    inline float StartCooldown(float duration)
    {
        return duration < 0.0f ? 0.0f : duration;
    }
}  // namespace gas_cooldown
