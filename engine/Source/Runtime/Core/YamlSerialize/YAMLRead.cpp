#include "Runtime/Core/YamlSerialize/YAMLRead.h"

#include "Runtime/Core/YamlSerialize/YamlText.h"

const YAMLRead::Value* YAMLRead::GetValueForKey(const YAMLRead::Value* parentNode, const char* keyStr)
{
    if (!parentNode)
        return nullptr;

    if (parentNode->GetType() != rapidjson::kObjectType)
    {
        return nullptr;
    }

    YAMLRead::Value::ConstMemberIterator value = parentNode->FindMember(keyStr);
    if (value == parentNode->MemberEnd())
        return nullptr;

    return &value->value;
}

const YAMLRead::Value*
YAMLRead::GetValueForKeyWithNameConversion(const char* type, const Value* parentNode, const char* name)
{
    const Value* resultNode = GetValueForKey(parentNode, name);
    if (resultNode != nullptr)
        return resultNode;

    return nullptr;
}

void YAMLRead::Init(TransferInstructionFlags flags)
{
    TextTransferReadBase<YAMLRead>::Init(flags);
}

YAMLRead::YAMLRead(const char* strBuffer, TransferInstructionFlags flags)
    : m_Document(&m_Allocator, 1024, &m_Allocator)
{
    Init(flags);

    ZYaml::ParseYaml(strBuffer, m_Document);

    m_CurrentNode = &m_Document;
}

YAMLRead::YAMLRead(const Value& node, TransferInstructionFlags flags)
    : m_Document(&m_Allocator, 1024, &m_Allocator)
{
    Init(flags);

    m_Document.CopyFrom(node, m_Document.GetAllocator());

    m_CurrentNode = &m_Document;
}

void YAMLRead::ResetCurrentNodeToOriginalState()
{
    m_CurrentNode = &m_Document;
}

void YAMLRead::TransferTypelessData(uint32_t size, void* data, TransferMetaFlags metaFlag) {}
