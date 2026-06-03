#pragma once

#include "GasTypes.h"

#include <string>
#include <unordered_map>

class AbilitySystemComponent;

// ============================================================================
// AttributeSet - 属性集基类
// ============================================================================
// 用于定义和管理游戏对象的属性（如生命值、法力值、攻击力等）
// 派生类应该定义具体的属性（如HealthAttributeSet, CombatAttributeSet等）

class AttributeSet
{
public:
    AttributeSet() = default;
    virtual ~AttributeSet() = default;

    // 初始化属性（在AbilitySystemComponent初始化时调用）
    virtual void InitializeAttributes();

    // 获取属性值
    AttributeValue* GetAttributeValue(const std::string& attribute_name);
    const AttributeValue* GetAttributeValue(const std::string& attribute_name) const;

    // 设置基础值
    void SetBaseValue(const std::string& attribute_name, float value);

    // 获取当前值
    float GetCurrentValue(const std::string& attribute_name) const;
    float GetBaseValue(const std::string& attribute_name) const;

    // 添加/移除修改器
    void AddModifier(const std::string& attribute_name, const AttributeModifier& modifier);
    void RemoveModifier(const std::string& attribute_name, const std::string& source_name);

    // 预修改回调（在属性值改变前调用，可以阻止修改）
    virtual void preAttributeChange(const std::string& attribute_name, float& new_value) {}

    // 后修改回调（在属性值改变后调用）
    virtual void postAttributeChange(const std::string& attribute_name, float old_value, float new_value) {}

    // 设置AbilitySystemComponent引用
    void setAbilitySystemComponent(AbilitySystemComponent* asc) { m_AbilitySystemComponent = asc; }
    AbilitySystemComponent* getAbilitySystemComponent() const { return m_AbilitySystemComponent; }

protected:
    // 注册属性（派生类在构造函数中调用）
    void RegisterAttribute(const std::string& name, const GameplayAttribute& attribute);

    // 属性存储
    std::unordered_map<std::string, AttributeValue> m_Attributes;
    std::unordered_map<std::string, GameplayAttribute> m_AttributeDefinitions;

private:
    AbilitySystemComponent* m_AbilitySystemComponent {nullptr};
};
