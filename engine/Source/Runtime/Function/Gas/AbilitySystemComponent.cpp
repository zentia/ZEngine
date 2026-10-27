#include "AbilitySystemComponent.h"

#include "Runtime/BaseClasses/GameObject.h"

#include <algorithm>

AbilitySystemComponent::~AbilitySystemComponent()
{
    // 清理所有激活的能力
    for (auto* spec : m_ActiveAbilities)
    {
        if (spec && spec->getAbility())
        {
            spec->getAbility()->EndAbility(this, *spec, true);
        }
    }
    m_ActiveAbilities.clear();

    // 清理所有激活的效果
    for (auto& spec : m_ActiveEffects)
    {
        if (spec && spec->getEffect())
        {
            spec->getEffect()->onEffectRemoved(spec.get(), this);
        }
    }
    m_ActiveEffects.clear();

    m_Abilities.clear();
}

void AbilitySystemComponent::PostLoadResource(GameObject* parent_object)
{
    Component::PostLoadResource(parent_object);

    // 初始化属性集
    if (m_AttributeSet)
    {
        m_AttributeSet->setAbilitySystemComponent(this);
        m_AttributeSet->InitializeAttributes();
    }
}

void AbilitySystemComponent::Tick(float delta_time)
{
    Component::Tick(delta_time);

    // 更新效果
    UpdateEffects(delta_time);

    // 更新能力
    UpdateAbilities(delta_time);

    // 更新冷却时间
    UpdateCooldowns(delta_time);
}

void AbilitySystemComponent::SetAttributeSet(AttributeSet* attr_set)
{
    m_AttributeSet.reset(attr_set);
    if (m_AttributeSet)
    {
        m_AttributeSet->setAbilitySystemComponent(this);
        m_AttributeSet->InitializeAttributes();
    }
}

void AbilitySystemComponent::AddTag(const GameplayTag& tag)
{
    if (tag.IsValid())
    {
        m_OwnedTags.AddTag(tag);
    }
}

void AbilitySystemComponent::RemoveTag(const GameplayTag& tag)
{
    m_OwnedTags.RemoveTag(tag);
}

bool AbilitySystemComponent::HasTag(const GameplayTag& tag) const
{
    return m_OwnedTags.HasTag(tag);
}

bool AbilitySystemComponent::HasAllTags(const GameplayTagContainer& tags) const
{
    return m_OwnedTags.HasAllTags(tags);
}

bool AbilitySystemComponent::HasAnyTag(const GameplayTagContainer& tags) const
{
    return m_OwnedTags.HasAnyTag(tags);
}

void AbilitySystemComponent::GiveAbility(GameplayAbility* ability, int32_t level)
{
    if (!ability)
        return;

    auto spec = std::make_unique<GameplayAbilitySpec>();
    spec->setAbility(ability);
    spec->setLevel(level);
    spec->setAbilityTags(ability->ability_tags);

    m_Abilities[ability] = std::move(spec);
}

void AbilitySystemComponent::RemoveAbility(GameplayAbility* ability)
{
    if (!ability)
        return;

    // 如果能力正在激活，先取消
    auto* spec = GetAbilitySpec(ability);
    if (spec && spec->isActive())
    {
        CancelAbility(ability);
    }

    m_Abilities.erase(ability);
}

bool AbilitySystemComponent::TryActivateAbility(GameplayAbility* ability)
{
    if (!ability)
        return false;

    auto* spec = GetAbilitySpec(ability);
    if (!spec)
        return false;

    if (spec->isActive())
        return false;

    if (!ability->CanActivateAbility(this, *spec))
        return false;

    ability->ActivateAbility(this, *spec);
    spec->setActive(true);
    m_ActiveAbilities.push_back(spec);

    // 起冷却（仅当此能力配置了冷却时长）。剩余冷却存在 spec 上，由 UpdateCooldowns
    // 每帧递减；CanActivateAbility 会在冷却未结束时拒绝再次激活。
    if (ability->cooldown_duration > 0.0f)
        spec->StartCooldown(ability->cooldown_duration);

    return true;
}

bool AbilitySystemComponent::TryActivateAbilityByTag(const GameplayTag& ability_tag)
{
    for (auto& pair : m_Abilities)
    {
        GameplayAbility* ability = pair.first;
        if (ability && ability->ability_tags.HasTag(ability_tag))
        {
            if (TryActivateAbility(ability))
                return true;
        }
    }
    return false;
}

void AbilitySystemComponent::CancelAbility(GameplayAbility* ability)
{
    if (!ability)
        return;

    auto* spec = GetAbilitySpec(ability);
    if (!spec || !spec->isActive())
        return;

    ability->CancelAbility(this, *spec);
    spec->setActive(false);

    // 从激活列表中移除
    m_ActiveAbilities.erase(std::remove(m_ActiveAbilities.begin(), m_ActiveAbilities.end(), spec),
                            m_ActiveAbilities.end());
}

void AbilitySystemComponent::CancelAbilitiesWithTag(const GameplayTag& tag)
{
    std::vector<GameplayAbility*> to_cancel;

    for (auto& pair : m_Abilities)
    {
        GameplayAbility* ability = pair.first;
        if (ability && ability->ability_tags.HasTag(tag))
        {
            auto* spec = GetAbilitySpec(ability);
            if (spec && spec->isActive())
            {
                to_cancel.push_back(ability);
            }
        }
    }

    for (auto* ability : to_cancel)
    {
        CancelAbility(ability);
    }
}

GameplayAbilitySpec* AbilitySystemComponent::GetAbilitySpec(GameplayAbility* ability)
{
    auto it = m_Abilities.find(ability);
    if (it != m_Abilities.end())
    {
        return it->second.get();
    }
    return nullptr;
}

GameplayEffectSpec*
AbilitySystemComponent::ApplyEffect(GameplayEffect* effect, int32_t level, GameObject* source_object)
{
    if (!effect)
        return nullptr;

    // 检查是否可以应用
    if (!effect->canApplyEffect(this, nullptr))
        return nullptr;

    // 创建效果规格
    auto spec = std::make_unique<GameplayEffectSpec>();
    spec->setEffect(effect);
    spec->setLevel(level);
    spec->setSourceObject(source_object);
    spec->setGrantedTags(effect->granted_tags);

    // 设置持续时间
    if (effect->duration.type == GameplayEffectDuration::DurationType::HasDuration)
    {
        spec->setDuration(effect->duration.duration);
    }
    else if (effect->duration.type == GameplayEffectDuration::DurationType::Infinite)
    {
        spec->setDuration(-1.0f);  // 负数表示无限
    }

    // 应用修改器
    if (m_AttributeSet)
    {
        for (const auto& modifier : effect->modifiers)
        {
            AttributeModifier attr_modifier(modifier.operation, modifier.magnitude, effect->effect_name);
            m_AttributeSet->AddModifier(modifier.attribute_name, attr_modifier);
        }
    }

    // 添加授予的标签
    m_OwnedTags.AppendTags(effect->granted_tags);

    // 移除标签
    m_OwnedTags.RemoveTags(effect->removed_tags);

    // 调用效果应用回调
    effect->onEffectApplied(spec.get(), this);

    // 周期性效果：应用时立即执行一次（execute_on_application），并把周期计时归零，
    // 避免首个周期在后续 Update 里被补触发重复执行。
    if (effect->period.is_periodic && effect->period.execute_on_application)
    {
        effect->onPeriodicExecute(spec.get(), this);
        spec->ResetPeriodic();
    }

    GameplayEffectSpec* spec_ptr = spec.get();
    m_ActiveEffects.push_back(std::move(spec));

    return spec_ptr;
}

void AbilitySystemComponent::RemoveEffect(GameplayEffect* effect)
{
    if (!effect)
        return;

    for (auto it = m_ActiveEffects.begin(); it != m_ActiveEffects.end(); ++it)
    {
        if ((*it)->getEffect() == effect)
        {
            RemoveEffectBySpec(it->get());
            m_ActiveEffects.erase(it);
            return;
        }
    }
}

void AbilitySystemComponent::RemoveEffectBySpec(GameplayEffectSpec* spec)
{
    if (!spec || !spec->getEffect())
        return;

    GameplayEffect* effect = spec->getEffect();

    // 移除修改器
    if (m_AttributeSet)
    {
        for (const auto& modifier : effect->modifiers)
        {
            m_AttributeSet->RemoveModifier(modifier.attribute_name, effect->effect_name);
        }
    }

    // 移除授予的标签
    m_OwnedTags.RemoveTags(effect->granted_tags);

    // 调用效果移除回调
    effect->onEffectRemoved(spec, this);
}

std::vector<GameplayEffectSpec*> AbilitySystemComponent::GetActiveEffects() const
{
    std::vector<GameplayEffectSpec*> result;
    for (const auto& spec : m_ActiveEffects)
    {
        result.push_back(spec.get());
    }
    return result;
}

std::vector<GameplayEffectSpec*> AbilitySystemComponent::GetEffectsWithTag(const GameplayTag& tag) const
{
    std::vector<GameplayEffectSpec*> result;
    for (const auto& spec : m_ActiveEffects)
    {
        if (spec && spec->getEffect() && spec->getEffect()->asset_tags.HasTag(tag))
        {
            result.push_back(spec.get());
        }
    }
    return result;
}

float AbilitySystemComponent::GetAttributeValue(const std::string& attribute_name) const
{
    if (m_AttributeSet)
    {
        return m_AttributeSet->GetCurrentValue(attribute_name);
    }
    return 0.0f;
}

void AbilitySystemComponent::SetAttributeBaseValue(const std::string& attribute_name, float value)
{
    if (m_AttributeSet)
    {
        m_AttributeSet->SetBaseValue(attribute_name, value);
    }
}

void AbilitySystemComponent::UpdateEffects(float delta_time)
{
    // 更新周期性效果
    for (auto& spec : m_ActiveEffects)
    {
        if (!spec || !spec->getEffect())
            continue;

        GameplayEffect* effect = spec->getEffect();

        // 处理周期性效果：推进计时，本帧跨过多少个周期就触发多少次
        // （低帧率/卡顿下一次 Update 可能跨过多个 period，需补触发以免丢拍）。
        if (effect->period.is_periodic && gas_periodic::IsValidPeriod(effect->period.period))
        {
            const int exec_count = spec->StepPeriodic(effect->period.period, delta_time);
            for (int i = 0; i < exec_count; ++i)
            {
                effect->onPeriodicExecute(spec.get(), this);
            }
        }

        // 更新持续时间
        if (spec->getDuration() > 0.0f)
        {
            spec->updateRemainingTime(delta_time);

            // 检查是否过期
            if (spec->isExpired())
            {
                RemoveEffectBySpec(spec.get());
            }
        }
    }

    // 移除已过期的效果
    m_ActiveEffects.erase(std::remove_if(m_ActiveEffects.begin(),
                                         m_ActiveEffects.end(),
                                         [](const std::unique_ptr<GameplayEffectSpec>& spec) {
                                             return spec->getDuration() > 0.0f && spec->isExpired();
                                         }),
                          m_ActiveEffects.end());
}

void AbilitySystemComponent::UpdateAbilities(float delta_time)
{
    for (auto* spec : m_ActiveAbilities)
    {
        if (spec && spec->getAbility() && spec->isActive())
        {
            spec->getAbility()->tickAbility(delta_time, this, *spec);
        }
    }
}

void AbilitySystemComponent::UpdateCooldowns(float delta_time)
{
    // 冷却对所有已授予的能力递减（不论是否激活）——与 UE GAS 一致：能力结束后
    // 冷却仍继续走，冷却未结束前 CanActivateAbility 会拒绝再次激活。
    // 只对真正在冷却中的 spec 做写入，避免无谓地触碰其余 spec。
    for (auto& pair : m_Abilities)
    {
        auto* spec = pair.second.get();
        if (spec != nullptr && spec->IsOnCooldown())
        {
            spec->StepCooldown(delta_time);
        }
    }
}