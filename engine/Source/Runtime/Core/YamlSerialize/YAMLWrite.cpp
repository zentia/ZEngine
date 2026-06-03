#include "Runtime/Core/YamlSerialize/YAMLWrite.h"

#include "Runtime/Core/YamlSerialize/YamlText.h"
#include "core/Log/LogSystem.h"

const TextTransferTraits<YAMLWrite>::InternalNodeRef TextTransferTraits<YAMLWrite>::kInvalidNode = NULL;

void YAMLWrite::TransferStringToCurrentNode(const char* str)
{
    m_CurrentNode->SetString(str, m_Allocator);
}

void YAMLWrite::AppendToNode(Value* parentNode, const char* keyStr, Value& valueNode)
{
    switch (parentNode->GetType())
    {
        case rapidjson::kObjectType:
        {
            Value nameNode(keyStr, m_Allocator);
            parentNode->AddMember(nameNode, valueNode, m_Allocator);
            break;
        }
        case rapidjson::kArrayType:
            parentNode->PushBack(valueNode, m_Allocator);
            break;
        default:
            LOG_ERROR(ZEngine, "Unexpected node type.");
            break;
    }
}

YAMLWrite::YAMLWrite(TransferInstructionFlags flags)
    : TextTransferWriteBase<YAMLWrite>(flags), m_Document(&m_Allocator, 1024, &m_Allocator)
{
    m_Document.SetObject();
    m_CurrentNode = &m_Document;
}

void YAMLWrite::OutputToString(eastl::string& str, bool /*prettyPrint*/)
{
    ZYaml::EmitYaml(m_Document, str);
}

void YAMLSerializeTraits<const char*>::Transfer(const char* const& data, YAMLWrite& transfer)
{
    transfer.TransferStringData(data);
}
