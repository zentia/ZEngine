#include "GasTypes.h"

#include <algorithm>
#include <sstream>

// ============================================================================
// GameplayTag Implementation
// ============================================================================

bool GameplayTag::MatchesTag(const GameplayTag& other) const
{
    if (m_TagString.empty() || other.m_TagString.empty())
        return false;

    // 检查是否完全匹配
    if (m_TagString == other.m_TagString)
        return true;

    // 检查是否是父标签（例如: "Character" 匹配 "Character.Player"）
    if (other.m_TagString.find(m_TagString) == 0)
    {
        // 确保是完整的标签段（不是部分匹配）
        if (other.m_TagString.length() > m_TagString.length())
        {
            return other.m_TagString[m_TagString.length()] == '.';
        }
    }

    return false;
}

bool GameplayTag::MatchesTagExact(const GameplayTag& other) const
{
    return m_TagString == other.m_TagString;
}

GameplayTag GameplayTag::GetParentTag() const
{
    if (m_TagString.empty())
        return GameplayTag();

    size_t last_dot = m_TagString.find_last_of('.');
    if (last_dot == std::string::npos)
        return GameplayTag();

    return GameplayTag(m_TagString.substr(0, last_dot));
}

// ============================================================================
// GameplayTagContainer Implementation
// ============================================================================

GameplayTagContainer::GameplayTagContainer(const std::vector<GameplayTag>& tags)
{
    for (const auto& tag : tags)
    {
        m_Tags.push_back(tag);
    }
}

void GameplayTagContainer::AddTag(const GameplayTag& tag)
{
    if (tag.IsValid())
    {
        m_Tags.push_back(tag);
    }
}

void GameplayTagContainer::RemoveTag(const GameplayTag& tag)
{
    m_Tags.erase(std::remove_if(m_Tags.begin(),
                                m_Tags.end(),
                                [tag](const GameplayTag& t) { return t.getHash() == tag.getHash(); }),
                 m_Tags.end());
}

bool GameplayTagContainer::HasTag(const GameplayTag& tag) const
{
    auto it = std::find_if(
        m_Tags.begin(), m_Tags.end(), [tag](const GameplayTag& t) { return t.getHash() == tag.getHash(); });
    if (it != m_Tags.end())
        return true;

    // 检查是否有父标签匹配
    for (const auto& existing_tag : m_Tags)
    {
        if (existing_tag.MatchesTag(tag))
            return true;
    }

    return false;
}

bool GameplayTagContainer::HasAnyTag(const GameplayTagContainer& other) const
{
    for (const auto& tag : other.m_Tags)
    {
        if (HasTag(tag))
            return true;
    }
    return false;
}

bool GameplayTagContainer::HasAllTags(const GameplayTagContainer& other) const
{
    for (const auto& tag : other.m_Tags)
    {
        if (!HasTag(tag))
            return false;
    }
    return true;
}

void GameplayTagContainer::AppendTags(const GameplayTagContainer& other)
{
    for (const auto& tag : other.m_Tags)
    {
        m_Tags.push_back(tag);
    }
}

void GameplayTagContainer::RemoveTags(const GameplayTagContainer& other)
{
    m_Tags.erase(std::remove_if(m_Tags.begin(),
                                m_Tags.end(),
                                [other](const GameplayTag& t) {
                                    for (const auto& tag : other.getTags())
                                    {
                                        if (t.getHash() == tag.getHash())
                                            return true;
                                    }
                                    return false;
                                }),
                 m_Tags.end());
}

// ============================================================================
// AttributeValue Implementation
// ============================================================================

void AttributeValue::SetBaseValue(float value)
{
    m_BaseValue = value;
    Recalculate();
}

void AttributeValue::AddModifier(const AttributeModifier& modifier)
{
    m_Modifiers.push_back(modifier);
    Recalculate();
}

void AttributeValue::RemoveModifier(const std::string& source_name)
{
    m_Modifiers.erase(
        std::remove_if(m_Modifiers.begin(),
                       m_Modifiers.end(),
                       [&source_name](const AttributeModifier& mod) { return mod.source_name == source_name; }),
        m_Modifiers.end());
    Recalculate();
}

void AttributeValue::ClearModifiers()
{
    m_Modifiers.clear();
    Recalculate();
}

void AttributeValue::Recalculate()
{
    float result = m_BaseValue;

    // 先应用所有加法修改器
    for (const auto& modifier : m_Modifiers)
    {
        if (modifier.operation == AttributeModifier::ModifierOp::Add)
        {
            result += modifier.value;
        }
    }

    // 然后应用所有乘法修改器
    for (const auto& modifier : m_Modifiers)
    {
        if (modifier.operation == AttributeModifier::ModifierOp::Multiply)
        {
            result *= modifier.value;
        }
    }

    // 最后应用覆盖修改器（如果有）
    for (const auto& modifier : m_Modifiers)
    {
        if (modifier.operation == AttributeModifier::ModifierOp::Override)
        {
            result = modifier.value;
            break;  // 覆盖操作只应用最后一个
        }
    }

    m_CurrentValue = result;
}
