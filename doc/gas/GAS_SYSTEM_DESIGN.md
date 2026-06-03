# ZEngine GAS系统设计文档

## 概述

ZEngine GAS（Gameplay Ability System）系统是基于Unreal Engine的GAS系统设计的游戏玩法能力系统。该系统提供了完整的能力、效果和属性管理框架，用于实现技能系统、Buff/Debuff、属性修改等游戏玩法功能。

## 核心特性

### 1. 能力系统（GameplayAbility）
- 可执行的能力（技能、攻击、移动等）
- 能力激活条件检查（标签、消耗、冷却等）
- 能力生命周期管理（激活、更新、结束）
- 能力等级系统

### 2. 效果系统（GameplayEffect）
- Buff/Debuff效果
- 属性修改（加法、乘法、覆盖）
- 持续时间管理（瞬时、有限、无限）
- 周期性效果（如每2秒造成伤害）
- 效果堆叠规则

### 3. 属性系统（AttributeSet）
- 属性定义和管理
- 属性修改器系统
- 属性值计算（基础值 + 修改器）
- 属性变化回调

### 4. 标签系统（GameplayTag）
- 层次化标签（如 "Character.Player.Hero"）
- 标签匹配和查询
- 标签容器管理

### 5. 能力系统组件（AbilitySystemComponent）
- 统一管理所有能力、效果和属性
- 标签管理
- 能力激活和取消
- 效果应用和移除
- 属性访问接口

## 系统架构

### 核心组件

```
┌─────────────────────────────────────────────────────────────┐
│              AbilitySystemComponent                          │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐      │
│  │   Abilities  │  │   Effects    │  │  Attributes   │      │
│  └──────────────┘  └──────────────┘  └──────────────┘      │
│  ┌──────────────┐  ┌──────────────┐                        │
│  │     Tags     │  │  Cooldowns   │                        │
│  └──────────────┘  └──────────────┘                        │
└─────────────────────────────────────────────────────────────┘
```

#### 1. GameplayTag (gas_types.h/cpp)
游戏标签系统，用于分类和查询：
- 层次化标签结构（"Parent.Child.GrandChild"）
- 标签匹配（完全匹配、父标签匹配）
- 标签容器管理

#### 2. AttributeSet (attribute_set.h/cpp)
属性集基类，管理游戏对象的属性：
- 属性注册和定义
- 属性值计算（基础值 + 修改器）
- 属性变化回调
- 属性限制（最小值、最大值）

#### 3. GameplayEffect (gameplay_effect.h)
游戏效果定义：
- 属性修改器
- 标签授予/移除
- 持续时间管理
- 周期性效果
- 堆叠规则

#### 4. GameplayAbility (gameplay_ability.h/cpp)
游戏能力基类：
- 能力激活条件
- 能力消耗（属性消耗）
- 能力生命周期
- 能力更新

#### 5. AbilitySystemComponent (ability_system_component.h/cpp)
能力系统组件：
- 能力管理（给予、移除、激活、取消）
- 效果管理（应用、移除、更新）
- 属性管理（通过AttributeSet）
- 标签管理

## 数据结构

### GameplayTag
```cpp
class GameplayTag
{
    std::string m_tag_string;  // 如 "Character.Player"
};
```

### AttributeValue
```cpp
class AttributeValue
{
    float m_base_value;                    // 基础值
    float m_current_value;                 // 当前值（基础值 + 修改器）
    std::vector<AttributeModifier> m_modifiers;  // 修改器列表
};
```

### GameplayEffectSpec
```cpp
class GameplayEffectSpec
{
    GameplayEffect* m_effect;      // 效果定义
    int32_t m_level;               // 效果等级
    float m_duration;              // 持续时间
    float m_remaining_time;         // 剩余时间
    GameplayTagContainer m_granted_tags;  // 授予的标签
    GameObject* m_source_object;    // 来源对象
};
```

### GameplayAbilitySpec
```cpp
class GameplayAbilitySpec
{
    GameplayAbility* m_ability;     // 能力定义
    int32_t m_level;                // 能力等级
    bool m_is_active;               // 是否激活
    GameplayTagContainer m_ability_tags;  // 能力标签
};
```

## 使用示例

### 1. 创建属性集

```cpp
class HealthAttributeSet : public AttributeSet
{
public:
    HealthAttributeSet()
    {
        // 注册属性
        registerAttribute("Health", GameplayAttribute("Health", 100.0f, 0.0f, 100.0f));
        registerAttribute("MaxHealth", GameplayAttribute("MaxHealth", 100.0f, 1.0f, 1000.0f));
        registerAttribute("Mana", GameplayAttribute("Mana", 50.0f, 0.0f, 100.0f));
    }
    
    void postAttributeChange(const std::string& attribute_name, float old_value, float new_value) override
    {
        if (attribute_name == "Health")
        {
            // 限制生命值在0到最大生命值之间
            float max_health = getCurrentValue("MaxHealth");
            if (new_value > max_health)
            {
                setBaseValue("Health", max_health);
            }
            else if (new_value < 0.0f)
            {
                setBaseValue("Health", 0.0f);
            }
        }
    }
};
```

### 2. 创建能力

```cpp
class FireballAbility : public GameplayAbility
{
public:
    FireballAbility()
    {
        ability_name = "Fireball";
        ability_tags.addTag(GameplayTag("Ability.Spell.Damage"));
        activation_required_tags.addTag(GameplayTag("Character.Alive"));
        cost_attribute_name = "Mana";
        cost_amount = 10.0f;
        cooldown_duration = 2.0f;
    }
    
    void activateAbility(AbilitySystemComponent* asc, const GameplayAbilitySpec& spec) override
    {
        GameplayAbility::activateAbility(asc, spec);
        
        // 执行火球逻辑
        // 例如：创建火球对象、播放动画等
    }
};
```

### 3. 创建效果

```cpp
class PoisonEffect : public GameplayEffect
{
public:
    PoisonEffect()
    {
        effect_name = "Poison";
        asset_tags.addTag(GameplayTag("Effect.Debuff.Damage"));
        granted_tags.addTag(GameplayTag("Status.Poisoned"));
        
        // 每2秒造成5点伤害
        period = GameplayEffectPeriod::Periodic(2.0f, true);
        duration = GameplayEffectDuration::HasDuration(10.0f);
        
        // 修改器：每周期减少5点生命值
        modifiers.push_back(GameplayEffectModifier(
            "Health",
            AttributeModifier::ModifierOp::Add,
            -5.0f
        ));
    }
    
    void onPeriodicExecute(GameplayEffectSpec* spec, AbilitySystemComponent* target_asc) override
    {
        // 周期性执行逻辑
        // 这里可以添加额外的效果，如播放粒子效果等
    }
};
```

### 4. 使用AbilitySystemComponent

```cpp
// 在GameObject上添加AbilitySystemComponent
auto* asc = game_object->tryGetComponent<AbilitySystemComponent>("AbilitySystemComponent");

// 设置属性集
asc->setAttributeSet(new HealthAttributeSet());

// 给予能力
auto* fireball = new FireballAbility();
asc->giveAbility(fireball, 1);

// 激活能力
asc->tryActivateAbility(fireball);

// 应用效果
auto* poison = new PoisonEffect();
asc->applyEffect(poison, 1, source_object);

// 检查标签
if (asc->hasTag(GameplayTag("Status.Poisoned")))
{
    // 处理中毒状态
}

// 获取属性值
float health = asc->getAttributeValue("Health");
```

## 工作流程

### 1. 初始化
```cpp
// 创建AbilitySystemComponent
auto* asc = new AbilitySystemComponent();
game_object->addComponent(asc);

// 设置属性集
asc->setAttributeSet(new HealthAttributeSet());

// 给予初始能力
asc->giveAbility(new FireballAbility(), 1);
```

### 2. 能力激活流程
```
1. tryActivateAbility() 被调用
2. canActivateAbility() 检查激活条件
   - 检查标签
   - 检查冷却时间
   - 检查消耗
3. commitCost() 提交消耗
4. activateAbility() 激活能力
5. 能力进入激活状态，每帧调用tickAbility()
6. endAbility() 结束能力
```

### 3. 效果应用流程
```
1. applyEffect() 被调用
2. canApplyEffect() 检查是否可以应用
3. 创建GameplayEffectSpec
4. 应用属性修改器
5. 添加/移除标签
6. onEffectApplied() 回调
7. 每帧更新效果（周期性、持续时间）
8. 效果过期时自动移除
```

### 4. 属性修改流程
```
1. 添加修改器（通过效果或直接调用）
2. AttributeValue::addModifier()
3. AttributeValue::recalculate()
   - 先应用加法修改器
   - 再应用乘法修改器
   - 最后应用覆盖修改器
4. postAttributeChange() 回调
```

## 与UE GAS的对比

### 相似之处
- 核心概念相同（Ability、Effect、Attribute、Tag）
- 属性修改器系统
- 标签系统
- 效果持续时间管理
- 能力激活条件检查

### 实现差异
- **简化设计**：ZEngine版本更简化，去除了UE的一些复杂特性（如预测、复制等）
- **组件化**：AbilitySystemComponent作为Component集成到GameObject系统
- **反射系统**：使用ZEngine的反射系统进行序列化
- **资源管理**：使用ZEngine的资源管理系统

## 文件结构

```
engine/source/runtime/function/gas/
├── gas_types.h              # 核心类型定义
├── gas_types.cpp            # 核心类型实现
├── attribute_set.h           # 属性集基类
├── attribute_set.cpp         # 属性集实现
├── gameplay_effect.h         # 游戏效果定义
├── gameplay_ability.h        # 游戏能力基类
├── gameplay_ability.cpp      # 游戏能力实现
├── ability_system_component.h    # 能力系统组件
└── ability_system_component.cpp  # 能力系统组件实现

engine/source/runtime/resource/res_type/data/
└── gas_asset.h              # GAS资源定义（用于序列化）
```

## 未来改进

1. **冷却时间系统**
   - 实现完整的冷却时间管理
   - 冷却时间显示和查询

2. **能力任务系统（AbilityTask）**
   - 异步能力执行
   - 等待条件（如等待动画完成、等待输入等）

3. **效果堆叠增强**
   - 更复杂的堆叠规则
   - 堆叠效果合并

4. **预测系统**
   - 客户端预测能力激活
   - 服务器验证和回滚

5. **网络复制**
   - 能力状态同步
   - 效果同步
   - 属性同步

6. **编辑器支持**
   - 可视化能力编辑器
   - 效果编辑器
   - 属性集编辑器

7. **性能优化**
   - 标签查询优化
   - 效果更新优化
   - 属性计算缓存

## 注意事项

1. **属性集生命周期**：AttributeSet由AbilitySystemComponent管理，不需要手动删除
2. **能力生命周期**：GameplayAbility对象需要手动管理，建议使用资源系统
3. **效果生命周期**：GameplayEffect对象需要手动管理，建议使用资源系统
4. **标签匹配**：标签匹配支持父标签匹配，注意标签命名规范
5. **属性修改顺序**：修改器按顺序应用（Add -> Multiply -> Override）
6. **效果过期**：效果在每帧更新时检查是否过期，过期效果会自动移除

## 集成点

- `GameObject`：通过Component系统集成
- `AssetManager`：用于加载能力/效果资源
- `Reflection System`：用于序列化和反序列化
- `WorldManager`：在游戏世界中管理AbilitySystemComponent

