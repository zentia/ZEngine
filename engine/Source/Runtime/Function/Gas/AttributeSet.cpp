#include "AttributeSet.h"

#include "AbilitySystemComponent.h"

void AttributeSet::InitializeAttributes()
{
    // 根据属性定义初始化所有属性
    for (const auto& pair : m_AttributeDefinitions)
    {
        const std::string& name = pair.first;
        const GameplayAttribute& attr_def = pair.second;

        AttributeValue& attr_value = m_Attributes[name];
        attr_value.SetBaseValue(attr_def.base_value);
    }
}

AttributeValue* AttributeSet::GetAttributeValue(const std::string& attribute_name)
{
    auto it = m_Attributes.find(attribute_name);
    if (it != m_Attributes.end())
    {
        return &it->second;
    }
    return nullptr;
}

const AttributeValue* AttributeSet::GetAttributeValue(const std::string& attribute_name) const
{
    auto it = m_Attributes.find(attribute_name);
    if (it != m_Attributes.end())
    {
        return &it->second;
    }
    return nullptr;
}

void AttributeSet::SetBaseValue(const std::string& attribute_name, float value)
{
    auto it = m_Attributes.find(attribute_name);
    if (it != m_Attributes.end())
    {
        float old_value = it->second.GetCurrentValue();
        float new_value = value;

        // 调用预修改回调
        preAttributeChange(attribute_name, new_value);

        // 应用限制
        auto def_it = m_AttributeDefinitions.find(attribute_name);
        if (def_it != m_AttributeDefinitions.end())
        {
            const GameplayAttribute& attr_def = def_it->second;
            if (attr_def.min_value != 0.0f || attr_def.max_value != 0.0f)
            {
                if (attr_def.min_value != 0.0f && new_value < attr_def.min_value)
                    new_value = attr_def.min_value;
                if (attr_def.max_value != 0.0f && new_value > attr_def.max_value)
                    new_value = attr_def.max_value;
            }
        }

        it->second.SetBaseValue(new_value);

        // 调用后修改回调
        postAttributeChange(attribute_name, old_value, new_value);
    }
}

float AttributeSet::GetCurrentValue(const std::string& attribute_name) const
{
    const AttributeValue* attr_value = GetAttributeValue(attribute_name);
    if (attr_value)
    {
        return attr_value->GetCurrentValue();
    }
    return 0.0f;
}

float AttributeSet::GetBaseValue(const std::string& attribute_name) const
{
    const AttributeValue* attr_value = GetAttributeValue(attribute_name);
    if (attr_value)
    {
        return attr_value->GetBaseValue();
    }
    return 0.0f;
}

void AttributeSet::AddModifier(const std::string& attribute_name, const AttributeModifier& modifier)
{
    AttributeValue* attr_value = GetAttributeValue(attribute_name);
    if (attr_value)
    {
        float old_value = attr_value->GetCurrentValue();
        attr_value->AddModifier(modifier);
        float new_value = attr_value->GetCurrentValue();

        // 调用后修改回调
        postAttributeChange(attribute_name, old_value, new_value);
    }
}

void AttributeSet::RemoveModifier(const std::string& attribute_name, const std::string& source_name)
{
    AttributeValue* attr_value = GetAttributeValue(attribute_name);
    if (attr_value)
    {
        float old_value = attr_value->GetCurrentValue();
        attr_value->RemoveModifier(source_name);
        float new_value = attr_value->GetCurrentValue();

        // 调用后修改回调
        postAttributeChange(attribute_name, old_value, new_value);
    }
}

void AttributeSet::RegisterAttribute(const std::string& name, const GameplayAttribute& attribute)
{
    m_AttributeDefinitions[name] = attribute;
}
