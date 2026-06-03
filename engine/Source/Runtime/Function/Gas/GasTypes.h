#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Forward declarations
class GameObject;
class AbilitySystemComponent;
class GameplayAbility;
class GameplayEffect;

// ============================================================================
// GameplayTag - 游戏标签系统
// ============================================================================
// 用于分类和查询游戏对象、能力、效果等
// 格式: "Parent.Child.GrandChild" (类似UE的GameplayTag)
class GameplayTag
{
public:
    GameplayTag() = default;
    explicit GameplayTag(const std::string& tag_string)
        : m_TagString(tag_string) {}

    const std::string& ToString() const { return m_TagString; }
    bool IsValid() const { return !m_TagString.empty(); }
    bool IsEmpty() const { return m_TagString.empty(); }

    // 检查是否包含子标签 (例如: "Character" 包含 "Character.Player")
    bool MatchesTag(const GameplayTag& other) const;

    // 检查是否匹配完整标签
    bool MatchesTagExact(const GameplayTag& other) const;

    // 获取父标签
    GameplayTag GetParentTag() const;

    bool operator==(const GameplayTag& other) const { return m_TagString == other.m_TagString; }
    bool operator!=(const GameplayTag& other) const { return m_TagString != other.m_TagString; }
    bool operator<(const GameplayTag& other) const { return m_TagString < other.m_TagString; }

    // 用于hash
    size_t getHash() const { return std::hash<std::string> {}(m_TagString); }

private:
    std::string m_TagString;
};

// Hash function for GameplayTag (for use in unordered_set)
struct GameplayTagHash
{
    size_t operator()(const GameplayTag& tag) const { return tag.getHash(); }
};

// GameplayTagContainer - 标签容器
class GameplayTagContainer
{
public:
    GameplayTagContainer() = default;
    explicit GameplayTagContainer(const std::vector<GameplayTag>& tags);

    void AddTag(const GameplayTag& tag);
    void RemoveTag(const GameplayTag& tag);
    bool HasTag(const GameplayTag& tag) const;
    bool HasAnyTag(const GameplayTagContainer& other) const;
    bool HasAllTags(const GameplayTagContainer& other) const;

    void AppendTags(const GameplayTagContainer& other);
    void RemoveTags(const GameplayTagContainer& other);

    size_t size() const { return m_Tags.size(); }
    bool IsEmpty() const { return m_Tags.empty(); }

    std::vector<GameplayTag> getTags() const { return m_Tags; }

private:
    std::vector<GameplayTag> m_Tags;
};

// ============================================================================
// GameplayAttribute - 游戏属性
// ============================================================================
// 定义属性的元数据（名称、基础值等）

struct GameplayAttribute
{
    std::string name;  // 属性名称（如 "Health", "Mana"）
    float base_value;  // 基础值
    float min_value;   // 最小值
    float max_value;   // 最大值（0表示无限制）

    GameplayAttribute()
        : base_value(0.0f), min_value(0.0f), max_value(0.0f) {}
    GameplayAttribute(const std::string& name, float base, float min = 0.0f, float max = 0.0f)
        : name(name), base_value(base), min_value(min), max_value(max)
    {
    }
};

// ============================================================================
// AttributeValue - 属性值（当前值、基础值、修改器）
// ============================================================================

struct AttributeModifier
{
    enum class ModifierOp
    {
        Add,       // 加法
        Multiply,  // 乘法
        Override,  // 覆盖
    };

    ModifierOp operation;
    float value;
    std::string source_name;  // 修改器来源（用于调试）

    AttributeModifier(ModifierOp op, float val, const std::string& source = "")
        : operation(op), value(val), source_name(source)
    {
    }
};

class AttributeValue
{
public:
    AttributeValue()
        : m_BaseValue(0.0f), m_CurrentValue(0.0f) {}
    explicit AttributeValue(float base_value)
        : m_BaseValue(base_value), m_CurrentValue(base_value) {}

    float GetBaseValue() const { return m_BaseValue; }
    float GetCurrentValue() const { return m_CurrentValue; }

    void SetBaseValue(float value);
    void AddModifier(const AttributeModifier& modifier);
    void RemoveModifier(const std::string& source_name);
    void ClearModifiers();

    void Recalculate();

private:
    float m_BaseValue;
    float m_CurrentValue;
    std::vector<AttributeModifier> m_Modifiers;
};

// ============================================================================
// GameplayEffectDuration - 效果持续时间
// ============================================================================
class GameplayEffectDuration
{
public:
    enum class DurationType
    {
        Instant,      // 瞬时效果
        Infinite,     // 无限持续时间
        HasDuration,  // 有持续时间
    };

    DurationType type;
    float duration;  // 持续时间（秒），仅在HasDuration时有效

    GameplayEffectDuration()
        : type(DurationType::Instant), duration(0.0f) {}
    GameplayEffectDuration(DurationType duration_type, float duration_seconds)
        : type(duration_type), duration(duration_seconds)
    {
    }
    static GameplayEffectDuration Instant() { return GameplayEffectDuration {DurationType::Instant, 0.0f}; }
    static GameplayEffectDuration Infinite() { return GameplayEffectDuration {DurationType::Infinite, 0.0f}; }
    static GameplayEffectDuration HasDuration(float seconds)
    {
        return GameplayEffectDuration {DurationType::HasDuration, seconds};
    }
};

// ============================================================================
// GameplayEffectPeriod - 效果周期（用于周期性效果）
// ============================================================================
class GameplayEffectPeriod
{
public:
    bool is_periodic;
    float period;                  // 周期（秒）
    float execute_on_application;  // 是否在应用时立即执行一次

    GameplayEffectPeriod()
        : is_periodic(false), period(0.0f), execute_on_application(false) {}
    static GameplayEffectPeriod Periodic(float period_seconds, bool execute_on_app = true)
    {
        GameplayEffectPeriod result;
        result.is_periodic = true;
        result.period = period_seconds;
        result.execute_on_application = execute_on_app;
        return result;
    }
};

// ============================================================================
// GameplayEffectModifier - 效果修改器
// ============================================================================
class GameplayEffectModifier
{
public:
    std::string attribute_name;  // 要修改的属性名称
    AttributeModifier::ModifierOp operation;
    float magnitude;  // 修改量

    GameplayEffectModifier()
        : operation(AttributeModifier::ModifierOp::Add), magnitude(0.0f) {}
    GameplayEffectModifier(const std::string& attr_name, AttributeModifier::ModifierOp op, float mag)
        : attribute_name(attr_name), operation(op), magnitude(mag)
    {
    }
};

// ============================================================================
// GameplayEffectStacking - 效果堆叠规则
// ============================================================================
class GameplayEffectStacking
{
public:
    enum class StackingType
    {
        None,               // 不堆叠
        AggregateBySource,  // 按来源聚合
        AggregateByTarget,  // 按目标聚合
    };

    StackingType type;
    int32_t max_count;  // 最大堆叠数（0表示无限制）

    GameplayEffectStacking()
        : type(StackingType::None), max_count(1) {}
};

// ============================================================================
// GameplayAbilityActivationInfo - 能力激活信息
// ============================================================================

struct GameplayAbilityActivationInfo
{
    GameplayTag activation_tag;
    bool is_predicted;      // 是否预测激活
    float activation_time;  // 激活时间
};

// ============================================================================
// GameplayAbilitySpec - 能力规格（实例化的能力）
// ============================================================================

class GameplayAbilitySpec
{
public:
    GameplayAbilitySpec()
        : m_Ability(nullptr), m_Level(1), m_IsActive(false) {}

    void setAbility(GameplayAbility* ability) { m_Ability = ability; }
    GameplayAbility* getAbility() const { return m_Ability; }

    void setLevel(int32_t level) { m_Level = level; }
    int32_t getLevel() const { return m_Level; }

    void setActive(bool active) { m_IsActive = active; }
    bool isActive() const { return m_IsActive; }

    const GameplayTagContainer& getAbilityTags() const { return m_AbilityTags; }
    void setAbilityTags(const GameplayTagContainer& tags) { m_AbilityTags = tags; }

private:
    GameplayAbility* m_Ability;
    int32_t m_Level;
    bool m_IsActive;
    GameplayTagContainer m_AbilityTags;
};

// ============================================================================
// GameplayEffectSpec - 效果规格（实例化的效果）
// ============================================================================

class GameplayEffectSpec
{
public:
    GameplayEffectSpec()
        : m_Effect(nullptr), m_Level(1), m_Duration(0.0f), m_RemainingTime(0.0f) {}

    void setEffect(GameplayEffect* effect) { m_Effect = effect; }
    GameplayEffect* getEffect() const { return m_Effect; }

    void setLevel(int32_t level) { m_Level = level; }
    int32_t getLevel() const { return m_Level; }

    void setDuration(float duration)
    {
        m_Duration = duration;
        m_RemainingTime = duration;
    }
    float getDuration() const { return m_Duration; }
    float getRemainingTime() const { return m_RemainingTime; }
    void updateRemainingTime(float delta_time) { m_RemainingTime -= delta_time; }
    bool isExpired() const { return m_RemainingTime <= 0.0f && m_Duration > 0.0f; }

    const GameplayTagContainer& getGrantedTags() const { return m_GrantedTags; }
    void setGrantedTags(const GameplayTagContainer& tags) { m_GrantedTags = tags; }

    GameObject* getSourceObject() const { return m_SourceObject; }
    void setSourceObject(GameObject* obj) { m_SourceObject = obj; }

private:
    GameplayEffect* m_Effect;
    int32_t m_Level;
    float m_Duration;
    float m_RemainingTime;
    GameplayTagContainer m_GrantedTags;
    GameObject* m_SourceObject;
};
