#include "GameplayAbility.h"

#include "AbilitySystemComponent.h"
#include "AttributeSet.h"

bool GameplayAbility::CanActivateAbility(AbilitySystemComponent* asc, const GameplayAbilitySpec& spec) const
{
    if (!asc)
        return false;

    // 检查激活所需的标签
    if (!activation_required_tags.IsEmpty())
    {
        if (!asc->HasAllTags(activation_required_tags))
            return false;
    }

    // 检查阻止激活的标签
    if (!activation_blocked_tags.IsEmpty())
    {
        if (asc->HasAnyTag(activation_blocked_tags))
            return false;
    }

    // 检查冷却时间
    // TODO: 实现冷却时间检查

    // 检查消耗
    if (!CheckCost(asc))
        return false;

    return true;
}

void GameplayAbility::ActivateAbility(AbilitySystemComponent* asc, const GameplayAbilitySpec& spec)
{
    if (!CanActivateAbility(asc, spec))
        return;

    // 提交消耗
    CommitCost(asc);

    m_IsActive = true;
}

void GameplayAbility::CancelAbility(AbilitySystemComponent* asc, const GameplayAbilitySpec& spec)
{
    if (!m_IsActive)
        return;

    EndAbility(asc, spec, true);
}

void GameplayAbility::EndAbility(AbilitySystemComponent* asc, const GameplayAbilitySpec& spec, bool was_cancelled)
{
    m_IsActive = false;
}

bool GameplayAbility::CheckCost(AbilitySystemComponent* asc) const
{
    if (cost_attribute_name.empty() || cost_amount <= 0.0f)
        return true;

    if (!asc)
        return false;

    AttributeSet* attr_set = asc->getAttributeSet();
    if (!attr_set)
        return false;

    float current_value = attr_set->GetCurrentValue(cost_attribute_name);
    return current_value >= cost_amount;
}

void GameplayAbility::CommitCost(AbilitySystemComponent* asc)
{
    if (cost_attribute_name.empty() || cost_amount <= 0.0f)
        return;

    if (!asc)
        return;

    AttributeSet* attr_set = asc->getAttributeSet();
    if (!attr_set)
        return;

    float current_value = attr_set->GetCurrentValue(cost_attribute_name);
    float new_value = current_value - cost_amount;
    attr_set->SetBaseValue(cost_attribute_name, new_value);
}