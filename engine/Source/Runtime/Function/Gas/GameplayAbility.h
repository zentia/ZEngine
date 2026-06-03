#pragma once

#include "GasTypes.h"

#include <functional>
#include <string>

class AbilitySystemComponent;
class GameObject;

// ============================================================================
// GameplayAbility - 游戏能力基类
// ============================================================================
// 用于定义可执行的能力（如技能、攻击、移动等）

class GameplayAbility
{
public:
    GameplayAbility() = default;
    virtual ~GameplayAbility() = default;

    // 能力名称
    std::string ability_name;

    // 能力标签
    GameplayTagContainer ability_tags;

    // 激活标签（激活此能力所需的标签）
    GameplayTagContainer activation_required_tags;

    // 阻止激活的标签（如果拥有这些标签，则无法激活）
    GameplayTagContainer activation_blocked_tags;

    // 冷却时间（秒）
    float cooldown_duration {0.0f};

    // 消耗（如消耗法力值）
    std::string cost_attribute_name;
    float cost_amount {0.0f};

    // 能力等级
    int32_t default_level {1};

    // 是否可以激活
    virtual bool CanActivateAbility(AbilitySystemComponent* asc, const GameplayAbilitySpec& spec) const;

    // 激活能力
    virtual void ActivateAbility(AbilitySystemComponent* asc, const GameplayAbilitySpec& spec);

    // 取消激活能力
    virtual void CancelAbility(AbilitySystemComponent* asc, const GameplayAbilitySpec& spec);

    // 结束能力
    virtual void EndAbility(AbilitySystemComponent* asc, const GameplayAbilitySpec& spec, bool was_cancelled);

    // 能力更新（每帧调用，如果能力处于激活状态）
    virtual void tickAbility(float delta_time, AbilitySystemComponent* asc, const GameplayAbilitySpec& spec) {}

protected:
    // 检查是否可以激活（内部使用）
    virtual bool CheckCost(AbilitySystemComponent* asc) const;
    virtual void CommitCost(AbilitySystemComponent* asc);

    // 能力状态
    bool m_IsActive {false};
};
