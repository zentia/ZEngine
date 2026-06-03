#include "JSONRead.h"

const JSONRead::Value* JSONRead::GetValueForKey(const JSONRead::Value* parentNode, const char* keyStr)
{
    if (!parentNode)
        return nullptr;

    if (parentNode->GetType() != rapidjson::kObjectType)
    {
        return nullptr;
    }

    JSONRead::Value::ConstMemberIterator value = parentNode->FindMember(keyStr);
    if (value == parentNode->MemberEnd())
        return nullptr;

    return &value->value;
}

const JSONRead::Value*
JSONRead::GetValueForKeyWithNameConversion(const char* type, const Value* parentNode, const char* name)
{
    const Value* resultNode = GetValueForKey(parentNode, name);
    if (resultNode != nullptr)
        return resultNode;

    return nullptr;
}

void JSONRead::Init(TransferInstructionFlags flags)
{
    TextTransferReadBase<JSONRead>::Init(flags);
}

JSONRead::JSONRead(const char* strBuffer, TransferInstructionFlags flags)
    : m_Document(&m_Allocator, 1024, &m_Allocator)
{
    Init(flags);

    m_Document.Parse(strBuffer);

    m_CurrentNode = &m_Document;
}

void JSONRead::ResetCurrentNodeToOriginalState()
{
    m_CurrentNode = &m_Document;
}

void JSONRead::TransferTypelessData(uint32_t size, void* data, TransferMetaFlags metaFlag) {}