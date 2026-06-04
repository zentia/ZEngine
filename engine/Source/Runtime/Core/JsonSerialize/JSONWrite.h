#pragma once

#include "Runtime/Core/JsonSerialize/JSONSerializeTraits.h"
#include "Runtime/Core/Serialize/TransferFunctions/TextTransferWriteBase.h"
#include "Runtime/Utility/Word.h"
#include "rapidjson/document.h"

class JSONWrite;

template<>
struct TextTransferTraits<JSONWrite>
{
    typedef rapidjson::GenericValue<rapidjson::UTF8<>, JSONAllocator> InternalNode;
    typedef InternalNode MetaParentNodeType;
    typedef InternalNode* InternalNodeRef;
    static const InternalNodeRef kInvalidNode;
};

class JSONWrite : public TextTransferWriteBase<JSONWrite>
{
public:
    typedef rapidjson::GenericDocument<rapidjson::UTF8<>, JSONAllocator, JSONAllocator> Document;
    typedef TextTransferTraits<JSONWrite>::InternalNode Value;

private:
    JSONAllocator m_Allocator;
    Document m_Document;

    void TransferStringToCurrentNode(const char* str);
    void AppendToNode(Value* parentNode, const char* keyStr, Value& valueNode);

    friend class TextTransferWriteBase<JSONWrite>;

public:
    JSONWrite(TransferInstructionFlags flags);

    // Static dispatch hooks consumed by templates such as PPtr<T>::Transfer
    // via `if constexpr`. JSONWrite is a write transfer.
    static constexpr bool IsReading() noexcept { return false; }
    static constexpr bool IsWriting() noexcept { return true; }

    template<typename T>
    void Transfer(T& data, const char* name, TransferMetaFlags metaFlag = TransferMetaFlags::kNoTransferFlags);

    template<typename T>
    void TransferBasicData(T& data);

    template<typename T>
    void TransferArray(T& data, TransferMetaFlags metaFlag = kNoTransferFlags);

    template<typename T>
    void TransferMap(T& data, TransferMetaFlags metaFlag = kNoTransferFlags);

    void OutputToString(eastl::string& str, bool prettyPrint = false);

    template<typename T>
    void TransferPair(T& data, TransferMetaFlags metaFlag = kNoTransferFlags);
};

template<>
inline void JSONWrite::TransferBasicData<bool>(bool& data)
{
    m_CurrentNode->SetBool(data);
}

template<>
inline void JSONWrite::TransferBasicData<float>(float& data)
{
    m_CurrentNode->SetDouble(data);
}

template<>
inline void JSONWrite::TransferBasicData<double>(double& data)
{
    m_CurrentNode->SetDouble(data);
}

template<>
inline void JSONWrite::TransferBasicData<char>(char& data)
{
    m_CurrentNode->SetUint(data);
}

template<>
inline void JSONWrite::TransferBasicData<int8_t>(int8_t& data)
{
    if (!m_MetaFlags.empty() && (m_MetaFlags.back() & kTreatIntegerValueAsBoolean))
    {
        m_CurrentNode->SetBool(data != 0);
    }
    else
    {
        m_CurrentNode->SetUint(data);
    }
}

template<>
inline void JSONWrite::TransferBasicData<int32_t>(int32_t& data)

{
    m_CurrentNode->SetUint(data);
}

template<>
inline void JSONWrite::TransferBasicData<uint32_t>(uint32_t& data)
{
    m_CurrentNode->SetUint(data);
}

template<>
inline void JSONWrite::TransferBasicData<uint16_t>(uint16_t& data)
{
    m_CurrentNode->SetUint(data);
}

template<>
inline void JSONWrite::TransferBasicData<uint8_t>(uint8_t& data)
{
    if (!m_MetaFlags.empty() && (m_MetaFlags.back() & kTreatIntegerValueAsBoolean))
    {
        m_CurrentNode->SetBool(data != 0);
    }
    else
    {
        m_CurrentNode->SetUint(data);
    }
}

template<>
inline void JSONWrite::TransferBasicData<int64_t>(int64_t& data)

{
    if (!m_MetaFlags.empty() && (m_MetaFlags.back() & kTransferHex64))
    {
        char valueStr[sizeof(int64_t) * 2 + 1];
        BytesToHexString(&data, sizeof(int64_t), valueStr);
        valueStr[sizeof(int64_t) * 2] = '\0';
        TransferStringToCurrentNode(valueStr);
    }
    else
    {
        m_CurrentNode->SetUint64(data);
    }
}

template<>
inline void JSONWrite::TransferBasicData<uint64_t>(uint64_t& data)

{
    if (!m_MetaFlags.empty() && (m_MetaFlags.back() & kTransferHex64))
    {
        char valueStr[sizeof(uint64_t) * 2 + 1];
        BytesToHexString(&data, sizeof(uint64_t), valueStr);
        valueStr[sizeof(uint64_t) * 2] = '\0';
        TransferStringToCurrentNode(valueStr);
    }
    else
    {
        m_CurrentNode->SetUint64(data);
    }
}

template<typename T>
void JSONWrite::Transfer(T& data, const char* _name, TransferMetaFlags metaFlag)

{
    Value* parent = m_CurrentNode;

    Value child;
    child.SetObject();
    m_CurrentNode = &child;
    m_MetaFlags.push_back(metaFlag);

    JSONSerializeTraits<T>::Transfer(data, *this);

    m_MetaFlags.pop_back();

    if (m_CurrentNode != NULL)
    {
        AppendToNode(parent, _name, child);
    }

    m_CurrentNode = parent;
}

template<typename T>
void JSONWrite::TransferArray(T& data, TransferMetaFlags metaFlag)

{
    m_CurrentNode->SetArray();

    typename T::iterator i = data.begin();
    typename T::iterator end = data.end();
    while (i != end)
    {
        Transfer(*i, "data", metaFlag);
        ++i;
    }
}

template<typename T>
void JSONWrite::TransferMap(T& data, TransferMetaFlags metaFlag)
{
    m_CurrentNode->SetArray();

    typename T::iterator i = data.begin();
    typename T::iterator end = data.end();

    Value* parentNode = m_CurrentNode;

    typedef typename NonConstContainerValueType<T>::value_type non_const_value_type;

    while (i != end)
    {
        non_const_value_type& p = reinterpret_cast<non_const_value_type&>(*i);
        Value pairNode;
        pairNode.SetObject();
        m_CurrentNode = &pairNode;

        TransferPair(p, metaFlag);

        parentNode->PushBack(pairNode, m_Allocator);
        i++;
    }

    m_CurrentNode = parentNode;
}

template<typename T>
void JSONWrite::TransferPair(T& data, TransferMetaFlags)
{
    typedef typename T::first_type first_type;
    typedef typename T::second_type second_type;

    Value* parent = m_CurrentNode;

    Value firstNode;
    firstNode.SetObject();
    m_CurrentNode = &firstNode;
    JSONSerializeTraits<first_type>::Transfer(data.first, *this);

    Value secondNode;
    secondNode.SetObject();
    m_CurrentNode = &secondNode;
    JSONSerializeTraits<second_type>::Transfer(data.second, *this);

    parent->AddMember("first", firstNode, m_Document.GetAllocator());
    parent->AddMember("second", secondNode, m_Document.GetAllocator());

    m_CurrentNode = parent;
}