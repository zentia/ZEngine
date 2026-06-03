# ZEngine GAS系统快速开始指南

## 概述

本指南将帮助你快速上手ZEngine的GAS（Gameplay Ability System）系统。

## 基本使用

### 1. 创建属性集

首先，创建一个继承自`AttributeSet`的类来定义你的属性：

```cpp
#include "runtime/function/gas/attribute_set.h"

class HealthAttributeSet : public AttributeSet
{
public:
    HealthAttributeSet()
    {
        // 注册属性：名称、基础值、最小值、最大值
        registerAttribute("Health", GameplayAttribute("Health", 100.0f, 0.0f, 100.0f));
        registerAttribute("MaxHealth", GameplayAttribute("MaxHealth", 100.0f, 1.0f, 1000.0f));
        registerAttribute("Mana", GameplayAttribute("Mana", 50.0f, 0.0f, 100.0f));
        registerAttribute("AttackPower", GameplayAttribute("AttackPower", 10.0f, 0.0f, 0.0f));
    }
    
    // 属性变化回调
    void postAttributeChange(const std::string& attribute_name, float old_value, float new_value) override
    {
        if (attribute_name == "Health")
        {
            // 确保生命值不超过最大生命值
            float max_health = getCurrentValue("MaxHealth");
            if (new_value > max_health)
            {
                setBaseValue("Health", max_health);
            }
        }
    }
};
```

### 2. 创建能力

创建一个继承自`GameplayAbility`的类来定义能力：

```cpp
#include "runtime/function/gas/gameplay_ability.h"

class FireballAbility : public GameplayAbility
{
public:
    FireballAbility()
    {
        ability_name = "Fireball";
        
        // 设置能力标签
        ability_tags.addTag(GameplayTag("Ability.Spell.Damage"));
        ability_tags.addTag(GameplayTag("Ability.Spell.Fire"));
        
        // 激活所需标签（角色必须活着才能使用）
        activation_required_tags.addTag(GameplayTag("Character.Alive"));
        
        // 消耗10点法力值
        cost_attribute_name = "Mana";
        cost_amount = 10.0f;
        
        // 冷却时间2秒
        cooldown_duration = 2.0f;
    }
    
    void activateAbility(AbilitySystemComponent* asc, const GameplayAbilitySpec& spec) override
    {
        // 调用基类方法（检查条件、提交消耗）
        GameplayAbility::activateAbility(asc, spec);
        
        // 执行火球逻辑
        // 例如：创建火球对象、播放动画、造成伤害等
        // ...
    }
    
    void tickAbility(float delta_time, AbilitySystemComponent* asc, const GameplayAbilitySpec& spec) override
    {
        // 能力激活期间的每帧更新
        // 例如：更新火球位置、检查碰撞等
    }
    
    void endAbility(AbilitySystemComponent* asc, const GameplayAbilitySpec& spec, bool was_cancelled) override
    {
        GameplayAbility::endAbility(asc, spec, was_cancelled);
        
        // 清理工作
        // ...
    }
};
```

### 3. 创建效果

创建一个继承自`GameplayEffect`的类来定义Buff/Debuff：

```cpp
#include "runtime/function/gas/gameplay_effect.h"

class PoisonEffect : public GameplayEffect
{
public:
    PoisonEffect()
    {
        effect_name = "Poison";
        
        // 效果标签
        asset_tags.addTag(GameplayTag("Effect.Debuff.Damage"));
        asset_tags.addTag(GameplayTag("Effect.Debuff.Poison"));
        
        // 授予标签（应用效果时添加）
        granted_tags.addTag(GameplayTag("Status.Poisoned"));
        
        // 持续时间10秒
        duration = GameplayEffectDuration::HasDuration(10.0f);
        
        // 每2秒执行一次，应用时立即执行
        period = GameplayEffectPeriod::Periodic(2.0f, true);
        
        // 每周期减少5点生命值
        modifiers.push_back(GameplayEffectModifier(
            "Health",
            AttributeModifier::ModifierOp::Add,
            -5.0f
        ));
    }
    
    void onPeriodicExecute(GameplayEffectSpec* spec, AbilitySystemComponent* target_asc) override
    {
        // 周期性执行逻辑
        // 例如：播放粒子效果、播放音效等
    }
    
    void onEffectRemoved(GameplayEffectSpec* spec, AbilitySystemComponent* target_asc) override
    {
        // 效果移除时的清理工作
    }
};

// 力量Buff示例
class StrengthBuff : public GameplayEffect
{
public:
    StrengthBuff()
    {
        effect_name = "StrengthBuff";
        asset_tags.addTag(GameplayTag("Effect.Buff.Strength"));
        granted_tags.addTag(GameplayTag("Status.Strengthened"));
        
        // 无限持续时间（直到手动移除）
        duration = GameplayEffectDuration::Infinite();
        
        // 增加10点攻击力（乘法）
        modifiers.push_back(GameplayEffectModifier(
            "AttackPower",
            AttributeModifier::ModifierOp::Multiply,
            1.5f  // 增加50%
        ));
    }
};
```

### 4. 使用AbilitySystemComponent

在GameObject上使用AbilitySystemComponent：

```cpp
#include "runtime/function/gas/ability_system_component.h"

// 获取或创建AbilitySystemComponent
auto* asc = game_object->tryGetComponent<AbilitySystemComponent>("AbilitySystemComponent");
if (!asc)
{
    // 创建并添加组件
    asc = new AbilitySystemComponent();
    game_object->addComponent(asc);
}

// 设置属性集
asc->setAttributeSet(new HealthAttributeSet());

// 给予能力
auto* fireball = new FireballAbility();
asc->giveAbility(fireball, 1);  // 等级1

// 激活能力
if (asc->tryActivateAbility(fireball))
{
    // 能力激活成功
}

// 应用效果
auto* poison = new PoisonEffect();
GameplayEffectSpec* poison_spec = asc->applyEffect(poison, 1, source_object);

// 检查标签
if (asc->hasTag(GameplayTag("Status.Poisoned")))
{
    // 角色处于中毒状态
}

// 获取属性值
float health = asc->getAttributeValue("Health");
float mana = asc->getAttributeValue("Mana");

// 设置属性值
asc->setAttributeBaseValue("Health", 80.0f);

// 移除效果
asc->removeEffect(poison);

// 取消能力
asc->cancelAbility(fireball);
```

## 标签系统使用

### 创建和使用标签

```cpp
// 创建标签
GameplayTag character_tag("Character");
GameplayTag player_tag("Character.Player");
GameplayTag hero_tag("Character.Player.Hero");

// 添加标签
asc->addTag(character_tag);
asc->addTag(player_tag);

// 检查标签
if (asc->hasTag(player_tag))
{
    // 拥有Player标签
}

// 标签匹配（父标签匹配）
GameplayTag parent_tag("Character");
if (asc->hasTag(parent_tag))
{
    // 拥有Character或其子标签（如Character.Player）
}

// 移除标签
asc->removeTag(player_tag);
```

## 属性修改器类型

```cpp
// 加法修改器（增加/减少值）
AttributeModifier add_modifier(
    AttributeModifier::ModifierOp::Add,
    10.0f,  // 增加10
    "BuffSource"
);

// 乘法修改器（按比例修改）
AttributeModifier multiply_modifier(
    AttributeModifier::ModifierOp::Multiply,
    1.5f,  // 增加50%
    "BuffSource"
);

// 覆盖修改器（直接设置值）
AttributeModifier override_modifier(
    AttributeModifier::ModifierOp::Override,
    100.0f,  // 直接设置为100
    "BuffSource"
);
```

## 效果持续时间类型

```cpp
// 瞬时效果（立即执行并移除）
duration = GameplayEffectDuration::Instant();

// 有限持续时间（10秒）
duration = GameplayEffectDuration::HasDuration(10.0f);

// 无限持续时间（直到手动移除）
duration = GameplayEffectDuration::Infinite();
```

## 周期性效果

```cpp
// 每2秒执行一次，应用时立即执行
period = GameplayEffectPeriod::Periodic(2.0f, true);

// 每5秒执行一次，应用时不立即执行
period = GameplayEffectPeriod::Periodic(5.0f, false);
```

## 完整示例

```cpp
// 1. 创建属性集
class MyAttributeSet : public AttributeSet
{
public:
    MyAttributeSet()
    {
        registerAttribute("Health", GameplayAttribute("Health", 100.0f, 0.0f, 100.0f));
        registerAttribute("Mana", GameplayAttribute("Mana", 50.0f, 0.0f, 100.0f));
    }
};

// 2. 创建能力
class MyAbility : public GameplayAbility
{
public:
    MyAbility()
    {
        ability_name = "MyAbility";
        cost_attribute_name = "Mana";
        cost_amount = 10.0f;
    }
    
    void activateAbility(AbilitySystemComponent* asc, const GameplayAbilitySpec& spec) override
    {
        GameplayAbility::activateAbility(asc, spec);
        // 执行能力逻辑
    }
};

// 3. 使用
void setupCharacter(GameObject* character)
{
    auto* asc = new AbilitySystemComponent();
    character->addComponent(asc);
    
    asc->setAttributeSet(new MyAttributeSet());
    asc->giveAbility(new MyAbility(), 1);
    
    // 添加初始标签
    asc->addTag(GameplayTag("Character.Alive"));
}
```

## 注意事项

1. **内存管理**：GameplayAbility和GameplayEffect对象需要手动管理，建议使用资源系统
2. **属性集生命周期**：AttributeSet由AbilitySystemComponent管理，不需要手动删除
3. **标签命名**：使用层次化命名（如"Character.Player"），便于查询和匹配
4. **修改器顺序**：修改器按顺序应用（Add -> Multiply -> Override）
5. **效果过期**：效果在每帧更新时检查是否过期，过期效果会自动移除

## 下一步

- 查看[GAS系统设计文档](GAS_SYSTEM_DESIGN.md)了解详细设计
- 实现更复杂的能力和效果
- 集成到你的游戏项目中

