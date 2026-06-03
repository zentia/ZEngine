#include "JSONWrite.h"

#include "rapidjson/writer.h"

const TextTransferTraits<JSONWrite>::InternalNodeRef TextTransferTraits<JSONWrite>::kInvalidNode = NULL;

void JSONWrite::TransferStringToCurrentNode(const char* str)
{
    m_CurrentNode->SetString(str, m_Allocator);
}

void JSONWrite::AppendToNode(Value* parentNode, const char* keyStr, Value& valueNode)
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

JSONWrite::JSONWrite(TransferInstructionFlags flags)
    : TextTransferWriteBase<JSONWrite>(flags), m_Document(&m_Allocator, 1024, &m_Allocator)
{
    m_Document.SetObject();
    m_CurrentNode = &m_Document;
}

class TempBufferWriter : NonCopyable
{
public:
    typedef char Ch;

    size_t Tell() const { return buf_.size(); }
    void Put(Ch c) { buf_.push_back(c); }
    void Flush() {}
    const char* GetBuf() { return &buf_[0]; }

private:
    eastl::vector<char> buf_;
};

void JSONWrite::OutputToString(eastl::string& str, bool prettyPrint)
{
    TempBufferWriter stream;

    if (!prettyPrint)
    {
        rapidjson::Writer<TempBufferWriter, rapidjson::UTF8<>, rapidjson::UTF8<>, JSONAllocator> writer(stream, &m_Allocator);
        m_Document.Accept(writer);
    }

    str.assign(stream.GetBuf(), stream.Tell());
}

void JSONSerializeTraits<const char*>::Transfer(const char* const& data, JSONWrite& transfer)
{
    transfer.TransferStringData(data);
}