#pragma once

#include "GasTypes.h"

#include <string>
#include <vector>

// ============================================================================
// GameplayEffect - 游戏效果（Buff/Debuff）
// ============================================================================
// 用于修改属性、应用标签、周期性效果等

class GameplayEffect
{
public:
    GameplayEffect() = default;
    virtual ~GameplayEffect() = default;

    // 效果名称
    std::string effect_name;

    // 效果标签
    GameplayTagContainer asset_tags;

    // 授予的标签（应用效果时添加）
    GameplayTagContainer granted_tags;

    // 移除的标签（应用效果时移除）
    GameplayTagContainer removed_tags;

    // 持续时间
    GameplayEffectDuration duration;

    // 周期（用于周期性效果，如每2秒造成伤害）
    GameplayEffectPeriod period;

    // 修改器列表
    std::vector<GameplayEffectModifier> modifiers;

    // 堆叠规则
    GameplayEffectStacking stacking;

    // 效果等级
    int32_t default_level {1};

    // 是否可以移除
    bool can_be_removed {true};

    // 应用效果时的回调（可以阻止应用）
    virtual bool canApplyEffect(class AbilitySystemComponent* target_asc,
                                class AbilitySystemComponent* source_asc) const
    {
        return true;
    }

    // 效果应用时的回调
    virtual void onEffectApplied(class GameplayEffectSpec* spec, class AbilitySystemComponent* target_asc) {}

    // 效果移除时的回调
    virtual void onEffectRemoved(class GameplayEffectSpec* spec, class AbilitySystemComponent* target_asc) {}

    // 周期性执行时的回调
    virtual void onPeriodicExecute(class GameplayEffectSpec* spec, class AbilitySystemComponent* target_asc) {}
};