#pragma once

#include "AttributeSet.h"
#include "GameplayAbility.h"
#include "GameplayEffect.h"
#include "GasTypes.h"
#include "Runtime/Function/Framework/Component/Component.h"

#include <memory>
#include <unordered_map>
#include <vector>

// ============================================================================
// AbilitySystemComponent - 能力系统组件
// ============================================================================
// 管理所有能力、效果和属性的核心组件

class AbilitySystemComponent : public Component
{
public:
    AbilitySystemComponent() = default;
    ~AbilitySystemComponent() override;

    void PostLoadResource(GameObject* parent_object) override;
    void Tick(float delta_time) override;

    // ========================================================================
    // 属性管理
    // ========================================================================

    // 设置属性集
    void SetAttributeSet(AttributeSet* attr_set);
    AttributeSet* getAttributeSet() const { return m_AttributeSet.get(); }

    // ========================================================================
    // 标签管理
    // ========================================================================

    // 添加/移除标签
    void AddTag(const GameplayTag& tag);
    void RemoveTag(const GameplayTag& tag);
    bool HasTag(const GameplayTag& tag) const;
    bool HasAllTags(const GameplayTagContainer& tags) const;
    bool HasAnyTag(const GameplayTagContainer& tags) const;

    const GameplayTagContainer& getOwnedTags() const { return m_OwnedTags; }

    // ========================================================================
    // 能力管理
    // ========================================================================

    // 给予能力
    void GiveAbility(GameplayAbility* ability, int32_t level = 1);

    // 移除能力
    void RemoveAbility(GameplayAbility* ability);

    // 激活能力
    bool TryActivateAbility(GameplayAbility* ability);
    bool TryActivateAbilityByTag(const GameplayTag& ability_tag);

    // 取消激活能力
    void CancelAbility(GameplayAbility* ability);
    void CancelAbilitiesWithTag(const GameplayTag& tag);

    // 获取能力规格
    GameplayAbilitySpec* GetAbilitySpec(GameplayAbility* ability);

    // ========================================================================
    // 效果管理
    // ========================================================================

    // 应用效果
    GameplayEffectSpec* ApplyEffect(GameplayEffect* effect, int32_t level = 1, GameObject* source_object = nullptr);

    // 移除效果
    void RemoveEffect(GameplayEffect* effect);
    void RemoveEffectBySpec(GameplayEffectSpec* spec);

    // 获取效果规格
    std::vector<GameplayEffectSpec*> GetActiveEffects() const;
    std::vector<GameplayEffectSpec*> GetEffectsWithTag(const GameplayTag& tag) const;

    // ========================================================================
    // 属性访问（便捷方法）
    // ========================================================================

    float GetAttributeValue(const std::string& attribute_name) const;
    void SetAttributeBaseValue(const std::string& attribute_name, float value);

private:
    // 更新效果（处理周期性和过期）
    void UpdateEffects(float delta_time);

    // 更新能力（调用tick）
    void UpdateAbilities(float delta_time);

    // 属性集
    std::unique_ptr<AttributeSet> m_AttributeSet;

    // 标签
    GameplayTagContainer m_OwnedTags;

    // 能力
    std::unordered_map<GameplayAbility*, std::unique_ptr<GameplayAbilitySpec>> m_Abilities;
    std::vector<GameplayAbilitySpec*> m_ActiveAbilities;

    // 效果
    std::vector<std::unique_ptr<GameplayEffectSpec>> m_ActiveEffects;

    // 冷却时间管理（TODO: 实现）
    // std::unordered_map<GameplayAbility*, float> m_CooldownTimers;
};